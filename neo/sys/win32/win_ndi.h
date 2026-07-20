#ifndef __WIN_NDI_H__
#define __WIN_NDI_H__

/*
================================================
PRD M7: NDI (network video) output. Windows.h-free header, same discipline
as win_midi.h/win_wallpaper.h/win_imgui.h.

Real NDI output requires NewTek/Vizrt's proprietary SDK -- confirmed no
open-source/SDK-free alternative exists (florisporro/awesome-ndi's curated
list only lists proprietary-SDK-dependent tools). BUT the SDK's own
redistributable headers (vendored under neo/external/ndi/, each carrying an
"MIT license applies to this file only" notice matching the SDK's own
license terms -- confirmed by reading Processing.NDI.Lib.h directly, and
the same headers DistroAV, the open-source OBS-NDI plugin, ships) include
Processing.NDI.DynamicLoad.h: NDI's OFFICIAL runtime dynamic-loading
mechanism. This means NDI output needs ZERO SDK presence at BUILD time --
this file LoadLibrary()s the free NDI Runtime redistributable (not the
paid/registered full SDK) at RUNTIME, and gracefully reports unavailable if
it isn't installed, the same IsAvailable()-before-using-a-feature pattern
idVisVideoEncoder already established for the FFmpeg encoder.
================================================
*/
bool Vis_NDIIsAvailable();     // real runtime check: is the NDI Runtime DLL loadable right now?
bool Vis_NDIEnable( const char * sourceName );   // creates an NDI sender named sourceName
void Vis_NDIDisable();
bool Vis_NDIIsEnabled();

// bgra must be width*height*4 bytes, BGRA8 (matches a direct GL_BGRA
// glReadPixels, see idRenderSystem::CaptureRenderToMemory), top-down row
// order. No-op if not enabled. unsigned char, not idlib's `byte`, so this
// header stays free of idlib/precompiled.h too, same discipline as
// win_midi.h.
void Vis_NDISendFrame( const unsigned char * bgra, int width, int height, int fpsNum, int fpsDen );

#endif // __WIN_NDI_H__
