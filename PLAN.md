# Project Plan: Sound Reactive Visualizer (DOOM 3 BFG engine)

## Objective
Build a MilkDrop/projectM-style sound-reactive visualizer on the DOOM 3 BFG engine:
audio input from files (WAV/OGG/more), playlists, or WASAPI device loopback; a UI to pick
song/playlist/device and preset; shader-driven visual presets.

Reference docs (research + audits live in `docs/`):
- `docs/audit-existing-visualizer-code.md` — line-by-line audit of the experimental code
- `docs/engine-map.md` — full engine subsystem map + visualizer integration seams
- `docs/research-milkdrop-projectm.md` — real .milk format, render pipeline, projectM API
- `docs/research-zge-visualizer.md` — FL Studio ZGE Visualizer layer/knob model
- `plans/visualizer-ui.md` — UI design (song/playlist/device/preset pickers)
- `plans/implement-fft-audio-analyzer.md`, `plans/partitioned-coalescing-mountain.md`

## Architecture decisions (settled by research)

1. **Audio tap = final mix, not per-voice buffers.** Tap the XAudio2 master/submix voice
   (effect chain, like the stock VU meter) instead of reading sample buffers with play
   cursors — works for every format, no engine-accessor additions needed.
2. **Audio features**: 1024-pt Hann FFT per frame → bass/mid/treb band energies
   normalized by long-run averages + attenuated (`_att`) versions + RMS + beat detector
   (short/long bass energy ratio) — the MilkDrop contract.
3. **Feature → visual bridges (all already in the engine)**:
   `gameLocal.globalShaderParms` → GLOBAL0..7 material registers;
   per-entity `SetShaderParm`; light `referenceSound` amplitude;
   `soundmap` material stages via `idSoundSystem::ImageForTime` (stubbed — implement to
   get a live waveform/spectrum texture); GLSL `newShaderStage_t` program stages.
4. **Visualizer render pipeline**: MilkDrop minimal pass set — ping-pong RGBA16F feedback
   canvas (warp pass w/ decay), waveform/shape overlay, optional blur chain, composite
   pass — injected at the renderer's `RC_POST_PROCESS` seam (or a sibling `RC_VISUALIZER`
   command). Fullscreen tris helper already exists (`backEnd.unitSquareSurface`).
5. **Preset format**: keep the simple custom format short-term but RENAME it (`.dviz`) —
   it is not MilkDrop-compatible. For real `.milk` support, vendor **projectm-eval**
   (EEL2-compatible C library) and bind q-vars/audio vars by pointer; equation-only
   presets first, HLSL preset-shader translation later (phase 2+).
6. **Effect/content model (from ZGE)**: effects are data files in a folder (manifest:
   params + ranges + audio-bindable flags, plus GLSL), never compiled-in; params
   routable from audio buses (peak/band/waveform) with per-link smoothing.
7. **UI**: ImGui debug panel first (RBDOOM pre-1.5 port), then legacy `.gui` +
   `idListGUI`/`listDef` picker screens, SWF shell screen later.
   See `plans/visualizer-ui.md`.
8. **Build hygiene**: experimental sources stay OUT of vcxproj until each compiles;
   add one at a time against the verified Debug|x64 baseline.
9. **Frame timing**: `soundSystem->Render()` runs LAST in `idCommonLocal::Frame()`
   (after visuals are committed) — the analyzer runs early in Frame() on the previous
   frame's mix; one-frame latency is the design, not a bug.
10. **Additional engine seams discovered** (docs/engine-map.md synthesis):
    `FullscreenFXManager` (new FullscreenFX subclass = sanctioned post-process);
    `g_testPostProcess` cvar (zero-code fullscreen material prototyping);
    `SS_POST_PROCESS` materials with `RENDERPARM_USER` uniforms (data-driven shader
    effects with no C++); `idDeclTable` + a small runtime setter (audio→every
    material/particle); orphaned non-stock `LoadShaderFromSource` (RenderProgs.cpp:413)
    to finish for runtime preset-shader compiles; `idDebugGraph` for instant spectrum
    bars; master-voice tap also captures DOOM-classic audio (shared IXAudio2).

## Phases

### Phase 1: Exploration & Analysis — DONE
- [x] Rendering pipeline analysis; integration points identified (RC_POST_PROCESS,
      GUI-on-surface, globalShaderParms, soundmap)
- [x] MilkDrop/projectM principles researched in depth
- [x] UI systems analyzed (legacy idWindow vs SWF menus)
- [x] XAudio2/WASAPI audit; audio subsystem confirmed stock
- [x] Full audit of experimental code (docs/audit-existing-visualizer-code.md)

### Phase 2: Core Audio Engine
- [x] Repair compile blockers in analyzer/capture/playlist/decoders (audit doc §repair order)
- [x] Correct Cooley-Tukey FFT (bit-reversal + butterfly) + sine-wave unit test (PASSED,
      verified against naive DFT)
- [x] Final-mix samples via WASAPI loopback of the default render device (supersedes the
      XAudio2 XAPO tap — loopback sees the engine's own output plus everything else)
- [x] Hook `AudioAnalyzer::Update()` into `idSoundSystemLocal::Render()`
- [x] Feature extraction: bass/mid/treb (+_att), RMS, 7 bands, beat detect
- [x] WASAPI loopback capture rewritten per real API contract ("Capture Mode")
- [x] OGG via vendored stb_vorbis; FFmpeg optional (excluded until dev libs present)
- [x] `idVisualizerManager` + vis_* console commands (playlists, transport, source switch)
- [ ] **Windows build gate + runtime smoke test (first step on the Windows machine)**
- [ ] Publish features → `globalShaderParms` + implement `ImageForTime` waveform/spectrum texture

### Phase 3: UI & Control (plans/visualizer-ui.md)
- [ ] `idVisualizerManager` + console commands (vis_play/vis_preset/vis_source)
- [ ] Playlist manager repair + `.m3u` parsing; music discovery under `base/music/`
- [ ] `guis/visualizer.gui` with track/preset lists, source toggle, transport controls
- [ ] Device selection (engine output vs system loopback; per-device later)

### Phase 4: Shader-Based Visuals
- [ ] Feedback canvas (ping-pong RGBA16F) + warp/decay pass at RC_POST_PROCESS
- [ ] Waveform + spectrum-bar overlay passes (Polar-style radial spectrum as flagship)
- [ ] Composite pass (gamma/echo); preset-driven GLSL effect files (.dviz manifest)
- [ ] projectm-eval vendored → per_frame/per_vertex equation support → real .milk subset
- [ ] Preset transitions (hard cut on beat; soft blend later)

### Phase 5: Integration / Standalone
- [ ] Option A: visualizer mode inside the game exe (console command entry) — default
- [ ] Option B: strip session/game for a standalone player (see engine-map sys notes)
- [ ] RBDOOM-3-BFG cherry-picks as needed (docs/research-rbdoom.md when complete)

## Technical stack
C++ / MSVC (VS2010-era solution, x64 verified) · OpenGL (stock backend) · XAudio2 2.7 +
WASAPI loopback · stb_vorbis (+ optional FFmpeg) · projectm-eval (planned vendored) ·
legacy idWindow GUI now, SWF shell later.
