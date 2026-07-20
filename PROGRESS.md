# Progress Tracker

## Completed
- [x] Project planning & initialization
- [x] Initial codebase scan (audio & rendering)
- [x] Audio subsystem audit (XAudio2, mixing, hooks)
- [x] Verified Debug|x64 baseline build (stock engine; experimental files excluded from projects)
- [x] Full audit of all experimental visualizer code — see `docs/audit-existing-visualizer-code.md`
  (every new file reviewed line-by-line; compile blockers and API errors cataloged)
- [x] Full-source codebase learning pass — `docs/engine-map.md` COMPLETE: all subsystems
      mapped (sound, renderer frontend/backend/resources, framework, idlib, d3xp
      core/world/entities/player/physics/ai/script/menus, ui, swf, sys, cm/aas,
      doomclassic/timidity) + a synthesis section mapping the visualizer onto the engine
- [x] External research docs: `docs/research-rbdoom.md` (modernization inventory +
      adoption order), `docs/research-zge-visualizer.md` (layer/knob model, 17 effect
      categories), `docs/research-milkdrop-projectm.md` (.milk format, pass pipeline,
      projectm-eval plan), `docs/research-audiorider.md` (band→parameter mapping lessons)
- [x] UI design plan: `plans/visualizer-ui.md` (song/playlist/device/preset pickers on
      the legacy .gui system; SWF shell integration later)
- [x] PLAN.md rewritten around settled architecture decisions

## Completed (repair pass, July 2026 — pending Windows build verification)
- [x] `audio_analyzer` rewritten: correct radix-2 FFT (bit-reversal + proper butterfly,
      **verified against a naive DFT + sine sweeps in a standalone test — all passed**),
      Hann window, sample-push ring architecture (no nonexistent engine accessors),
      7-band split + MilkDrop bass/mid/treb + `_att` envelopes with running-average
      normalization, RMS, beat detection with refractory period
- [x] `AudioCapture_WASAPI` rewritten per the real WASAPI loopback contract (device mix
      format, GetNextPacketSize packet loop, silence flag, float + 16-bit paths, pushes
      mono into the analyzer). Loopback of the default render device captures the full
      system mix — including engine audio — so no XAudio2 XAPO tap is needed
- [x] Analyzer + manager hooked into the frame loop (`idSoundSystemLocal::Render`, `// VIS`)
- [x] `idPlaylistManager` fixed (value semantics) + real `.m3u/.m3u8` parsing with
      `#EXTINF` titles, via idFileSystem
- [x] New `idVisualizerManager` (`neo/sound/visualizer_manager.*`) with console commands:
      `vis_play`, `vis_stop`, `vis_next`, `vis_prev`, `vis_source engine|loopback`,
      `vis_status` (prints live band levels), `listMusic`; auto-advance cvar
- [x] `MilkDropParser` routed through idFileSystem (fixed includes)
- [x] Decoders: stb_vorbis v1.22 vendored (`neo/external/`, own TU, no PCH);
      `idOggDecoder` uses the real API from memory; `idFFmpegDecoder` corrected
      (nb_channels, swr_* names/signature, frame math, EOF drain)
- [x] `XA2_SoundSample` fixes: `.ogg` extension bug, float→int16 conversion count
- [x] `visualizer.cpp` bar mapping fixed (was sampling past bin 511)
- [x] Repaired sound files + stb_vorbis added to `doomexe.vcxproj` (WASAPI + stb TUs
      marked NotUsing PCH); FFmpeg decoder + VisualizerUI/MilkDropParser still excluded
- [x] Deleted dead `temp_file.cpp`; build logs gitignored

## Next up (needs the Windows machine)
- [ ] **Build gate**: compile Debug|x64 with the new files; fix any MSVC-specific fallout
      (requires a C++11-thread-capable toolset, v140+, for std::thread/mutex/atomic)
- [ ] Runtime check: `vis_play music/<file>.wav` + `vis_status` shows moving band values;
      `vis_source loopback` reacts to any system audio
- [ ] Publish features → `globalShaderParms` (game-side hook) + `idDebugGraph` spectrum bars
- [ ] First visual: `g_testPostProcess` material driven by parms (see docs/engine-map.md)

## Backlog
- [ ] Preset format decision: keep custom format (rename from `.milk`) vs real MilkDrop compatibility
- [ ] Shader-based visuals (post-processing hook, feedback texture, warp/composite passes)
- [ ] Modern audio format support end-to-end (OGG first, FFmpeg optional)
- [ ] Integration vs standalone build decision (Phase 5)

## Known-broken / quarantined
- `temp_file.cpp` (repo root) — dead scratch copy of XA2_SoundSample.cpp; delete
- `neo/renderer/RenderBackend.h` — syntactically invalid; keep excluded until redesigned

## FIRST VISUAL WORKING (July 15) 🎉
- [x] Live audio-reactive spectrum overlay rendering in-engine with ZERO retail game
      data: N log-spaced bands (vis_bands 3/5/7/9), rainbow red→blue, auto-gained bar
      heights driven by WASAPI-loopback FFT. Confirmed on screen.
- Full chain proven end to end: WASAPI capture → FFT → log bands → global auto-gain →
  `idVisualizerManager::Draw2D` (hooked in `idCommonLocal::Draw`) → GUI 2D render.
- Key fixes that unblocked rendering (all engine/dataless-boot issues, not analysis):
  - x64 boot bugs: `CPUCount` infinite loop, `Sys_ListFiles` `_findfirst` handle
    truncation, `Sys_DLL_Load`/`gameDLL` int-vs-uintptr, PlatformToolset placement.
  - Dataless boot assets (font/charset/_default) so the engine reaches a render state.
  - idlib clean-build PCH disabled (x64) — clean rebuilds were broken.
  - **Render bug 1**: `DrawFilled` uses `_white`, which has no `blend` stage and isn't
    drawn by the 2D overlay pass. Fixed by drawing every rect through a proper blend
    material (`visualizer/solid`, a real white TGA — the intrinsic `_white` image crashed).
  - **Render bug 2 (the crash)**: `declManager->FindMaterial` was called from `Draw2D`
    on the SMP draw-worker thread (decl lookups are main-thread-only → NULL-call crash).
    Now resolved once in `Frame()` on the main thread and only the cached pointer is used.
- Build/infra note: incremental builds here silently skip recompiles; force-delete the
  changed .obj before building. Local Mac repo access (TCC on ~/Documents) was lost and
  restored mid-session — that caused earlier "pushes not landing" confusion.

## Feedback trail + audio routing (July 15, morning) 🎉
- [x] **#2 MilkDrop feedback/warp trail** (`vis_feedback 1`): captures the composited
      frame each tick (`CaptureRenderToImage("visualizer/feedbackrt")`), redraws it the
      next frame zoomed + rotated about center + dimmed, so effects leave warping,
      spinning, fading trails. Tunables `vis_feedbackDecay`, `vis_feedbackZoom`. Opt-in.
      Overlay draws before the console so the capture excludes console text.
- [x] **Console-artifact fix**: `tr.whiteMaterial` resolved to intrinsic `_white` (no
      `blend` stage) so every `DrawFilled()` — including the console's own background —
      rendered nothing, leaving console text floating over the live visualizer. Now points
      at `visualizer/solid` in dataless boot (falls back to `_white`). Console is a clean
      translucent panel; confirmed on screen.
- [x] **#3 Audio→visual parameter routing** (ZGE/AudioRider knob model): a modulation
      matrix maps each visual TARGET (scale/hue/bright/zoom/rotate) to one audio SOURCE
      (bass/mid/treb + attack envelopes, rms, beat, level, LFO) via `value = base +
      source*amount`. Routes evaluated once/frame on the MAIN thread (analyzer just
      updated) and cached in `s_mod[]`/`s_rotAngle` for the draw worker. All three effects
      + the feedback loop consume the cached values; feedback spins (rotatable frame draw)
      and its zoom is bass-pushed. Musical defaults out of the box. Console:
      `vis_routes` (list), `vis_route <target> <source> <amount> [base]` (re-patch live),
      `vis_mod` (master toggle), `vis_lfoPeriod`.

## #1 auto-arm + #4 picker UI (July 15, morning) 🎉 — all 4 steps done
- [x] **#1 vis_play "just works"**: nothing fed the analyzer in ENGINE mode, so
      reactivity needed a manual `vis_source loopback`. The manager now brings WASAPI
      loopback up once at boot (throttled retry until the device is ready) and then leaves
      the source mode alone. Loopback captures the full system mix — engine `vis_play`
      tracks AND any browser/other audio — so it all reacts with zero commands. Opt-out
      `vis_autoLoopback 0`.
- [x] **#4 on-screen picker overlay**: immediate-mode, keyboard-driven menu built from the
      proven primitives (`VisFillRect` + `DrawSmallStringExt`) instead of the `.gui`/SWF
      stack (which is unreliable in dataless boot). Tabs: **Music** + **Playlists** scanned
      from `base/music` and `base/playlists` on open; **Effects** toggles effect / feedback
      / fullscreen / bands / routing live. Input hooked in `idCommonLocal::ProcessEvent`
      after the console (so `~` still works) and before the no-map console-force fallback;
      runs on the MAIN thread so file listing is safe. `F1` or `vis_menu` toggles; arrows
      navigate; Enter activates; Esc closes. Draws on top of the effect and is excluded
      from the feedback capture (no smear).

## Presets + device picker + warp mesh (July 15, late morning) 🎉
- [x] **Saveable presets**: a preset is a runnable `.cfg` (`seta` cvars + `vis_route`
      lines) written to `fs_savepath/presets` and loaded via `exec` — reuses the console,
      no new parser. New **PRESETS** tab lists `base/presets` + user-saved presets, loads on
      Enter. Console: `vis_presetSave <name>`, `vis_presetLoad <name>`, `vis_presets`. Ships
      four starter presets (classic / tunnel / bloom / strobe) from the boot-asset script.
- [x] **Device-input picker**: enumerates active WASAPI render (loopback) + capture
      (mic/line-in) endpoints; user picks which drives the visualizer.
      `AudioCapture_WASAPI::Initialize` takes a device id + isCapture (render→loopback flag,
      capture→direct); `EnumerateDevices` returns UTF-8 names/ids (`PKEY_Device_FriendlyName`
      defined inline to dodge GUID link issues). `AudioAnalyzer::SetCaptureDevice` rebuilds
      capture on the chosen endpoint. New **DEVICES** tab (system-default + all endpoints,
      active marked `*`); console `vis_devices` / `vis_device <n>`.
- [x] **MilkDrop-style warp mesh** on the feedback pass (`vis_warp`): a 24×18 grid whose
      per-vertex texcoords follow inverse-zoom + rotation + a radial ripple that flows over
      time and swells with bass, sampled through the existing textured-blend GUI shader (GPU
      bilinearly interpolates each cell). Deliberately a warp mesh, NOT a custom fragment
      shader — the dataless renderprog path can't safely take a new builtin shader (remote
      `base/renderprogs` is empty; GUI shaders are compiled into the exe). Cvars
      `vis_warp/vis_warpAmount/vis_warpFreq/vis_warpSpeed`, Effects-tab toggle, preset
      fields, and the `tunnel` preset enables it.

## Visual-effects roadmap — all shipped & confirmed (July 15, midday) 🎉
See `plans/visual-effects-roadmap.md` for the full plan + runtime reference.
- [x] **Beat-synced preset cycling**: `vis_presetCycle` rotates presets every
      `vis_presetCycleSecs`, holding for the next beat (`vis_presetCycleOnBeat`),
      optional shuffle. Effects-tab toggle; pauses while the picker is open.
- [x] **New effects**: `vis_effect 3` waveform ring (spinning), `vis_effect 4`
      particles (beat-spawned, bass-driven, fading). In the Effects tab + cycle.
- [x] **Hi-res warp mesh + modes**: `vis_warp` 0/1/2 (off/mesh/hi-res 48x36),
      `vis_warpMode` ripple/swirl/tunnel/fisheye.
- [x] **True fragment-shader warp** (`vis_warp 3`): custom GLSL program
      (`visualizer_warp.vertex/.pixel`) bound via the `visualizer/warp` material;
      **confirmed the 2D overlay path honors material custom programs.** Animated +
      audio-reactive by passing warp params through the vertex color (no render-parm
      plumbing). Falls back to the hi-res mesh if the program fails to load.
- [x] **WAV filenames with spaces**: `idSoundShader::SetDefaultText` now quotes the
      implicit sample path so spaced names parse as one token (general engine fix).
- Confirmed on the Windows host: all effects, warp modes, and the spaces fix work.

## Boot-blocker fixes (July 14, evening)
- [x] **CPUCount infinite loop** (`win_cpu.cpp`): affinity-mask shift was outside the
      while loop (x64-port brace slip) — every launch ever hung at 100% core before
      any window. Found via the new `early_console.log` tee in `Conbuf_AppendText`.
- [x] **Sys_ListFiles handle truncation** (`win_main.cpp`): `_findfirst` returns
      intptr_t on x64; storing in `int` corrupted the handle for `_findnext` —
      intermittent infinite loops on any directory listing (hit in decl init).
- [x] **Dataless boot assets generated** (`scripts/generate_boot_assets.py`):
      `textures/bigchars.tga` (console text), `newfonts/Arial_Narrow/` (default idFont,
      kills the missing-font FatalError), `materials/visualizer_boot.mtr` — engine now
      boots to GL init with zero retail game data (verified headless to GL/input init).
