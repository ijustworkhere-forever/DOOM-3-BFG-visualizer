#include "XA2_FFmpegDecoder.h"
#include <vector>

idFFmpegDecoder::idFFmpegDecoder() :
	m_formatCtx(NULL),
	m_codecCtx(NULL),
	m_stream(NULL),
	m_streamIndex(-1),
	m_frame(NULL),
	m_packet(NULL),
	m_swrCtx(NULL),
	m_av_sampleRate(0),
	m_av_channels(0),
	m_endOfStream(false) {
}

idFFmpegDecoder::~idFFmpegDecoder() {
	Close();
}

bool idFFmpegDecoder::Open( const char * filename ) {
	Close();

	idLib::Printf( "FFmpeg: opening '%s'\n", filename ? filename : "(null)" );

	int err = avformat_open_input(&m_formatCtx, filename, NULL, NULL);
	if (err < 0) {
		char msg[256]; av_strerror( err, msg, sizeof( msg ) );
		idLib::Warning( "FFmpeg: avformat_open_input failed (%d: %s) for '%s'", err, msg, filename );
		m_formatCtx = NULL;
		return false;
	}

	if ((err = avformat_find_stream_info(m_formatCtx, NULL)) < 0) {
		char msg[256]; av_strerror( err, msg, sizeof( msg ) );
		idLib::Warning( "FFmpeg: find_stream_info failed (%s)", msg );
		avformat_close_input(&m_formatCtx);
		return false;
	}

	m_streamIndex = av_find_best_stream(m_formatCtx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
	if (m_streamIndex < 0) {
		idLib::Warning( "FFmpeg: no audio stream found" );
		avformat_close_input(&m_formatCtx);
		return false;
	}

	m_stream = m_formatCtx->streams[m_streamIndex];
	const AVCodec * codec = avcodec_find_decoder(m_stream->codecpar->codec_id);
	if (!codec) {
		idLib::Warning( "FFmpeg: no decoder for codec_id %d", (int)m_stream->codecpar->codec_id );
		avformat_close_input(&m_formatCtx);
		return false;
	}

	m_codecCtx = avcodec_alloc_context3(codec);
	if (!m_codecCtx) {
		avformat_close_input(&m_formatCtx);
		return false;
	}

	if (avcodec_parameters_to_context(m_codecCtx, m_stream->codecpar) < 0 ||
		(err = avcodec_open2(m_codecCtx, codec, NULL)) < 0) {
		char msg[256]; av_strerror( err, msg, sizeof( msg ) );
		idLib::Warning( "FFmpeg: avcodec_open2 failed (%s)", msg );
		Close();
		return false;
	}
	idLib::Printf( "FFmpeg: decoding %s, %d Hz, %d ch\n", codec->name, m_codecCtx->sample_rate, m_codecCtx->ch_layout.nb_channels );

	m_frame = av_frame_alloc();
	m_packet = av_packet_alloc();
	if (!m_frame || !m_packet) {
		Close();
		return false;
	}

	m_av_channels = m_codecCtx->ch_layout.nb_channels;
	m_av_sampleRate = m_codecCtx->sample_rate;

	m_channels = m_av_channels;
	m_sampleRate = m_av_sampleRate;
	m_samplesPerBlock = 1024;
	m_streaming = true;
	m_endOfStream = false;

	// convert whatever the codec outputs to packed float32 at the source rate
	AVChannelLayout outLayout;
	av_channel_layout_default(&outLayout, m_av_channels);

	int swrErr = swr_alloc_set_opts2(&m_swrCtx,
		&outLayout, AV_SAMPLE_FMT_FLT, m_av_sampleRate,
		&m_codecCtx->ch_layout, m_codecCtx->sample_fmt, m_codecCtx->sample_rate,
		0, NULL);
	av_channel_layout_uninit(&outLayout);

	if (swrErr < 0 || !m_swrCtx || swr_init(m_swrCtx) < 0) {
		Close();
		return false;
	}

	m_decodedBuffer.reserve((size_t)m_samplesPerBlock * m_av_channels * 4);
	return true;
}

void idFFmpegDecoder::Close() {
	if (m_swrCtx) {
		swr_free(&m_swrCtx);
	}
	if (m_frame) {
		av_frame_free(&m_frame);
	}
	if (m_packet) {
		av_packet_free(&m_packet);
	}
	if (m_codecCtx) {
		avcodec_free_context(&m_codecCtx);
	}
	if (m_formatCtx) {
		avformat_close_input(&m_formatCtx);
	}
	m_stream = NULL;
	m_streamIndex = -1;
	m_channels = 0;
	m_sampleRate = 0;
	m_samplesPerBlock = 0;
	m_streaming = false;
	m_endOfStream = false;
	m_decodedBuffer.clear();
}

// pulls all frames currently available from the codec into m_decodedBuffer
void idFFmpegDecoder::DrainCodec() {
	while (avcodec_receive_frame(m_codecCtx, m_frame) == 0) {
		const int maxOut = swr_get_out_samples(m_swrCtx, m_frame->nb_samples);
		if (maxOut <= 0) {
			av_frame_unref(m_frame);
			continue;
		}
		std::vector<float> tmp((size_t)maxOut * m_av_channels);
		uint8_t * outPlanes[1] = { reinterpret_cast<uint8_t *>(tmp.data()) };

		const int converted = swr_convert(m_swrCtx, outPlanes, maxOut,
			(const uint8_t **)m_frame->extended_data, m_frame->nb_samples);
		if (converted > 0) {
			m_decodedBuffer.insert(m_decodedBuffer.end(),
				tmp.begin(), tmp.begin() + (size_t)converted * m_av_channels);
		}
		av_frame_unref(m_frame);
	}
}

int idFFmpegDecoder::DecodeNext( float * buffer, int numSamples ) {
	if (!m_formatCtx || !m_codecCtx || m_av_channels <= 0) {
		return 0;
	}

	const size_t targetFloats = (size_t)numSamples * m_av_channels;

	while (m_decodedBuffer.size() < targetFloats && !m_endOfStream) {
		const int readErr = av_read_frame(m_formatCtx, m_packet);
		if (readErr < 0) {
			// end of file: flush the codec, then drain remaining frames
			avcodec_send_packet(m_codecCtx, NULL);
			DrainCodec();
			m_endOfStream = true;
			break;
		}

		if (m_packet->stream_index == m_streamIndex) {
			if (avcodec_send_packet(m_codecCtx, m_packet) == 0) {
				DrainCodec();
			}
		}
		av_packet_unref(m_packet);
	}

	const size_t copyFloats = ( m_decodedBuffer.size() < targetFloats ) ? m_decodedBuffer.size() : targetFloats;
	if (copyFloats > 0) {
		memcpy(buffer, m_decodedBuffer.data(), copyFloats * sizeof(float));
		m_decodedBuffer.erase(m_decodedBuffer.begin(), m_decodedBuffer.begin() + copyFloats);
	}

	return (int)(copyFloats / m_av_channels);
}
