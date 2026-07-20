# Research: DiscoDiffusion-AudioRider (a 2022 audio-reactive diffusion-animation project) — reusable ideas

Source: a prior open-source project pairing audio analysis with Disco Diffusion v5.2
Warp (examined directly: repo tree + `Disco_Diffusion_v5_2_Warp_AudioRider.ipynb` audio
cells). The project later migrated to a successor repo, `audiorider-diffusion`; check
that one for the latest iteration if deeper archaeology is wanted.

## What it is

Colab notebooks layering an audio-reactive control system on top of Disco Diffusion v5.2
Warp (CLIP-guided diffusion animation). Audio analysis (offline, whole-song) generates
per-frame parameter curves that drive the diffusion animation's camera/warp parameters
(zoom, rotation, translation — the Chigozie-style keyframe system). Repo contents: 4
notebook variants (v5.2 Warp, custom-model, v5.1 turbo prerelease) + `image_morphing_3d.ipynb`.

## The audio pipeline (non-ML part — the transferable piece)

Stack: pydub (+ `pydub.scipy_effects` filters) + librosa + numpy.

1. **Multiband split.** The song is filtered into named bands, each becoming a control
   signal (`rangeLookup`): Full, **Sub Bass** (lowpass), **Bass** (bp1), **Lower
   Midrange** (bp2), **Midrange** (bp3), **Upper Midrange** (bp4), **Presence** (bp5),
   Brilliance, **Ceiling** (highpass).
2. **Synthetic LFO sources sit beside audio sources**: `3StepLFO`
   (`a + sin(t·2π·c)^9·0.5` — a stepped pulse), `Sine`, `Saw`, `Square`, all
   time-parameterized with a/b/c rate params. Any visual parameter can ride either an
   audio band **or** a deterministic LFO through the same code path.
3. **Conditioning per signal** (`audioBandpassDataFetch`):
   - `smoothing(data, kernel_size)` — box-kernel convolution (moving average),
     kernel size user-chosen per mapping;
   - `normAndScale(data, mult)` — min-max normalize then map to **[-mult, +mult]**
     (bipolar, for rotation/translation) or `normAndScalePositive` → [0, mult]
     (unipolar, for zoom/strength);
   - `panner(data, r)` — stereo balance weighting before mono mixdown.
4. **Mapping**: each animation parameter (zoom, angle/rotation, translation_x/y, etc.)
   is assigned a source band + smoothing + multiplier, and the resulting curve is baked
   into the keyframe schedule the diffusion animation consumes.

## What carries forward into the real-time C++ visualizer

- **"Every parameter picks a source + smoothing + gain"** — identical to the ZGE
  modulation-routing conclusion, independently validated by this prior project.
  Our per-link model should be: source (band N / RMS / waveform / LFO) → smoothing
  window (or attack/decay) → bipolar-or-unipolar range map → parameter.
- **More than 3 bands.** MilkDrop's bass/mid/treb is the floor; AudioRider's 7-band
  split (sub-bass … ceiling) matches our existing `AudioBand` enum (SubBass, Bass,
  MidLow, MidHigh, High, Treble) — keep the finer bands as routable sources.
- **LFOs as first-class modulation sources** alongside audio — cheap to add
  (the engine even has table-driven oscillators in material expressions already) and
  they rescue quiet passages where pure audio-reactivity goes static.
- **Bipolar vs unipolar mapping matters**: rotation/translation want signed ranges
  centered at 0; zoom/brightness want positive ranges. Bake the distinction into the
  routing UI as a per-link toggle, as AudioRider did with its two norm functions.
- **Normalization over the whole song** worked offline; real-time must replace it with
  running-average normalization (the MilkDrop approach in
  docs/research-milkdrop-projectm.md §5) — same intent, causal implementation.
- **Difference from then:** AudioRider was offline keyframe baking at ~frames-per-song
  scale; the visualizer is real-time at 60 fps. The mapping design transfers; the
  analysis must move from librosa/pydub to our in-engine FFT + envelope followers.

---

## Deep-dive addendum (full notebook analysis)

**Pipeline confirmed end-to-end** (offline keyframe baking on Disco Diffusion v5.2 Warp):
mel-spectrogram of the track's opening seeds `init_image`; 7 pydub bands (breakpoints:
60 / 250 / 500 / 2000 / 4000 / 6000 / 17000 / 20000 Hz) + LFOs → per-frame bucket means
→ smoothing → norm/scale → Disco keyframe strings ("0:(v0), 1:(v1), ...");
librosa beats/onsets → prompt-swap frame indices. AudioRider never modifies Disco's
core — audio is just another author of the same keyframe curves.

**Default band→parameter bindings** (the tuned "patch" worth mirroring):
| param | source | mult | smoothing kernel |
|---|---|---|---|
| angle (2D) | Sub Bass | 3 | 20 |
| zoom (2D) | Bass | 0.5 | 20 |
| translation_x | Sub Bass | 6 | 40 |
| translation_y | Lower Mid | 6 | 60 |
| translation_z | 3-step LFO × Bass | 7 | 60 |
| rotation_3d_x | Lower Mid | 0.5 | 40 |
| rotation_3d_y | Midrange | 1 | 20 |
| rotation_3d_z | Presence | 1 | 20 |
| blooming FX | Bass | 1 | 20 |
| cut schedule | Full | 24 | 120 |
Pattern: slow motions get long smoothing (inertia), reactive ones short — port as
per-parameter one-pole IIR time constants (box convolution needs lookahead; use
asymmetric attack/release envelope followers instead, which is what MilkDrop's `_att`
approximates).

**The highest-value trick — cross-modulation**: `translation_z = LFO × (bass + 1)`.
Steady base motion whose amplitude is modulated by a transient band; every kick punches
without motion ever stalling. Implement as a per-link "×(1 + k·band)" modifier.

**Beat lane**: four librosa detectors (beat_track, PLP, Superflux w/ HPSS, and
onset_backtrack-to-RMS-minimum — snap triggers to the true attack). Real-time
equivalent: adaptive spectral-flux threshold + refractory period, with an energy-minimum
lookback so flashes land on transients. Keep the two-lane split: continuous envelopes
drive motion; discrete beat events drive palette/preset/shape changes.

**Also carry forward**: louder-passages-get-more-detail (his cut_overview scheduling) —
maps to e.g. particle counts/mesh density scaling with loudness; live scrolling
spectrogram as a feedback-warp texture. **Don't carry**: box smoothing, pydub filter
cascades (use FFT band sums), keyframe-string emission.
