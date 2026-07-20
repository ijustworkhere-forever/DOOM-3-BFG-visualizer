#ifndef __AUDIO_ANALYZER_H__
#define __AUDIO_ANALYZER_H__

// deliberately engine-header-free: this header is included from non-PCH
// translation units (WASAPI capture) as well as engine code
#include <vector>
#include <complex>
#include <mutex>
#include <string>

#ifndef FFT_SIZE
#define FFT_SIZE 1024
#endif

enum class AudioSourceMode {
    ENGINE,             // analyze audio pushed by the engine's own playback path
    WASAPI_LOOPBACK     // analyze audio captured from a WASAPI endpoint (render loopback or a capture device)
};

// A selectable capture endpoint. `id` is the opaque WASAPI device id (UTF-8);
// empty id means the system default. isCapture distinguishes a recording device
// (mic/line-in, captured directly) from a render device (speakers, loopback).
struct AudioDeviceInfo {
    std::string name;
    std::string id;
    bool        isCapture = false;
};

// Seven-band split per docs/research-audiorider.md (sub-bass .. treble),
// plus the MilkDrop three-band contract exposed separately (bass/mid/treb + _att).
enum class AudioBand {
    SubBass,    // 20 - 60 Hz
    Bass,       // 60 - 250 Hz
    MidLow,     // 250 - 500 Hz
    MidHigh,    // 500 - 2000 Hz
    High,       // 2000 - 6000 Hz
    Treble,     // 6000+ Hz
    COUNT
};

class AudioCapture_WASAPI;

class AudioAnalyzer {
public:
    AudioAnalyzer();
    ~AudioAnalyzer();

    // Producers (capture thread / decoder / playback path) push mono float samples here.
    // Thread-safe; keeps the most recent samples in a ring.
    void PushSamples( const float * samples, int count, int sampleRate );

    // Consumes the latest FFT_SIZE window; called once per frame from the main thread.
    // NOTE (frame ordering): soundSystem->Render() runs after the frame's visuals are
    // committed, so consumers always see the previous frame's analysis by design.
    void Update();

    void SetSourceMode( AudioSourceMode mode );
    AudioSourceMode GetSourceMode() const { return m_mode; }

    // --- capture device selection (WASAPI) ---
    // Enumerate active render (loopback) + capture endpoints.
    static void EnumerateDevices( std::vector<AudioDeviceInfo> & out );
    // Choose the endpoint used in WASAPI_LOOPBACK mode. Empty id = system default
    // render device. Switches to loopback mode and (re)starts capture immediately.
    void SetCaptureDevice( const std::string & id, bool isCapture );
    const std::string & GetCaptureDeviceId() const { return m_deviceId; }
    bool GetCaptureIsCapture() const { return m_deviceIsCapture; }
    // Direct user report: "i connected my bluetooth headphones and then the
    // loopback signal died, i am still playing soundcloud music in browser
    // though" -- Windows moved the OS default output to the new device, but
    // this capture (opened against whatever WAS default) never re-checks, so
    // it just goes quiet. Call periodically (see the existing auto-arm retry
    // in Frame()); no-ops unless currently in loopback mode AND following the
    // system default (GetCaptureDeviceId().empty()) -- an explicitly-picked
    // device is left alone even if some OTHER device becomes the new default.
    void RecheckDefaultDevice();

    // --- raw spectrum ---
    float GetFrequencyBin( int bin ) const;         // 0 .. FFT_SIZE/2-1
    float GetSmoothedMagnitude( int bin ) const;
    const float * GetMagnitudes() const { return m_magnitudes.data(); }
    const float * GetSmoothedMagnitudes() const { return m_smoothedMagnitudes.data(); }

    // --- fixed six-band (SubBass..Treble) + aggregate features ---
    float GetBandMagnitude( AudioBand band ) const;
    float GetRMS() const { return m_rms; }

    // --- configurable log-spaced spectrum bands (the primary multi-band API) ---
    // Band count is runtime-settable (e.g. 3/5/7/9); edges are log-spaced from
    // ~30 Hz to min(Nyquist*0.9, 16 kHz). Levels are normalized so ~1.0 is the
    // band's own running-average loudness.
    void  SetBandCount( int n );
    int   GetBandCount() const { return m_bandCount; }
    float GetBandLevel( int i ) const;        // normalized (~1.0 avg, clamp for display)
    float GetBandLevelRaw( int i ) const;     // pre-normalization magnitude
    float GetBandCenterHz( int i ) const;     // geometric-center frequency of band i

    // MilkDrop contract
    float GetBass() const   { return m_bass; }
    float GetMid() const    { return m_mid; }
    float GetTreb() const   { return m_treb; }
    float GetBassAtt() const{ return m_bassAtt; }
    float GetMidAtt() const { return m_midAtt; }
    float GetTrebAtt() const{ return m_trebAtt; }

    // Beat detection: true only on the frame a beat fires (bass transient).
    bool  GetBeat() const   { return m_beat; }
    void  SetBeatSensitivity( float s ) { m_beatSensitivity = s; }
    float GetBeatSensitivity() const { return m_beatSensitivity; }

    // PRD FR-E5: a lightweight BPM estimate, averaged from recent inter-beat
    // intervals (outlier intervals outside a plausible 40-220 BPM range are
    // rejected rather than skewing the average -- a fast onset-based
    // estimator, not a real beat-tracking/autocorrelation algorithm, so
    // treat it as "roughly in the right ballpark," not authoritative tempo
    // detection. Returns 0 until enough consistent beats have been seen.
    float GetBPM() const { return m_bpm; }

    // Legacy accessors kept for existing callers
    float GetBassMagnitude() const  { return m_bass; }
    float GetTrebleMagnitude() const{ return m_treb; }
    float GetMaxBass() const        { return m_bass; }
    float GetMaxTreble() const      { return m_treb; }

    // Waveform access for scope-style rendering (last analyzed window, pre-window-function)
    const float * GetWaveform() const { return m_waveform.data(); }

    // Diagnostics: raw (pre-normalization) values so console tools can prove the pipeline
    int   GetSampleRate() const { return m_sampleRate; }
    float GetBandRaw( AudioBand band ) const;
    float GetBandAverage( AudioBand band ) const;

private:
    void PrecomputeTables();
    void PerformFFT( const float * input, float * outMagnitudes );
    void ComputeDynamicBands();
    float SumBins( int firstBin, int lastBin ) const;
    int   HzToBin( float hz ) const;

    // sample intake ring (written by producers, read by Update)
    std::mutex           m_ringMutex;
    std::vector<float>   m_ring;
    size_t               m_ringWrite;      // next write index
    unsigned long long   m_ringTotal;      // total samples ever written
    int                  m_sampleRate;

    // FFT tables
    std::vector<std::complex<float>> m_twiddles;   // FFT_SIZE/2 factors
    std::vector<int>     m_bitReverse;             // FFT_SIZE permutation
    std::vector<float>   m_hannWindow;             // FFT_SIZE window

    // analysis products
    std::vector<float>   m_waveform;               // FFT_SIZE latest samples
    std::vector<float>   m_magnitudes;             // FFT_SIZE/2 magnitudes
    std::vector<float>   m_smoothedMagnitudes;
    std::vector<float>   m_bandRaw;                // AudioBand::COUNT
    std::vector<float>   m_bandAvg;                // running averages (normalizers)
    std::vector<float>   m_bandMagnitudes;         // normalized band values

    // configurable log-spaced spectrum bands
    int                  m_bandCount;
    std::vector<float>   m_dynEdgeHz;              // m_bandCount + 1 log-spaced edges
    std::vector<float>   m_dynRaw;                 // m_bandCount raw magnitudes
    std::vector<float>   m_dynAvg;                 // m_bandCount running averages
    std::vector<float>   m_dynLevel;              // m_bandCount display levels (0..~1.5)
    int                  m_dynEdgeSampleRate;      // sampleRate the edges were built for
    float                m_dynGlobalMax;          // slow peak-follower for auto-gain

    float m_rms;
    float m_smoothingFactor;

    float m_bass, m_mid, m_treb;
    float m_bassAtt, m_midAtt, m_trebAtt;
    float m_bassAvg, m_midAvg, m_trebAvg;          // long-run normalizers

    // beat detection
    bool  m_beat;
    float m_beatSensitivity;                       // fire when bass > sensitivity * long avg
    int   m_beatRefractoryMs;
    int   m_lastBeatTime;

    // PRD FR-E5: lightweight BPM estimate from recent inter-beat intervals
    static const int BPM_HISTORY = 8;
    float m_beatIntervalsSec[BPM_HISTORY];
    int   m_beatIntervalCount;      // how many of the above are valid (0..BPM_HISTORY)
    int   m_beatIntervalWrite;      // next ring-buffer write slot
    float m_bpm;

    AudioSourceMode m_mode = AudioSourceMode::ENGINE;
    AudioCapture_WASAPI * m_wasapiCapture = nullptr;
    std::string m_deviceId;               // selected endpoint (empty = default)
    bool m_deviceIsCapture = false;       // true = recording device, false = render/loopback
};

#endif // __AUDIO_ANALYZER_H__
