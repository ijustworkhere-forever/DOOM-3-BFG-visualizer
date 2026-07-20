#include "../idlib/precompiled.h"
#pragma hdrstop
#include "VisVideoEncoder.h"

#if ID_HAVE_FFMPEG_ENCODE

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

// Opaque impl, defined only here -- keeps every FFmpeg type out of the
// header so anything that just wants to call Open/EncodeFrame/Close (e.g.
// visualizer_manager.cpp) never needs to see libav*/libsw* at all, encode-
// capable build or not.
struct idVisVideoEncoder::Impl {
	AVFormatContext *	fmtCtx;
	AVCodecContext *	codecCtx;
	AVStream *			stream;
	SwsContext *		swsCtx;
	AVFrame *			frame;
	AVPacket *			pkt;
	int64_t				frameIndex;
	int					srcWidth, srcHeight;    // dimensions of the RGBA buffers EncodeFrame receives
	int					dstWidth, dstHeight;    // dimensions actually encoded (destination preset scaling)

	Impl() : fmtCtx( NULL ), codecCtx( NULL ), stream( NULL ), swsCtx( NULL ),
			 frame( NULL ), pkt( NULL ), frameIndex( 0 ),
			 srcWidth( 0 ), srcHeight( 0 ), dstWidth( 0 ), dstHeight( 0 ) {}
};

idVisVideoEncoder::idVisVideoEncoder() : m_impl( new Impl() ) {
}

idVisVideoEncoder::~idVisVideoEncoder() {
	Close();
	delete m_impl;
}

/*
================
idVisVideoEncoder::IsAvailable

Real runtime check -- FFmpeg dev packages enumerate every codec ID in their
headers regardless of which ones a specific vendored avcodec.lib was built
with, so compiling against the headers proves nothing about what's actually
linkable. This is the only reliable signal.
================
*/
bool idVisVideoEncoder::IsAvailable() {
	return avcodec_find_encoder( AV_CODEC_ID_H264 ) != NULL || avcodec_find_encoder( AV_CODEC_ID_MPEG4 ) != NULL;
}

// Shared by EncodeFrame (drain after each real frame) and Close (final
// flush, signaled by passing flush=true so avcodec_send_frame gets NULL --
// the encoder may still be holding buffered frames, e.g. B-frames, that
// only come out once it knows no more input is coming).
static void DrainPackets( idVisVideoEncoder::Impl * im, bool flush ) {
	int ret = avcodec_send_frame( im->codecCtx, flush ? NULL : im->frame );
	if ( ret < 0 ) {
		return;
	}
	while ( ret >= 0 ) {
		ret = avcodec_receive_packet( im->codecCtx, im->pkt );
		if ( ret < 0 ) {
			break;   // EAGAIN (needs more input) or EOF (fully flushed) -- both normal here
		}
		av_packet_rescale_ts( im->pkt, im->codecCtx->time_base, im->stream->time_base );
		im->pkt->stream_index = im->stream->index;
		av_interleaved_write_frame( im->fmtCtx, im->pkt );
		av_packet_unref( im->pkt );
	}
}

/*
================
idVisVideoEncoder::Open
================
*/
bool idVisVideoEncoder::Open( const char * outFile, int srcWidth, int srcHeight, int fps,
							   int dstWidth, int dstHeight, int64_t bitRate ) {
	Close();   // insurance against a stale/partial previous Open

	Impl * im = m_impl;
	im->srcWidth  = srcWidth;
	im->srcHeight = srcHeight;
	im->dstWidth  = ( dstWidth  > 0 ) ? dstWidth  : srcWidth;
	im->dstHeight = ( dstHeight > 0 ) ? dstHeight : srcHeight;
	const int useFps = ( fps > 0 ) ? fps : 30;

	avformat_alloc_output_context2( &im->fmtCtx, NULL, NULL, outFile );
	if ( im->fmtCtx == NULL ) {
		idLib::Warning( "idVisVideoEncoder::Open: couldn't guess a container format from '%s' (try a .mp4 extension)", outFile );
		return false;
	}

	// H264 preferred (broadly compatible); MPEG4 as a fallback for FFmpeg
	// builds without an H264 encoder (e.g. no libx264). If neither exists,
	// this vendored FFmpeg build simply has no usable video encoder --
	// documented as a real, un-verifiable-from-this-Mac possibility in the
	// header comment.
	const AVCodec * codec = avcodec_find_encoder( AV_CODEC_ID_H264 );
	if ( codec == NULL ) {
		codec = avcodec_find_encoder( AV_CODEC_ID_MPEG4 );
	}
	if ( codec == NULL ) {
		idLib::Warning( "idVisVideoEncoder::Open: no usable video encoder (H264/MPEG4) in this FFmpeg build -- use vis_recordStart's image sequence instead" );
		Close();
		return false;
	}

	im->stream = avformat_new_stream( im->fmtCtx, NULL );
	im->codecCtx = avcodec_alloc_context3( codec );
	if ( im->stream == NULL || im->codecCtx == NULL ) {
		idLib::Warning( "idVisVideoEncoder::Open: couldn't allocate stream/codec context" );
		Close();
		return false;
	}

	im->codecCtx->width = im->dstWidth;
	im->codecCtx->height = im->dstHeight;
	im->codecCtx->time_base = AVRational{ 1, useFps };
	im->codecCtx->framerate = AVRational{ useFps, 1 };
	im->codecCtx->gop_size = 12;
	im->codecCtx->pix_fmt = AV_PIX_FMT_YUV420P;
	if ( bitRate > 0 ) {
		// PRD FR-E3 destination presets (YouTube HD/4K, Instagram/TikTok):
		// a target bitrate: takes priority over the "preset"/"crf" quality
		// knobs below when the caller wants a specific bitrate, matching how
		// real export presets are usually specified (a target bitrate, not a
		// quality factor).
		im->codecCtx->bit_rate = bitRate;
	} else if ( codec->id == AV_CODEC_ID_H264 ) {
		// Best-effort only: a no-op if this isn't actually libx264 under the
		// hood (some other H264 impl may not expose "preset"/"crf" private
		// options) -- not treated as fatal either way.
		av_opt_set( im->codecCtx->priv_data, "preset", "fast", 0 );
		av_opt_set( im->codecCtx->priv_data, "crf", "23", 0 );
	}
	if ( im->fmtCtx->oformat->flags & AVFMT_GLOBALHEADER ) {
		im->codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	}

	if ( avcodec_open2( im->codecCtx, codec, NULL ) < 0 ) {
		idLib::Warning( "idVisVideoEncoder::Open: avcodec_open2 failed" );
		Close();
		return false;
	}
	avcodec_parameters_from_context( im->stream->codecpar, im->codecCtx );
	im->stream->time_base = im->codecCtx->time_base;

	if ( !( im->fmtCtx->oformat->flags & AVFMT_NOFILE ) ) {
		if ( avio_open( &im->fmtCtx->pb, outFile, AVIO_FLAG_WRITE ) < 0 ) {
			idLib::Warning( "idVisVideoEncoder::Open: couldn't open '%s' for writing", outFile );
			Close();
			return false;
		}
	}
	if ( avformat_write_header( im->fmtCtx, NULL ) < 0 ) {
		idLib::Warning( "idVisVideoEncoder::Open: avformat_write_header failed" );
		Close();
		return false;
	}

	im->swsCtx = sws_getContext( im->srcWidth, im->srcHeight, AV_PIX_FMT_RGBA,
								  im->dstWidth, im->dstHeight, AV_PIX_FMT_YUV420P,
								  SWS_BILINEAR, NULL, NULL, NULL );
	if ( im->swsCtx == NULL ) {
		idLib::Warning( "idVisVideoEncoder::Open: sws_getContext failed" );
		Close();
		return false;
	}
	im->frame = av_frame_alloc();
	im->frame->format = AV_PIX_FMT_YUV420P;
	im->frame->width = im->dstWidth;
	im->frame->height = im->dstHeight;
	if ( av_frame_get_buffer( im->frame, 0 ) < 0 ) {
		idLib::Warning( "idVisVideoEncoder::Open: av_frame_get_buffer failed" );
		Close();
		return false;
	}
	im->pkt = av_packet_alloc();
	im->frameIndex = 0;
	return true;
}

/*
================
idVisVideoEncoder::EncodeFrame
================
*/
bool idVisVideoEncoder::EncodeFrame( const byte * rgba ) {
	Impl * im = m_impl;
	if ( im->swsCtx == NULL || im->frame == NULL ) {
		return false;
	}
	const uint8_t * srcSlices[1] = { rgba };
	int srcStride[1] = { im->srcWidth * 4 };
	sws_scale( im->swsCtx, srcSlices, srcStride, 0, im->srcHeight, im->frame->data, im->frame->linesize );
	im->frame->pts = im->frameIndex++;
	DrainPackets( im, false );
	return true;
}

/*
================
idVisVideoEncoder::Close
================
*/
void idVisVideoEncoder::Close() {
	Impl * im = m_impl;
	if ( im->codecCtx != NULL && im->fmtCtx != NULL && im->fmtCtx->pb != NULL ) {
		DrainPackets( im, true );   // flush any buffered frames before the trailer
		av_write_trailer( im->fmtCtx );
	}
	if ( im->fmtCtx != NULL && im->fmtCtx->oformat != NULL &&
		 !( im->fmtCtx->oformat->flags & AVFMT_NOFILE ) && im->fmtCtx->pb != NULL ) {
		avio_closep( &im->fmtCtx->pb );
	}
	if ( im->swsCtx != NULL )   { sws_freeContext( im->swsCtx ); im->swsCtx = NULL; }
	if ( im->frame != NULL )    { av_frame_free( &im->frame ); }
	if ( im->pkt != NULL )      { av_packet_free( &im->pkt ); }
	if ( im->codecCtx != NULL ) { avcodec_free_context( &im->codecCtx ); }
	if ( im->fmtCtx != NULL )   { avformat_free_context( im->fmtCtx ); im->fmtCtx = NULL; }
	im->stream = NULL;
	im->frameIndex = 0;
}

#endif // ID_HAVE_FFMPEG_ENCODE
