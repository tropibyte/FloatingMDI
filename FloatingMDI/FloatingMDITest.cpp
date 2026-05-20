#include "framework.h"
#include "FloatingMDI.h"
#include "FloatingMDITest.h"

#define MAX_LOADSTRING 100

// Global Variables:
HINSTANCE hInst;                                // current instance
WCHAR szTitle[MAX_LOADSTRING];                  // The title bar text
WCHAR szWindowClass[MAX_LOADSTRING];            // the main window class name
WCHAR szChildClass[MAX_LOADSTRING];             // the MDI child window class name
HWND hMDIClient = nullptr;                      // MDI client window handle
HWND hMainFrame = nullptr;                      // main MDI frame window handle
int  gChildCount = 0;                           // running counter for child window titles
bool gDarkWorkspace = false;                    // Options > Dark Workspace
bool gHideFloats   = false;                     // Options > Hide Floats When Minimized
HBRUSH gWorkspaceBrush = nullptr;               // custom workspace brush (lazy)

// Forward declarations of functions included in this code module:
ATOM                MyRegisterClass(HINSTANCE hInstance);
ATOM                MyRegisterChildClass(HINSTANCE hInstance);
BOOL                InitInstance(HINSTANCE, int);
LRESULT CALLBACK    WndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK    MDIChildWndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK    About(HWND, UINT, WPARAM, LPARAM);
HWND                CreateNewMDIChild(HWND hMDIClient);


int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                     _In_opt_ HINSTANCE hPrevInstance,
                     _In_ LPWSTR    lpCmdLine,
                     _In_ int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    // Initialize global strings
    LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
    LoadStringW(hInstance, IDC_FLOATINGMDITEST, szWindowClass, MAX_LOADSTRING);
    LoadStringW(hInstance, IDC_FLOATINGMDICHILD, szChildClass, MAX_LOADSTRING);
    MyRegisterClass(hInstance);
    MyRegisterChildClass(hInstance);
    RegisterFloatingMDIClientClass(hInstance);

    // Perform application initialization:
    if (!InitInstance (hInstance, nCmdShow))
    {
        return FALSE;
    }

    HACCEL hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_FLOATINGMDITEST));

    MSG msg;

    // Main message loop: TranslateMDISysAccel must run first so Ctrl+F4/F6/Tab
    // reach the MDI client before regular accelerators or dispatch.
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        // Translate against the FRAME, not msg.hwnd. With FMC's children
        // not chained via DefMDIChildProc, focus typically sits on a child
        // (or a floating host) — sending WM_COMMAND there would drop the
        // command. The frame's WndProc is what actually handles them.
        if (!TranslateAccelerator(hMainFrame, hAccelTable, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    return (int) msg.wParam;
}

ATOM MyRegisterClass(HINSTANCE hInstance)
{
    WNDCLASSEXW wcex;

    wcex.cbSize = sizeof(WNDCLASSEX);

    wcex.style          = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc    = WndProc;
    wcex.cbClsExtra     = 0;
    wcex.cbWndExtra     = 0;
    wcex.hInstance      = hInstance;
    wcex.hIcon          = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_FLOATINGMDITEST));
    wcex.hCursor        = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground  = (HBRUSH)(COLOR_APPWORKSPACE + 1);
    wcex.lpszMenuName   = MAKEINTRESOURCEW(IDC_FLOATINGMDITESTMENU);
    wcex.lpszClassName  = szWindowClass;
    wcex.hIconSm        = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));

    return RegisterClassExW(&wcex);
}

ATOM MyRegisterChildClass(HINSTANCE hInstance)
{
    WNDCLASSEXW wcex = {};
    wcex.cbSize        = sizeof(WNDCLASSEX);
    wcex.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wcex.lpfnWndProc   = MDIChildWndProc;
    wcex.hInstance     = hInstance;
    wcex.hIcon         = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_SMALL));
    wcex.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszClassName = szChildClass;
    wcex.hIconSm       = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_SMALL));

    return RegisterClassExW(&wcex);
}

BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
   hInst = hInstance;

   HWND hWnd = CreateWindowW(szWindowClass, szTitle, WS_OVERLAPPEDWINDOW,
      CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, nullptr, nullptr, hInstance, nullptr);

   if (!hWnd)
   {
      return FALSE;
   }

   hMainFrame = hWnd;
   ShowWindow(hWnd, nCmdShow);
   UpdateWindow(hWnd);

   return TRUE;
}

HWND CreateNewMDIChild(HWND hClient)
{
    WCHAR szBase[MAX_LOADSTRING];
    LoadStringW(hInst, IDS_CHILD_TITLE, szBase, MAX_LOADSTRING);

    WCHAR szTitleBuf[MAX_LOADSTRING + 16];
    wsprintfW(szTitleBuf, L"%s %d", szBase, ++gChildCount);

    MDICREATESTRUCTW mcs = {};
    mcs.szClass = szChildClass;
    mcs.szTitle = szTitleBuf;
    mcs.hOwner  = hInst;
    mcs.x = mcs.cx = CW_USEDEFAULT;
    mcs.y = mcs.cy = CW_USEDEFAULT;
    mcs.style   = 0;

    HWND hChild = (HWND)SendMessageW(hClient, WM_MDICREATE, 0, (LPARAM)&mcs);
    return hChild;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
        {
            // Hook the Window submenu so MDI auto-lists open children at its bottom.
            HMENU hMenu = GetMenu(hWnd);
            HMENU hWindowMenu = GetSubMenu(hMenu, 1); // 0=File, 1=Window, 2=Help

            CLIENTCREATESTRUCT ccs;
            ccs.hWindowMenu  = hWindowMenu;
            ccs.idFirstChild = 50000;

            hMDIClient = CreateWindowW(
                kFloatingMDIClientClass,
                nullptr,
                WS_CHILD | WS_CLIPCHILDREN | WS_VISIBLE | FMCS_FLATTABS,
                0, 0, 0, 0,
                hWnd,
                (HMENU)1,
                hInst,
                (LPVOID)&ccs);

            if (!hMDIClient)
            {
                return -1;
            }
        }
        break;

    case WM_COMMAND:
        {
            int wmId = LOWORD(wParam);
            switch (wmId)
            {
            case IDM_FILE_NEW:
                CreateNewMDIChild(hMDIClient);
                break;
            case IDM_ABOUT:
                DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, About);
                break;
            case IDM_EXIT:
                DestroyWindow(hWnd);
                break;
            case IDM_WINDOW_NEXT:
                SendMessageW(hMDIClient, WM_MDINEXT, 0, 0);
                break;
            case IDM_WINDOW_PREV:
                SendMessageW(hMDIClient, WM_MDINEXT, 0, 1);
                break;
            case IDM_WINDOW_MOVE_LEFT:
                {
                    int idx = (int)SendMessageW(hMDIClient, FMCM_GETACTIVETAB, 0, 0);
                    if (idx > 0)
                        SendMessageW(hMDIClient, FMCM_MOVE_TAB, idx, idx - 1);
                }
                break;
            case IDM_WINDOW_MOVE_RIGHT:
                {
                    int idx = (int)SendMessageW(hMDIClient, FMCM_GETACTIVETAB, 0, 0);
                    if (idx >= 0)
                        SendMessageW(hMDIClient, FMCM_MOVE_TAB, idx, idx + 1);
                }
                break;
            case IDM_VIEW_DARKWORKSPACE:
                gDarkWorkspace = !gDarkWorkspace;
                CheckMenuItem(GetMenu(hWnd), IDM_VIEW_DARKWORKSPACE,
                    MF_BYCOMMAND | (gDarkWorkspace ? MF_CHECKED : MF_UNCHECKED));
                InvalidateRect(hMDIClient, nullptr, TRUE);
                break;
            case IDM_VIEW_HIDEFLOATS:
                gHideFloats = !gHideFloats;
                CheckMenuItem(GetMenu(hWnd), IDM_VIEW_HIDEFLOATS,
                    MF_BYCOMMAND | (gHideFloats ? MF_CHECKED : MF_UNCHECKED));
                SendMessageW(hMDIClient, FMCM_SET_HIDEFLOATS, gHideFloats ? TRUE : FALSE, 0);
                break;
            case IDM_VIEW_FLATTABS:
                {
                    // Toggle the FMCS_FLATTABS window style at runtime; FMC
                    // reads it live and repaints on WM_STYLECHANGED.
                    LONG_PTR style = GetWindowLongPtrW(hMDIClient, GWL_STYLE);
                    bool on = (style & FMCS_FLATTABS) == 0;
                    style = on ? (style | FMCS_FLATTABS) : (style & ~(LONG_PTR)FMCS_FLATTABS);
                    SetWindowLongPtrW(hMDIClient, GWL_STYLE, style);
                    CheckMenuItem(GetMenu(hWnd), IDM_VIEW_FLATTABS,
                        MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
                }
                break;
            case IDM_WINDOW_CASCADE:
                SendMessageW(hMDIClient, WM_MDICASCADE, 0, 0);
                break;
            case IDM_WINDOW_TILE_HORZ:
                SendMessageW(hMDIClient, WM_MDITILE, MDITILE_HORIZONTAL, 0);
                break;
            case IDM_WINDOW_TILE_VERT:
                SendMessageW(hMDIClient, WM_MDITILE, MDITILE_VERTICAL, 0);
                break;
            case IDM_WINDOW_ARRANGE:
                SendMessageW(hMDIClient, WM_MDIICONARRANGE, 0, 0);
                break;
            case IDM_WINDOW_CLOSE:
                // Ctrl+F4 — FMC picks the contextually-active child (focused
                // float, active docked child, or frontmost float) and closes
                // it through the veto/prompt path.
                SendMessageW(hMDIClient, FMCM_CLOSEACTIVE, 0, 0);
                break;
            case IDM_WINDOW_CLOSE_ALL:
                // Closes docked + floating; each dirty child gets a prompt.
                SendMessageW(hMDIClient, FMCM_CLOSEALL, 0, 0);
                break;
            default:
                // Child-activation IDs (>= idFirstChild) come from the Window
                // menu. DefFloatingFrameProc forwards them to the MDI client.
                return DefFloatingFrameProc(hWnd, hMDIClient, message, wParam, lParam);
            }
        }
        break;

    case FMCM_CTLCOLOR:
        // FMC asks for a surface color. Override the workspace when the
        // "Dark Workspace" option is on; otherwise decline (return 0) and
        // FMC uses its default grey.
        if (gDarkWorkspace && lParam == FMC_CLR_WORKSPACE)
        {
            if (!gWorkspaceBrush)
                gWorkspaceBrush = CreateSolidBrush(RGB(32, 32, 38));
            return (LRESULT)gWorkspaceBrush;
        }
        return 0;

    case WM_CLOSE:
        // Query every child first — a dirty document may prompt and cancel.
        if (SendMessageW(hMDIClient, FMCM_CLOSEALL, 0, 0))
            DestroyWindow(hWnd);
        return 0;

    case WM_DESTROY:
        if (gWorkspaceBrush) DeleteObject(gWorkspaceBrush);
        PostQuitMessage(0);
        break;

    default:
        return DefFloatingFrameProc(hWnd, hMDIClient, message, wParam, lParam);
    }
    return 0;
}

LRESULT CALLBACK MDIChildWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);

            RECT rc;
            GetClientRect(hWnd, &rc);

            WCHAR szText[MAX_LOADSTRING + 16];
            int len = GetWindowTextW(hWnd, szText, ARRAYSIZE(szText));
            DrawTextW(hdc, szText, len, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            EndPaint(hWnd, &ps);
        }
        return 0;

    case WM_LBUTTONDBLCLK:
        {
            // Demo: rename the child on double-click to exercise live
            // tab-title sync. The " *" suffix doubles as a "dirty" marker.
            WCHAR title[MAX_LOADSTRING + 16];
            GetWindowTextW(hWnd, title, ARRAYSIZE(title));
            int len = lstrlenW(title);
            if (len >= 2 && title[len - 2] == L' ' && title[len - 1] == L'*')
                title[len - 2] = L'\0';
            else
                lstrcatW(title, L" *");
            SetWindowTextW(hWnd, title);   // → DefFloatingMDIChildProc syncs the tab
            InvalidateRect(hWnd, nullptr, TRUE);
        }
        return 0;

    case WM_CLOSE:
        {
            // Demo of the close-veto flow: a "dirty" document (" *" suffix)
            // prompts; Cancel vetoes the close. A clean document closes
            // silently. Agreeing routes through the MDI destroy path.
            WCHAR title[MAX_LOADSTRING + 16];
            GetWindowTextW(hWnd, title, ARRAYSIZE(title));
            int len = lstrlenW(title);
            bool dirty = (len >= 2 && title[len - 2] == L' ' && title[len - 1] == L'*');
            if (dirty)
            {
                WCHAR prompt[MAX_LOADSTRING + 64];
                wsprintfW(prompt, L"Save changes to \"%s\"?", title);
                int r = MessageBoxW(hWnd, prompt, L"FloatingMDI",
                                    MB_YESNOCANCEL | MB_ICONWARNING);
                if (r == IDCANCEL)
                    return 0;   // veto — child survives
            }
            SendMessageW(GetParent(hWnd), WM_MDIDESTROY, (WPARAM)hWnd, 0);
        }
        return 0;
    }
    // Forward to the child-side default proc (counterpart to DefMDIChildProc)
    // so caption changes propagate to the tab / floating host.
    return DefFloatingMDIChildProc(hWnd, message, wParam, lParam);
}

// Message handler for about box.
INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    switch (message)
    {
    case WM_INITDIALOG:
        return (INT_PTR)TRUE;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(hDlg, LOWORD(wParam));
            return (INT_PTR)TRUE;
        }
        break;
    }
    return (INT_PTR)FALSE;
}
