// FloatingMDIClient — a from-scratch MDI client + frame proc.
//
// Design notes:
//   • Not derived from MDICLIENT — owns the WM_MDI* protocol itself.
//   • Children are plain WS_CHILDs of FMC (when docked) or of a future
//     FloatingMDIHost popup (when floating, step 3). No WS_EX_MDICHILD.
//   • The system DefFrameProc loops at ~50k WM_PAINT/sec when its MDI-client
//     argument isn't a real MDICLIENT — so we ship our own minimal replacement
//     (DefFloatingFrameProc) for the frame to call. It implements only the
//     pieces of DefFrameProc that we actually need.
//
// This file is step 1 + basic children: creating, destroying, activating,
// the auto-listed Window menu, and active-child-fills-FMC layout. No tabs
// yet, no floating hosts yet.

#include "framework.h"
#include "FloatingMDI.h"
#include <vector>
#include <algorithm>
#include <commctrl.h>
#include <windowsx.h>            // GET_X_LPARAM / GET_Y_LPARAM
#pragma comment(lib, "comctl32.lib")

const wchar_t* const kFloatingMDIClientClass = L"FloatingMDIClient";
const wchar_t* const kFloatingMDIHostClass   = L"FloatingMDIHost";
static const wchar_t* const kDockHintClass   = L"FloatingMDIDockHint";

// FMCM_TEAROFF / FMCM_REDOCK / FMCM_MOVE_TAB are declared in FloatingMDI.h
// as part of the public FMC message protocol.

// Custom WM_SYSCOMMAND id for the host's "Dock to MDI" item. Must be < 0xF000
// (system range) and aligned to 16 (low 4 bits reserved by Windows).
#define SC_DOCK_TO_MDI 0x1000

// Internal: DefFloatingFrameProc → FMC, frame minimize-state changed.
// wParam = TRUE if the frame just minimized, FALSE if restored/maximized.
#define FMCM_FRAMEMINIMIZED (WM_APP + 0x180)

// Vertical drag past this many pixels on a tab initiates tearoff.
static const int kTearoffThreshold = 18;

struct ChildEntry
{
    HWND  hChild       = nullptr;
    HWND  hHost        = nullptr;   // nullptr = docked; non-null = floating (step 3)
    DWORD savedExStyle = 0;
};

struct FMCState
{
    UINT  idFirstChild         = 0;
    HMENU hWindowMenu          = nullptr;
    UINT  windowMenuFixedCount = 0; // # items in submenu before children appended
    HWND  hTabCtrl     = nullptr;   // tab strip across the top
    int   tabStripHeight = 24;      // live-measured height of the tab strip
    HWND  hDockHint    = nullptr;   // layered overlay shown during a redock drag
    std::vector<ChildEntry> children;
    HWND  hActive      = nullptr;
    bool  isDestroying = false;
    bool  hideFloatsOnMinimize = false;  // FMCM_SET_HIDEFLOATS setting
    bool  floatsHiddenByMin    = false;  // we hid the floats; restore on un-minimize
};

static FMCState* GetState(HWND hWnd)
{
    return reinterpret_cast<FMCState*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
}

static ChildEntry* FindByChild(FMCState* s, HWND h)
{
    for (auto& e : s->children) if (e.hChild == h) return &e;
    return nullptr;
}

// Tab strip helpers. Each tab's lParam holds the child HWND, so the strip is
// the source of truth for tab→child mapping.
static int TabIndexFor(HWND hTab, HWND hChild)
{
    int n = TabCtrl_GetItemCount(hTab);
    for (int i = 0; i < n; i++)
    {
        TCITEMW tci = {};
        tci.mask = TCIF_PARAM;
        TabCtrl_GetItem(hTab, i, &tci);
        if ((HWND)tci.lParam == hChild) return i;
    }
    return -1;
}

static void InsertTabFor(HWND hTab, HWND hChild)
{
    WCHAR title[256];
    GetWindowTextW(hChild, title, ARRAYSIZE(title));

    TCITEMW tci = {};
    tci.mask    = TCIF_TEXT | TCIF_PARAM;
    tci.pszText = title;
    tci.lParam  = (LPARAM)hChild;
    TabCtrl_InsertItem(hTab, TabCtrl_GetItemCount(hTab), &tci);
}

// ---- FloatingMDIHost: top-level popup that hosts a torn-off MDI child ----
//
// Owner-pointer (hOwnerFMC) lets the host route WM_MDI* back to FMC, so a
// floating child's SendMessage(GetParent, WM_MDIDESTROY) still works. The
// host's WM_DESTROY destroys its child too — guaranteeing "no orphans."

struct FMHState
{
    HWND hOwnerFMC     = nullptr;
    HWND hChild        = nullptr;
    bool pendingRedock = false;     // cursor was over FMC's dock zone on last WM_MOVING
};

static FMHState* GetHostState(HWND hWnd)
{
    return reinterpret_cast<FMHState*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
}

static LRESULT CALLBACK FMHWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_NCCREATE:
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    case WM_CREATE:
    {
        // Add "Dock to MDI" to the system menu (right-click on caption).
        HMENU hSysMenu = GetSystemMenu(hWnd, FALSE);
        if (hSysMenu)
        {
            InsertMenuW(hSysMenu, 0, MF_BYPOSITION | MF_STRING,
                        SC_DOCK_TO_MDI, L"&Dock to MDI Window");
            InsertMenuW(hSysMenu, 1, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
        }
        return 0;
    }

    case WM_SYSCOMMAND:
        // Low 4 bits of wParam are reserved by Windows — mask them off.
        if ((wParam & 0xFFF0) == SC_DOCK_TO_MDI)
        {
            auto* s = GetHostState(hWnd);
            if (s && s->hOwnerFMC)
                SendMessageW(s->hOwnerFMC, FMCM_REDOCK, (WPARAM)hWnd, 0);
            return 0;
        }
        return DefWindowProcW(hWnd, msg, wParam, lParam);

    case WM_SIZE:
    {
        auto* s = GetHostState(hWnd);
        if (s && s->hChild && IsWindow(s->hChild))
        {
            SetWindowPos(s->hChild, nullptr, 0, 0, LOWORD(lParam), HIWORD(lParam),
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    }

    case WM_SETFOCUS:
    {
        auto* s = GetHostState(hWnd);
        if (s && s->hChild) SetFocus(s->hChild);
        return 0;
    }

    case WM_MOVING:
    {
        // While the user drags us by the caption, track whether the cursor
        // is over the owner FMC's dock zone. On entering/leaving the zone,
        // show/hide the dashed dock hint.
        auto* s = GetHostState(hWnd);
        if (s && s->hOwnerFMC)
        {
            POINT pt;
            GetCursorPos(&pt);
            bool over = SendMessageW(s->hOwnerFMC, FMCM_HITTEST_DOCK,
                                     0, (LPARAM)&pt) != 0;
            if (over != s->pendingRedock)
            {
                s->pendingRedock = over;
                SendMessageW(s->hOwnerFMC, FMCM_DOCKHINT, over ? TRUE : FALSE, 0);
            }
        }
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    case WM_EXITSIZEMOVE:
    {
        // Drag finished — clear the hint, and if we ended over the dock zone,
        // redock. Post (not send) FMCM_REDOCK so this WndProc fully unwinds
        // before the host is destroyed.
        auto* s = GetHostState(hWnd);
        if (s && s->hOwnerFMC)
        {
            SendMessageW(s->hOwnerFMC, FMCM_DOCKHINT, FALSE, 0);
            if (s->pendingRedock)
            {
                s->pendingRedock = false;
                PostMessageW(s->hOwnerFMC, FMCM_REDOCK, (WPARAM)hWnd, 0);
            }
        }
        return 0;
    }

    case WM_CLOSE:
    {
        // Route to FMC so the standard WM_MDIDESTROY teardown happens
        // (vector removal, tab cleanup if it had been re-docked, etc.).
        auto* s = GetHostState(hWnd);
        if (s && s->hOwnerFMC && s->hChild)
            SendMessageW(s->hOwnerFMC, WM_MDIDESTROY, (WPARAM)s->hChild, 0);
        return 0;
    }

    case WM_DESTROY:
    {
        // If the child is still parented to us, take it down too.
        auto* s = GetHostState(hWnd);
        if (s && s->hChild && IsWindow(s->hChild) && GetParent(s->hChild) == hWnd)
            DestroyWindow(s->hChild);
        return 0;
    }

    case WM_NCDESTROY:
        delete GetHostState(hWnd);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
        return DefWindowProcW(hWnd, msg, wParam, lParam);

    default:
        // WM_MDI* forwarding: a floating child's SendMessage(GetParent, WM_MDI*)
        // would land here — route it back to FMC so behavior matches docked.
        if (msg >= WM_MDICREATE && msg <= WM_MDIREFRESHMENU)
        {
            auto* s = GetHostState(hWnd);
            if (s && s->hOwnerFMC)
                return SendMessageW(s->hOwnerFMC, msg, wParam, lParam);
        }
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
}

// ---- Tab control subclass: detects drag-to-tear gestures ----

struct TabDragState
{
    HWND  hFMC      = nullptr;
    bool  tracking  = false;
    POINT startPt   = {};       // screen coords at LBUTTONDOWN
    int   startTab  = -1;
};

static LRESULT CALLBACK TabSubclassProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                        UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    auto* drag = reinterpret_cast<TabDragState*>(dwRefData);

    switch (msg)
    {
    case WM_LBUTTONDOWN:
    {
        // Let the tab control do its normal selection first (TCN_SELCHANGE).
        LRESULT r = DefSubclassProc(hWnd, msg, wParam, lParam);

        TCHITTESTINFO ht = {};
        ht.pt.x = GET_X_LPARAM(lParam);
        ht.pt.y = GET_Y_LPARAM(lParam);
        int idx = TabCtrl_HitTest(hWnd, &ht);
        if (idx >= 0)
        {
            drag->tracking = true;
            drag->startTab = idx;
            drag->startPt.x = ht.pt.x;
            drag->startPt.y = ht.pt.y;
            ClientToScreen(hWnd, &drag->startPt);
            SetCapture(hWnd);
        }
        return r;
    }

    case WM_MOUSEMOVE:
    {
        if (drag->tracking && (wParam & MK_LBUTTON))
        {
            POINT curClient = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            POINT curScreen = curClient;
            ClientToScreen(hWnd, &curScreen);
            int dy = curScreen.y - drag->startPt.y;

            if (dy > kTearoffThreshold || dy < -kTearoffThreshold)
            {
                // Crossed the threshold — initiate tearoff. Release capture
                // here; the host will re-acquire via WM_NCLBUTTONDOWN.
                ReleaseCapture();
                drag->tracking = false;

                // Use SendMessage so FMC's reparent + host creation finishes
                // before we return (so the hand-off PostMessage targets a
                // real host).
                SendMessageW(drag->hFMC, FMCM_TEAROFF,
                             (WPARAM)drag->startTab,
                             MAKELPARAM(curScreen.x, curScreen.y));
                return 0;
            }

            // Still inside the tab strip — check whether we've dragged over
            // a different tab and, if so, swap the dragged tab into that slot.
            TCHITTESTINFO ht = {};
            ht.pt = curClient;
            int hit = TabCtrl_HitTest(hWnd, &ht);
            if (hit >= 0 && hit != drag->startTab)
            {
                SendMessageW(drag->hFMC, FMCM_MOVE_TAB,
                             (WPARAM)drag->startTab, (LPARAM)hit);
                drag->startTab = hit;  // the dragged tab now lives at `hit`
            }
        }
        break;
    }

    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        if (drag->tracking)
        {
            drag->tracking = false;
            if (msg == WM_LBUTTONUP) ReleaseCapture();
        }
        break;

    case WM_NCDESTROY:
        RemoveWindowSubclass(hWnd, TabSubclassProc, uIdSubclass);
        delete drag;
        break;
    }

    return DefSubclassProc(hWnd, msg, wParam, lParam);
}

// ---- Tearoff: pull a child out of FMC, into a new FloatingMDIHost ----

// Forward decls — these helpers are defined later in this TU.
static void RefreshWindowMenu(HWND hWnd, FMCState* s);
static void LayoutDocked(HWND hWnd, FMCState* s);
static void ActivateChild(HWND hWnd, FMCState* s, HWND hChild);

static void TearOff(HWND hFMC, FMCState* s, HWND hChild, int screenX, int screenY)
{
    auto* e = FindByChild(s, hChild);
    if (!e || e->hHost) return;  // not docked or already floating

    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hFMC, GWLP_HINSTANCE));

    WCHAR title[256];
    GetWindowTextW(hChild, title, ARRAYSIZE(title));

    // Snapshot the ex-style. We don't actually set WS_EX_MDICHILD on our
    // children, but if someone subclasses FMC to add it, redock would need
    // to restore the original bits.
    e->savedExStyle = GetWindowLongW(hChild, GWL_EXSTYLE);

    // Host size: roughly the current child's client size, with a sensible
    // minimum. Position the host so the cursor lands on its caption.
    RECT childRc;
    GetClientRect(hChild, &childRc);
    int w = childRc.right  + 16; if (w < 360) w = 360;
    int h = childRc.bottom + 40; if (h < 240) h = 240;

    auto* hs = new FMHState{};
    hs->hOwnerFMC = hFMC;
    hs->hChild    = hChild;

    // Own the host to the frame: it follows the frame in z-order and is
    // destroyed when the frame is destroyed (no orphan popups, no
    // independent taskbar entry).
    HWND hFrame = GetAncestor(hFMC, GA_ROOT);

    HWND hHost = CreateWindowExW(
        0, kFloatingMDIHostClass, title,
        WS_OVERLAPPEDWINDOW,    // not visible yet — show after reparent
        screenX - w / 4, screenY - 10, w, h,
        hFrame, nullptr, hInst, hs);

    if (!hHost) { delete hs; return; }

    // Reparent child to the host. Child stays WS_CHILD; only parent changes.
    SetParent(hChild, hHost);

    // Update FMC's view of this entry: it's now floating.
    e->hHost = hHost;

    // Remove the tab.
    int tabIdx = TabIndexFor(s->hTabCtrl, hChild);
    if (tabIdx >= 0) TabCtrl_DeleteItem(s->hTabCtrl, tabIdx);

    // If we just tore off the active docked child, activate the next one.
    if (s->hActive == hChild)
    {
        HWND next = nullptr;
        for (auto& c : s->children)
            if (!c.hHost && c.hChild != hChild) { next = c.hChild; break; }
        s->hActive = nullptr;
        if (next) ActivateChild(hFMC, s, next);
        else { LayoutDocked(hFMC, s); RefreshWindowMenu(hFMC, s); }
    }
    else
    {
        LayoutDocked(hFMC, s);
        RefreshWindowMenu(hFMC, s);
    }

    // Show the host and hand off the drag: WM_NCLBUTTONDOWN with HTCAPTION
    // makes the popup enter its system caption-drag mode while the user is
    // still holding the mouse button — drag stays continuous.
    ShowWindow(hHost, SW_SHOWNORMAL);
    SetForegroundWindow(hHost);
    PostMessageW(hHost, WM_NCLBUTTONDOWN, HTCAPTION, MAKELPARAM(screenX, screenY));
}

// ---- Redock: put a floating child back into the tab strip ----

static void Redock(HWND hFMC, FMCState* s, HWND hHost)
{
    auto* hs = GetHostState(hHost);
    if (!hs || !hs->hChild) return;
    HWND hChild = hs->hChild;

    auto* e = FindByChild(s, hChild);
    if (!e || !e->hHost) return;  // not floating, nothing to redock

    // Reparent back to FMC. Child stays WS_CHILD throughout.
    SetParent(hChild, hFMC);
    e->hHost = nullptr;

    // Re-insert tab and make it active.
    if (s->hTabCtrl) InsertTabFor(s->hTabCtrl, hChild);
    ActivateChild(hFMC, s, hChild);

    // Detach the child from the host so host's WM_DESTROY doesn't take it
    // down with it (the GetParent check would catch this too, but this is
    // belt + suspenders and makes intent explicit).
    hs->hChild = nullptr;

    DestroyWindow(hHost);
}

// ---- Dock hint: a per-pixel layered overlay showing where a float docks ----
//
// A translucent highlight wash over the dock zone plus a crisp dark dashed
// border. Per-pixel alpha (UpdateLayeredWindow) keeps the dashes sharp while
// the fill stays see-through. The window is click-through and topmost.

static void RenderDockHint(HWND hint, int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) return;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;          // top-down DIB
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void*   bits    = nullptr;
    HDC     screen  = GetDC(nullptr);
    HBITMAP dib     = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC     memDC   = CreateCompatibleDC(screen);
    HGDIOBJ oldBmp  = SelectObject(memDC, dib);

    DWORD*   px = static_cast<DWORD*>(bits);
    COLORREF hl = GetSysColor(COLOR_HIGHLIGHT);

    // Translucent highlight fill — premultiplied ARGB (alpha applied to RGB).
    const int fillA = 64;
    DWORD fillPx = (DWORD(fillA) << 24)
                 | (DWORD(GetRValue(hl) * fillA / 255) << 16)
                 | (DWORD(GetGValue(hl) * fillA / 255) << 8)
                 |  DWORD(GetBValue(hl) * fillA / 255);
    for (int i = 0; i < w * h; ++i) px[i] = fillPx;

    // Opaque dark dashed border, 2px thick (alpha 255 → no premultiply scaling).
    const DWORD borderPx = 0xFF303030;
    auto plot = [&](int cx, int cy)
    {
        if (cx >= 0 && cx < w && cy >= 0 && cy < h) px[cy * w + cx] = borderPx;
    };
    const int dash = 5, period = 8;           // 5 px on, 3 px off
    for (int cx = 0; cx < w; ++cx)
        if (cx % period < dash)
            { plot(cx, 0); plot(cx, 1); plot(cx, h - 1); plot(cx, h - 2); }
    for (int cy = 0; cy < h; ++cy)
        if (cy % period < dash)
            { plot(0, cy); plot(1, cy); plot(w - 1, cy); plot(w - 2, cy); }

    POINT ptDst = { x, y };
    POINT ptSrc = { 0, 0 };
    SIZE  size  = { w, h };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(hint, screen, &ptDst, &size, memDC, &ptSrc, 0, &bf, ULW_ALPHA);

    SelectObject(memDC, oldBmp);
    DeleteDC(memDC);
    DeleteObject(dib);
    ReleaseDC(nullptr, screen);
}

static void ShowDockHint(HWND hFMC, FMCState* s, bool show)
{
    if (!show)
    {
        if (s->hDockHint) ShowWindow(s->hDockHint, SW_HIDE);
        return;
    }

    // Dock zone = the FMC's top tab-strip band, in screen coords.
    RECT rc; GetWindowRect(hFMC, &rc);
    rc.bottom = rc.top + s->tabStripHeight;
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;

    if (!s->hDockHint)
    {
        HINSTANCE hInst = reinterpret_cast<HINSTANCE>(
            GetWindowLongPtrW(hFMC, GWLP_HINSTANCE));
        s->hDockHint = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
            WS_EX_TOPMOST  | WS_EX_NOACTIVATE,
            kDockHintClass, nullptr, WS_POPUP,
            rc.left, rc.top, w, h, nullptr, nullptr, hInst, nullptr);
        if (!s->hDockHint) return;
    }

    RenderDockHint(s->hDockHint, rc.left, rc.top, w, h);
    ShowWindow(s->hDockHint, SW_SHOWNA);
    SetWindowPos(s->hDockHint, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// Hide or re-show every floating host (used by the hide-on-minimize setting).
static void SetFloatsHidden(FMCState* s, bool hide)
{
    for (auto& e : s->children)
        if (e.hHost && IsWindow(e.hHost))
            ShowWindow(e.hHost, hide ? SW_HIDE : SW_SHOWNA);
}

static void RefreshWindowMenu(HWND hWnd, FMCState* s)
{
    if (!s->hWindowMenu) return;

    // Trim previously-added child items back to the fixed prefix.
    int count = GetMenuItemCount(s->hWindowMenu);
    while (count > (int)s->windowMenuFixedCount)
    {
        DeleteMenu(s->hWindowMenu, (UINT)count - 1, MF_BYPOSITION);
        count--;
    }

    if (s->children.empty()) { DrawMenuBar(GetAncestor(hWnd, GA_ROOT)); return; }

    AppendMenuW(s->hWindowMenu, MF_SEPARATOR, 0, nullptr);

    int index = 1;
    for (auto& e : s->children)
    {
        WCHAR title[256];
        GetWindowTextW(e.hChild, title, ARRAYSIZE(title));

        WCHAR item[300];
        if (index < 10) swprintf_s(item, L"&%d %s", index, title);
        else            swprintf_s(item, L"%d %s",  index, title);

        UINT id    = s->idFirstChild + (UINT)(index - 1);
        UINT flags = MF_STRING;
        if (e.hChild == s->hActive) flags |= MF_CHECKED;
        AppendMenuW(s->hWindowMenu, flags, id, item);
        index++;
    }
    DrawMenuBar(GetAncestor(hWnd, GA_ROOT));
}

// Tab strip layout — three cases:
//   • docked tabs present : full tab control fills FMC; active child in body.
//   • no docked, but some floating: empty strip at tab height as a redock
//     drop target; grey workspace below.
//   • no children at all  : strip hidden; FMC is bare grey, like empty MDICLIENT.
// Children are siblings of the tab control (both WS_CHILD of FMC).
static void LayoutDocked(HWND hWnd, FMCState* s)
{
    RECT rc; GetClientRect(hWnd, &rc);

    int  dockedTabs  = s->hTabCtrl ? TabCtrl_GetItemCount(s->hTabCtrl) : 0;
    bool anyFloating = false;
    for (auto& e : s->children) if (e.hHost) { anyFloating = true; break; }

    if (s->hTabCtrl)
    {
        if (dockedTabs > 0)
        {
            ShowWindow(s->hTabCtrl, SW_SHOWNA);
            SetWindowPos(s->hTabCtrl, HWND_BOTTOM,
                rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                SWP_NOACTIVATE);

            // Re-measure the live (themed) strip height so the empty-strip
            // case uses an identical value — the WM_CREATE probe runs before
            // theming and can be off by a pixel or two.
            RECT m = rc;
            TabCtrl_AdjustRect(s->hTabCtrl, FALSE, &m);
            int h = m.top - rc.top;
            if (h > 0 && h < 200) s->tabStripHeight = h;
        }
        else if (anyFloating)
        {
            // Empty strip at tab height — a place to drop a float back into.
            ShowWindow(s->hTabCtrl, SW_SHOWNA);
            SetWindowPos(s->hTabCtrl, HWND_BOTTOM,
                rc.left, rc.top, rc.right - rc.left, s->tabStripHeight,
                SWP_NOACTIVATE);
        }
        else
        {
            ShowWindow(s->hTabCtrl, SW_HIDE);
        }
    }

    RECT body = rc;
    if (dockedTabs > 0)
        TabCtrl_AdjustRect(s->hTabCtrl, FALSE, &body);

    for (auto& e : s->children)
    {
        if (e.hHost) continue;  // floating: laid out by its host
        if (e.hChild == s->hActive)
        {
            SetWindowPos(e.hChild, HWND_TOP,
                body.left, body.top, body.right - body.left, body.bottom - body.top,
                SWP_SHOWWINDOW);
        }
        else
        {
            ShowWindow(e.hChild, SW_HIDE);
        }
    }
}

static void ActivateChild(HWND hWnd, FMCState* s, HWND hChild)
{
    if (s->hActive == hChild) return;

    HWND prev  = s->hActive;
    s->hActive = hChild;
    LayoutDocked(hWnd, s);

    // Keep tab selection in sync with active child.
    if (s->hTabCtrl)
    {
        int idx = TabIndexFor(s->hTabCtrl, hChild);
        if (idx >= 0 && TabCtrl_GetCurSel(s->hTabCtrl) != idx)
            TabCtrl_SetCurSel(s->hTabCtrl, idx);
    }

    // Mirror MDI's WM_MDIACTIVATE notification: deactivated first, then active.
    if (prev)   SendMessageW(prev,   WM_MDIACTIVATE, (WPARAM)prev,   (LPARAM)hChild);
    if (hChild) SendMessageW(hChild, WM_MDIACTIVATE, (WPARAM)prev,   (LPARAM)hChild);

    if (hChild) SetFocus(hChild);
    RefreshWindowMenu(hWnd, s);
}

static HWND HandleMDICreate(HWND hWnd, FMCState* s, MDICREATESTRUCTW* mcs)
{
    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hWnd, GWLP_HINSTANCE));

    // WS_CHILD parented to FMC. We omit WS_EX_MDICHILD on purpose — FMC owns
    // the MDI semantics, and the flag would invite DefMDIChildProc to talk to
    // a real MDI client that doesn't exist.
    HWND hChild = CreateWindowExW(
        0,
        mcs->szClass, mcs->szTitle, WS_CHILD | mcs->style,
        0, 0, 0, 0,
        hWnd, nullptr, hInst,
        reinterpret_cast<LPVOID>(mcs->lParam));
    if (!hChild) return nullptr;

    s->children.push_back({hChild, nullptr, 0});
    if (s->hTabCtrl) InsertTabFor(s->hTabCtrl, hChild);
    ActivateChild(hWnd, s, hChild);
    return hChild;
}

static void HandleMDIDestroy(HWND hWnd, FMCState* s, HWND hChild)
{
    auto* e = FindByChild(s, hChild);
    if (!e) return;

    bool wasActive = (s->hActive == hChild);
    HWND host = e->hHost;

    if (s->hTabCtrl)
    {
        int idx = TabIndexFor(s->hTabCtrl, hChild);
        if (idx >= 0) TabCtrl_DeleteItem(s->hTabCtrl, idx);
    }

    s->children.erase(
        std::remove_if(s->children.begin(), s->children.end(),
            [hChild](const ChildEntry& x) { return x.hChild == hChild; }),
        s->children.end());

    if (wasActive)
    {
        HWND next = nullptr;
        for (auto& c : s->children) { if (!c.hHost) { next = c.hChild; break; } }
        s->hActive = nullptr;
        if (next)
        {
            ActivateChild(hWnd, s, next);
        }
        else
        {
            // No docked children left — re-layout so the (now empty) tab
            // strip is hidden and the FMC shows bare grey workspace.
            LayoutDocked(hWnd, s);
            RefreshWindowMenu(hWnd, s);
        }
    }
    else
    {
        // A non-active child went away — could have been the last floating
        // one, so re-layout in case the empty redock strip should now hide.
        LayoutDocked(hWnd, s);
        RefreshWindowMenu(hWnd, s);
    }

    if (host)
        DestroyWindow(host);     // host's WM_DESTROY takes the child with it
    else
        DestroyWindow(hChild);   // docked: destroy directly
}

static void HandleMDINext(HWND hWnd, FMCState* s, HWND hFrom, bool prev)
{
    if (s->children.empty()) return;
    if (!hFrom) hFrom = s->hActive;

    int idx = -1, n = (int)s->children.size();
    for (int i = 0; i < n; i++)
        if (s->children[i].hChild == hFrom) { idx = i; break; }
    if (idx < 0) idx = 0;

    int next = prev ? (idx - 1 + n) % n : (idx + 1) % n;
    ActivateChild(hWnd, s, s->children[next].hChild);
}

static LRESULT CALLBACK FMCWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_NCCREATE:
    {
        auto* cs    = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* state = new FMCState{};
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));

        if (cs->lpCreateParams)
        {
            auto* ccs = reinterpret_cast<CLIENTCREATESTRUCT*>(cs->lpCreateParams);
            state->idFirstChild = ccs->idFirstChild;
            state->hWindowMenu  = static_cast<HMENU>(ccs->hWindowMenu);
            if (state->hWindowMenu)
                state->windowMenuFixedCount = (UINT)GetMenuItemCount(state->hWindowMenu);
        }
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    case WM_CREATE:
    {
        auto* s = GetState(hWnd);
        HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hWnd, GWLP_HINSTANCE));

        // Created hidden — LayoutDocked shows it only when there are tabs,
        // so an empty FMC is just the bare grey workspace.
        s->hTabCtrl = CreateWindowExW(
            0, WC_TABCONTROLW, nullptr,
            WS_CHILD | WS_CLIPSIBLINGS | TCS_SINGLELINE | TCS_FOCUSNEVER,
            0, 0, 0, 0,
            hWnd, nullptr, hInst, nullptr);
        if (!s->hTabCtrl) return -1;

        SendMessageW(s->hTabCtrl, WM_SETFONT,
                     (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);

        // Measure the tab-strip height once, using a throwaway tab, so we can
        // later reserve a proper-height empty strip as a redock drop target.
        {
            WCHAR probeText[] = L"Mg";
            TCITEMW probe = {};
            probe.mask    = TCIF_TEXT;
            probe.pszText = probeText;
            TabCtrl_InsertItem(s->hTabCtrl, 0, &probe);
            RECT r = { 0, 0, 400, 400 };
            TabCtrl_AdjustRect(s->hTabCtrl, FALSE, &r);
            if (r.top > 0 && r.top < 200) s->tabStripHeight = r.top;
            TabCtrl_DeleteItem(s->hTabCtrl, 0);
        }

        // Subclass the tab control so we can detect drag-to-tear gestures.
        auto* drag = new TabDragState{};
        drag->hFMC = hWnd;
        SetWindowSubclass(s->hTabCtrl, TabSubclassProc, 1, (DWORD_PTR)drag);
        return 0;
    }

    case FMCM_TEAROFF:
    {
        auto* s = GetState(hWnd);
        int idx = (int)wParam;
        int sx  = GET_X_LPARAM(lParam);
        int sy  = GET_Y_LPARAM(lParam);
        if (s && s->hTabCtrl && idx >= 0 && idx < TabCtrl_GetItemCount(s->hTabCtrl))
        {
            TCITEMW tci = {};
            tci.mask = TCIF_PARAM;
            TabCtrl_GetItem(s->hTabCtrl, idx, &tci);
            TearOff(hWnd, s, (HWND)tci.lParam, sx, sy);
        }
        return 0;
    }

    case FMCM_REDOCK:
    {
        auto* s = GetState(hWnd);
        if (s) Redock(hWnd, s, (HWND)wParam);
        return 0;
    }

    case FMCM_GETACTIVETAB:
    {
        auto* s = GetState(hWnd);
        if (s && s->hTabCtrl && s->hActive)
            return TabIndexFor(s->hTabCtrl, s->hActive);
        return -1;
    }

    case FMCM_HITTEST_DOCK:
    {
        // Dock zone = the FMC's top tab-strip band, in screen coords.
        auto* s  = GetState(hWnd);
        POINT* pt = reinterpret_cast<POINT*>(lParam);
        if (!s || !pt) return FALSE;
        RECT rc;
        GetWindowRect(hWnd, &rc);
        rc.bottom = rc.top + s->tabStripHeight;
        return PtInRect(&rc, *pt) ? TRUE : FALSE;
    }

    case FMCM_DOCKHINT:
    {
        auto* s = GetState(hWnd);
        if (s) ShowDockHint(hWnd, s, wParam != 0);
        return 0;
    }

    case FMCM_SET_HIDEFLOATS:
    {
        auto* s = GetState(hWnd);
        if (!s) return 0;
        s->hideFloatsOnMinimize = (wParam != 0);

        // Apply immediately to the current frame state.
        bool frameMin = IsIconic(GetAncestor(hWnd, GA_ROOT)) != 0;
        if (s->hideFloatsOnMinimize && frameMin && !s->floatsHiddenByMin)
        {
            SetFloatsHidden(s, true);
            s->floatsHiddenByMin = true;
        }
        else if (!s->hideFloatsOnMinimize && s->floatsHiddenByMin)
        {
            SetFloatsHidden(s, false);
            s->floatsHiddenByMin = false;
        }
        return 0;
    }

    case FMCM_FRAMEMINIMIZED:
    {
        auto* s = GetState(hWnd);
        if (!s) return 0;
        if (wParam)   // frame minimized
        {
            if (s->hideFloatsOnMinimize && !s->floatsHiddenByMin)
            {
                SetFloatsHidden(s, true);
                s->floatsHiddenByMin = true;
            }
        }
        else          // frame restored / maximized
        {
            if (s->floatsHiddenByMin)
            {
                SetFloatsHidden(s, false);
                s->floatsHiddenByMin = false;
            }
        }
        return 0;
    }

    case FMCM_MOVE_TAB:
    {
        auto* s = GetState(hWnd);
        if (!s || !s->hTabCtrl) return 0;
        int src = (int)wParam;
        int dst = (int)lParam;
        int n   = TabCtrl_GetItemCount(s->hTabCtrl);
        if (src < 0 || src >= n || dst < 0 || dst >= n || src == dst) return 0;

        // Snapshot the source tab (text + lParam) so we can re-insert.
        WCHAR text[256];
        TCITEMW tci = {};
        tci.mask       = TCIF_TEXT | TCIF_PARAM;
        tci.pszText    = text;
        tci.cchTextMax = ARRAYSIZE(text);
        TabCtrl_GetItem(s->hTabCtrl, src, &tci);
        HWND hChild = (HWND)tci.lParam;

        // Reorder on the strip.
        TabCtrl_DeleteItem(s->hTabCtrl, src);
        TabCtrl_InsertItem(s->hTabCtrl, dst, &tci);
        TabCtrl_SetCurSel(s->hTabCtrl, dst);

        // Keep the children vector in sync — tab dst-th docked entry slot.
        // The vector mixes docked + floating, so we find the dst-th docked
        // slot by walking and counting.
        auto it = std::find_if(s->children.begin(), s->children.end(),
            [hChild](const ChildEntry& e) { return e.hChild == hChild; });
        if (it != s->children.end())
        {
            ChildEntry entry = *it;
            s->children.erase(it);

            int dockedIdx = 0;
            auto insertAt = s->children.end();
            for (auto i = s->children.begin(); i != s->children.end(); ++i)
            {
                if (i->hHost) continue;          // skip floating entries
                if (dockedIdx == dst) { insertAt = i; break; }
                dockedIdx++;
            }
            s->children.insert(insertAt, entry);
        }

        RefreshWindowMenu(hWnd, s);
        return 0;
    }

    case WM_SIZE:
        if (auto* s = GetState(hWnd)) LayoutDocked(hWnd, s);
        return 0;

    case WM_ERASEBKGND:
    {
        // Ask the frame for a workspace brush (WM_CTLCOLOR-style); fall back
        // to the standard MDI workspace grey if it declines.
        HDC hdc = reinterpret_cast<HDC>(wParam);
        HBRUSH br = reinterpret_cast<HBRUSH>(SendMessageW(
            GetParent(hWnd), FMCM_CTLCOLOR, (WPARAM)hdc, FMC_CLR_WORKSPACE));
        if (!br) br = reinterpret_cast<HBRUSH>(COLOR_APPWORKSPACE + 1);
        RECT rc; GetClientRect(hWnd, &rc);
        FillRect(hdc, &rc, br);
        return 1;
    }

    case WM_NOTIFY:
    {
        auto* s = GetState(hWnd);
        NMHDR* hdr = reinterpret_cast<NMHDR*>(lParam);
        if (s && s->hTabCtrl && hdr->hwndFrom == s->hTabCtrl && hdr->code == TCN_SELCHANGE)
        {
            int sel = TabCtrl_GetCurSel(s->hTabCtrl);
            if (sel >= 0)
            {
                TCITEMW tci = {};
                tci.mask = TCIF_PARAM;
                TabCtrl_GetItem(s->hTabCtrl, sel, &tci);
                ActivateChild(hWnd, s, (HWND)tci.lParam);
            }
            return 0;
        }
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    case WM_SETFOCUS:
    {
        auto* s = GetState(hWnd);
        if (s && s->hActive) SetFocus(s->hActive);
        return 0;
    }

    case WM_MDICREATE:
        if (auto* s = GetState(hWnd))
            return (LRESULT)HandleMDICreate(hWnd, s, reinterpret_cast<MDICREATESTRUCTW*>(lParam));
        return 0;

    case WM_MDIDESTROY:
        if (auto* s = GetState(hWnd)) HandleMDIDestroy(hWnd, s, (HWND)wParam);
        return 0;

    case WM_MDIACTIVATE:
        if (auto* s = GetState(hWnd)) ActivateChild(hWnd, s, (HWND)wParam);
        return 0;

    case WM_MDIGETACTIVE:
        if (lParam) *reinterpret_cast<BOOL*>(lParam) = TRUE;  // docked = always maxed
        return (LRESULT)(GetState(hWnd) ? GetState(hWnd)->hActive : nullptr);

    case WM_MDINEXT:
        if (auto* s = GetState(hWnd)) HandleMDINext(hWnd, s, (HWND)wParam, lParam != 0);
        return 0;

    case WM_MDISETMENU:
    {
        auto* s = GetState(hWnd);
        if (s)
        {
            HMENU hNew = (HMENU)lParam;
            if (hNew && hNew != s->hWindowMenu)
            {
                s->hWindowMenu          = hNew;
                s->windowMenuFixedCount = (UINT)GetMenuItemCount(hNew);
            }
            RefreshWindowMenu(hWnd, s);
        }
        return 0;
    }

    case WM_MDIREFRESHMENU:
        if (auto* s = GetState(hWnd)) RefreshWindowMenu(hWnd, s);
        return 0;

    // Spatial ops are meaningless once one child fills the area; step 3 will
    // reinterpret these to operate on floating hosts.
    case WM_MDICASCADE:
    case WM_MDITILE:
    case WM_MDIICONARRANGE:
    case WM_MDIMAXIMIZE:
    case WM_MDIRESTORE:
        return 0;

    // Window-menu child picks (DefFloatingFrameProc forwards them to us).
    case WM_COMMAND:
        if (auto* s = GetState(hWnd))
        {
            UINT id = LOWORD(wParam);
            if (id >= s->idFirstChild && id < s->idFirstChild + s->children.size())
            {
                ActivateChild(hWnd, s, s->children[id - s->idFirstChild].hChild);
                return 0;
            }
        }
        return DefWindowProcW(hWnd, msg, wParam, lParam);

    case WM_DESTROY:
        if (auto* s = GetState(hWnd))
        {
            s->isDestroying = true;
            if (s->hDockHint) DestroyWindow(s->hDockHint);
            // Destroy hosts BEFORE clearing the vector — entries are needed
            // to find them. Each host's WM_DESTROY destroys its child too.
            for (auto& e : s->children)
                if (e.hHost && IsWindow(e.hHost)) DestroyWindow(e.hHost);
            s->children.clear();
            // Docked children are real WS_CHILDs of FMC — OS destroys them.
        }
        return 0;

    case WM_NCDESTROY:
        delete GetState(hWnd);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
        return DefWindowProcW(hWnd, msg, wParam, lParam);

    default:
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
}

ATOM RegisterFloatingMDIClientClass(HINSTANCE hInstance)
{
    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_TAB_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wcex = {};
    wcex.cbSize        = sizeof(WNDCLASSEXW);
    wcex.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wcex.lpfnWndProc   = FMCWndProc;
    wcex.hInstance     = hInstance;
    wcex.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_APPWORKSPACE + 1);
    wcex.lpszClassName = kFloatingMDIClientClass;
    ATOM fmcAtom = RegisterClassExW(&wcex);

    // Also register the host class — single entry point keeps the test app simple.
    WNDCLASSEXW wch = {};
    wch.cbSize        = sizeof(WNDCLASSEXW);
    wch.style         = CS_HREDRAW | CS_VREDRAW;
    wch.lpfnWndProc   = FMHWndProc;
    wch.hInstance     = hInstance;
    wch.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wch.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wch.lpszClassName = kFloatingMDIHostClass;
    RegisterClassExW(&wch);

    // Dock-hint overlay class. Content is set via UpdateLayeredWindow, so the
    // class proc is just DefWindowProc.
    WNDCLASSEXW wcd = {};
    wcd.cbSize        = sizeof(WNDCLASSEXW);
    wcd.lpfnWndProc   = DefWindowProcW;
    wcd.hInstance     = hInstance;
    wcd.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wcd.lpszClassName = kDockHintClass;
    RegisterClassExW(&wcd);

    return fmcAtom;
}

// -----------------------------------------------------------------------------
// DefFloatingFrameProc — minimal replacement for DefFrameProc.
//
// The system DefFrameProc, when handed a non-MDICLIENT as the MDI client,
// spins WM_PAINT at ~50k/sec. This implementation does only what we need:
//   • WM_COMMAND  : route child-activation IDs to the MDI client
//   • WM_SIZE     : resize the MDI client to fill the frame's client rect
//   • WM_SETFOCUS : pass focus to the MDI client
// Everything else falls through to DefWindowProc.
// -----------------------------------------------------------------------------
LRESULT DefFloatingFrameProc(HWND hWnd, HWND hMDIClient,
                             UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (hMDIClient)
    {
        switch (msg)
        {
        case WM_COMMAND:
            // Forward to MDI client; it knows its own idFirstChild range.
            return SendMessageW(hMDIClient, msg, wParam, lParam);

        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED)
            {
                SendMessageW(hMDIClient, FMCM_FRAMEMINIMIZED, TRUE, 0);
            }
            else
            {
                SendMessageW(hMDIClient, FMCM_FRAMEMINIMIZED, FALSE, 0);
                MoveWindow(hMDIClient, 0, 0, LOWORD(lParam), HIWORD(lParam), TRUE);
            }
            return 0;

        case WM_SETFOCUS:
            SetFocus(hMDIClient);
            return 0;
        }
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}
