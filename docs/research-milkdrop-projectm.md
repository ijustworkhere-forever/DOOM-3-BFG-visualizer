# Research: MilkDrop (.milk), MilkDrop3, and projectM

Sources: MilkDrop3 repo (milkdrop2077), projectM repo (README + C API headers: core.h,
audio.h, render_opengl.h, parameters.h), projectm-eval README, Geiss's MilkDrop preset
authoring guide.

## 1. The real .milk preset format

INI-style text, `[preset00]` header. Contents:
- **Scalar params**: baseline values for ~60 built-ins (`fDecay`, `zoom`, `rot`, `warp`,
  `fVideoEchoZoom`, `nWaveMode`, `ob_*`/`ib_*` borders, `mv_*` motion vectors, …).
- **`per_frame_init_`**: runs once at load; values written to `q1..q32` become each
  frame's starting q values.
- **`per_frame_`**: once per frame; reads audio/time, writes motion/color/effect vars and q's.
- **`per_pixel_` (per-vertex)**: at every warp-mesh vertex (default 32×24); modulates
  `zoom/rot/warp/dx/dy/sx/sy/cx/cy` as f(`x`,`y`,`rad`,`ang`).
- **`warp_` / `comp_` shaders**: HLSL ps_2_0/ps_3_0 pixel shaders (MilkDrop 2/3).
- **`[wavecode_NN]`** custom waves (per-frame + per-point code; `sample`, `value1/2`
  audio inputs; writable x,y,r,g,b,a) and **`[shapecode_NN]`** custom shapes (`sides`,
  `rad`, `ang`, textured, instanced).

**Variable pools are separate per section**; only bridges cross: `q1..q32` (init →
per_frame → per_vertex → shader uniforms; MD3: q1..q64), `t1..t8` (per wave/shape),
`reg00..99`/`gmegabuf[]` (global persistent).

**Audio built-ins**: `bass`, `mid`, `treb` (instantaneous band energy, ~1.0 = average)
plus `bass_att`, `mid_att`, `treb_att` (smoothed envelope followers). Also `time`,
`frame`, `fps`, `progress`, `meshx/y`, `pixelsx/y`, `aspectx/y`.

**Shader uniform/sampler contract**: `sampler_main` (canvas), `GetBlur1/2/3()` blurred
mips, noise samplers (`sampler_noise_lq/mq/hq`, 3D `noisevol`), user textures
`sampler_FILENAME` with filter/wrap prefixes (`fw_`, `fc_`, `pw_`, `pc_`), uniforms
`q1..q32` (packed `_qa.._qh`), `rot_s/d/f/vf/uf/rand` matrices, blur ranges.

## 2. Classic MilkDrop render pipeline (exact pass order)

Ping-pong two internal canvases (half-float matters — 8-bit feedback bands):
1. CPU: run per_frame equations → globals + q's.
2. **Warp pass**: draw the warp mesh sampling the *previous* frame's canvas; per-vertex
   equations produce each vertex's sample UV; warp pixel shader applies decay (~0.97) and
   effects. This single feedback step produces all perceived motion.
3. Motion vectors (`mv_*` arrows/dots) onto the canvas.
4. Custom shapes (triangle fans, optionally textured).
5. Custom waves + the built-in waveform (PCM or spectrum).
6. Outer/inner borders.
7. Blur passes (3 progressively blurred copies) if referenced.
8. **Composite pass** to screen: comp shader (video echo, gamma, brighten/darken,
   solarize, darken-center folded in here).
9. **Transitions**: soft cut = render BOTH presets fully and cross-blend composites over
   blend time; hard cut (beat-triggered) = instant switch.

Invariant: the warp canvas is the only long-lived state; everything drawn becomes history
the next frame's warp resamples and decays.

## 3. projectM

libprojectM = cross-platform MilkDrop reimplementation: parses presets, does PCM →
FFT/beat detection, evaluates equations, renders via OpenGL (to default FBO, an FBO, or a
texture). Playlist logic is a separate lib (libprojectM-playlist); frontends (SDL2 etc.)
are separate repos.

- **projectm-eval**: portable C reimplementation of NullSoft EEL2 (the equation
  language). Compile once, execute per frame. Host binds variables by pointer:
  `projectm_eval_context_create` → `register_variable(ctx,"bass")` returns `double*` →
  `code_compile` → `code_execute`. megabuf per-context, gmegabuf shared (host must
  provide lock callbacks). **Recommended to vendor this instead of writing our own
  evaluator** — it's self-contained C, no GL dependency, real .milk equations just work.
- **Shader translation**: HLSL→GLSL transpile at preset load + preamble injecting the
  MilkDrop uniform/sampler set. (Heavy; defer — support equation-only presets first.)
- **Audio API**: `projectm_pcm_add_float/int16/uint8(handle, samples, count, channels)`
  (interleaved stereo; mono duplicated); library runs its own FFT + beat detection.
- **Core API**: `projectm_create`, `projectm_opengl_render_frame[_fbo]`,
  `projectm_load_preset_file(path, smooth)`, `set_mesh_size`, `set_fps`,
  `set_window_size`, `set_preset_duration`, `set_soft/hard_cut_duration`,
  `set_hard_cut_sensitivity`, `set_beat_sensitivity`, `set_preset_locked`.

## 4. MilkDrop3 over MilkDrop2

`.milk2` double-presets ("mash-ups": blend two full presets with many blend patterns,
live-creatable), deep mash-up (blends all 5 bins), 16 custom waves + 16 shapes (was 4),
shapes to 500 sides, q1..q64, full FFT array + mouse input available in shaders, shader
cache + in-app editor, new stabler expression VM, 60/90/120 fps, hi-res audio, hardware
tiering at setup. 100% backward compatible with MilkDrop/projectM presets.

## 5. Takeaways for the idTech4 build

**Minimum credible pass set** (4 FBO-ish targets, 3–4 draw calls):
1. Feedback/warp pass: mesh-or-fullscreen draw sampling `prevFrame` → `curFrame`,
   per-vertex UV distortion + decay; ping-pong RGBA16F textures.
2. Waveform/shape overlay onto `curFrame` (additive).
3. Optional separable blur chain for `GetBlur*`.
4. Composite to screen (gamma/echo/tint folded into one shader).
Add soft-cut blending later (two pipelines + lerp). Warp mesh 32×24, CPU-computed UVs is
authentic and cheap.

**Expression eval**: vendor projectm-eval; bind `zoom/rot/warp/dx/dy/q*/bass/...` as
registered `double*`. Writing our own = subtle incompatibilities (precedence, if(),
megabuf semantics).

**Audio features per frame** (from our XAudio2 tap):
- ring buffer ≥512 samples/channel (waveform display),
- 512/1024-pt Hann FFT each frame → 3 band sums: bass ~20–250 Hz, mid ~250–2500 Hz,
  treb 2500+ Hz,
- normalize each band by its long-running average (≈1.0 at typical loudness),
- attenuated versions: `x_att = 0.6*x_att + 0.4*x`,
- beat detection: short-term vs long-term bass energy ratio against sensitivity threshold
  (drives hard cuts / auto-advance). Mirror projectM's `beat_sensitivity` knobs.

**Cleanest embedding**: vendor projectm-eval for math + native idTech4 GLSL passes for
the pipeline, fed by our own FFT. Real .milk equation compatibility without dragging in
libprojectM's renderer. HLSL preset shaders: phase 2.
