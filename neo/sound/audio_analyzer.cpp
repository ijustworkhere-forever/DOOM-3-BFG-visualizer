#include "../idlib/precompiled.h"
#pragma hdrstop
#include "audio_analyzer.h"
#include "AudioCapture_WASAPI.h"
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const int   RING_SIZE          = FFT_SIZE * 8;
static const float BAND_AVG_RATE      = 0.005f;   // long-run normalizer time constant
static const float ATT_RATE           = 0.4f;     // x_att = x_att*(1-r) + x*r (MilkDrop-style)
static const float NORM_EPSILON       = 1e-6f;

// AudioBand breakpoints in Hz (upper edge of each band); see audio_analyzer.h
static const float BAND_EDGES_HZ[static_cast<int>(AudioBand::COUNT) + 1] = {
    20.0f, 60.0f, 250.0f, 500.0f, 2000.0f, 6000.0f, 20000.0f
};

AudioAnalyzer::AudioAnalyzer() :
    m_ringWrite( 0 ),
    m_ringTotal( 0 ),
    m_sampleRate( 44100 ),
    m_rms( 0.0f ),
    m_smoothingFactor( 0.2f ),
    m_bass( 0.0f ), m_mid( 0.0f ), m_treb( 0.0f ),
    m_bassAtt( 0.0f ), m_midAtt( 0.0f ), m_trebAtt( 0.0f ),
    m_bassAvg( NORM_EPSILON ), m_midAvg( NORM_EPSILON ), m_trebAvg( NORM_EPSILON ),
    m_beat( false ),
    m_beatSensitivity( 1.6f ),
    m_beatRefractoryMs( 150 ),
    m_lastBeatTime( 0 ),
    m_beatIntervalCount( 0 ),
    m_beatIntervalWrite( 0 ),
    m_bpm( 0.0f ),
    m_bandCount( 0 ),
    m_dynEdgeSampleRate( 0 ),
    m_dynGlobalMax( NORM_EPSILON ) {

    for ( int i = 0; i < BPM_HISTORY; i++ ) {
        m_beatIntervalsSec[i] = 0.0f;
    }

    m_ring.resize( RING_SIZE, 0.0f );
    m_waveform.resize( FFT_SIZE, 0.0f );
    m_magnitudes.resize( FFT_SIZE / 2, 0.0f );
    m_smoothedMagnitudes.resize( FFT_SIZE / 2, 0.0f );
    m_bandRaw.resize( static_cast<int>( AudioBand::COUNT ), 0.0f );
    m_bandAvg.resize( static_cast<int>( AudioBand::COUNT ), NORM_EPSILON );
    m_bandMagnitudes.resize( static_cast<int>( AudioBand::COUNT ), 0.0f );
    PrecomputeTables();
    SetBandCount( 7 );   // default: 7 log-spaced spectrum bands
}

/*
========================
AudioAnalyzer::SetBandCount

Resize the configurable spectrum-band arrays. Edges are (re)built lazily in
ComputeDynamicBands() so a sample-rate change is picked up automatically.
========================
*/
void AudioAnalyzer::SetBandCount( int n ) {
    if ( n < 1 ) {
        n = 1;
    }
    if ( n > 64 ) {
        n = 64;
    }
    if ( n == m_bandCount ) {
        return;
    }
    m_bandCount = n;
    m_dynEdgeHz.assign( n + 1, 0.0f );
    m_dynRaw.assign( n, 0.0f );
    m_dynAvg.assign( n, NORM_EPSILON );
    m_dynLevel.assign( n, 0.0f );
    m_dynEdgeSampleRate = 0;   // force edge rebuild
}

AudioAnalyzer::~AudioAnalyzer() {
    if ( m_wasapiCapture ) {
        m_wasapiCapture->Stop();
        delete m_wasapiCapture;
        m_wasapiCapture = nullptr;
    }
}

void AudioAnalyzer::PrecomputeTables() {
    m_twiddles.resize( FFT_SIZE / 2 );
    for ( int i = 0; i < FFT_SIZE / 2; i++ ) {
        const float angle = -2.0f * (float)M_PI * i / FFT_SIZE;
        m_twiddles[i] = std::complex<float>( std::cos( angle ), std::sin( angle ) );
    }

    // bit-reversal permutation for radix-2 iterative FFT
    m_bitReverse.resize( FFT_SIZE );
    int bits = 0;
    while ( ( 1 << bits ) < FFT_SIZE ) {
        bits++;
    }
    for ( int i = 0; i < FFT_SIZE; i++ ) {
        int rev = 0;
        for ( int b = 0; b < bits; b++ ) {
            if ( i & ( 1 << b ) ) {
                rev |= 1 << ( bits - 1 - b );
            }
        }
        m_bitReverse[i] = rev;
    }

    m_hannWindow.resize( FFT_SIZE );
    for ( int i = 0; i < FFT_SIZE; i++ ) {
        m_hannWindow[i] = 0.5f * ( 1.0f - std::cos( 2.0f * (float)M_PI * i / ( FFT_SIZE - 1 ) ) );
    }
}

void AudioAnalyzer::SetSourceMode( AudioSourceMode mode ) {
    if ( mode == m_mode && ( mode != AudioSourceMode::WASAPI_LOOPBACK || m_wasapiCapture != nullptr ) ) {
        return;
    }
    m_mode = mode;
    if ( mode == AudioSourceMode::WASAPI_LOOPBACK ) {
        if ( !m_wasapiCapture ) {
            m_wasapiCapture = new AudioCapture_WASAPI( this );
            if ( !m_wasapiCapture->Initialize( m_deviceId, m_deviceIsCapture ) ) {
                delete m_wasapiCapture;
                m_wasapiCapture = nullptr;
                m_mode = AudioSourceMode::ENGINE;
                idLib::Warning( "AudioAnalyzer: WASAPI capture init failed, staying on engine source" );
                return;
            }
            m_wasapiCapture->Start();
        }
    } else {
        if ( m_wasapiCapture ) {
            m_wasapiCapture->Stop();
            delete m_wasapiCapture;
            m_wasapiCapture = nullptr;
        }
    }
}

void AudioAnalyzer::EnumerateDevices( std::vector<AudioDeviceInfo> & out ) {
    AudioCapture_WASAPI::EnumerateDevices( out );
}

void AudioAnalyzer::RecheckDefaultDevice() {
    if ( m_mode != AudioSourceMode::WASAPI_LOOPBACK || m_wasapiCapture == nullptr ) {
        return;
    }
    if ( !m_wasapiCapture->IsFollowingSystemDefault() || !m_wasapiCapture->HasDeviceChangedFromDefault() ) {
        return;
    }
    // Same rebuild SetCaptureDevice already uses: tear down, force m_mode
    // back to ENGINE so SetSourceMode's "already there" early-return doesn't
    // skip the rebuild, then re-arm against whatever the CURRENT default is
    // (m_deviceId is still empty -- we're following the default, not pinned).
    m_wasapiCapture->Stop();
    delete m_wasapiCapture;
    m_wasapiCapture = nullptr;
    m_mode = AudioSourceMode::ENGINE;
    SetSourceMode( AudioSourceMode::WASAPI_LOOPBACK );
}

void AudioAnalyzer::SetCaptureDevice( const std::string & id, bool isCapture ) {
    m_deviceId = id;
    m_deviceIsCapture = isCapture;
    // tear down any current capture so the next mode switch rebuilds on the new
    // endpoint, then (re)start in loopback/capture mode.
    if ( m_wasapiCapture ) {
        m_wasapiCapture->Stop();
        delete m_wasapiCapture;
        m_wasapiCapture = nullptr;
    }
    m_mode = AudioSourceMode::ENGINE;   // force SetSourceMode to rebuild
    SetSourceMode( AudioSourceMode::WASAPI_LOOPBACK );
}

void AudioAnalyzer::PushSamples( const float * samples, int count, int sampleRate ) {
    if ( samples == nullptr || count <= 0 ) {
        return;
    }
    std::lock_guard<std::mutex> lock( m_ringMutex );
    m_sampleRate = sampleRate;
    for ( int i = 0; i < count; i++ ) {
        m_ring[m_ringWrite] = samples[i];
        m_ringWrite = ( m_ringWrite + 1 ) % RING_SIZE;
    }
    m_ringTotal += count;
}

int AudioAnalyzer::HzToBin( float hz ) const {
    const float binWidth = (float)m_sampleRate / FFT_SIZE;
    int bin = (int)( hz / binWidth );
    if ( bin < 0 ) {
        bin = 0;
    }
    if ( bin > FFT_SIZE / 2 - 1 ) {
        bin = FFT_SIZE / 2 - 1;
    }
    return bin;
}

float AudioAnalyzer::SumBins( int firstBin, int lastBin ) const {
    float sum = 0.0f;
    for ( int i = firstBin; i <= lastBin; i++ ) {
        sum += m_magnitudes[i];
    }
    return sum;
}

/*
========================
AudioAnalyzer::PerformFFT

Iterative radix-2 Cooley-Tukey with bit-reversal permutation.
input:  FFT_SIZE real samples (already windowed)
output: FFT_SIZE/2 magnitudes
========================
*/
void AudioAnalyzer::PerformFFT( const float * input, float * outMagnitudes ) {
    std::complex<float> data[FFT_SIZE];
    for ( int i = 0; i < FFT_SIZE; i++ ) {
        data[m_bitReverse[i]] = std::complex<float>( input[i], 0.0f );
    }

    for ( int len = 2; len <= FFT_SIZE; len <<= 1 ) {
        const int half = len >> 1;
        const int step = FFT_SIZE / len;
        for ( int i = 0; i < FFT_SIZE; i += len ) {
            for ( int j = 0; j < half; j++ ) {
                const std::complex<float> t = m_twiddles[j * step] * data[i + j + half];
                const std::complex<float> even = data[i + j];
                data[i + j]        = even + t;
                data[i + j + half] = even - t;
            }
        }
    }

    const float scale = 2.0f / FFT_SIZE;
    for ( int i = 0; i < FFT_SIZE / 2; i++ ) {
        outMagnitudes[i] = std::abs( data[i] ) * scale;
    }
}

void AudioAnalyzer::Update() {
    m_beat = false;

    // snapshot the newest FFT_SIZE samples from the ring
    float windowed[FFT_SIZE];
    {
        std::lock_guard<std::mutex> lock( m_ringMutex );
        if ( m_ringTotal < FFT_SIZE ) {
            return; // not enough audio yet
        }
        size_t readPos = ( m_ringWrite + RING_SIZE - FFT_SIZE ) % RING_SIZE;
        for ( int i = 0; i < FFT_SIZE; i++ ) {
            m_waveform[i] = m_ring[readPos];
            readPos = ( readPos + 1 ) % RING_SIZE;
        }
    }

    float sumSq = 0.0f;
    for ( int i = 0; i < FFT_SIZE; i++ ) {
        sumSq += m_waveform[i] * m_waveform[i];
        windowed[i] = m_waveform[i] * m_hannWindow[i];
    }
    m_rms = std::sqrt( sumSq / FFT_SIZE );

    PerformFFT( windowed, m_magnitudes.data() );

    for ( int i = 0; i < FFT_SIZE / 2; i++ ) {
        m_smoothedMagnitudes[i] += m_smoothingFactor * ( m_magnitudes[i] - m_smoothedMagnitudes[i] );
    }

    // seven-band split, normalized against running averages
    for ( int b = 0; b < static_cast<int>( AudioBand::COUNT ); b++ ) {
        const int first = HzToBin( BAND_EDGES_HZ[b] );
        const int last  = HzToBin( BAND_EDGES_HZ[b + 1] );
        const float raw = SumBins( first, last ) / (float)( last - first + 1 );
        m_bandRaw[b] = raw;
        m_bandAvg[b] += BAND_AVG_RATE * ( raw - m_bandAvg[b] );
        m_bandMagnitudes[b] = raw / std::max( m_bandAvg[b], NORM_EPSILON );
    }

    // MilkDrop three-band contract (bass 20-250, mid 250-2500, treb 2500+)
    const float bassRaw = SumBins( HzToBin( 20.0f ),   HzToBin( 250.0f ) );
    const float midRaw  = SumBins( HzToBin( 250.0f ),  HzToBin( 2500.0f ) );
    const float trebRaw = SumBins( HzToBin( 2500.0f ), FFT_SIZE / 2 - 1 );

    m_bassAvg += BAND_AVG_RATE * ( bassRaw - m_bassAvg );
    m_midAvg  += BAND_AVG_RATE * ( midRaw  - m_midAvg );
    m_trebAvg += BAND_AVG_RATE * ( trebRaw - m_trebAvg );

    m_bass = bassRaw / std::max( m_bassAvg, NORM_EPSILON );
    m_mid  = midRaw  / std::max( m_midAvg,  NORM_EPSILON );
    m_treb = trebRaw / std::max( m_trebAvg, NORM_EPSILON );

    m_bassAtt += ATT_RATE * ( m_bass - m_bassAtt );
    m_midAtt  += ATT_RATE * ( m_mid  - m_midAtt );
    m_trebAtt += ATT_RATE * ( m_treb - m_trebAtt );

    ComputeDynamicBands();

    // beat: bass transient over its own envelope, with a refractory period
    const int now = Sys_Milliseconds();
    if ( m_bass > m_beatSensitivity * std::max( m_bassAtt, NORM_EPSILON ) &&
         now - m_lastBeatTime > m_beatRefractoryMs ) {
        m_beat = true;

        // PRD FR-E5: lightweight BPM estimate from this interval, if it's the
        // first beat there's nothing to measure yet. Outlier intervals outside
        // a plausible 40-220 BPM range are rejected (avoids one glitched
        // onset -- e.g. a refractory-period edge case -- wrecking the running
        // average) rather than silently averaged in.
        if ( m_lastBeatTime != 0 ) {
            const float intervalSec = (float)( now - m_lastBeatTime ) * 0.001f;
            const float instBpm = ( intervalSec > 0.001f ) ? ( 60.0f / intervalSec ) : 0.0f;
            if ( instBpm >= 40.0f && instBpm <= 220.0f ) {
                m_beatIntervalsSec[m_beatIntervalWrite] = intervalSec;
                m_beatIntervalWrite = ( m_beatIntervalWrite + 1 ) % BPM_HISTORY;
                if ( m_beatIntervalCount < BPM_HISTORY ) {
                    m_beatIntervalCount++;
                }
                float sum = 0.0f;
                for ( int i = 0; i < m_beatIntervalCount; i++ ) {
                    sum += m_beatIntervalsSec[i];
                }
                const float avgIntervalSec = sum / (float)m_beatIntervalCount;
                m_bpm = ( avgIntervalSec > 0.001f ) ? ( 60.0f / avgIntervalSec ) : 0.0f;
            }
        }
        m_lastBeatTime = now;
    }
}

float AudioAnalyzer::GetFrequencyBin( int bin ) const {
    if ( bin >= 0 && bin < FFT_SIZE / 2 ) {
        return m_magnitudes[bin];
    }
    return 0.0f;
}

float AudioAnalyzer::GetSmoothedMagnitude( int bin ) const {
    if ( bin >= 0 && bin < FFT_SIZE / 2 ) {
        return m_smoothedMagnitudes[bin];
    }
    return 0.0f;
}

float AudioAnalyzer::GetBandMagnitude( AudioBand band ) const {
    const int i = static_cast<int>( band );
    if ( i >= 0 && i < static_cast<int>( AudioBand::COUNT ) ) {
        return m_bandMagnitudes[i];
    }
    return 0.0f;
}

/*
========================
AudioAnalyzer::ComputeDynamicBands

Log-spaced spectrum bands. Edges rebuild whenever the band count or sample rate
changes, spanning 30 Hz .. min(Nyquist*0.9, 16 kHz). Each band is the mean bin
magnitude over its range, normalized by its own slow running average.
========================
*/
void AudioAnalyzer::ComputeDynamicBands() {
    if ( m_bandCount < 1 ) {
        return;
    }

    if ( m_dynEdgeSampleRate != m_sampleRate ) {
        const float fLow  = 30.0f;
        const float fHigh = std::min( m_sampleRate * 0.5f * 0.9f, 16000.0f );
        const float ratio = ( fHigh > fLow ) ? ( fHigh / fLow ) : 2.0f;
        for ( int i = 0; i <= m_bandCount; i++ ) {
            const float t = (float)i / (float)m_bandCount;
            m_dynEdgeHz[i] = fLow * std::pow( ratio, t );
        }
        m_dynEdgeSampleRate = m_sampleRate;
    }

    // pass 1: raw band magnitudes + track the loudest band this frame
    float maxRaw = NORM_EPSILON;
    for ( int b = 0; b < m_bandCount; b++ ) {
        int first = HzToBin( m_dynEdgeHz[b] );
        int last  = HzToBin( m_dynEdgeHz[b + 1] );
        if ( last < first ) {
            last = first;   // very narrow low band maps to a single bin
        }
        const float raw = SumBins( first, last ) / (float)( last - first + 1 );
        m_dynRaw[b] = raw;
        m_dynAvg[b] += BAND_AVG_RATE * ( raw - m_dynAvg[b] );   // kept for diagnostics
        if ( raw > maxRaw ) {
            maxRaw = raw;
        }
    }

    // global auto-gain: a peak-follower with fast attack / slow decay keeps the
    // display filling nicely while a silent band still reads ~0 (unlike per-band
    // AGC, which amplifies noise in quiet bands). Floor prevents runaway gain.
    if ( maxRaw > m_dynGlobalMax ) {
        m_dynGlobalMax = maxRaw;                                   // fast attack
    } else {
        m_dynGlobalMax += 0.02f * ( maxRaw - m_dynGlobalMax );     // slow decay
    }
    const float gainFloor = 0.02f;
    const float invGain = 1.0f / std::max( m_dynGlobalMax, gainFloor );

    // pass 2: display levels normalized against the global peak
    for ( int b = 0; b < m_bandCount; b++ ) {
        m_dynLevel[b] = m_dynRaw[b] * invGain;
    }
}

float AudioAnalyzer::GetBandLevel( int i ) const {
    if ( i >= 0 && i < m_bandCount ) {
        return m_dynLevel[i];
    }
    return 0.0f;
}

float AudioAnalyzer::GetBandLevelRaw( int i ) const {
    if ( i >= 0 && i < m_bandCount ) {
        return m_dynRaw[i];
    }
    return 0.0f;
}

float AudioAnalyzer::GetBandCenterHz( int i ) const {
    if ( i >= 0 && i < m_bandCount && (int)m_dynEdgeHz.size() > i + 1 ) {
        return std::sqrt( m_dynEdgeHz[i] * m_dynEdgeHz[i + 1] );   // geometric center
    }
    return 0.0f;
}

float AudioAnalyzer::GetBandRaw( AudioBand band ) const {
    const int i = static_cast<int>( band );
    if ( i >= 0 && i < static_cast<int>( AudioBand::COUNT ) ) {
        return m_bandRaw[i];
    }
    return 0.0f;
}

float AudioAnalyzer::GetBandAverage( AudioBand band ) const {
    const int i = static_cast<int>( band );
    if ( i >= 0 && i < static_cast<int>( AudioBand::COUNT ) ) {
        return m_bandAvg[i];
    }
    return 0.0f;
}
