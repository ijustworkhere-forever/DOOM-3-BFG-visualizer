// PRD M7: "wallpaper mode" via the Progman/WorkerW technique.
// See win_wallpaper.h for the design notes (why this header stays
// windows.h-free, and why Enable() is treated as best-effort).
#pragma hdrstop
#include "../../idlib/precompiled.h"
#include "win_local.h"
#include "win_wallpaper.h"

static bool s_wallpaperEnabled = false;
static HWND s_prevParent = NULL;   // restore point for Disable()
static LONG s_prevStyle = 0;       // Fixed real bug: restoring a hardcoded bit
                                   // set (below) instead of the window's actual
                                   // prior style granted Minimize/Maximize
                                   // buttons the window never had before --
                                   // snapshot and restore the exact value instead.
static LONG s_prevExStyle = 0;     // same reasoning for GWL_EXSTYLE (WS_EX_TOPMOST
                                   // in fake-fullscreen mode -- see win_glimp.cpp).

// Callback for EnumWindows: looks for a top-level window that hosts a
// SHELLDLL_DefView child (the desktop icon view) and, when found, grabs
// that window's WorkerW sibling -- the one actually used as the wallpaper
// backdrop on modern Windows (the WorkerW spawned in response to the
// 0x052C message below sits BEHIND that one).
static BOOL CALLBACK FindWorkerWProc( HWND hwnd, LPARAM lparam ) {
	HWND shellView = FindWindowExW( hwnd, NULL, L"SHELLDLL_DefView", NULL );
	if ( shellView != NULL ) {
		// Fixed real bug: this used to write through unconditionally, so a
		// later top-level window that hosts SHELLDLL_DefView but has no
		// WorkerW sibling (FindWindowExW returning NULL) could clobber an
		// already-found valid handle from an earlier one -- "last matching
		// candidate wins" instead of "first valid one wins", on a multi-
		// monitor desktop where more than one such window can exist. Only
		// write (and only then stop enumerating) once a real match is found.
		const HWND found = FindWindowExW( NULL, hwnd, L"WorkerW", NULL );
		if ( found != NULL ) {
			HWND * out = (HWND *)lparam;
			*out = found;
			return FALSE;   // stop enumerating -- first valid match wins
		}
	}
	return TRUE;   // keep enumerating
}

static HWND FindWorkerW() {
	HWND progman = FindWindowW( L"Progman", NULL );
	if ( progman == NULL ) {
		return NULL;
	}
	// Undocumented message that makes explorer.exe spawn (or re-use) a
	// WorkerW window behind Progman -- this is the well-known technique
	// every open-source "desktop wallpaper" tool relies on; there is no
	// documented/supported API for this on Windows.
	SendMessageTimeoutW( progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, NULL );

	HWND workerw = NULL;
	EnumWindows( FindWorkerWProc, (LPARAM)&workerw );
	return workerw;
}

bool Vis_WallpaperEnable() {
	if ( s_wallpaperEnabled ) {
		return true;
	}
	if ( win32.hWnd == NULL ) {
		idLib::Warning( "Vis_WallpaperEnable: no window yet" );
		return false;
	}
	HWND workerw = FindWorkerW();
	if ( workerw == NULL ) {
		idLib::Warning( "Vis_WallpaperEnable: couldn't find/create a WorkerW window -- "
						 "explorer.exe's internals may differ on this Windows version" );
		return false;
	}

	s_prevParent = GetParent( win32.hWnd );
	s_prevStyle = GetWindowLong( win32.hWnd, GWL_STYLE );
	s_prevExStyle = GetWindowLong( win32.hWnd, GWL_EXSTYLE );

	// borderless, so it doesn't look like a floating window sitting on the desktop
	LONG style = s_prevStyle;
	style &= ~( WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX );
	SetWindowLong( win32.hWnd, GWL_STYLE, style );
	// Fixed real bug: WS_EX_TOPMOST (set in fake-fullscreen mode, see
	// win_glimp.cpp) was never touched here or in Disable() below -- left on
	// a reparented child window it's outside documented Win32 behavior and
	// could leave the "wallpaper" rendering on top of everything, defeating
	// the point. Strip it going in; s_prevExStyle restores it on the way out.
	SetWindowLong( win32.hWnd, GWL_EXSTYLE, s_prevExStyle & ~WS_EX_TOPMOST );

	SetParent( win32.hWnd, workerw );
	SetWindowPos( win32.hWnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED );

	s_wallpaperEnabled = true;
	idLib::Printf( "Vis_WallpaperEnable: window reparented behind desktop icons\n" );
	return true;
}

void Vis_WallpaperDisable() {
	if ( !s_wallpaperEnabled ) {
		return;
	}
	if ( win32.hWnd != NULL ) {
		SetParent( win32.hWnd, s_prevParent );
		// Fixed real bug: this used to OR back a hardcoded bit set
		// (WS_CAPTION|WS_THICKFRAME|WS_SYSMENU|WS_MINIMIZEBOX|WS_MAXIMIZEBOX)
		// instead of restoring the window's actual prior style -- the
		// engine's real base style (WINDOW_STYLE, win_local.h) never
		// included WS_MINIMIZEBOX/WS_MAXIMIZEBOX, so this granted a working
		// Minimize/Maximize button (and matching system-menu entries) the
		// window never had before wallpaper mode was ever engaged. Restore
		// the exact snapshotted value instead.
		SetWindowLong( win32.hWnd, GWL_STYLE, s_prevStyle );
		SetWindowLong( win32.hWnd, GWL_EXSTYLE, s_prevExStyle );
		const HWND zOrder = ( s_prevExStyle & WS_EX_TOPMOST ) ? HWND_TOPMOST : HWND_NOTOPMOST;
		SetWindowPos( win32.hWnd, zOrder, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED );
	}
	s_wallpaperEnabled = false;
	idLib::Printf( "Vis_WallpaperDisable: window restored to normal desktop parenting\n" );
}

bool Vis_WallpaperIsEnabled() {
	return s_wallpaperEnabled;
}
