#include "AudioCapture_WASAPI.h"
#include "audio_analyzer.h"
#include <mmreg.h>
#include <vector>
#include <chrono>

#pragma comment( lib, "Ole32.lib" )

// PKEY_Device_FriendlyName = {a45c254e-df1c-4efd-8020-67d146a850e0}, 14.
// Defined inline to avoid pulling <functiondiscoverykeys_devpkey.h> + <initguid.h>
// (which risk duplicate-GUID link errors from this isolated, non-PCH TU).
static const PROPERTYKEY kPKEY_Device_FriendlyName =
    { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };

static std::string WideToUtf8( const wchar_t * w ) {
    if ( w == nullptr ) {
        return std::string();
    }
    const int len = WideCharToMultiByte( CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr );
    if ( len <= 1 ) {
        return std::string();
    }
    std::string s( (size_t)( len - 1 ), '\0' );
    WideCharToMultiByte( CP_UTF8, 0, w, -1, &s[0], len, nullptr, nullptr );
    return s;
}

static std::wstring Utf8ToWide( const std::string & s ) {
    if ( s.empty() ) {
        return std::wstring();
    }
    const int len = MultiByteToWideChar( CP_UTF8, 0, s.c_str(), -1, nullptr, 0 );
    if ( len <= 1 ) {
        return std::wstring();
    }
    std::wstring w( (size_t)( len - 1 ), L'\0' );
    MultiByteToWideChar( CP_UTF8, 0, s.c_str(), -1, &w[0], len );
    return w;
}

// 200 ms shared-mode buffer, in 100-nanosecond units
static const REFERENCE_TIME CAPTURE_BUFFER_DURATION = 200 * 10000;

AudioCapture_WASAPI::AudioCapture_WASAPI( AudioAnalyzer * sink ) :
    m_sink( sink ) {
}

AudioCapture_WASAPI::~AudioCapture_WASAPI() {
    Stop();
    ReleaseAll();
}

void AudioCapture_WASAPI::ReleaseAll() {
    if ( m_pFormat )        { CoTaskMemFree( m_pFormat ); m_pFormat = nullptr; }
    if ( m_pCaptureClient ) { m_pCaptureClient->Release(); m_pCaptureClient = nullptr; }
    if ( m_pAudioClient )   { m_pAudioClient->Release(); m_pAudioClient = nullptr; }
    if ( m_pDevice )        { m_pDevice->Release(); m_pDevice = nullptr; }
    if ( m_pEnumerator )    { m_pEnumerator->Release(); m_pEnumerator = nullptr; }
    if ( m_comInitialized ) { CoUninitialize(); m_comInitialized = false; }
}

bool AudioCapture_WASAPI::Initialize( const std::string & deviceId, bool isCapture ) {
    HRESULT hr = CoInitializeEx( NULL, COINIT_MULTITHREADED );
    // RPC_E_CHANGED_MODE means COM is already initialized on this thread in a
    // different mode - that's fine, just don't pair a CoUninitialize with it.
    if ( SUCCEEDED( hr ) ) {
        m_comInitialized = true;
    } else if ( hr != RPC_E_CHANGED_MODE ) {
        return false;
    }

    hr = CoCreateInstance( __uuidof( MMDeviceEnumerator ), NULL, CLSCTX_ALL,
                           __uuidof( IMMDeviceEnumerator ), (void **)&m_pEnumerator );
    if ( FAILED( hr ) ) { ReleaseAll(); return false; }

    m_followDefault = deviceId.empty();
    m_isCaptureRole = isCapture;
    if ( deviceId.empty() ) {
        hr = m_pEnumerator->GetDefaultAudioEndpoint( isCapture ? eCapture : eRender, eConsole, &m_pDevice );
    } else {
        const std::wstring wid = Utf8ToWide( deviceId );
        hr = m_pEnumerator->GetDevice( wid.c_str(), &m_pDevice );
    }
    if ( FAILED( hr ) ) { ReleaseAll(); return false; }

    m_boundDeviceId.clear();
    {
        LPWSTR wid = nullptr;
        if ( SUCCEEDED( m_pDevice->GetId( &wid ) ) && wid != nullptr ) {
            m_boundDeviceId = WideToUtf8( wid );
            CoTaskMemFree( wid );
        }
    }

    hr = m_pDevice->Activate( __uuidof( IAudioClient ), CLSCTX_ALL, NULL, (void **)&m_pAudioClient );
    if ( FAILED( hr ) ) { ReleaseAll(); return false; }

    // Loopback capture must use the device's own mix format.
    hr = m_pAudioClient->GetMixFormat( &m_pFormat );
    if ( FAILED( hr ) ) { ReleaseAll(); return false; }

    m_sampleRate  = (int)m_pFormat->nSamplesPerSec;
    m_numChannels = (int)m_pFormat->nChannels;

    m_isFloatFormat = ( m_pFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT );
    if ( m_pFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE ) {
        // the first GUID dword of an extensible subformat is the basic format tag
        // (same convention the engine uses in XA2_SoundSample.cpp)
        const WAVEFORMATEXTENSIBLE * ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>( m_pFormat );
        m_isFloatFormat = ( ext->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT );
    }
    if ( !m_isFloatFormat && m_pFormat->wBitsPerSample != 16 ) {
        // mix formats are float in practice; 16-bit PCM handled as fallback, others rejected
        ReleaseAll();
        return false;
    }

    // render endpoints are captured in loopback mode; recording endpoints (mic/
    // line-in) are captured directly with no loopback flag.
    const DWORD streamFlags = isCapture ? 0 : AUDCLNT_STREAMFLAGS_LOOPBACK;
    hr = m_pAudioClient->Initialize( AUDCLNT_SHAREMODE_SHARED,
                                     streamFlags,
                                     CAPTURE_BUFFER_DURATION,
                                     0,
                                     m_pFormat,
                                     NULL );
    if ( FAILED( hr ) ) { ReleaseAll(); return false; }

    hr = m_pAudioClient->GetService( __uuidof( IAudioCaptureClient ), (void **)&m_pCaptureClient );
    if ( FAILED( hr ) ) { ReleaseAll(); return false; }

    return true;
}

bool AudioCapture_WASAPI::HasDeviceChangedFromDefault() const {
    if ( !m_followDefault || m_pEnumerator == nullptr ) {
        return false;
    }
    IMMDevice * pCurrentDefault = nullptr;
    if ( FAILED( m_pEnumerator->GetDefaultAudioEndpoint( m_isCaptureRole ? eCapture : eRender, eConsole, &pCurrentDefault ) )
         || pCurrentDefault == nullptr ) {
        return false;   // e.g. no active render endpoint at all right now -- nothing to switch to
    }
    std::string currentId;
    LPWSTR wid = nullptr;
    if ( SUCCEEDED( pCurrentDefault->GetId( &wid ) ) && wid != nullptr ) {
        currentId = WideToUtf8( wid );
        CoTaskMemFree( wid );
    }
    pCurrentDefault->Release();
    return !currentId.empty() && currentId != m_boundDeviceId;
}

void AudioCapture_WASAPI::Start() {
    if ( m_isRunning.load() || m_pAudioClient == nullptr ) {
        return;
    }
    if ( FAILED( m_pAudioClient->Start() ) ) {
        return;
    }
    m_isRunning.store( true );
    m_captureThread = std::thread( &AudioCapture_WASAPI::CaptureThread, this );
}

void AudioCapture_WASAPI::Stop() {
    m_isRunning.store( false );
    if ( m_captureThread.joinable() ) {
        m_captureThread.join();
    }
    if ( m_pAudioClient ) {
        m_pAudioClient->Stop();
    }
}

void AudioCapture_WASAPI::EnumerateDevices( std::vector<AudioDeviceInfo> & out ) {
    out.clear();

    HRESULT hr = CoInitializeEx( NULL, COINIT_MULTITHREADED );
    const bool comInit = SUCCEEDED( hr );   // don't uninit if it was RPC_E_CHANGED_MODE

    IMMDeviceEnumerator * pEnum = nullptr;
    hr = CoCreateInstance( __uuidof( MMDeviceEnumerator ), NULL, CLSCTX_ALL,
                           __uuidof( IMMDeviceEnumerator ), (void **)&pEnum );
    if ( SUCCEEDED( hr ) && pEnum != nullptr ) {
        const EDataFlow flows[2] = { eRender, eCapture };
        for ( int fi = 0; fi < 2; fi++ ) {
            IMMDeviceCollection * coll = nullptr;
            if ( SUCCEEDED( pEnum->EnumAudioEndpoints( flows[fi], DEVICE_STATE_ACTIVE, &coll ) ) && coll != nullptr ) {
                UINT count = 0;
                coll->GetCount( &count );
                for ( UINT i = 0; i < count; i++ ) {
                    IMMDevice * dev = nullptr;
                    if ( FAILED( coll->Item( i, &dev ) ) || dev == nullptr ) {
                        continue;
                    }
                    LPWSTR wid = nullptr;
                    dev->GetId( &wid );

                    std::string name;
                    IPropertyStore * props = nullptr;
                    if ( SUCCEEDED( dev->OpenPropertyStore( STGM_READ, &props ) ) && props != nullptr ) {
                        PROPVARIANT var;
                        memset( &var, 0, sizeof( var ) );   // PropVariantInit
                        if ( SUCCEEDED( props->GetValue( kPKEY_Device_FriendlyName, &var ) )
                             && var.vt == VT_LPWSTR && var.pwszVal != nullptr ) {
                            name = WideToUtf8( var.pwszVal );
                            CoTaskMemFree( var.pwszVal );
                        }
                        props->Release();
                    }

                    AudioDeviceInfo info;
                    info.name = name.empty() ? std::string( "(unknown device)" ) : name;
                    info.id = ( wid != nullptr ) ? WideToUtf8( wid ) : std::string();
                    info.isCapture = ( flows[fi] == eCapture );
                    out.push_back( info );

                    if ( wid != nullptr ) {
                        CoTaskMemFree( wid );
                    }
                    dev->Release();
                }
                coll->Release();
            }
        }
        pEnum->Release();
    }

    if ( comInit ) {
        CoUninitialize();
    }
}

void AudioCapture_WASAPI::CaptureThread() {
    std::vector<float> mono;

    while ( m_isRunning.load() ) {
        UINT32 packetFrames = 0;
        HRESULT hr = m_pCaptureClient->GetNextPacketSize( &packetFrames );

        while ( SUCCEEDED( hr ) && packetFrames > 0 ) {
            BYTE * pData = nullptr;
            UINT32 numFrames = 0;
            DWORD  flags = 0;

            hr = m_pCaptureClient->GetBuffer( &pData, &numFrames, &flags, NULL, NULL );
            if ( FAILED( hr ) ) {
                break;
            }

            if ( numFrames > 0 && m_sink != nullptr ) {
                mono.resize( numFrames );

                if ( flags & AUDCLNT_BUFFERFLAGS_SILENT ) {
                    std::fill( mono.begin(), mono.end(), 0.0f );
                } else if ( m_isFloatFormat ) {
                    const float * f = reinterpret_cast<const float *>( pData );
                    for ( UINT32 i = 0; i < numFrames; i++ ) {
                        float sum = 0.0f;
                        for ( int ch = 0; ch < m_numChannels; ch++ ) {
                            sum += f[i * m_numChannels + ch];
                        }
                        mono[i] = sum / (float)m_numChannels;
                    }
                } else { // 16-bit PCM fallback
                    const short * s = reinterpret_cast<const short *>( pData );
                    for ( UINT32 i = 0; i < numFrames; i++ ) {
                        float sum = 0.0f;
                        for ( int ch = 0; ch < m_numChannels; ch++ ) {
                            sum += (float)s[i * m_numChannels + ch] / 32768.0f;
                        }
                        mono[i] = sum / (float)m_numChannels;
                    }
                }

                m_sink->PushSamples( mono.data(), (int)numFrames, m_sampleRate );
            }

            m_pCaptureClient->ReleaseBuffer( numFrames );
            hr = m_pCaptureClient->GetNextPacketSize( &packetFrames );
        }

        std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
    }
}
