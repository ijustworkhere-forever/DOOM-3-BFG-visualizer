#ifndef __XA2_OGG_DECODER_H__
#define __XA2_OGG_DECODER_H__

#include "XA2_Decoder.h"

#define STB_VORBIS_HEADER_ONLY
#include "../../external/stb_vorbis.h"
#undef STB_VORBIS_HEADER_ONLY

/*
================================================
idOggDecoder

stb_vorbis-backed OGG decoder. The whole file is read through idFileSystem into
memory (so presets/music inside resource containers work), then decoded
incrementally from that buffer.
================================================
*/
class idOggDecoder : public idDecoder {
public:
    idOggDecoder();
    ~idOggDecoder();

    bool Open( const char * filename ) override;
    void Close() override;

    // Decodes up to numSamples frames of interleaved float PCM into buffer
    // (buffer must hold numSamples * GetChannels() floats).
    // Returns the number of frames decoded (0 at end of stream).
    int DecodeNext( float * buffer, int numSamples ) override;

private:
    stb_vorbis *    m_vorbis;
    void *          m_fileData;     // owned copy of the .ogg file
    int             m_fileLength;
};

#endif // __XA2_OGG_DECODER_H__
