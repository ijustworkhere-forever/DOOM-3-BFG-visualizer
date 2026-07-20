@echo off
rem Double-click launcher. Sets CWD to this script's own folder (C:\DOOM-3-BFG)
rem so fs_basepath (which defaults to the current working directory, NOT the
rem exe's folder -- see neo/sys/win32/win_main.cpp Sys_DefaultBasePath) finds
rem base/ correctly even when build\x64\Debug\Doom3BFG.exe is run from
rem somewhere else (e.g. double-clicked directly from Explorer).
cd /d "%~dp0"
"%~dp0build\x64\Debug\Doom3BFG.exe" +set com_allowConsole 1 +set com_skipIntroVideos 1 +set win_viewlog 1
