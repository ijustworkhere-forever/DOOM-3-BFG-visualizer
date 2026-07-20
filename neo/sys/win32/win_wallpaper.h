#ifndef __WIN_WALLPAPER_H__
#define __WIN_WALLPAPER_H__

/*
================================================
PRD M7: "wallpaper mode" -- reparents this engine's main window behind the
desktop icons using the standard (undocumented but widely relied-on, e.g. by
Wallpaper Engine/Lively Wallpaper) Progman/WorkerW technique: send Progman
the undocumented 0x052C message to make explorer.exe spawn a WorkerW window,
find the one that's a sibling of the WorkerW hosting SHELLDLL_DefView, then
SetParent() this engine's window into it.

Windows-only, and the exact mechanism relies on unofficial explorer.exe
internals, so treated as a genuine best-effort feature: Enable() returns
false (with a warning logged) rather than crashing if the expected window
hierarchy isn't found -- e.g. on a Windows version/build where explorer.exe's
internal structure has changed.

This header stays windows.h-free, same discipline as win_midi.h, so
cross-platform-intended code (visualizer_manager.cpp) can call it directly.
================================================
*/
bool Vis_WallpaperEnable();    // false (logs why) if the WorkerW window couldn't be found/created
void Vis_WallpaperDisable();   // restores the window to its normal top-level parent/style; safe if never enabled
bool Vis_WallpaperIsEnabled();

#endif // __WIN_WALLPAPER_H__
