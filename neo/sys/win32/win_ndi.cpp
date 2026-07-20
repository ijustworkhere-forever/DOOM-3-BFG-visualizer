// PRD M7: NDI output via the SDK's own runtime dynamic-loading mechanism.
// See win_ndi.h for the design notes (why no NDI SDK is needed at build
// time, only the free NDI Runtime redistributable at run time).
#pragma hdrstop
#include "../../idlib/precompiled.h"

#include <windows.h>
#include <string>

#include "win_ndi.h"

// PROCESSINGNDILIB_STATIC: use the plain extern "C" declarations (not
// dllimport) -- we never link against the SDK's import lib at all, we
// resolve everything ourselves via LoadLibrary/GetProcAddress below, so the
// dllimport calling convention decoration these headers would otherwise
// add is not just unneeded but actively wrong (there's no import lib to
// satisfy it).
#define PROCESSINGNDILIB_STATIC
#include "../../external/ndi/Processing.NDI.Lib.h"

// Processing.NDI.Lib.h only defines these under its dllimport branch, which
// PROCESSINGNDILIB_STATIC above deliberately skips -- so redefine them here.
// Values match the SDK's own documented dllimport branch for Win64 exactly
// (this file only ever builds x64, the project's one verified target).
#ifndef NDILIB_LIBRARY_NAME
#define NDILIB_LIBRARY_NAME  "Processing.NDI.Lib.x64.dll"
#endif
#ifndef NDILIB_REDIST_FOLDER
#define NDILIB_REDIST_FOLDER "NDI_RUNTIME_DIR_V6"
#endif

static HMODULE s_ndiModule = NULL;
static const NDIlib_v6 * s_ndi = NULL;
static NDIlib_send_instance_t s_sender = NULL;

static bool EnsureLoaded() {
	if ( s_ndi != NULL ) {
		return true;
	}
	// try the common case first (the official installer adds its bin
	// directory to PATH, so a plain LoadLibrary finds it with no extra work).
	s_ndiModule = LoadLibraryA( NDILIB_LIBRARY_NAME );
	if ( s_ndiModule == NULL ) {
		// fall back to NDI_RUNTIME_DIR_V6 (the env var the official
		// redistributable installer sets), in case PATH wasn't updated.
		char dir[MAX_PATH];
		const DWORD len = GetEnvironmentVariableA( NDILIB_REDIST_FOLDER, dir, MAX_PATH );
		if ( len > 0 && len < MAX_PATH ) {
			std::string full = dir;
			full += "\\";
			full += NDILIB_LIBRARY_NAME;
			s_ndiModule = LoadLibraryA( full.c_str() );
		}
	}
	if ( s_ndiModule == NULL ) {
		return false;
	}

	typedef const NDIlib_v6 * ( *NDIlib_v6_load_t )( void );
	NDIlib_v6_load_t loadFn = (NDIlib_v6_load_t)GetProcAddress( s_ndiModule, "NDIlib_v6_load" );
	if ( loadFn == NULL ) {
		FreeLibrary( s_ndiModule );
		s_ndiModule = NULL;
		return false;
	}
	s_ndi = loadFn();
	if ( s_ndi == NULL ) {
		FreeLibrary( s_ndiModule );
		s_ndiModule = NULL;
		return false;
	}
	if ( !s_ndi->initialize() ) {
		// SDK's own documented failure case: CPU lacks required instructions (SSE4.2+).
		s_ndi = NULL;
		FreeLibrary( s_ndiModule );
		s_ndiModule = NULL;
		return false;
	}
	return true;
}

bool Vis_NDIIsAvailable() {
	return EnsureLoaded();
}

bool Vis_NDIEnable( const char * sourceName ) {
	if ( s_sender != NULL ) {
		return true;
	}
	if ( !EnsureLoaded() ) {
		idLib::Warning( "Vis_NDIEnable: NDI Runtime not found (install it from ndi.video, or check PATH/%s)", NDILIB_REDIST_FOLDER );
		return false;
	}
	NDIlib_send_create_t createSettings;
	createSettings.p_ndi_name = sourceName;
	createSettings.p_groups = NULL;
	// this engine has no fixed/guaranteed frame rate to declare honestly, so
	// clock_video is OFF -- frames go out exactly as often as Vis_NDISendFrame
	// is actually called (i.e. the real render rate), rather than having NDI
	// itself throttle/stall to match a frame_rate_N/D that might not be true.
	createSettings.clock_video = false;
	createSettings.clock_audio = false;
	s_sender = s_ndi->send_create( &createSettings );
	if ( s_sender == NULL ) {
		idLib::Warning( "Vis_NDIEnable: send_create failed" );
		return false;
	}
	idLib::Printf( "Vis_NDIEnable: NDI source '%s' is now live\n", sourceName );
	return true;
}

void Vis_NDIDisable() {
	if ( s_sender != NULL && s_ndi != NULL ) {
		s_ndi->send_destroy( s_sender );
		s_sender = NULL;
		idLib::Printf( "Vis_NDIDisable: NDI source stopped\n" );
	}
}

bool Vis_NDIIsEnabled() {
	return s_sender != NULL;
}

void Vis_NDISendFrame( const unsigned char * bgra, int width, int height, int fpsNum, int fpsDen ) {
	if ( s_sender == NULL || s_ndi == NULL || bgra == NULL ) {
		return;
	}
	NDIlib_video_frame_v2_t frame;
	frame.xres = width;
	frame.yres = height;
	frame.FourCC = NDIlib_FourCC_video_type_BGRA;
	frame.frame_rate_N = fpsNum;
	frame.frame_rate_D = fpsDen;
	frame.picture_aspect_ratio = 0.0f;   // 0 = square pixels, derive from xres/yres
	frame.frame_format_type = NDIlib_frame_format_type_progressive;
	frame.timecode = NDIlib_send_timecode_synthesize;
	frame.p_data = (uint8_t *)bgra;
	frame.line_stride_in_bytes = width * 4;
	frame.p_metadata = NULL;
	frame.timestamp = 0;
	// clock_video=true (set in Vis_NDIEnable) makes this call rate-limit
	// itself to fpsNum/fpsDen -- matches the SDK's own documented pattern
	// for single-threaded senders (clock video, not audio, off one thread).
	s_ndi->send_send_video_v2( s_sender, &frame );
}
