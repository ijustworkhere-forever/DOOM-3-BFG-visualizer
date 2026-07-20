#ifndef __VIS_VIDEO_ENCODER_H__
#define __VIS_VIDEO_ENCODER_H__

#include "../idlib/precompiled.h"

/*
================================================
PRD FR-E3/M6: real video-file export -- mux the already-captured TGA image
sequence (vis_recordStart/vis_recordStop, PRD FR-E4) into an actual .mp4,
using the FFmpeg libs already linked for audio decode (see
XAudio2/XA2_FFmpegDecoder). That existing usage is 100% decode-only; this is
the first ENCODE-side usage in this codebase.

Feature-detected via __has_include, mirroring XA2_SoundSample.cpp's exact
pattern for the decoder, since this Mac dev environment has neither the
FFmpeg headers nor any way to confirm what the eventual Windows build's
vendored FFmpeg package actually supports. IMPORTANT: ID_HAVE_FFMPEG_ENCODE
only means "the swscale header exists alongside avcodec" -- FFmpeg dev
packages ship headers for every codec ID regardless of which ones a given
avcodec.lib was actually BUILT with. IsAvailable() below does the real
runtime check (avcodec_find_encoder returning non-NULL); callers must check
it (or a failed Open()) and fall back to the image-sequence recording
instead of assuming this feature works just because it compiled.
================================================
*/
#if defined(__has_include)
#if __has_include(<libavcodec/avcodec.h>) && __has_include(<libavformat/avformat.h>) && __has_include(<libswscale/swscale.h>)
#define ID_HAVE_FFMPEG_ENCODE 1
#endif
#endif
#ifndef ID_HAVE_FFMPEG_ENCODE
#define ID_HAVE_FFMPEG_ENCODE 0
#endif

#if ID_HAVE_FFMPEG_ENCODE

class idVisVideoEncoder {
public:
	idVisVideoEncoder();
	~idVisVideoEncoder();

	// Real runtime check: is there an actually-usable video encoder (H264 or
	// MPEG4) in whatever FFmpeg build this binary linked against? Headers
	// being present (which is all ID_HAVE_FFMPEG_ENCODE confirms) does NOT
	// guarantee this.
	static bool		IsAvailable();

	// outFile's extension picks the container (use .mp4). srcWidth/srcHeight
	// must match the RGBA frames passed to EncodeFrame. dstWidth/dstHeight
	// (0 = same as source) let a destination preset (PRD FR-E3: YouTube
	// HD/4K, Instagram/TikTok) scale the encoded output to a different
	// resolution than the captured frames, via swscale -- both must be even
	// (most video codecs, including H264, require it; callers round before
	// calling). bitRate (0 = encoder default) sets a target bitrate for
	// preset-driven quality profiles. Returns false (logging why) on any
	// failure -- no usable encoder, bad container guess, I/O error, etc. --
	// leaving the encoder unusable but safely destructible.
	bool			Open( const char * outFile, int srcWidth, int srcHeight, int fps,
						  int dstWidth = 0, int dstHeight = 0, int64_t bitRate = 0 );

	// rgba must be width*height*4 bytes, RGBA8, top-down row order (matches
	// what vis_videoEncode's TGA reader produces). Returns false if Open()
	// didn't succeed.
	bool			EncodeFrame( const byte * rgba );

	// Flushes any buffered frames, writes the container trailer, and frees
	// everything. Safe to call multiple times / without a successful Open().
	void			Close();

	struct Impl;

private:
	Impl *			m_impl;
};

#else // !ID_HAVE_FFMPEG_ENCODE

class idVisVideoEncoder {
public:
	static bool		IsAvailable() { return false; }
	bool			Open( const char *, int, int, int, int = 0, int = 0, int64_t = 0 ) { return false; }
	bool			EncodeFrame( const byte * ) { return false; }
	void			Close() {}
};

#endif // ID_HAVE_FFMPEG_ENCODE

#endif // __VIS_VIDEO_ENCODER_H__
