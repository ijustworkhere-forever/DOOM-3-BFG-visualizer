# Research: ZGameEditor Visualizer (FL Studio) + ZGameEditor

Sources: FL Studio online manual (ZGameEditor Visualizer page; the "01–17" links are the
17 expandable effect-category groups in the effect dropdown, not separate pages),
zgameeditor.org, github.com/VilleKrumlinde/zgameeditor, community walkthroughs.

## What ZGE Visualizer is

A VFX plugin bundled with FL Studio: drop it on a mixer track, it renders audio-reactive
graphics live and exports video. The plugin is a **thin host** — effects are `.zgeproj`
files authored in the open-source ZGameEditor engine, dropped in an `Effects` folder.
**Biggest structural lesson: the visualizer is a player for a content format, not a fixed
set of hardcoded effects.**

## The layer-stack model (core UX)

- Horizontal strip of layer slots (A, B, C, …; up to ~100). **Leftmost = background,
  rightmost = foreground.** Reorderable, per-layer solo/toggle/alpha.
- Each layer is either a **source** (image, particles, scene, text) or a **processor**
  (blur, feedback, color, distortion) operating on the composite beneath it —
  one uniform list covers backgrounds, overlays, blends, and post-processing.
- **"To buffer"**: snapshot the current composite into a named buffer for downstream
  layers to sample (multi-stage compositions). Maps naturally onto FBO snapshots.
- Two-tab split: **Add Content** (import images/video/text as named assets) vs **Main**
  (the layer chain referencing assets by name).

## Audio → parameter mapping (the crucial part)

Every layer parameter is a **named, DAW-automatable knob**. There is no single "react to
beat" toggle — audio-reactivity is generic modulation routing:
- Fruity Peak Controller (envelope from any track — often a ghost track with an isolated
  kick) → right-click any knob → "Link to controller".
- Envelope controllers / automation clips / MIDI for scripted motion.
- Effects also read the plugin's **internal analysis** directly: peak/amplitude,
  FFT spectrum bands (e.g. Polar's `BANDS`), and raw waveform.
- Knobs animate live when driven — instant visual confirmation the routing works.

## The 17 effect categories

1 Background · 2 Foreground · 3 Blend · 4 Canvas effects (procedural plasma/lava) ·
5 Feedback (trails/infinite zoom) · 6 Hardware DMX (drives real stage lights!) ·
7 Image effects · 8 Misc · 9 Object Arrays (instanced grids/rings) · 10 Particles ·
11 Peak effects (flagship **Polar**: radial spectrum, `RADIUS/BANDS/THICKNESS/MAGNITUDE`) ·
12 Physics · 13 Postprocess (bloom/blur/glitch/color) · 14 Scenes (3D fly-throughs) ·
15 Terrain · 16 Text (titles/lyrics) · 17 Tunnel effects. Plus HUD "PreFab" composite
overlays.

## Video export

Offline frame-accurate renderer decoupled from live preview. Destination presets
(YouTube HD/4K, Instagram/TikTok 1080×1920), fps 30/60, bitrate controls, supersampling.

## How ZGameEditor enables it

Delphi/FPC OpenGL engine, MIT, "procedural content in 64 KB" ethos. Project = a
**component tree** (ZApplication → states/models/meshes/materials/expressions) edited in
a property inspector. **ZExpression** = small C-like language in per-frame slots
(`OnUpdate`, `BeforeRender`) with access to audio analysis/time/state. **Shader
component** wraps raw GLSL vertex/fragment source with a `UniformVariables` list binding
engine values/expressions to uniforms — post-process/canvas effects are fullscreen-quad +
fragment shader. Procedural `BitmapProducers` texture graph; particle emitters; there is
even a **Shadertoy→ZGE converter** (huge free effect library).

## Takeaways for our visualizer

**UI patterns to copy:**
1. Single layer strip (source-or-processor slots, back-to-front, add/reorder/solo/alpha).
2. Per-layer effect dropdown grouped by category; selecting swaps in that effect's knobs.
3. Generic modulation routing: every knob linkable to an audio bus (peak / band N /
   waveform) with per-link smoothing (attack/decay) and gain/range remap. No bespoke
   "beat mode".
4. Assets tab vs pipeline tab.
5. Live knob animation as routing feedback.
6. Export presets by destination.

**v1 category subset:** Background, Image, Canvas/Procedural (shader), Particles,
Peak/Spectrum (build Polar first — it's iconic), Postprocess (bloom/feedback), Text,
Tunnel. Skip DMX/Terrain/Physics/ObjectArrays.

**Content-format strategy (biggest architecture lesson):** effects = data + shader files
in a folder, not compiled-in code. Define an effect as a small manifest (name, category,
parameters with ranges + audio-bindable flags) + a GLSL shader; the layer system exposes
manifest parameters as knobs automatically. That makes Shadertoy-style shaders drop-in
and gives a shareable community format — exactly ZGE's win. Note this dovetails with the
engine's existing material system (expression registers + GLSL stages) and with the
MilkDrop preset plan (docs/research-milkdrop-projectm.md): the layer stack can host a
"MilkDrop layer" as one effect type among many.

**Minimum viable feature set DAW users expect:** layer stack; image layer; spectrum/peak
reactive layer; procedural shader layer; text layer; one post-process; FFT+peak+waveform
buses with per-knob links and smoothing; save/load whole-stack presets; video export.
