# Visual Effects Roadmap

Live plan for pushing the visualizer's visuals further, plus a consolidated
runtime reference for everything shipped this session. **This file is updated as
each item lands** — check the status boxes and the "Status" line under each
feature.

Related docs: **`plans/PRD-zge-milkdrop-visualizer.md` (the north-star product
spec — ZGE-style layer/knob GUI + MilkDrop2/3/projectM preset loading + MD3
hotkeys)**, `PROGRESS.md` (chronological build log), `plans/visualizer-ui.md`
(original UI plan), `docs/research-milkdrop-projectm.md`,
`docs/research-zge-visualizer.md`, `docs/research-audiorider.md`.

---

## Upcoming features

### 1. Beat-synced preset auto-switching
**Status: DONE (shipped).** `vis_presetCycle` rotates through `base/presets`
every `vis_presetCycleSecs` (default 30). `vis_presetCycleOnBeat` (default 1)
holds the switch until the next detected beat so it lands musically.
`vis_presetCycleShuffle` randomizes order via a seeded Fisher-Yates. Cycling
pauses while the picker is open. Effects tab has a "Preset cycle" toggle.
Implemented in `Frame()` (main thread) + `BuildCycleList`/`AdvancePreset`.

MilkDrop rotates through a preset playlist on a timer with optional beat-aware
blending. We already have presets as `.cfg` files (see reference below) and beat
detection in the analyzer, so this is mostly orchestration.

Design:
- New cvars: `vis_presetCycle` (bool, default 0), `vis_presetCycleSecs` (float,
  seconds between switches, default 30), `vis_presetCycleOnBeat` (bool — when on,
  the switch waits for the next detected beat after the timer elapses so changes
  land musically).
- `idVisualizerManager` scans `base/presets` into a cycle list at cycle-enable
  time (reuse the menu's `VisAppendFiles`). Keeps an index + next-switch timestamp.
- In `Frame()` (main thread): when cycling is on and `Sys_Milliseconds()` passes
  the next-switch time, and (if `vis_presetCycleOnBeat`) `GetBeat()` is true,
  advance to the next preset via the existing `LoadPresetPath` and reschedule.
- Order: sequential by default; `vis_presetCycleShuffle` for a shuffled order
  (index permutation regenerated when the list is (re)scanned — no RNG in the
  hot path; permute once with a simple LCG seeded from the file count + a cvar).
- Console: `vis_presetCycle`, and reuse `vis_presets` to show the list. Effects
  tab in the picker gets a "Preset cycle: ON/off" row.
- Guard: skip cycling while the picker menu is open (don't yank the UI around).

Risks/notes: `exec` is asynchronous (buffered command), so a switch takes effect
a frame or two later — fine for a 30s cadence. Avoid switching more than ~once/sec.

### 2. More effect types
**Status: partially DONE.** `vis_effect` now 0–4.
- **3 — Waveform ring (DONE)**: the scope bent into a spinning circle (radius =
  base + waveform sample), rotates with `s_rotAngle`, hue cycles. Great with warp.
- **4 — Particles (DONE)**: up to 300 dots spawned in bursts of 40 on each beat,
  pushed outward with speed scaled by bass, drag + fade over a randomized life.
  State in a static array; integrated once/frame in `DrawEffectParticles` (uses a
  file-static LCG for spawn randomness — no `Math::random`).
- **5 — Spectrogram (DONE)**: `vis_effect 5`. Scrolling time(x)×frequency(y)
  heatmap, 80×32, log-spaced bins (~40 Hz–16 kHz) from `GetSmoothedMagnitude` with
  a per-row peak-follower for auto-gain. Newest column on the right; blue→red heat
  ramp by intensity.
- Each effect: `vis_effect` range, `vis_nextEffect` names, Effects-tab labels,
  and a `DrawEffectX()`.

Risk seen: keep per-frame draw-call counts sane (particles capped at 300 live).

### 3. Fragment-shader warp (true per-pixel feedback)
**Status: mesh upgrade DONE; true shader = active spike.**

Decided (with user) to ship the safe mesh upgrade first, then spike the shader.
- **DONE — hi-res mesh + modes**: `vis_warp` is now 0 off / 1 mesh (24×18) /
  2 hi-res (48×36). `vis_warpMode` 0 ripple / 1 swirl / 2 tunnel / 3 fisheye.
  Effects tab cycles both. Near-per-pixel look at hi-res, zero render-path risk.
- **Findings for the true shader**: renderprogs load from git-tracked
  `.vertex`/`.pixel` files (deploy to remote ✓); material stages support custom
  `program`/`fragmentProgram`/`fragmentMap` (`Material.cpp` ~L1542). Shaders are
  HLSL-dialect with `#include "global.inc"`, translated to GLSL at load.
- **Unverified (each costs a build+visual round-trip)**: (a) does the 2D guiModel
  overlay path honor a material's custom program or force `BindShader_GUI`?
  (b) how to feed time-varying audio uniforms into the 2D path.
- **DONE — true fragment-shader warp confirmed working (`vis_warp 3`).** The boot
  log proved the shader compiled, translated to GLSL, and linked (`GLSL program N
  ... visualizer_warp`; only a harmless `gl_FragColor` reserved-name warning), and
  the feedback rendered distorted → **the 2D overlay path DOES honor a material's
  custom program.** `visualizer_warp.vertex`/`.pixel` + `visualizer/warp` material
  (`vertexProgram`/`fragmentProgram`/`fragmentMap 0 visualizer/feedbackrt`).
  Animation + audio without render-parm plumbing: the warp params ride in on the
  **vertex color** (`SetColor` → `gl_Color`; `swizzleColor` is identity on this GL
  build, so channels are preserved): `r`=amount, `g`=phase(0..1), `b`=mode(0..1),
  `a`=dim. Pixel shader does ripple/swirl/tunnel/fisheye. `FindMaterial(...,false)`
  + null-check falls back to the hi-res mesh, so a load failure can't break the
  overlay.
- **DONE — richer shader (blur + chroma-split)**: the pixel shader now samples the
  feedback with a 6-tap kernel (2 taps/channel) — a tangential blur for a soft glow
  plus an RGB chroma-split (red pushed radially outward, blue inward). Both spreads
  derive from the warp `amount` and collapse to a clean single sample at amount~0.

#### Original notes

The current warp is a 24×18 mesh (see reference). A true MilkDrop per-pixel warp
runs a fragment shader over the feedback texture with per-pixel warp equations.
In idTech BFG this means adding a **builtin renderprog**:
- Investigate `neo/renderer/RenderProgs.cpp` + `RenderProgs_GLSL.cpp`: the
  BUILTIN_* enum, the `builtins[]` init table, and how a program is bound for a
  material stage. GUI shaders are compiled into the exe (remote `base/renderprogs`
  is empty), so the new program must be registered there, not shipped as a file.
- Add a `BUILTIN_VIS_WARP` program: vertex passthrough (2D screen quad), fragment
  samples the feedback image with warp math (radial zoom + rotation + ripple,
  driven by uniforms fed from `globalShaderParms`/`RENDERPARM_*`).
- Bind it via a dedicated material (`visualizer/warp`) whose stage uses the new
  program; draw one fullscreen quad instead of the mesh when
  `vis_warp 2` (mesh = 1, shader = 2).
- Feed audio params (bass, phase, zoom, rot) through render parms the shader reads.

Why last: registering a builtin renderprog couples to the shader-generation path
that has been the most fragile part of dataless boot. Do it in isolation, keep the
mesh warp as the safe fallback (`vis_warp 1`), and be ready to revert cleanly.

---

## Current runtime reference (shipped this session)

Everything below already works in the build. This is the consolidated reference
that was previously only scattered across commits/PROGRESS.

### The picker overlay (#4)
Immediate-mode, keyboard-driven, drawn from `idVisualizerManager::MenuDraw2D`;
input hooked in `idCommonLocal::ProcessEvent` (after the console, before the
no-map console-force). Toggle with **F1** or `vis_menu`.
- Tabs: **MUSIC**, **PLAYLISTS**, **PRESETS**, **DEVICES**, **DISPLAY**, **EFFECTS**.
- DISPLAY: fullscreen-per-monitor (native) + windowed presets; `vis_displays`,
  `vis_display <n>`, `vis_resolution <w> <h>` (apply via cvars + `vid_restart`).
- Controls: ↑/↓ select · ←/→ (or Tab) change tab · Enter activate · Esc/F1 close.
- MUSIC/PLAYLISTS scan `base/music` (`.wav`/`.ogg`) and `base/playlists`
  (`.m3u`/`.m3u8`) on open. EFFECTS toggles live state. Runs on the main thread
  (file listing / decl access safe); draws on top of the effect and is excluded
  from the feedback capture.

### Audio → visual routing (#3, ZGE/AudioRider knob model)
`value = base + source * amount`, evaluated once/frame on the main thread, cached
in `s_mod[]`/`s_rotAngle` for the draw worker.
- **Targets**: `scale` (bar/spoke/amp size), `hue` (color shift), `bright`
  (overlay alpha), `zoom` (feedback zoom add), `rotate` (spin velocity, rad/s).
- **Sources**: `none bass mid treb bassatt midatt trebatt rms beat level lfo`.
- Console: `vis_routes` (list), `vis_route <target> <source> <amount> [base]`.
- `vis_mod` master toggle; `vis_lfoPeriod` LFO seconds/cycle.

### Feedback trail + warp (#2)
- `vis_feedback` — capture prev frame, redraw zoomed/rotated/dimmed, recapture.
- `vis_feedbackDecay` (trail length, 0.5–0.995), `vis_feedbackZoom` (0.95–1.1).
- `vis_warp` 0 off / 1 mesh (24×18) / 2 hi-res (48×36) warp mesh; implies feedback.
  `vis_warpMode` 0 ripple / 1 swirl / 2 tunnel / 3 fisheye. `vis_warpAmount`,
  `vis_warpFreq`, `vis_warpSpeed`. Ripple depth swells with bass. Mesh, not a
  fragment shader yet (see roadmap item 3).
- Feedback material `visualizer/feedbackrt` (render target) + `visualizer/feedback`.

### Effects & spectrum
- `vis_effect` 0 bars / 1 radial / 2 scope / 3 waveform ring / 4 particles /
  5 spectrogram; `vis_nextEffect` cycles.
- `vis_bands` 3/5/7/9 (log-spaced, auto-gained); `vis_fullscreen`; `vis_show`.
- `vis_kaleido` 0 off / 2–16 mirrored wedges: folds the warp-mesh feedback into
  radial symmetry (mandala). Needs `vis_warp` 1/2 on. Effects-tab "Kaleidoscope".
- `vis_palette` 0 rainbow / 1 fire / 2 ocean / 3 synthwave / 4 mono +
  `vis_hueShiftGlobal`: global recolor of the whole visualizer (remapped in
  `HueColor`). Effects-tab "Palette".
- `vis_effect 6` starfield: stars stream outward from center, bass-driven speed,
  beat streaks (aspect-corrected).
- `vis_bloomBeat`: routes bass-attack into the bloom intensity so the glow throbs.
- `vis_bloom` (0 off): soft neon glow post-process over the whole visualizer
  (capture → bright-pass + 13-tap blur → additive), `vis_bloomThreshold`,
  `vis_bloomRadius`. Custom `visualizer/bloom` shader; Effects-tab "Bloom / glow".
- `vis_hud` (default 1): bottom-left status line (effect/bands/fb/warp/cycle +
  now-playing) and a fading banner on preset/track change. Hidden under the menu,
  drawn after the feedback capture so it never smears.

### Preset auto-cycling
- `vis_presetCycle` (auto-rotate through `base/presets`), `vis_presetCycleSecs`
  (interval), `vis_presetCycleOnBeat` (switch on the next beat after the timer),
  `vis_presetCycleShuffle` (random order). Effects-tab "Preset cycle" toggle.
  Pauses while the picker is open.

### Presets
- A preset is a runnable `.cfg` (`seta` cvars + `vis_route` lines) under
  `base/presets` (shipped) or `fs_savepath/presets` (user-saved).
- Console: `vis_presetSave <name>`, `vis_presetLoad <name>`, `vis_presets`.
- Starter presets (12): `classic`, `tunnel`, `bloom`, `strobe`, plus `nebula`,
  `kaleidoscope`, `signal`, `ribbon`, `pulse`, `aurora`, `vortex`, `glass`.
  Generated by `scripts/generate_boot_assets.py`.
- **Capture current**: PRESETS-tab row 0 "[+ save current as new preset]" writes
  the next free `custom-NN.cfg` and rescans. (`vis_presetSave <name>` still works.)
- Serialized fields: `vis_effect/bands/fullscreen/feedback/feedbackDecay/
  feedbackZoom/warp/warpAmount/warpFreq/warpSpeed/mod/lfoPeriod` + all 5 routes.

### Audio source & devices (#1 + device picker)
- **#1 auto-arm**: `vis_autoLoopback` (default 1) brings WASAPI loopback up at
  boot (throttled retry until the device is ready), then leaves source control to
  the user. Any system audio (engine `vis_play`, browser, etc.) drives the visuals
  with no commands.
- `vis_source engine|loopback` — manual source mode.
- `vis_backgroundAudio` (default 1): keep audio + reactivity running when the
  window loses focus (suppresses the engine's deactivate-mute) so the visualizer
  works in the background / while screenshotting.
- **Device picker**: `vis_devices` (list render/loopback + capture endpoints),
  `vis_device <n>` (0 = system default). DEVICES tab in the picker. Endpoints via
  `AudioCapture_WASAPI::EnumerateDevices`; selection via
  `AudioAnalyzer::SetCaptureDevice(id, isCapture)`.

### Playback
- `vis_play <music/file | playlists/list.m3u>`, `vis_stop`, `vis_next`,
  `vis_prev`, `vis_autoAdvance`, `listMusic`.
- Playback goes through the stock sound pipeline (`PlayShaderDirectly` on an
  implicitly generated sound shader); loopback capture sees the final mix.

### Diagnostics
- `vis_status` (state + band levels), `vis_debug` (raw analyzer dump),
  `vis_beatSens` (beat threshold).

### Dataless-boot foundation (why any of this renders with zero retail data)
- `scripts/generate_boot_assets.py` generates fonts/charset/`_default`/white TGA/
  materials/presets so the engine boots to a render state with no `.resources`.
- Key engine fixes: `tr.whiteMaterial` → `visualizer/solid` (blend stage) so
  `DrawFilled` paints; material resolution on the main thread (not the draw
  worker); x64 boot bugs (`CPUCount` loop, `_findfirst` handle, `Sys_DLL_Load`
  types, PlatformToolset); idlib PCH disabled for x64.
- Build workflow: edit on Mac → push → Windows pull → **force-delete changed
  .obj** (incremental build silently skips recompiles) → MSBuild `doomexe`.

---

## Backlog / future ideas (not yet started)

### Windowed mode + in-GUI resolution/display selection
**Status: DONE (shipped).** New **DISPLAY** tab in the picker lists "Fullscreen —
Monitor N (native WxH)" per attached monitor (probed via `R_GetModeListForDisplay`)
plus windowed presets (720p/900p/1080p/1440p/4K); Enter applies via
`r_fullscreen` / `r_vidMode -1` / `r_customWidth`/`r_customHeight` + `vid_restart`.
Console: `vis_displays`, `vis_display <n>` (fullscreen native on monitor n),
`vis_resolution <w> <h>` (windowed). `vid_restart` here is `R_SetNewMode(false)`
(no image/decl purge), so materials survive; we still reset the feedback capture
for a clean trail at the new size and null the cached material pointers to
re-resolve as insurance. `vidMode_t`/`R_GetModeListForDisplay` forward-declared in
the sound TU to avoid pulling `tr_local.h`.
Known follow-up: effects render in a 640×480 virtual space stretched to the
backbuffer, so at non-4:3 window sizes the circular effects (radial/ring) become
elliptical — add aspect-correct centering later.

#### Original notes (superseded)
Today the app runs like the game (typically fullscreen / a fixed
window). Being able to run the visualizer in an arbitrary, resizable, optionally
borderless window would make it usable as a desktop "now playing" widget or on a
second monitor.
- The renderer already draws the overlay in a virtual **640×480** space
  (`SCREEN_WIDTH`/`SCREEN_HEIGHT`) that scales to the backbuffer, so the effects
  themselves need no layout changes — only the window/backbuffer size changes.
- Relevant existing engine cvars to drive/validate: `r_fullscreen`,
  `r_customWidth` / `r_customHeight` (and the mode list `r_vidMode`),
  `r_swapInterval`, `r_windowX/Y`. First step: confirm `r_fullscreen 0` +
  `r_customWidth/Height` gives a clean windowed boot in the dataless build.
- Likely work: a small `vis_window <w> <h>` convenience command that sets those
  cvars + `vid_restart`; verify aspect handling (letterbox vs stretch) so the
  circular effects (radial/ring) stay round; make sure input focus + the F1 menu
  still behave when not fullscreen; borderless/always-on-top would need a Win32
  window-style tweak in `neo/sys/win32/win_glimp.cpp`.
- Watch-outs: `vid_restart` re-inits GL → re-resolve cached materials
  (`s_visSolid`/`s_visFeedback`/`s_visWarp`) and the feedback RT after a restart;
  the dataless boot-asset materials must survive the restart.

### In-GUI resolution + display (monitor) selection
**Status: DONE — shipped together with windowed mode above (DISPLAY tab +
vis_displays/vis_display/vis_resolution).** Original design notes kept below.
Extends "Windowed mode". The picker should let you pick the
**resolution** and **which monitor** to run on from a new **DISPLAY** tab (and via
console), important for a laptop-screen + external-4K-monitor setup — e.g. run
fullscreen 3840×2160 on the external monitor while working on the laptop screen.
- **Enumerate at runtime**, don't hardcode: the Win32 layer (`neo/sys/win32/
  win_glimp.cpp`, `R_GetModeListForDisplay` / the mode list the engine already
  builds) can list supported resolutions per adapter; `EnumDisplayMonitors` /
  `GetMonitorInfo` gives the monitor list + their native sizes and desktop rects.
  Surface both as lists the DISPLAY tab renders (mirror how the DEVICES tab lists
  audio endpoints).
- **Apply** by setting the engine video cvars and issuing `vid_restart`:
  - resolution: `r_customWidth` / `r_customHeight` (+ `r_vidMode -1` for custom),
    or pick a `r_vidMode` index from the enumerated mode list.
  - monitor: confirm the multi-display cvar in this build — likely `r_fullscreen`
    as a 1-based monitor index and/or `r_windowX`/`r_windowY` to place a windowed
    instance on a chosen monitor's desktop rect. Verify on the dataless build.
  - fullscreen vs windowed: `r_fullscreen`, tie into the windowed-mode work above.
- **Console commands** to add alongside the tab: `vis_display <n>` (target monitor),
  `vis_resolution <w> <h>` (set custom size), `vis_displays` (list monitors +
  modes) — each sets the cvars and triggers `vid_restart`.
- **Caveats**: `vid_restart` re-inits GL → re-resolve cached materials
  (`s_visSolid`/`s_visFeedback`/`s_visWarp`) and the feedback render target, and
  make sure the dataless boot-asset materials survive the restart (same caveat as
  windowed mode). At 4K the per-cell/per-tap counts (hi-res warp mesh 48×36,
  spectrogram 80×32, 300 particles) are unchanged — they're in virtual 640×480
  space — so cost scales with fill, not geometry; watch fragment cost of the
  fullscreen shader-warp pass at 2160p.

### External companion GUI (control panel outside the game window)
**Status: idea.** The in-window HUD/picker is great fullscreen, but a control
surface *outside* the render window (separate OS window or app) would let you
drive presets/effects/routing/device while the visualizer runs clean and
uncluttered on another screen.
- Everything the GUI would touch is already a **cvar or console command**
  (vis_effect, vis_bands, vis_warp*, vis_route, vis_presetLoad, vis_device, …), so
  the control channel is the only new piece — no new engine logic needed.
- Options, cheapest → richest:
  1. **File/pipe command channel**: an external app writes lines to a watched
     file or named pipe; a tiny poller in `idVisualizerManager::Frame()` feeds them
     to `cmdSystem->BufferCommandText`. Trivial, language-agnostic front-end.
  2. **Local socket / loopback TCP**: same idea but live; a small listener thread
     (mirror the WASAPI thread pattern) forwarding text commands to the buffer.
     Enables a web UI or Electron/Qt panel.
  3. **Native second window** (Win32) owned by the engine process: more work
     (message loop, controls) and less flexible than an external app; probably not
     worth it vs. option 1/2.
- Recommended path: option 1 or 2 (expose the existing cvar/command surface over a
  simple text channel), then build the actual panel as a separate lightweight app
  that also *reads* `vis_status`-style state for feedback. Keep it optional/off by
  default (a `vis_control <port|path>` opt-in) so headless/normal runs are unaffected.

### Image layers (sound-reactive, ZGE-style)
**Status: DONE (TGA/JPG).** IMAGES tab picks an image from `base/images` as a
reactive layer (row 0 = off): drawn centered, scaled by the routed SCALE, spun by
the routed rotation, alpha-pulsed (`vis_layerAlpha`×BRIGHT), optionally hue-tinted
(`vis_layerColorize`). Composited before the feedback capture so it trails/warps.
Material = the engine's implicit `blend`+`colored`+`clamp` material for any image
name (`idMaterial::SetDefaultText`) — no material generation, any dropped TGA/JPG
works; resolved main-thread in `Frame()` on `vis_layerImg` change. cvars
`vis_layer`/`Img`/`Scale`/`Alpha`/`Colorize`, in preset serialization. Ships sample
`glow.tga`/`ring.tga`.
- **Multiple layers (DONE)**: `vis_layerList` is a `;`-separated list; the IMAGES
  tab toggles images in/out (row 0 = clear all), each drawn as its own layer with
  alternating spin + nested scale so stacked layers read distinctly. Resolved into
  `s_layerMats` on the main thread when the list changes.
- **Aspect fix (DONE)**: circular effects (radial/ring/particles) and image layers
  are corrected by `s_aspectFix` (= virtualAspect/actualAspect from
  `GetPixelAspect`) so circles stay round on 16:9/4K; image layers also use their
  own `GetImageWidth/Height` aspect so non-square images don't stretch.
- Follow-ups: SVG loader (see "Broader media formats"); per-layer position/
  route overrides.
- **Image filename limitation**: idTech parses a material's image name as an image
  *program* (operators `-` subtract, `*` scale, `()` etc.), so image filenames must
  avoid `-`, spaces, and operator chars — use plain/underscore names
  (`my_logo.png`, not `My-Logo Transparent.png`). Lowercase recommended. TGA/JPG/PNG
  all supported (PNG via the zlib loader; mixed-case resolved case-insensitively).

Original idea notes: load an image as a compositing layer (with alpha) that reacts to
audio — zoom/scale, distort, rotate, color/hue shift, alpha pulse — the way ZGE
layers a bitmap and links its knobs to bands.
- **Loading**: the engine's `R_LoadImage` only handles **`.tga` and `.jpg`**
  (LoadTGA → LoadJPG fallback; no PNG/DDS in `Image_files.cpp`). TGA already
  carries alpha, so image layers can ship with TGA today. **PNG/SVG** need new
  loaders — see "Broader media formats" below. Author a `visualizer/layerN`
  material (blend + the user image) or load directly and cache the `idMaterial*` —
  MAIN-THREAD resolve in `Frame()` like `s_visSolid`/`s_visFeedback`.
- **Drawing**: composite via `DrawStretchPic` (4-corner overload) so the layer can
  be scaled/rotated/skewed per-corner — same primitive the warp mesh uses. Alpha
  from the image + a routed alpha multiplier.
- **Reactivity**: reuse the existing routing (`s_mod`/`s_rotAngle` + `vis_route`
  targets scale/hue/bright/zoom/rotate) so an image layer is just another consumer
  of the modulation matrix. Distort by drawing the layer through the feedback/warp
  pass (mesh or `visualizer_warp` shader) instead of a plain quad.
- **UI**: an IMAGES/LAYERS tab (scan `base/images/`), pick an image, toggle it as
  a background or foreground layer; per-layer route overrides later. Persist in
  presets (add `vis_layer*` cvars to the serializer).
- **Watch-outs**: keep it main-thread for decl/image resolution; premultiplied-alpha
  in the 2D blend path; large images cost fill at 4K (the warp-cost note applies).

### Video layers (play a clip as a reactive layer)
**Status: idea.** Same as image layers, but the texture is a moving video frame.
- **Engine support**: `idCinematic` (`neo/renderer/Cinematic.h`, `idCinematic::Alloc`
  / `ImageForTime`) already decodes Bink (`.bik`) and RoQ into a texture each frame;
  the `BUILTIN_BINK`/`bink_gui` shaders and the material `cinematic`/`videoMap`
  keyword drive it. Simplest path: a material with a `videoMap <file>` stage → draw
  it as a layer, then feed it through the same routing/warp as image layers.
- **Modern formats** (mp4/webm): would need the vendored FFmpeg decoder
  (`XA2_FFmpegDecoder`, already in-tree for audio) extended to video → upload frames
  to a texture. Bigger; do Bink/`videoMap` first as the low-risk proof.
- **Watch-outs**: frame pacing vs the visualizer framerate; audio from the clip vs
  the analyzed source (probably keep them separate — the visualizer still reacts to
  the loopback/engine mix, the video is just visuals).

### Record the display to video (for YouTube etc.)
**Status: idea.** Capture the rendered output to disk so users can make videos of
their own music.
- **Frame capture**: the engine already has `renderSystem->TakeScreenshot(w,h,file,
  samples,ref)` + `R_ScreenshotFilename` and an aviDemo path (`com_aviDemo*` cvars,
  `TakeScreenshot` sequences in `common_frame.cpp`). MVP: a `vis_record` toggle that
  writes a numbered frame (`.tga`/`.jpg`) every frame to a folder, at a fixed
  timestep for smooth output; the user muxes with ffmpeg afterward.
- **Better**: pipe frames straight into an encoder. We already link FFmpeg for
  audio — an `libavformat`/`libavcodec` writer could encode H.264 to `.mp4`
  in-process. Capture the framebuffer via `CaptureRenderToImage` → GPU readback →
  feed the encoder.
- **Audio is the hard part** (and the whole point for YouTube): the output needs the
  music muxed in. Options: (a) record video only and let the user add audio in an
  editor (MVP); (b) also capture the analyzed PCM (we already have the sample ring
  in `AudioAnalyzer`) and mux A/V with FFmpeg. Sync/drift is the real work — lock
  video frames to audio time.
- **Watch-outs**: disk throughput at 4K/60 (frame sequences are huge → prefer the
  encoder path); don't capture the picker/console (reuse the overlay-freeze gate);
  a fixed-timestep record mode so playback speed is correct regardless of live fps.

### Broader media formats (audio via FFmpeg; PNG/SVG images)
**Status: idea.** Widen the file types the visualizer accepts.

**Audio (MP3, AAC/M4A, FLAC, Opus, …) — DONE (in-engine FFmpeg decode).**
Shipped: `idFFmpegDecoder` (modern AVChannelLayout/swr_alloc_set_opts2 API, builds
against FFmpeg 8.1.2) enabled for Debug|x64 via `ID_HAVE_FFMPEG` + the
`ffmpeg-8.1.2-full_build-shared` include/lib in `doomexe.vcxproj` (links
avcodec/avformat/avutil/swresample). Extension routing added in `snd_shader`
ParseShader, `XA2_SoundSample::LoadResource` (→ LoadMedia), and the MUSIC-tab
scan/`listMusic` for mp3/flac/m4a/aac/opus/wma. The 7 ffmpeg `bin\*.dll` are
copied next to the exe. `vis_play music/x.mp3` stream-decodes in-engine.
Build note: hardcoded `C:\ffmpeg\ffmpeg-8.1.2-full_build-shared` path in the
vcxproj (single build machine); Release/Retail configs exclude the decoder.

Original notes:
- The decoder already exists and is **format-agnostic**: `XA2_FFmpegDecoder.cpp`
  uses `avformat_open_input` + `av_find_best_stream(AUDIO)` +
  `avcodec_find_decoder`, so it decodes whatever the linked FFmpeg build supports
  (mp3/aac/flac/opus/m4a/wav/ogg). Today playback is `.wav` (+ `.ogg` via
  stb_vorbis); MP3 etc. are a **wiring** task, not new decode work.
- Work: add the FFmpeg decoder TU + libs to the build (it was excluded earlier),
  route non-wav/ogg extensions to it in the sound-sample loader
  (`XA2_SoundSample` / the implicit shader path), and add those extensions to the
  MUSIC-tab scan (`VisAppendFiles`) and `listMusic`. Ship FFmpeg DLLs next to the
  exe (licensing: LGPL build, dynamically linked).
- **Not VLC**: libVLC would duplicate FFmpeg (VLC is built on it) at much larger
  size/complexity, and it wants to own output/window. Since FFmpeg is already
  in-tree, prefer it; revisit VLC only if we ever need network/stream URLs it
  handles turnkey.
- Bonus: the same FFmpeg dependency is what the "record to video" item needs for
  H.264 muxing — do the audio wiring first, reuse the libs for capture later.

**Images: PNG and (optionally) SVG.**
- **PNG (DONE)**: `LoadPNG` added to `Image_files.cpp` using the in-tree zlib
  (`framework/zlib`), no new libs. Supports 8-bit non-interlaced color types
  0/2/3/4/6 (incl. alpha + palette tRNS), all 5 row filters; RGBA output matching
  LoadTGA; hooked into `R_LoadImage` (tga→jpg→png fallback + explicit `.png`).
  So `.png` images (with transparency) work everywhere, including image layers.
- **SVG**: vector, so it must be **rasterized at load** to an RGBA buffer at a
  chosen size (no live vector zoom unless re-rasterized). Cheapest path: vendor
  `nanosvg.h` + `nanosvgrast.h` (single-header, permissive) → rasterize to a buffer
  → feed the image system. Lower priority/niche; note that transforms (the ZGE-style
  zoom/warp) then act on the rasterized bitmap, not the vector.
- All image work stays MAIN-THREAD for decl/image creation; premultiplied-alpha in
  the 2D blend path (as with `visualizer/solid`).

### Release build (performance / choppiness)
**Status: doomexe FFmpeg wiring DONE; root cause of the doomclassic Release
failure found and fixed; game-d3xp/idlib health unverified.**
`doomexe.vcxproj` Release|x64 mirrors the Debug FFmpeg setup (ID_HAVE_FFMPEG,
ffmpeg-8.1.2 include/lib, decoder un-excluded, warnings-as-errors off).

**Root cause found for the "Cannot open idlib/precompiled.h" error**: comparing
`doomclassic.vcxproj`'s `ImportGroup Label="PropertySheets"` blocks directly (not
just the `ItemDefinitionGroup` compiler settings) showed **Release|x64 was
missing the import entirely** — Debug|Win32, Debug|x64, Release|Win32, Retail|Win32,
and Retail|x64 all import `DoomClassicCommon.props` (which sets
`AdditionalIncludeDirectories` to `$(ProjectDir)Main;$(SolutionDir);
C:\DOOM-3-BFG\neo;...` — the paths needed to resolve `idlib/precompiled.h`), but
Release|x64 had no `PropertySheets` `ImportGroup` at all, so it silently got zero
include paths. Fixed by adding the missing `ImportGroup` block, mirroring the
other five configs exactly (`plans/PRD-implementation-status.md` M7 has the
full detail). `doomclassic/timidity/timidity.vcxproj` already had its Release|x64
import correctly — this bug was specific to `doomclassic.vcxproj`.

**Not verified, and not additionally "fixed" without evidence**: the roadmap's
prior note said "then likely `game-d3xp`/`idlib`" — that was speculative, never a
confirmed separate failure. Checked those `.vcxproj` files directly: none of them
have ANY `PropertySheets` `ImportGroup` for *any* configuration (not just missing
one for Release|x64 like doomclassic did) — they may rely on relative-path
includes with no shared props file at all, which could be a completely different
(and possibly non-broken) situation, or they could hit a different failure. Left
untouched pending an actual Windows build attempt to see what (if anything) still
fails, rather than guessing at a fix for an unconfirmed problem.
Follow-up once doomclassic's fix is confirmed: copy the ffmpeg `bin\*.dll` next to
the Release exe. Would smooth framerate.

### Load external presets: MilkDrop / projectM (.milk) + ZGE (FL Studio)
**Status: idea (large). Not supported today — our presets are just cvar .cfgs.**
Research already exists: `docs/research-milkdrop-projectm.md`,
`docs/research-zge-visualizer.md`.

- **MilkDrop `.milk` and projectM** (same format — projectM is a MilkDrop reimpl):
  a `.milk` is INI-ish text with per-frame + per-pixel **equations** in a custom
  expression language, a set of `q1..q32`/`t1..t8` vars, wave/shape params, and
  (MilkDrop2) `warp`/`comp` **HLSL shaders**. To run them we'd need:
  1. An **expression evaluator** — vendor `projectm-eval` (its clean-room ns-eel)
     or ns-eel2; compile the per-frame/per-pixel code, feed it `time/bass/mid/treb/
     bass_att/...` (we already have all those in the analyzer), read back
     `zoom/rot/warp/dx/dy/cx/cy/...`.
  2. A **warp mesh driven per-pixel** by the evaluated per-pixel eqs — we already
     have the warp-mesh infrastructure (`DrawFeedbackWarpMesh`); swap the hardcoded
     warp math for the evaluated `dx,dy,zoom,rot` per grid vertex.
  3. **Shader translation** for the `warp`/`comp` HLSL — hardest part; MilkDrop HLSL
     ≠ our HLSL-dialect→GLSL path. Start by supporting the (many) preset that use
     only per-frame/per-pixel eqs + built-in waves (no custom shader), then add a
     shader transpiler. Custom-textured presets need their sampler images too.
  Milestone plan: (a) `.milk` parser + var table; (b) per-frame eqs → our existing
  zoom/rot/warp cvars (instant partial support for classic presets); (c) per-pixel
  eqs → warp-mesh vertices; (d) waveforms/shapes; (e) shader transpile.
- **ZGE (ZGameEditor Visualizer)**: FL Studio's visualizer; presets are ZGameEditor
  **project files** (component tree + GLSL snippets), not equations. Would need a
  ZGE-format parser and a mapping of its component/render model onto ours — separate,
  less-documented effort. Lower priority than MilkDrop given format complexity and
  smaller shared preset ecosystem.
- **Reuse**: we already expose the exact audio signals these formats want
  (bass/mid/treb + attack envelopes, rms, beat, waveform, spectrum) and have a
  warp-mesh + feedback pipeline — so the *rendering* substrate exists; the work is
  the parser + expression VM (+ eventual shader transpile).
- **Test corpus**: projectM ships small, self-contained `.milk` presets meant for
  exercising a parser/evaluator here:
  https://github.com/projectM-visualizer/projectm/tree/master/presets/tests
  These are ideal for milestone (a)/(b) above — mostly per-frame/per-pixel eqs with
  minimal custom-shader dependence, so they validate the expression VM + warp-mesh
  path before we tackle the HLSL transpiler. Drop them under `base/presets/milk/`
  once a `.milk` loader exists.
- **Preset packs** (`.milk` / MilkDrop2, linked from the projectM README —
  https://github.com/projectM-visualizer/projectm/blob/master/README.md). These are
  the real-world corpus to target once the loader works; grab a small pack first and
  scale up as coverage improves:
  - **Milkdrop texture pack** — shared textures many presets sample; install first
    regardless of pack (browse the files here):
    https://github.com/projectM-visualizer/presets-milkdrop-texture-pack/tree/master
    Once we have a `.milk` loader these `.jpg`/`.png` textures go under a
    `base/textures/milkdrop/` dir so preset `sampler_*` lookups resolve.
  - **Cream of the Crop** (~10K presets, Jason Fletcher; projectM's current default):
    https://github.com/projectM-visualizer/presets-cream-of-the-crop
  - **Classic projectM** (~4K, shipped with older projectM):
    https://github.com/projectM-visualizer/presets-projectm-classic
  - **Milkdrop 2 originals** (shipped with MilkDrop/Winamp):
    https://github.com/projectM-visualizer/presets-milkdrop-original
  - **En D** (~50 presets): https://github.com/projectM-visualizer/presets-en-d
  - **Classic collections bundle** (bltc201, Milkdrop 1/2, projectM, tryptonaut, yin):
    http://spiegelmc.com/pub/projectm_presets.zip
  - **MegaPack** (~130K presets, 4.08 GB incl. textures):
    https://drive.google.com/file/d/1DlszoqMG-pc5v1Bo9x4NhemGPiwT-0pv/view
  Note the huge spread of MilkDrop1 (per-pixel eqs only) vs MilkDrop2 (custom
  `warp`/`comp` HLSL) presets: milestone (a)–(c) already renders the MilkDrop1-style
  packs; the MegaPack/Cream sets lean on shaders and need the transpiler (e).

## Keyboard shortcuts (MilkDrop-style)

We match a **sensible subset** of MilkDrop 3.x's hotkeys — the ones that map onto
features we actually have — rather than the full set (many MilkDrop keys are for
sprites, double-presets, ratings, always-on-top, etc. that don't apply here, and
some conflict with ours: MilkDrop's **F1** is help, but ours opens the menu).

### Shipped
Global, active while the picker menu is **closed** (handled in
`idVisualizerManager::MenuProcessEvent`, which runs after the console so typing is
never eaten):

| Key         | Action                          | MilkDrop equivalent            |
|-------------|---------------------------------|--------------------------------|
| **F1**      | Toggle the picker menu          | (MilkDrop: M / help)           |
| **SPACE**   | Next preset                     | SPACE / H (next preset)        |
| **BACKSPACE** | Previous preset               | BACKSPACE (prev preset)        |
| **N**       | Next effect type (cycles 0–7)   | (ours; MilkDrop N = show info) |
| **R**       | Toggle shuffle order (`vis_presetCycleShuffle`) | R                  |
| **`` ` ``** | Lock/unlock (pause/resume auto-cycle, `vis_presetCycle`) | `/~     |
| **C**       | Next palette (`vis_palette`, cycles rainbow→fire→ocean→synthwave→mono) | C |
| **F3**      | Cycle time-between-presets (`vis_presetCycleSecs`: 15/30/60/120/300s) | F3 |
| **F8**      | Toggle auto-cut-on-beat (`vis_presetCycleOnBeat`) | F8 (cut on music)       |
| **CTRL+←**  | Previous track (`Prev()`)       | CTRL+← (player transport)      |
| **CTRL+→**  | Next track (`Next()`)           | CTRL+→ (player transport)      |
| **F**       | Cycle windowed → fullscreen monitor 1..N → windowed | F (single/double/all-screen) |
| **A**       | Mash-up: start (random 2nd `.milk` preset) or nudge mix +0.1 toward it | A (mash-up) |
| **Z**       | Mash-up: nudge mix -0.1 toward the first preset, clearing at 0 | Z (mash-up) |

Preset nav reuses the auto-cycle list (`NavigatePreset()` builds it on first use,
so the hotkeys work even when `vis_presetCycle` is off) and wraps both directions.
CTRL+←/→ check `idKeyInput::IsDown(K_LCTRL/K_RCTRL)` so plain ←/→ still reach the
menu/list-navigation code elsewhere (only fires when the picker menu is closed).
**F is a deliberately honest facsimile, not exact parity**: this engine has no
"span multiple monitors as one canvas" or "render to every monitor at once"
concept, so F cycles through each monitor individually plus windowed, rather
than MilkDrop's literal single/double/all-screen semantics. **A/Z drive the
PRD M5 mash-up mechanism** (`vis_milkMash`/`vis_milkMashClear`) — needs preset A
(`vis_milkPreset`) active first; A picks a random `.milk` file from
`presets/milk` as B if no mash-up is running yet.

| **F11**     | Toggle all effects (feedback/warp/bloom master, save+restore) | F11 |

The design decision previously deferred here is now made: **save-and-restore**,
not zero-and-forget — toggling F11 off saves the current `vis_feedback`/
`vis_warp`/`vis_bloom` values, zeros them, then toggling it back on restores
exactly what was there before (not some default state).

### Candidate additions (MilkDrop parity, not yet wired)
Ordered by value / low conflict risk:
- **CTRL + ↑/↓** — seek within track. **Blocked**: no seek/scrub API exists on the
  playback path today (`PlayShaderDirectly` is fire-and-forget); would need new
  playback-position plumbing, not just a hotkey — bigger than this backlog item implies.
- **B** — toggle bloom; **K** — cycle kaleidoscope; **W** — cycle warp mode.

Deliberately **not** matching: sprites (SHIFT+K, 00–99), double-preset (F9),
ratings (F6), always-on-top/borderless (F7).
When external `.milk` preset loading lands, SPACE/BACKSPACE will page through the
loaded MilkDrop presets too, giving full MilkDrop-style navigation.
