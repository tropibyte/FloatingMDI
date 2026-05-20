#pragma once

#include "resource.h"

// Window class name for FloatingMDIClient — a from-scratch MDI client.
// Drop-in API-compatible with MDICLIENT: it accepts a CLIENTCREATESTRUCT in
// lpCreateParams and responds to WM_MDICREATE/DESTROY/ACTIVATE/GETACTIVE/
// NEXT/SETMENU/REFRESHMENU. It is NOT derived from MDICLIENT.
extern const wchar_t* const kFloatingMDIClientClass;

// Register the FloatingMDIClient class. Call once at startup.
ATOM RegisterFloatingMDIClientClass(HINSTANCE hInstance);

// Replacement for the system DefFrameProc. The system one's WM_PAINT path
// loops when the MDI client isn't a real MDICLIENT (50k invalidations/sec).
// Use this from the frame's WndProc default branch instead of DefFrameProc.
LRESULT DefFloatingFrameProc(HWND hWnd, HWND hMDIClient,
                             UINT msg, WPARAM wParam, LPARAM lParam);

// Child-side default proc — the counterpart to DefMDIChildProc, completing
// the triad: DefFloatingFrameProc / FloatingMDIClient / DefFloatingMDIChildProc.
// An MDI child's WndProc should forward unhandled messages here instead of to
// DefWindowProc to get FloatingMDI child behaviors — currently live tab-title
// sync when the child changes its caption (WM_SETTEXT). Opt-in: a child that
// keeps using DefWindowProc still works, just without these behaviors.
LRESULT DefFloatingMDIChildProc(HWND hChild, UINT msg, WPARAM wParam, LPARAM lParam);

// ---- FloatingMDIClient message protocol (beyond the standard WM_MDI*) ----
// The frame (or any code holding the FMC handle) may send these.

// Move a docked tab. wParam = source tab index, lParam = destination index.
// Reorders both the tab strip and FMC's internal child order. Out-of-range
// or equal indices are ignored. This is also what the drag-reorder uses.
#define FMCM_MOVE_TAB (WM_APP + 0x102)

// Tear a docked tab off into a floating host. wParam = tab index,
// lParam = MAKELPARAM(screenX, screenY) for the new popup's position.
#define FMCM_TEAROFF  (WM_APP + 0x100)

// Redock a floating child back into the tab strip. wParam = host HWND.
#define FMCM_REDOCK   (WM_APP + 0x101)

// Query the tab index of the active docked child. Returns the index, or
// -1 if there is no active docked child. wParam/lParam unused.
#define FMCM_GETACTIVETAB (WM_APP + 0x103)

// Hit-test a screen point against FMC's dock zone (the top tab-strip band).
// lParam = POINT* in screen coords. Returns TRUE if the point is in the zone.
#define FMCM_HITTEST_DOCK (WM_APP + 0x104)

// Show/hide the dock-target hint — a dashed outline of the dock zone.
// wParam = TRUE to show, FALSE to hide. Sent by a host during a redock drag.
#define FMCM_DOCKHINT     (WM_APP + 0x105)

// WM_CTLCOLOR analog. FMC sends this to its parent frame before painting a
// surface, letting the frame override colors at runtime. wParam = HDC,
// lParam = one of the FMC_CLR_* part ids. The parent returns an HBRUSH for
// that part (and may SetBkColor/SetTextColor on the HDC), or 0 to accept
// FMC's default. The parent owns the returned brush (don't create per call).
#define FMCM_CTLCOLOR     (WM_APP + 0x106)

// Part ids for FMCM_CTLCOLOR's lParam.
#define FMC_CLR_WORKSPACE 0     // the deep-grey MDI workspace background

// Enable/disable auto-hiding the floating children when the frame is
// minimized (and re-showing them on restore). wParam = TRUE to enable.
#define FMCM_SET_HIDEFLOATS (WM_APP + 0x107)

// Close every child (docked + floating). Each child receives a WM_CLOSE and
// may veto — e.g. a save-changes prompt the user cancels. Returns TRUE if all
// children closed (none remain), FALSE if any vetoed. A frame's WM_CLOSE
// handler can gate frame destruction on the result.
#define FMCM_CLOSEALL (WM_APP + 0x108)

// Close the contextually-active child — intended for a Ctrl+F4 handler.
// Picks the target by focus: a foreground floating child if one is focused,
// else the active docked child, else (no docked children) the frontmost
// float — which is focused first, then closed, with focus returned to the
// frame afterward. The child still goes through its WM_CLOSE veto path.
#define FMCM_CLOSEACTIVE (WM_APP + 0x109)
