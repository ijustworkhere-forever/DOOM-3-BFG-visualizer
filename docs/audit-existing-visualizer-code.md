# Audit: Existing Visualizer Code (July 2026)

Full-file review of every visualizer-related source file in the tree. Bottom line: **the
engine's own sound/UI files are stock and healthy; every defect lives in the new,
not-yet-compiled prototype files.** None of the new files are referenced by any
`.vcxproj` — `neo/doomexe.vcxproj` (line ~527) carries the comment *"Experimental
audio/visualizer sources are maintained separately until their public engine adapters are
complete."* The successful Debug|x64 build therefore proves the stock engine, not the
visualizer.

## Inventory of new (non-stock) files

| File | Purpose | Compiles? |
|---|---|---|
| `neo/sound/audio_analyzer.h/.cpp` | FFT analyzer, band magnitudes, RMS, WASAPI/engine source switch | **No** (see below) |
| `neo/sound/AudioCapture_WASAPI.h/.cpp` | WASAPI loopback capture thread | **No** |
| `neo/sound/visualizer.h/.cpp` | 64-bar smoothed bar heights from analyzer | Yes (given analyzer) |
| `neo/sound/visualizer_data.h/.cpp` | global `g_audioAnalyzer` instance | Yes |
| `neo/sound/playlist_manager.h/.cpp` | `idPlaylistManager` track list | **No** |
| `neo/sound/XAudio2/XA2_Decoder.h` | abstract `idDecoder` (channels/rate/block/streaming) | Yes |
| `neo/sound/XAudio2/XA2_OggDecoder.h/.cpp` | stb_vorbis-based OGG decoder | **No** |
| `neo/sound/XAudio2/XA2_FFmpegDecoder.h/.cpp` | FFmpeg/swresample decoder | **No** |
| `neo/ui/VisualizerUI.h/.cpp` | `idUserInterfaceLocal` subclass: preset/device/playlist UI + bar drawing | **No** |
| `neo/ui/VisualizerPreset.h` | preset struct (name, vertex/fragment shader text, param map) | Yes |
| `neo/ui/MilkDropParser.h/.cpp` | parser for the custom `.milk` INI format | **No** (bad includes) |
| `neo/ui/presets/*.milk`, root `test_*.milk` | custom-format presets (NOT real MilkDrop format) | n/a |
| `temp_file.cpp` (repo root) | dead, truncated older copy of XA2_SoundSample.cpp (`m_oggDecoder`, `LoadOgg`, brace imbalance, ends mid-function) | scratch — delete |
| `test_parser.py`, `test_parser_v2.py` | Python prototypes of the preset parser (v2 handles shader blocks) | n/a |
| `fix_vcxproj.py` | rewrites ProjectConfigurations blocks to add x64 (hardcodes `C:\DOOM-3-BFG`) | n/a |

Modified stock files (small, benign):
- `neo/ui/UserInterface.h` — added `virtual bool IsVisualizer() const { return false; }`.
- `neo/ui/UserInterface.cpp` — added `#include "VisualizerUI.h"` only. The previously
  reported "malformed inserted member definitions" are gone; the file is otherwise stock.
- `neo/sound/XAudio2/XA2_SoundSample.h/.cpp` — added `idDecoder * m_decoder`, `LoadMedia()`
  (FFmpeg-gated via `__has_include(<libavcodec/avcodec.h>)`), decoder cleanup in
  `FreeData/LoadAmplitude/LoadGeneratedSample`. Compiles without FFmpeg installed
  (falls back to `return false`), but has logic bugs (below).
- `neo/renderer/RenderBackend.h` — new abstract backend interface, **syntactically broken**,
  correctly excluded from the build.

Everything else inspected in `neo/sound/` (snd_system, snd_world, snd_emitter, snd_shader,
snd_local.h, sound.h, SoundVoice, WaveFile, XA2_SoundVoice, XA2_SoundHardware) is byte-level
stock BFG code.

## Defects, file by file

### audio_analyzer.cpp / .h
1. `.cpp` defines and calls `precomputeTwiddleFactors()`, which is **not declared in the
   header** → compile error.
2. The Cooley-Tukey FFT has **no bit-reversal permutation**, and the butterfly computes
   `even ± i·t` instead of `even ± t` (it adds the twiddled odd term rotated by 90°).
   Output spectrum is wrong (still "reactive", but bins don't mean what they claim).
3. Engine mode calls `soundSystemLocal.hardware.GetVoices()`,
   `voice->GetPlayCursor()`, `voice->GetLeadInSample()`, `voice->IsPlaying()` (private),
   and reads `sample->buffers` (protected). **None of these accessors exist** in
   `XA2_SoundHardware.h` / `XA2_SoundVoice.h` / `XA2_SoundSample.h`. The engine adapters
   named in `plans/implement-fft-audio-analyzer.md` were never added.
4. Nothing ever calls `AudioAnalyzer::Update()` — `idSoundSystemLocal::Render()`
   (snd_system.cpp) is stock. The PROGRESS item "Fix snd_system.cpp update" refers to this
   missing hook.
5. `FFT_SIZE` is `#define`d in both `audio_analyzer.h` (1024) and `AudioCapture_WASAPI.h`
   (1024) — redefinition landmine.
6. `GetChannelData` decodes raw buffer bytes as 16-bit PCM; correct for PCM `.wav` but
   garbage for ADPCM/XMA2-compressed samples (`s_useCompression` defaults to 1 → most
   engine samples are msadpcm).

### AudioCapture_WASAPI.cpp
Invented/wrong Win32 API usage throughout `Initialize()`:
- `__uuid(...)` → should be `__uuidof(...)`.
- `wFormatTag = WAVEFORMATEXTENSIBLE` → should be `WAVE_FORMAT_EXTENSIBLE`.
- `nSubAlgs` is not a `WAVEFORMATEX` field; `nAvgBytesPerSec` is never set.
- Casting a stack `WAVEFORMATEX` to `WAVEFORMATEXTENSIBLE*` and writing `->subformat`
  (real field: `SubFormat`) **writes past the end of the struct**; the IEEE-float GUID
  constant is misnamed (`KSD_WAVEFORMAT_IEEE_FLOAT` vs `KSDATAFORMAT_SUBTYPE_IEEE_FLOAT`).
- Loopback capture must use the **device mix format** (`GetMixFormat` result), not a
  requested format; buffer duration `100` (100-ns units = 10 µs) is far too small;
  `AUDCLNT_STREAMFLAGS_EVENTCALLBACK` is set but no event handle is registered.
- Capture loop calls `GetBuffer` twice (first call misused as a packet-size probe —
  should be `GetNextPacketSize`), writes into the ring buffer starting at index
  `i % size` every packet (no persistent write cursor, so the buffer is a torn mix of
  old/new), and `GetLatestSamples` has no "new data" tracking.

### playlist_manager.cpp
- `AddTrack` calls a free function `Alloc()` that doesn't exist.
- `m_tracks` is `idList<idPlaylistEntry>` (values), but code uses pointer semantics:
  `m_tracks.DeleteContents(true)` (pointer-list API), `m_tracks[i]->path` (deref).
- Won't compile; needs a decision: value list (simplest) or `idList<idPlaylistEntry*>`.

### XA2_OggDecoder.cpp
- Includes `../../external/stb_vorbis.h` — **`neo/external/` does not exist** in the tree.
- Calls `stb_vorbis_open_filename(filename, &error)` — real signature takes a third
  `stb_vorbis_alloc*` arg; `stb_vorbis_get_pcm_f32_float_array` **is not an stb_vorbis
  function** (real one: `stb_vorbis_get_samples_float_interleaved(v, channels, buffer,
  num_floats)`). Also opens via OS path, bypassing `idFileSystem`.

### XA2_FFmpegDecoder.cpp
- `m_codecCtx->ch_layout.nb_channel` → real field `nb_channels`.
- `av_swr_get_out_samples` / `av_swr_convert` → real names `swr_get_out_samples` /
  `swr_convert`; `swr_convert` needs `(uint8_t**)&outPtr`, not a `float*` data pointer.
- `m_frame->data_size` doesn't exist (should pass `m_frame->nb_samples`).
- `tmpBuf` is sized `out_samples` but holds `out_samples * channels` floats → overflow.
- `m_bufferOffset` is used as both "samples available" and an offset; naming/logic muddle.
- No EOF flush (`avcodec_send_packet(ctx, NULL)` drain) — matches the known plan item.

### XA2_SoundSample.cpp (LoadResource / LoadMedia)
- Extension bug: `.wav`/`.msadpcm` is appended **before** the `.ogg` check, so an OGG
  sample named `music/track.ogg` asks the decoder to open `music/track.ogg.wav`.
- `LoadMedia` pre-decodes exactly one 1024-frame block into one buffer; there is no
  streaming continuation, so "streaming" OGG/FFmpeg playback would play ~23 ms of audio.
  The float→int16 conversion loop iterates `decoded` (frames) but the buffer holds
  frames×channels samples.

### VisualizerUI.cpp
- Calls `desktop->AddStateVar(...)` and `desktop->AddText(...)` — **`idWindow` has no such
  methods** (verified against `neo/ui/Window.h`).
- `dc->DrawRect(x,y,w,h,color)` — real signature is `DrawRect(x, y, w, h, size, color)`;
  `dc->foreColor` / `DrawText(x+5, y+5, text, color)` don't match `idDeviceContext`.
- References `soundSystemLocal.playlistManager` — no such member on `idSoundSystemLocal`.
- `desktop->GetStateString(...)` is an `idUserInterfaceLocal` method, not `idWindow`.
- Bar loop reads bins `i * (FFT_SIZE/128)` = `i*8` up to 504 — fine — but
  `Visualizer::Update()` (visualizer.cpp) maps `i*16` up to 1008, past the 511 valid bins,
  so the top half of its 64 bars is permanently zero.
- `uiManagerLocal.screenRect` is accessible only because the class lives in the same TU
  chain; `uiManagerLocal` is not exported in a header — needs an accessor.

### MilkDropParser.cpp
- `#include "idlib/idFile.h"` and `"idlib/idString.h"` — neither path exists
  (`framework/File.h`, `idlib/Str.h`). Uses `std::ifstream` with a raw path, bypassing
  `idFileSystem` (presets inside resource containers would be invisible).
- Parser itself is coherent for the custom format and matches `test_parser_v2.py`.

### RenderBackend.h
- `public.` instead of `public:`; `DrawStretchTri(...) =` missing the `0;`;
  `SetState(...) = 1;` invalid pure-specifier; includes `../idlib/idVec3.h`,
  `idVec4.h`, `idScreenRect.h` which don't exist (real: `idlib/math/Vector.h`,
  `idlib/geometry/…`, renderer `ScreenRect.h`). Keep excluded; rewrite when a real
  abstraction is designed.

## Important conceptual gap: the ".milk" format here is not MilkDrop

The custom format (`[preset]/[effect]/[param]`, GLSL-ish blocks terminated by `END`,
`param 400`-style audio bindings where 400=RMS, 401/402=frequency bins) is unrelated to
real MilkDrop `.milk` files (INI `per_frame_N=`/`per_pixel_N=` equations, q/t variables,
wavecode/shapecode, HLSL warp/comp shaders). Even the embedded "GLSL" is invalid as
written (`float rms = param 400;` is not GLSL — it implies a preprocessing/substitution
step that doesn't exist yet). A decision is needed: rename ours (e.g. `.dviz`) and keep it
simple, or implement genuine MilkDrop-preset compatibility (see research docs).

## What the engine already gives us (verified in stock code)

- **`idSoundEmitter::CurrentAmplitude()`** — 0–1 amplitude from precomputed `.amp`
  sidecar files (60 Hz envelope, generated by the `amplitude` tool in `neo/amplitude/`);
  used today for shader effects. `idSoundWorld::CurrentShakeAmplitude()` aggregates it.
- **XAudio2 VU meter APF** — `idSoundVoice_XAudio2::GetAmplitude()` reads a real-time RMS
  from an XAudio2 volume-meter effect on each voice (when `shakes` set), and
  `idSoundHardware_XAudio2::Update()` reads master-voice RMS/peak per channel
  (`s_showLevelMeter`). **A master-voice effect chain is the cleanest place to tap the
  final mix** — an FFT APO/tap there would see everything the engine plays, at the right
  time, with no per-voice buffer spelunking and no play-cursor problem.
- **Device enumeration** — `listDevices` command + `s_device` cvar already enumerate and
  select output devices (XAudio2 2.7 API), directly reusable for the device-picker UI.
- **`ImageForTime()`** (sound level meter window) is a stubbed hook intended for exactly
  this: an image generated from audio each frame.

## Recommended repair order (compile-safety first)

1. Delete `temp_file.cpp`; add `.gitignore` entries for build logs.
2. Declare `precomputeTwiddleFactors()` in `audio_analyzer.h`; fix the FFT (bit-reversal +
   correct butterfly) and validate against a known sine (plans/implement-fft-audio-analyzer.md).
3. Replace the per-voice buffer-reading `GetChannelData` design with a **master-voice tap**
   (XAudio2 effect chain or a submix tap), which eliminates the need for
   `GetPlayCursor`/`GetVoices`/`buffers` accessors entirely. If per-voice access is still
   wanted, add the small public adapters to the XA2 headers instead of friending.
4. Rewrite `AudioCapture_WASAPI::Initialize()` per the real WASAPI loopback contract
   (mix format, event handle, proper ring buffer) — this is the "Capture Mode" milestone.
5. Fix `playlist_manager` list semantics; add `playlistManager` member (or a standalone
   manager owned by the visualizer, not `idSoundSystemLocal`).
6. Fix `MilkDropParser` includes; route file IO through `idFileSystem`.
7. Rebuild `VisualizerUI` against the real `idWindow`/`idDeviceContext` APIs — or (better)
   as an SWF menu screen; see `plans/visualizer-ui.md`.
8. Only then add the files to `doomexe.vcxproj` one at a time, compiling at each step.
