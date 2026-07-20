#ifndef __AUDIO_CAPTURE_WASAPI_H__
#define __AUDIO_CAPTURE_WASAPI_H__

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <thread>
#include <atomic>
#include <string>
#include <vector>

class AudioAnalyzer;
struct AudioDeviceInfo;

/*
================================================
AudioCapture_WASAPI

Shared-mode loopback capture of the default render device ("what you hear").
Captures in the device mix format (typically float32 interleaved), downmixes to
mono, and pushes samples into the owning AudioAnalyzer's ring buffer.

Because the engine itself plays through the default render device, loopback
capture sees the full final mix (game audio, DOOM-classic audio, and our own
music playback) without needing an XAudio2 XAPO tap.
================================================
*/
class AudioCapture_WASAPI {
public:
    explicit AudioCapture_WASAPI( AudioAnalyzer * sink );
    ~AudioCapture_WASAPI();

    // deviceId: opaque WASAPI id (UTF-8), empty = system default endpoint.
    // isCapture: true opens a recording device directly; false opens a render
    // device in loopback ("what you hear").
    bool Initialize( const std::string & deviceId = std::string(), bool isCapture = false );

    // Enumerate active render + capture endpoints (self-contained; safe to call
    // without an instance).
    static void EnumerateDevices( std::vector<AudioDeviceInfo> & out );
    void Start();
    void Stop();
    bool IsRunning() const { return m_isRunning.load(); }

    int  GetSampleRate() const { return m_sampleRate; }

    // Direct user report: switching the OS default output device mid-session
    // (e.g. connecting Bluetooth headphones) silently kills the visualizer's
    // reactivity -- GetDefaultAudioEndpoint is resolved ONCE in Initialize(),
    // so this capture stays bound to whichever endpoint was default at that
    // moment. When Windows moves the default elsewhere, that old endpoint
    // just stops receiving audio (still "active", not invalidated -- no error,
    // no device-lost event, just silence), so nothing here would ever notice
    // on its own. True only when Initialize() was called with an empty
    // deviceId (following the system default) -- a device explicitly picked
    // by id shouldn't be yanked out from under the user just because some
    // OTHER device became the new default.
    bool IsFollowingSystemDefault() const { return m_followDefault; }
    // Re-queries the CURRENT OS default endpoint (same eRender/eCapture role
    // this capture opened with) and compares it against the endpoint actually
    // bound at Initialize() time.
    bool HasDeviceChangedFromDefault() const;

private:
    void CaptureThread();
    void ReleaseAll();

    AudioAnalyzer *         m_sink;

    IMMDeviceEnumerator *   m_pEnumerator = nullptr;
    IMMDevice *             m_pDevice = nullptr;
    IAudioClient *          m_pAudioClient = nullptr;
    IAudioCaptureClient *   m_pCaptureClient = nullptr;
    WAVEFORMATEX *          m_pFormat = nullptr;

    int                     m_sampleRate = 0;
    int                     m_numChannels = 0;
    bool                    m_isFloatFormat = false;
    bool                    m_comInitialized = false;
    bool                    m_followDefault = false;   // Initialize() was called with an empty deviceId
    bool                    m_isCaptureRole = false;    // eCapture vs eRender -- which role to re-check against
    std::string             m_boundDeviceId;            // the endpoint actually bound at Initialize() time

    std::thread             m_captureThread;
    std::atomic<bool>       m_isRunning{ false };
};

#endif // __AUDIO_CAPTURE_WASAPI_H__
