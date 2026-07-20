#ifndef __XA2_FFMPEG_DECODER_H__
#define __XA2_FFMPEG_DECODER_H__

#include "../../idlib/precompiled.h"
#include "XA2_Decoder.h"
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

class idFFmpegDecoder : public idDecoder {
public:
	idFFmpegDecoder();
	~idFFmpegDecoder();

	bool Open( const char * filename ) override;
	void Close() override;

	// Decodes up to numSamples frames of interleaved float PCM into buffer
	// (buffer must hold numSamples * GetChannels() floats).
	// Returns the number of frames decoded (0 at end of stream).
	int DecodeNext( float * buffer, int numSamples ) override;

private:
	void DrainCodec();

	AVFormatContext * m_formatCtx;
	AVCodecContext * m_codecCtx;
	AVStream * m_stream;
	int m_streamIndex;
	AVFrame * m_frame;
	AVPacket * m_packet;
	SwrContext * m_swrCtx;

	int m_av_sampleRate;
	int m_av_channels;
	bool m_endOfStream;

	// interleaved float samples decoded but not yet consumed
	std::vector<float> m_decodedBuffer;
};

#endif // __XA2_FFMPEG_DECODER_H__
