#ifndef __XA2_DECODER_H__
#define __XA2_DECODER_H__

#include "../../idlib/precompiled.h"

class idDecoder {
public:
	idDecoder() : m_channels(0), m_sampleRate(0), m_samplesPerBlock(0), m_streaming(false) {}
	virtual ~idDecoder() {}

	virtual bool Open( const char * filename ) = 0;
	virtual void Close() = 0;
	virtual int DecodeNext( float * buffer, int numSamples ) = 0;

	int GetChannels() const { return m_channels; }
	int GetSampleRate() const { return m_sampleRate; }
	int GetSamplesPerBlock() const { return m_samplesPerBlock; }
	bool IsStreaming() const { return m_streaming; }

protected:
	int m_channels;
	int m_sampleRate;
	int m_samplesPerBlock;
	bool m_streaming;
};

#endif // __XA2_DECODER_H__
