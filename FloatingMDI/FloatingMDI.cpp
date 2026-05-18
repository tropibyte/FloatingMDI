// FloatingMDIClient — a drop-in replacement for the system MDICLIENT.
//
// Step 1 (this file): an empty wrapper that owns a real MDICLIENT internally
// and forwards every WM_MDI* and WM_COMMAND message to it. No tabs yet, no
// floating hosts yet — the goal is to prove that existing MDI children work
// unchanged when their parent is FloatingMDIClient instead of MDICLIENT.
//
// Topology (after later steps):
//   Frame
//   └── FloatingMDIClient
//       ├── TabCtrl                  (step 2)
//       └── inner MDICLIENT          ← children of this are docked
//           └── MDI child windows
//
//   FloatingMDIHost (top-level popup, step 3)
//   └── MDI child window (torn off; still WS_CHILD, just reparented)
//
// Lifecycle policy: an MDI child is ALWAYS a WS_CHILD window. It is parented
// either to the inner MDICLIENT (docked) or to a FloatingMDIHost (floating).
// No child is ever reparented to the desktop. On FMC's WM_DESTROY, every
// floating host is destroyed first (taking its child with it), THEN the
// vector is cleared. Docked children die naturally with the inner MDICLIENT.

#include "framework.h"
#include "FloatingMDI.h"
#include <vector>

const wchar_t* const kFloatingMDIClientClass = L"FloatingMDIClient";

// One entry per MDI child the FMC knows about. hHost distinguishes docked
// (nullptr) from floating (non-null). In step 1 the vector stays empty —
// the inner MDICLIENT tracks children itself; we only need the vector once
// tabs and tearoff arrive.
struct ChildEntry
{
    HWND  hChild       = nullptr;
    HWND  hHost        = nullptr;
    int   tabIndex     = -1;
    DWORD savedExStyle = 0;
};

struct FMCState
{
    HWND  hInnerClient = nullptr;
    UINT  idFirstChild = 0;
    HMENU hWindowMenu  = nullptr;
    std::vector<ChildEntry> children;
    bool  isDestroying = false;
};

static FMCState* GetState(HWND hWnd)
{
    return reinterpret_cast<FMCState*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
}

static LRESULT CALLBACK FMCWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_NCCREATE:
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* state = new FMCState{};
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));

        // The frame passes a CLIENTCREATESTRUCT in lpCreateParams — same calling
        // convention as MDICLIENT, which is the whole point of being a drop-in.
        if (cs->lpCreateParams)
        {
            auto* ccs = reinterpret_cast<CLIENTCREATESTRUCT*>(cs->lpCreateParams);
            state->idFirstChild = ccs->idFirstChild;
            state->hWindowMenu  = ccs->hWindowMenu;
        }
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    case WM_CREATE:
    {
        auto* state = GetState(hWnd);
        HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hWnd, GWLP_HINSTANCE));

        CLIENTCREATESTRUCT innerCcs;
        innerCcs.hWindowMenu  = state->hWindowMenu;
        innerCcs.idFirstChild = state->idFirstChild;

        state->hInnerClient = CreateWindowExW(
            0, L"MDICLIENT", nullptr,
            WS_CHILD | WS_CLIPCHILDREN | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL,
            0, 0, 0, 0,           // sized by WM_SIZE below
            hWnd, nullptr, hInst,
            reinterpret_cast<LPVOID>(&innerCcs));

        if (!state->hInnerClient)
            return -1;
        return 0;
    }

    case WM_SIZE:
    {
        auto* state = GetState(hWnd);
        if (state && state->hInnerClient)
        {
            // Step 1: inner MDICLIENT fills the whole FMC client area.
            // Step 2 will reserve a strip at the top for the TabCtrl.
            SetWindowPos(state->hInnerClient, nullptr,
                0, 0, LOWORD(lParam), HIWORD(lParam),
                SWP_NOZORDER);
        }
        return 0;
    }

    case WM_DESTROY:
    {
        auto* state = GetState(hWnd);
        if (state)
        {
            state->isDestroying = true;

            // Destroy hosts BEFORE clearing the vector — we need the entries
            // to find them. Each host's WM_DESTROY destroys its child too.
            // (Empty in step 1; harmless until step 3 wires up tearoff.)
            for (auto& e : state->children)
            {
                if (e.hHost && IsWindow(e.hHost))
                    DestroyWindow(e.hHost);
            }
            state->children.clear();

            // Docked children are real children of the inner MDICLIENT. The
            // OS destroys them as part of FMC's child-window teardown — no
            // explicit action needed here.
        }
        return 0;
    }

    case WM_NCDESTROY:
    {
        auto* state = GetState(hWnd);
        delete state;
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    default:
        break;
    }

    // Forwarding for unhandled messages. Two cases reach the inner MDICLIENT:
    //
    // 1. WM_MDI* — children call SendMessage(GetParent(hChild), WM_MDI*, ...)
    //    and this routes them through. In step 3 the FloatingMDIHost will do
    //    the same forwarding, so a floating child's WM_MDIDESTROY still ends
    //    up here, then is routed to the inner client.
    //
    // 2. WM_COMMAND — DefFrameProc forwards child-activation IDs (>= idFirstChild)
    //    to the MDI client; MDICLIENT's own WM_COMMAND handler activates the
    //    matching child. Forwarding the whole message is simplest; FMC has no
    //    commands of its own yet.
    auto* state = GetState(hWnd);
    if (state && state->hInnerClient)
    {
        if ((msg >= WM_MDICREATE && msg <= WM_MDIREFRESHMENU) || msg == WM_COMMAND)
            return SendMessageW(state->hInnerClient, msg, wParam, lParam);
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

ATOM RegisterFloatingMDIClientClass(HINSTANCE hInstance)
{
    WNDCLASSEXW wcex = {};
    wcex.cbSize        = sizeof(WNDCLASSEXW);
    wcex.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wcex.lpfnWndProc   = FMCWndProc;
    wcex.hInstance     = hInstance;
    wcex.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_APPWORKSPACE + 1);
    wcex.lpszClassName = kFloatingMDIClientClass;
    return RegisterClassExW(&wcex);
}
