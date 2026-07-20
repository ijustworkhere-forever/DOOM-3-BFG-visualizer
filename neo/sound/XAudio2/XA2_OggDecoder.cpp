#include "../../idlib/precompiled.h"
#pragma hdrstop
#include "XA2_OggDecoder.h"
// the stb_vorbis implementation lives in neo/external/stb_vorbis_impl.cpp

idOggDecoder::idOggDecoder() :
    m_vorbis( NULL ),
    m_fileData( NULL ),
    m_fileLength( 0 ) {
}

idOggDecoder::~idOggDecoder() {
    Close();
}

bool idOggDecoder::Open( const char * filename ) {
    Close();

    m_fileLength = fileSystem->ReadFile( filename, &m_fileData, NULL );
    if ( m_fileLength <= 0 || m_fileData == NULL ) {
        m_fileData = NULL;
        m_fileLength = 0;
        return false;
    }

    int error = VORBIS__no_error;
    m_vorbis = stb_vorbis_open_memory( (const unsigned char *)m_fileData, m_fileLength, &error, NULL );
    if ( m_vorbis == NULL ) {
        idLib::Warning( "idOggDecoder: stb_vorbis failed on '%s' (error %d)", filename, error );
        fileSystem->FreeFile( m_fileData );
        m_fileData = NULL;
        m_fileLength = 0;
        return false;
    }

    stb_vorbis_info info = stb_vorbis_get_info( m_vorbis );
    m_channels = info.channels;
    m_sampleRate = (int)info.sample_rate;
    m_samplesPerBlock = 1024;
    m_streaming = true;

    return true;
}

void idOggDecoder::Close() {
    if ( m_vorbis ) {
        stb_vorbis_close( m_vorbis );
        m_vorbis = NULL;
    }
    if ( m_fileData ) {
        fileSystem->FreeFile( m_fileData );
        m_fileData = NULL;
    }
    m_fileLength = 0;
    m_channels = 0;
    m_sampleRate = 0;
    m_samplesPerBlock = 0;
    m_streaming = false;
}

int idOggDecoder::DecodeNext( float * buffer, int numSamples ) {
    if ( !m_vorbis || m_channels <= 0 ) {
        return 0;
    }
    // returns frames per channel actually decoded
    return stb_vorbis_get_samples_float_interleaved( m_vorbis, m_channels, buffer, numSamples * m_channels );
}
