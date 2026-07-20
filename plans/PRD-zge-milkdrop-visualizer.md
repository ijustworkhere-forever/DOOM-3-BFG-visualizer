# PRD — "idVIS": a ZGameEditor-style visualizer with MilkDrop / projectM preset compatibility

**Status:** In implementation (M0 underway) · **Date:** 2026-07-15
**Host:** dataless RBDOOM-3-BFG (idTech4/5), C++, Windows x64 (Mac dev / Windows build)
**Live progress tracker:** `plans/PRD-implementation-status.md` — updated per milestone/step.

> Synthesizes: `docs/research-zge-visualizer.md`, `docs/research-milkdrop-projectm.md`,
> `docs/research-audiorider.md`, `plans/visualizer-ui.md`, `plans/visual-effects-roadmap.md`,
> `docs/audit-existing-visualizer-code.md`, `docs/engine-map.md`. This PRD is the north-star
> spec; the roadmap remains the running task log.

---

## 1. Vision

Turn the dataless DOOM-3-BFG boot into a **standalone music visualizer** that is, at once:

1. **A player for the world's biggest visual-preset ecosystem** — loads and runs real
   **MilkDrop 2 (`.milk`)**, **MilkDrop 3 (`.milk2`)**, and **projectM** presets
   (100k+ community presets exist), not a fixed set of hand-coded effects.
2. **Authored and controlled like FL Studio's ZGameEditor Visualizer** — a horizontal
   **layer stack** of source/processor slots, every parameter a **knob** that can be
   **linked to an audio source** with smoothing and range, plus an assets/main split and
   destination-based video export.
3. **Driven like MilkDrop 3** — the **same keyboard shortcuts** so anyone who has used
   MilkDrop/Winamp is instantly at home.
4. **Able to turn the host engine itself into ambience** — since we're built *on* idTech4,
   an optional layer type flies a camera through the user's own legally-owned DOOM 3: BFG
   maps — retail campaign **or community mod maps** — as a living 3D backdrop (Pillar F) —
   a capability no MilkDrop/projectM/ZGE clone has, because none of them are a game engine.

The through-line across all three references, independently reached by AudioRider, is one
idea: **content is data, and every parameter picks a source + smoothing + gain.** The
product is a *player + router*, not a bag of effects.

### Non-goals (v1)

- Not a general game engine feature; the visualizer *is* the app. (Pillar F's map
  flythrough is the one deliberate exception — see FR-F1 for why it doesn't violate the
  dataless-boot principle below.)
- **No retail DOOM data *dependency*** — the shipped product must boot and run with
  generated assets only, with zero required retail files. Pillar F is strictly *optional*:
  if (and only if) the user points the app at retail `.resources` they already legally own
  (e.g. their own Steam/GOG install), map flythrough activates; nothing is bundled,
  downloaded, or required. Dataless boot remains the unconditional default.
- No authoring of MilkDrop presets from scratch in-app (MD3 has an editor; we *run* them).
- Terrain, Physics, and Object-Array ZGE categories are out of scope.
- **DMX is no longer a non-goal** — direct user request (2026-07-18): promoted to
  FR-C8/Pillar C, see below. (Previously listed here as skipped; superseded.)

---

## 2. Background — what already ships today (baseline)

The current build (`neo/sound/visualizer_manager.cpp` + custom renderprogs + generated
boot assets) already delivers a working single-layer visualizer. The PRD builds **on top
of** this, not from zero. Shipped:

- **Rendering:** immediate-mode 2D overlay; feedback trail + **warp mesh** (24×18 / 48×36,
  ripple/swirl/tunnel/fisheye), **kaleidoscope** fold, **bloom** post (custom HLSL→GLSL
  renderprog), palettes + global hue shift.
- **Effects:** bars, radial, scope, waveform ring, particles, spectrogram, starfield,
  phase-scope (8 total; `vis_effect`, `vis_nextEffect`).
- **Audio:** in-engine FFT analyzer (bass/mid/treb + attack envelopes, RMS, beat,
  waveform, N-band); WASAPI **loopback auto-arm**; FFmpeg decode (mp3/flac/m4a/aac/opus/
  wma/ogg/wav); background-audio-on-focus-loss.
- **Modulation matrix (proto-routing):** `value = base + source*amount` over targets
  {scale,hue,bright,zoom,rotate} × sources {bass,mid,treb,*_att,rms,beat,level,lfo}.
- **UX:** F1 picker overlay (MUSIC/PLAYLISTS/PRESETS/DEVICES/DISPLAY/IMAGES/EFFECTS tabs),
  image layers, preset save/load (`.cfg` = cvars + routes), preset auto-cycle, multi-
  monitor/windowed DISPLAY tab, HUD, **MilkDrop-style hotkeys** (SPACE/BACKSPACE/N).

**Gap this PRD closes:** the shipped system is a *single hardcoded effect with knobs*.
The target is a *multi-layer stack that can host MilkDrop presets as one layer type* and a
*real routing GUI*. Everything shipped becomes either a **built-in layer type** or the
**post/feedback substrate** the MilkDrop pipeline reuses.

---

## 3. Users & use cases

| Persona | Wants | Key features |
|---|---|---|
| **VJ / live performer** | Beat-synced visuals reacting to a live audio device; fast preset switching | Loopback capture, MD3 hotkeys, preset cycle/lock, multi-monitor fullscreen |
| **Music producer (FL Studio refugee)** | Author a reactive scene per track, export video for release | Layer stack, per-knob routing, text/lyrics layer, video export presets |
| **MilkDrop enthusiast** | "Just run my 130k preset pack" | `.milk`/`.milk2`/projectM loader, playlist, ratings, mash-ups |
| **Casual listener** | Pretty screensaver reacting to whatever's playing | Auto-loopback, auto-cycle shuffle, zero-config boot |

Primary use case: **point it at audio (file, playlist, or system loopback), pick or
auto-cycle presets, show fullscreen on a chosen monitor.** Secondary: **compose a layered
scene with audio-linked knobs and export it to video.**

---

## 4. Product pillars & requirements

Requirement IDs: `PR-#` product, `FR-#` functional, `NFR-#` non-functional. Priority:
**P0** (must, defines the product) · **P1** (should) · **P2** (nice).

### Pillar A — MilkDrop / projectM preset compatibility (the differentiator)

> Reference: `docs/research-milkdrop-projectm.md`. Strategy decided there and adopted here:
> **vendor `projectm-eval` for the equation VM + native idTech GLSL passes for the pipeline,
> fed by our own FFT.** Do NOT drag in libprojectM's full renderer; do NOT write our own
> expression evaluator (subtle EEL2 incompatibilities in precedence/`if()`/megabuf).

- **FR-A1 (P0) `.milk` parser.** Parse INI-style presets: scalar params (~60 built-ins:
  `fDecay`, `zoom`, `rot`, `warp`, `fVideoEcho*`, `nWaveMode`, `ob_*`/`ib_*`, `mv_*`, …),
  `per_frame_init_`, `per_frame_`, `per_pixel_` code blocks, `[wavecode_NN]`,
  `[shapecode_NN]`, and `warp_`/`comp_` shader blocks. Tolerant of malformed presets
  (skip → log, never crash) since community packs are inconsistent.
- **FR-A2 (P0) Expression VM.** Vendor `projectm-eval` (portable EEL2). Register host
  variables by pointer (`bass`, `mid`, `treb`, `*_att`, `time`, `frame`, `fps`,
  `progress`, `q1..q64`, `t1..t8`, `reg00..99`, `meshx/y`, `aspectx/y`, …). Compile each
  block once at load; execute per-frame / per-vertex. Honor separate variable pools with
  the `q`/`t`/`reg`/`gmegabuf` bridges (§ research doc). MD3 `q1..q64` supported.
- **FR-A3 (P0) MilkDrop render pipeline** (native GLSL, ping-pong RGBA16F — half-float is
  required, 8-bit banding is visible):
  1. run `per_frame` → globals + q's;
  2. **warp pass**: draw warp mesh (default 32×24) sampling *previous* canvas, per-vertex
     `per_pixel` eqs produce sample UVs, apply decay (~0.97);
  3. motion vectors (`mv_*`);
  4. custom shapes (triangle fans, optional textured/instanced);
  5. custom waves + built-in waveform (PCM or spectrum);
  6. outer/inner borders;
  7. optional 3-tap separable blur chain (`GetBlur1/2/3`);
  8. **composite pass** (video echo, gamma, brighten/darken, solarize, darken-center).
  Reuses the shipped feedback/warp-mesh + bloom infrastructure as the substrate.
- **FR-A4 (P1) HLSL shader presets.** Transpile MilkDrop `warp_`/`comp_` HLSL (ps_2_0/
  ps_3_0) → GLSL at load, injecting the MilkDrop uniform/sampler preamble
  (`sampler_main`, `GetBlur1/2/3`, `sampler_noise_*`, `sampler_FILENAME` with
  `fw_`/`fc_`/`pw_`/`pc_` prefixes, `q1..q32` packed as `_qa.._qh`, `rot_*` matrices).
  **Phased:** ship equation-only preset support first (many MilkDrop1-era presets), then
  the transpiler for MilkDrop2/3 shader presets.
- **FR-A5 (P0) Preset transitions.** Soft cut (render both presets, cross-blend composites
  over blend time) + hard cut (beat-triggered instant switch). Map to
  `set_soft_cut_duration`/`set_hard_cut_*` semantics.
- **FR-A6 (P1) MilkDrop3 features.** `.milk2` **double-presets / mash-ups** (blend two full
  presets, live-creatable), up to 16 custom waves + 16 shapes, shapes to 500 sides, full
  FFT array in shaders. 100% backward-compatible with MilkDrop2/projectM presets.
- **FR-A7 (P0) Texture pack.** Bundle/resolve the MilkDrop texture pack so preset
  `sampler_FILENAME` lookups resolve. Install under `base/textures/milkdrop/`.
  (Repo: presets-milkdrop-texture-pack.)
- **FR-A8 (P0) Preset library & playlist.** Ingest the projectM preset packs
  (Cream of the Crop ~10k, classic ~4k, MilkDrop2 originals, MegaPack ~130k — see roadmap
  §"Load external presets" for URLs) under `base/presets/milk/`. Browse, search, rate,
  lock, shuffle, auto-advance on timer/beat. Robust to presets that fail to load.

**Acceptance (A):** Load a random 100-preset sample from Cream-of-the-Crop; ≥80% render
recognizably at ≥45 fps@1080p; SPACE advances; no crashes; failures logged and skipped.

### Pillar B — ZGameEditor-style GUI & layer model

> Reference: `docs/research-zge-visualizer.md`. Core lesson: **the visualizer is a player
> for a content format, not hardcoded effects**, presented as a **layer stack** where every
> knob is DAW-automatable. GUI tech decision from `plans/visualizer-ui.md`: **ImGui debug
> panel first → console cvars → legacy `.gui` picker → SWF shell (later).** For the rich
> layer/knob editor, **ImGui is the pragmatic v1** (~~the RBDOOM port already carries it~~
> **correction, confirmed by direct search of this fork: it does not** — zero ImGui files/
> references anywhere in `neo/`. See `plans/PRD-implementation-status.md` M4 for the
> corrected plan: vendor ImGui fresh from `ocornut/imgui` (MIT, real internet access
> confirmed available this session) rather than assuming it's already present.);
> a polished `.gui`/SWF skin is a later re-skin.

- **FR-B1 (P0) Layer stack.** Horizontal strip of layer slots (A, B, C…; target ≤32).
  **Leftmost = background, rightmost = foreground.** Reorderable; per-layer
  solo / mute / opacity / blend-mode. One uniform list covers sources *and* processors.
- **FR-B2 (P0) Layer types (v1 subset).** Each layer is a **source** or **processor**:
  - *Sources:* Image, Text/Lyrics, Spectrum/Peak (build **Polar** — radial spectrum,
    `RADIUS/BANDS/THICKNESS/MAGNITUDE` — first; it's iconic), Procedural/Shader (fullscreen
    quad + GLSL, Shadertoy-import-friendly), Particles, **MilkDrop preset** (Pillar A as a
    single layer type), and the shipped built-in effects (bars/scope/ring/starfield/…).
  - *Processors:* Feedback/trail, Warp, Blur, Bloom/Glow, Kaleidoscope, Color/Palette,
    Distortion. (All already exist — expose as reorderable processor layers.)
- **FR-B3 (P1) "To buffer."** Snapshot the current composite into a named buffer for
  downstream layers to sample (multi-stage compositions). Maps onto FBO snapshots — the
  `CaptureRenderToImage` mechanism already used for feedback/bloom.
- **FR-B4 (P0) Category dropdown.** Per-layer effect dropdown **grouped by category**
  (subset of ZGE's 17: Background, Image, Canvas/Procedural, Feedback, Particles,
  Peak/Spectrum, Postprocess, Text, Tunnel). Selecting an effect **swaps in that effect's
  knobs** automatically.
- **FR-B5 (P0) Assets vs Main split.** Two-tab UX: **Add Content** (import images/video/
  text/shaders as named assets) vs **Main** (the layer chain referencing assets by name).
- **FR-B6 (P1) Content-format manifest.** An effect = a small **manifest** (name, category,
  parameters with ranges + audio-bindable flags) + a GLSL shader — *not* compiled-in code.
  The layer system exposes manifest parameters as knobs automatically. This is the
  shareable community-format win and the drop-in path for Shadertoy shaders. (Dovetails
  with the engine's material expression-register + GLSL stage system.)
- **FR-B7 (P2) Live knob animation.** Driven knobs visibly animate in the UI as routing
  feedback (instant confirmation a link works) — exactly ZGE's affordance.
- **FR-B8 (P0) Layer chrome & menu.** Each layer header carries the exact ZGE affordances
  (see Appendix E): an **A/B/C… enable toggle**, an **effect-preset stepper** (◀/▶ to page
  through that category's presets in place), a **color swatch** (shown when the effect has
  Hue/Sat/Color knobs), a **drag handle** (reorder), and a **layer menu** with
  *Insert / Clone / Move left-right / Rename / Collapse*. Right-click on empty layer area:
  *Collapse all / Expand all / Auto-collapse / Layer-arrangement (text) editor / Save still
  image*. **Collapse** is essential to keep a 20-layer stack navigable.
- **FR-B9 (P0) Per-layer content dropdowns.** At the top of each layer column, the ZGE
  "content selectors": **Image Source** (an asset or a *To-buffer* layer, FR-B3) and
  **Mesh** (for 3D/scene effects, P2). A layer's knobs appear *below* these selectors and
  swap with the chosen effect.
  - **No per-layer "Audio Source" (mixer-track) selector.** In FL Studio, ZGE reads a
    chosen *mixer track's* audio per layer. As a **standalone app we are not an FL plugin**,
    so there is **no multi-track mixer to tap** — getting per-track stems out of FL would
    require a full VST integration, which is **out of scope** (overkill for this product).
    Instead we have **one system audio input** (file playback or WASAPI loopback of the
    final mix) analyzed into buses; **per-knob bus routing (Pillar C)** is how a layer
    "picks its audio," replacing ZGE's per-layer track selector. A single **input picker**
    (which file/device) lives globally in the DEVICES tab, not per layer.
- **FR-B10 (P1) Template Wizard.** A guided "make something good fast" panel (ZGE's
  flagship onboarding): pick a **preset template** (thumbnail grid + randomize **dice**),
  set a **Background** (browse / find-online / *Process* effect dropdown), a **Foreground**
  (color + an X/Y move pad), an optional **title/identification** text block (with move/
  rotate/font/justify controls), choose the **audio source**, then **"Continue to render
  and save video."** This is the fastest on-ramp and the export funnel in one flow; high
  value for the producer persona.
- **FR-B11 Window modes.** ZGE's **Attached/Detached** modes are FL-plugin-host concepts
  (embed in the mixer vs float) that **don't apply** to a standalone app — we are always our
  own window. What carries over: our shipped **fullscreen-per-monitor + windowed** DISPLAY
  tab (already done), plus **max-rate** and **aspect-ratio** controls (P1). **Wallpaper /
  desktop-background render is deferred to M7** (P2) — it's a new engine surface and
  shouldn't compete with the MilkDrop-loading core.
- **FR-B12 (P0) Add-Content panel.** A tabbed asset importer separate from the layer chain
  (ZGE's "Add Content"): **Images** (Add Pictures / Videos / WebCam / Window / Preset / URL
  + *find online* + video-preloading toggles: enable/compress/with-audio/sync-to-song),
  **HTML**, **Text**, **Meshes** (.3ds), **Video cue points**. v1 subset: Images (pictures
  + video), Text, Shaders; defer WebCam/Window/HTML/Meshes/cue-points (FR-E/roadmap).
- **FR-B13 (P2) Global settings panel.** A settings pane mirroring ZGE's: **antialias**
  (incl. render targets), **spectrogram band count** + **internal FFT precision** (already
  our `vis_bands`/FFT size), **internal controllers** (enable + base-offset + level-mult —
  our LFO/routing globals), **MIDI I/O** config (port/channel/learn — the real feature is
  FR-E5), **NDI output** (network video out, P2). DMX and Stereoscopic (skip, non-goals).

### Pillar C — Audio → parameter routing (the crucial part)

> References: `docs/research-zge-visualizer.md` §"Audio → parameter mapping" +
> `docs/research-audiorider.md` (independently validated by a prior audio-reactive
> diffusion-animation project).
> **No bespoke "beat mode":** audio-reactivity is generic modulation routing. Every knob
> can link to a bus. This generalizes the shipped `vis_route` matrix from 5 fixed targets
> to *every parameter of every layer*.

- **FR-C1 (P0) Per-knob links.** Right-click any knob → **Link to source**. A link is:
  **source → smoothing → range-map → parameter.** Multiple links can stack on a knob
  (sum/replace).
- **FR-C2 (P0) Sources.** Audio buses: **7-band split** (Sub-Bass, Bass, Lower-Mid,
  Mid, Upper-Mid, Presence, Brilliance/Ceiling — matches the existing `AudioBand` enum
  and AudioRider's split; MilkDrop's bass/mid/treb is the *floor*, keep finer bands),
  plus RMS, peak/amplitude, per-band **attack envelopes**, raw waveform, **beat events**,
  **LFOs** (Sine/Saw/Square/**3-step pulse**), and **MIDI note/CC** (FR-E5) as first-class
  sources beside audio — LFOs rescue quiet passages where pure reactivity goes static, and
  MIDI lets a controller or sequencer drive visuals directly.
- **FR-C3 (P0) Conditioning per link.** (a) **Smoothing** = asymmetric attack/release
  envelope follower (causal one-pole IIR; real-time replacement for AudioRider's offline
  box convolution — slow motions get long release/inertia, reactive ones short).
  (b) **Range map** with a **bipolar vs unipolar** toggle: signed ranges centered at 0 for
  rotation/translation, positive ranges for zoom/brightness. (c) **Gain/offset**.
- **FR-C4 (P1) Cross-modulation.** Per-link **"×(1 + k·band)"** modifier — the
  highest-value AudioRider trick: a steady LFO base motion whose amplitude is punched by a
  transient band (`translation_z = LFO × (bass+1)`), so every kick lands but motion never
  stalls.
- **FR-C5 (P0) Two-lane audio events.** **Continuous envelopes** drive motion/size/color;
  **discrete beat events** drive palette/preset/shape changes. Beat detector: adaptive
  spectral-flux threshold + refractory period + energy-minimum lookback so flashes land on
  true transients (real-time equivalent of AudioRider's 4-detector ensemble).
- **FR-C6 (P1) Running normalization.** Each band normalized by its long-running average
  (≈1.0 at typical loudness), the MilkDrop approach — causal replacement for AudioRider's
  whole-song min-max normalize.
- **FR-C7 (P2) Default "patch."** Ship the AudioRider tuned bindings as a starter routing
  preset (angle←Sub-Bass, zoom←Bass, translation←Sub-Bass/Lower-Mid, bloom←Bass, etc.),
  so a new scene reacts sensibly out of the box.

> **Reality check (2026-07-18, direct user report):** as shipped today, the modulation
> registry (`VisRegisterModTarget`) has only **8 fixed, global targets**
> (`scale`/`hue`/`bright`/`zoom`/`rotate`/`warpamountmod`/`layerscalemod`/`hueshiftmod`) —
> nowhere near "every slider of every layer" (FR-C1's stated goal). The user's own words:
> *"I'd like all sliders/options to be able to be controlled by audio levels or midi
> input... we really want to do everything that ZGE can do in FL Studio."* Closing this
> gap for real (a per-layer-instance target registry, not a handful of global cvars) is
> its own multi-milestone effort — tracked here as **FR-C1-EXPAND**, not assumed done just
> because FR-C1 exists as a requirement. Two other real, reported gaps closed alongside
> this update (see `PRD-implementation-status.md`'s changelog for the fix writeups):
> `vis_effect` switching/reactivity going invisible whenever a map was also loaded, and the
> map flythrough camera having always been a fixed, non-audio-reactive speed constant
> (never regressed — never built).

- **FR-C1-EXPAND (P0) Universal parameter binding.** Every numeric GUI slider/knob in
  every panel — not just the current 8 — gets a modulation target: layer opacity/blend
  mode index, particle/starfield/spectrogram per-effect params, warp mesh grid
  density, feedback zoom/decay, bloom threshold/intensity, the map flythrough camera's
  speed/height/FOV (closes the "camera not reactive" report), MilkDrop per-preset `q1-q32`/
  custom-shape params, video-export params excluded (one-shot, not live). Mechanically:
  extend `VisRegisterModTarget` (or a successor registry) to take an **owner handle**
  (layer-stack slot index, or -1 for global) so two instances of the same effect type
  don't fight over one shared target, mirroring the per-instance state pattern
  `visStackParticleState_t`/`visStackStarState_t`/`visStackSpectroState_t` already
  established for effect state.
- **FR-C8 (P1) Curve/response shaping per link.** Direct user request: *"allows for
  curves or smoothing to be applied to values (formulas) so that 1 to 100% can become a
  boolean or tapered in some fashion."* FR-C3's existing (a) smoothing / (b) range-map /
  (c) gain-offset chain is **linear-only** today — add a **shape** stage between range-map
  and gain/offset, picked per-link from: **Linear** (identity, default) · **Exponential**
  (`x^k`, k configurable, for punchier low-end response) · **Logarithmic** (`log(1+k·x)/
  log(1+k)`, for perceptual/loudness-style compression) · **S-curve/smoothstep** (soft
  knee, avoids harsh on/off snapping) · **Threshold/Boolean** (configurable threshold +
  hysteresis band, output is 0 or 1 — "1 to 100% becomes a boolean," exactly as asked) ·
  **Quantize/Step** (N discrete levels, for stepped/strobing looks) · **Invert** (`1-x`,
  composable with any of the above, not a separate exclusive mode). Each shape is a pure
  function of the post-smoothing, post-range-map value — composable with FR-C4's
  cross-modulation multiplier, not a replacement for it.
- **FR-C9 (P2) DMX output.** Direct user request (2026-07-18), reversing this PRD's
  earlier "DMX out of scope" non-goal. ZGE's own DMX feature (confirmed from the FL
  Studio manual): *"Enable DMX output - Connects ZGameEditor to your DMX device. DMX is
  the equivalent of MIDI, for lighting control,"* exposed as its own **Hardware** effect
  category and a **Settings > Enable DMX output** device-selection dropdown ("several DMX
  output devices, with slightly different computer communication protocols"). Our analog:
  reuse FR-C1-EXPAND's modulation registry as the *source* of DMX channel values (any
  target — hue, brightness, a specific band level, a MilkDrop preset's beat state — can
  be assigned to any of 512 DMX channels/an addressable fixture), and add DMX as an
  **output** transport alongside the existing MIDI *input* path (FR-E5). Recommended
  transport: **Art-Net or sACN (E1.31) over UDP** — no vendor-specific USB driver needed,
  works with any Art-Net/sACN-compatible DMX gateway (the modern standard for
  software-driven lighting, and avoids per-vendor "several output devices, slightly
  different protocols" the FL plugin itself has to special-case). A raw serial DMX-512
  (RS-485/USB-DMX dongle) transport is a reasonable P3 follow-up for users without a
  network gateway, but Art-Net/sACN alone covers the mainstream use case. GUI: a new
  "DMX" panel section (parallel to the existing MIDI section) with per-channel
  target-assignment rows (reusing FR-C1-EXPAND's link UI) and a device/protocol picker.
- **FR-C10 (P2) ShaderToy import.** Direct user request, with
  `https://www.shadertoy.com/view/MsjBDR` as a concrete example. ShaderToy shaders are
  **already plain GLSL** (unlike MilkDrop's HLSL-flavored `warp_`/`comp_` code, which
  needs the M3 `hlsl2glslfork` transpiler) — the only work is adapting ShaderToy's
  fixed-function wrapper convention to ours: a ShaderToy shader defines
  `void mainImage( out vec4 fragColor, in vec2 fragCoord )` and expects a small fixed set
  of uniforms (`iResolution`, `iTime`, `iTimeDelta`, `iFrame`, `iMouse`, `iChannel0-3` +
  `iChannelResolution`/`iChannelTime`, `iDate`); import is: paste/fetch the shader body,
  wrap it in a real `main()` that supplies those uniforms (mapping `iChannel0` to this
  app's existing feedback/image-layer texture slots where a shader expects an input
  buffer) and calls `mainImage(gl_FragColor, gl_FragCoord.xy)`, then compile through the
  existing GLSL shader-quad layer type (already listed as "Shadertoy-import-friendly" in
  §4 Pillar B) — no new transpiler needed, just the uniform-adapter wrapper and (P2) a
  fetch-by-URL helper (ShaderToy exposes shader source via its public API given a shader
  ID, e.g. `MsjBDR` from the URL path) since not every user will want to copy-paste GLSL
  by hand.

### Pillar D — MilkDrop 3 keyboard shortcuts

> Reference: MilkDrop 3.28 hotkey chart (user-supplied). Match the **subset that maps onto
> features we have**; F1 stays our menu (MilkDrop's F1 is help). Global while no modal UI
> owns input; handled in `MenuProcessEvent` after the console. **SPACE/BACKSPACE/N already
> ship.** Extends the shipped hotkey layer.

- **FR-D1 (P0) Preset navigation.** `SPACE`/`H` next preset · `BACKSPACE` prev ·
  `` ` ``/`~` lock preset · `R` random/shuffle order · `L` load specific (opens browser).
- **FR-D2 (P0) Preset lifecycle.** `S` save · `O` reload current · `N` show info/toggle
  effect (see note) · `M` menu.
- **FR-D3 (P1) Look controls.** `C`/`SHIFT+C` random/next palette (maps to `vis_palette`) ·
  `U` flip · `P` pattern · `D` warp/comp lock · `F11` toggle effects on/off.
- **FR-D4 (P1) Function row.** `F2` max-FPS toggle · `F3` time-between-presets ·
  `F4` show preset name · `F5` show FPS · `F6` rating · `F7` always-on-top/borderless ·
  `F8` auto-cut on music (beat) · `F9` double-preset (mash-up).
- **FR-D5 (P1) Player transport.** `CTRL+←/→` prev/next track · `CTRL+↑/↓` seek.
- **FR-D6 (P2) Mash-ups & sprites.** `A`/`Z` mash-up blend · `F` fullscreen mode (single/
  double/all monitors) · sprites (`SHIFT+K`, `00–99`, `DELETE`) — lowest priority, no
  current analog.

- **FR-D7 (P1) ZGE layer-editing shortcuts (editor context).** Distinct from the MilkDrop
  *performance* hotkeys, ZGE's plugin uses a **layer-manipulation** set that is active when
  the **editor has focus**: `←/→` select layer · `Shift+←/→` move layer · `C` collapse/
  expand · `Ctrl+C`/`Ctrl+V` copy/paste layer · `S` solo layer · `Del` delete layer ·
  `Ins` insert layer · `X` export video. Adopt these for the layer editor.

> **Two shortcut contexts (design rule).** There are two hotkey layers that must
> **coexist**: (1) MilkDrop-3 *performance* hotkeys (preset nav/lifecycle, FR-D1–D6),
> global while playing; (2) ZGE *editor* hotkeys (layer manipulation, FR-D7), active only
> when the layer editor is focused/open. Route keys by focus: editor-open → ZGE set wins;
> performance/fullscreen → MilkDrop set. This resolves the collisions (`S`
> save-preset vs solo-layer, `C` color vs collapse, `Del` erase vs delete-layer, `N`
> next-effect vs info). **Shipped `N` = next effect; MilkDrop `N` = show info** — keep
> `N`=next-effect in built-in performance mode; when a MilkDrop preset is the active layer,
> `N`=info. Full mapping table lives in `plans/visual-effects-roadmap.md §Keyboard
> shortcuts` (source of truth); this PRD sets priority + context routing.

### Pillar E — Media, devices, display, export

- **FR-E1 (P0) Audio input.** File (all FFmpeg formats — shipped), playlist (`.m3u`),
  and **system/device loopback** (WASAPI, auto-arm — shipped). Device picker enumerates
  render/capture endpoints (shipped).
- **FR-E2 (P0) Display.** Multi-monitor fullscreen (per-monitor native) + windowed presets
  (shipped DISPLAY tab). `F` single/double/all-monitor modes (FR-D6).
- **FR-E3 (P1) Video export.** Offline, frame-accurate renderer decoupled from live preview
  (ZGE model). Export dialog mirrors ZGE's exactly: **destination presets**
  (*YouTube HD 1920×1080*, *YouTube 4K 3840×2160*, *Instagram 1080×1920*, *TikTok
  1080×1920*) + **Advanced**: resolution (custom via right-click), **bitrate** slider,
  **uncompressed** checkbox, **2× supersample** checkbox, **fps 30/60**, **audio codec**
  (default/FLAC) + audio bitrate, and **"use final (post-master) audio."** Buttons: OK /
  START. Encode via the already-linked FFmpeg libs. Reachable both standalone and as the
  Wizard's final step (FR-B10).
- **FR-E4 (P2) Screen recording.** Live capture of the display to file (documented in
  roadmap backlog) as a simpler alternative to offline export.
- **FR-E5 (P1) MIDI I/O.** Kept as a first-class feature (not the FL-plugin mixer tap).
  - **MIDI in:** open a MIDI input port; incoming **notes/CC/pitch-bend** become routing
    **sources** (FR-C2) — map a CC to any knob, trigger preset/effect/palette changes on
    notes, use note velocity as an envelope. This is our version of ZGE's "MIDI Input
    Control" (§E.2). Config: port select + channel filter + a learn ("MIDI-learn") gesture
    on any knob.
  - **MIDI out:** send **notes/CC/clock** derived from the analysis (e.g., beat → note,
    a band envelope → CC, transport → MIDI clock) so the visualizer can **drive external
    gear** — lighting rigs, sequencers, other MilkDrop instances — or sync to a DAW.
  - Windows: use the standard multimedia MIDI API (winmm `midiIn*/midiOut*`) or RtMidi;
    no external SDK. Purely additive to the audio pipeline; degrade gracefully if no MIDI
    device is present.

### Pillar F — DOOM 3: BFG map flythrough ambience (our unique differentiator)

> No MilkDrop/projectM/ZGE clone can do this, because none of them are a game engine —
> we are. This is an **optional** layer type (ZGE's "Scenes: 3D fly-throughs" category,
> which the rest of this PRD otherwise skips) that turns a real, already-installed DOOM 3
> BFG map — retail **or a modder's custom map** — into an audio-reactive 3D backdrop with a
> camera that flies through it. Strictly **opt-in and additive**: it activates only if the
> user points the app at game/mod data they already own or have separately downloaded; the
> dataless-boot default (Pillars A-E) is completely unaffected and requires nothing from
> this pillar. Composites with the existing 2D pipeline exactly like today's effects do —
> feedback trail/warp/bloom/kaleidoscope/palette (Pillar B processor layers) already work
> on **whatever is drawn to the screen**, so a 3D scene becomes just another thing they
> process, no special-casing needed downstream.

- **FR-F1 (P1) Opt-in map data, standard idTech4 mod selection.** **Corrected after
  checking the file-system internals directly** (same discipline as the `AddResourceFile`
  finding): `idFileSystem`'s `gamedir` parameter (on `ListFilesTree`/`OpenFileRead`/etc.)
  only **filters against search paths already registered at boot** — it can't add a brand
  new, previously-unknown mod directory at runtime. The engine-internal
  `AddGameDirectory`/`SetupGameDirectories` that actually register a mod directory are
  called once, at `idFileSystemLocal::Init()`, driven by the `fs_game`/`fs_game_base`
  cvars — and, like `AddResourceFile`, are **not** exposed on the public `idFileSystem`
  interface. So a live, in-app "**+ Add mod folder**" button that registers arbitrary new
  directories on the fly is **not** a small addition — it needs either a new public
  `idFileSystem` method, or accepting idTech4's standard flow instead: **launch (or
  restart) with `+set fs_game <moddir>`** to select which mod/map-pack is active, exactly
  how DOOM 3 mods have always been chosen. **Revised v1 scope**: the MAPS tab discovers
  and lists maps from **whichever single game directory is already active** (`base/`, or
  the mod named by `fs_game` at launch) — a **DISPLAY-tab-adjacent "MAPS" picker entry**
  lists what that directory's `maps/*.resources`/loose `.proc` files offer. Switching
  between the retail campaign and a specific downloaded mod is a **launch-time choice**
  (`+set fs_game <moddir>`), not a **live multi-source browser** — that richer "several
  registered sources side by side" UX from the original FR-F1 draft becomes a **P2
  follow-up** gated on adding that one new public `idFileSystem` method, not a v1
  requirement. This still fully supports ModDB-downloaded mods (`moddb.com/games/
  doom-iii/addons`) laid out in the standard `fs_game` mod-directory convention — just
  selected at launch rather than added from within a running session.
  **Nothing is bundled, downloaded, or required** — this only reads files the user
  already has or has separately fetched themselves; we never fetch, mirror, or link
  directly to any specific ModDB file.
  - **Format-compatibility caveat:** many ModDB DOOM 3 addons target the **original 2004
    DOOM 3**, not BFG Edition, which changed some asset formats (materials/animations) and
    renderer behavior. BFG-native mods/maps (or the subset of classic mods already ported/
    compatible) are the safe-bet target; full retro-DOOM3-mod compatibility is **not
    promised** — flagged as a real risk (see §8) rather than overclaimed here. Since FR-F2
    loads geometry only (no scripts/entity defs/AI), most *gameplay*-side incompatibilities
    a classic mod would hit simply don't apply to us — only asset-format mismatches do.
- **FR-F2 (P0-if-built) Map geometry load, no gameplay.** Load a map's **render geometry
  only** — `idRenderWorld`'s world model, portals/areas, and static light defs — without
  spawning game entities, AI, or running game logic. This is a rendering-only path (closer
  to idTech4's model-viewer/renderdemo tooling than to `idGameLocal::LoadMap`), so it's
  cheap, stable, and immune to gameplay-side crashes (broken scripts, missing entity defs,
  monster AI, etc.) — we only need the *level geometry and lights to look at*, not a
  playable level.
- **FR-F3 (P0-if-built) Portal-graph camera flythrough.** idTech4's BSP **areas + portals**
  are already a navigable graph connecting every room to its neighbors through a walkable
  opening — reuse them directly as the flythrough path graph instead of hand-authoring
  waypoints or running expensive navmesh generation:
  1. Build a simple graph: one node per area, one edge per portal.
  2. Pick a route (random walk, or longest-path for a "tour" of the whole map) and turn
     the sequence of portal-center points into a smooth spline (Catmull-Rom or similar).
  3. Fly the camera along the spline at a height offset above the floor, with **line-of-
     sight/collision checks** (`idClipModel`/`CM_` trace calls the engine already exposes)
     nudging the path away from walls — cheap and robust versus hand-tuning per map.
  4. On reaching the end (or periodically), pick a new route so the tour never repeats
     identically and never gets stuck.
  This is deliberately **procedural, not scripted per-map** — it must work on *any* loaded
  map with zero per-map authoring, which is the only way this feature scales across a
  user's whole map collection.
- **FR-F4 (P1) Audio-reactive camera & scene.** The camera/scene become routing **targets**
  (Pillar C, reusing the same registry FR-C1/M0 just generalized): fly-speed pulses with
  RMS/bass, FOV punches in on beat, subtle camera shake keyed to treble transients, dynamic
  light intensity/color keyed to bands (relighting the map itself to the music), and
  particle/fog density keyed to spectral energy. Same "every parameter is a knob" model as
  the rest of the product — no bespoke camera-animation code path.
  > **Status confirmed 2026-07-18 (direct user report: "camera... doesn't seem to be
  > reactive to the audio anymore"):** this was never actually built, not a regression —
  > `MILK_CAM_SPEED` in `VisUpdateMilkCamera` (`neo/sound/visualizer_manager.cpp`) is a
  > fixed compile-time constant with zero modulation-registry involvement. Blocked on
  > FR-C1-EXPAND (the camera speed/FOV/light-intensity targets need to exist in the
  > registry before they can be routed to).
- **FR-F5 (P1) Composability with the 2D/post pipeline.** The 3D scene render becomes the
  base layer the existing processor stack (feedback trail, warp, bloom, kaleidoscope,
  palette) operates on, via the same `CaptureRenderToImage` composite mechanism already
  used for the 2D overlay — so a MilkDrop-style feedback trail over a DOOM 3 hallway is a
  natural, low-effort combination, not a separate rendering mode to maintain.
- **FR-F6 (P2) Map browser + per-map settings.** MAPS tab lists every map discovered in
  the **currently active** game directory (`base/`, or whichever mod `fs_game` named at
  launch — see FR-F1's revised scope); per-map saved camera-tour seed/speed/height-offset
  overrides (most maps should need zero tuning given FR-F3's collision-safe fallback, but
  some benefit from a manual height/speed nudge, and a map that fails to load geometry
  cleanly is skipped with a log line, never a crash). **Grouping by multiple simultaneous
  sources** (the original draft's "base game, then each mod folder" list) becomes relevant
  only once the FR-F1 P2 follow-up (new public `idFileSystem` method for live directory
  registration) lands — until then there's exactly one active source per session, so
  there's nothing to group.

**Acceptance (F):** Launch (or restart) with a real DOOM 3: BFG install active (optionally
`+set fs_game <moddir>` for a specific mod); MAPS tab lists that directory's maps;
selecting one loads geometry-only (no entity/AI errors) and flies a collision-safe camera
tour indefinitely without getting stuck in geometry, reactive to whatever audio is playing,
composable with the existing bloom/feedback/kaleidoscope processors.

**Milestone placement:** independent of the MilkDrop preset work (Pillars A-D) — scheduled
as **M4.5**, right after the layer-editor milestone (M4) since map flythrough is one more
layer *type* in that same stack, and before M5-M7. See §7 milestones.

---

## 5. Non-functional requirements

- **NFR-1 (P0) Dataless boot.** Runs with generated assets only; no retail DOOM data.
  Regenerable via `scripts/generate_boot_assets.py`.
- **NFR-2 (P0) Performance.** 60 fps @ 1080p for typical presets; graceful degradation
  (mesh size, blur taps, FBO resolution scale) on weaker GPUs. Hardware tiering at setup
  (MD3-style). Preset load must not stall the render thread >1 frame (compile off the
  draw worker; decl/material creation stays main-thread — a known engine constraint).
- **NFR-3 (P0) Robustness.** Community presets are inconsistent; a bad preset **logs and
  is skipped**, never crashes. `idMath::ATan(0,0)` and similar asserts must be guarded.
- **NFR-4 (P0) SMP correctness.** Respect the engine's threading split: Draw2D on the
  draw-worker thread (read-only cached state), Render()/Frame() + `declManager->FindMaterial`
  on the main thread. Routing evaluated once/frame on main, cached for the worker.
- **NFR-5 (P1) Platforms.** Windows x64 primary (Debug shipped; Release build is a known
  blocker — sub-project Release configs need repair, see roadmap). Build workflow: Mac edit
  → push → Windows pull → force-delete changed `.obj` → MSBuild.
- **NFR-6 (P1) Licensing.** `projectm-eval` (clean-room EEL2) and preset packs carry their
  own licenses; vendor with attribution. MIT/compatible only in-tree.

---

## 6. Architecture summary

```
                 ┌────────────────────────── idVisualizerManager (owner) ──────────────────────────┐
Audio in         │                                                                                   │
(file/loopback)  │   AudioAnalyzer ──► buses: 7 bands, *_att, RMS, peak, waveform, beat, LFOs        │
   │             │        │                                                                          │
   ▼             │        ▼                                                                          │
XAudio2/WASAPI ──┼──►  Routing matrix  (per-knob: source→smoothing→range→param, ×(1+k·band))         │
   FFmpeg        │        │                                                                          │
                 │        ▼                                                                          │
                 │   Layer stack  [ bg ▸ … ▸ fg ]  each = source|processor, knobs bound to routing   │
                 │        │              ├─ MilkDrop layer ─► [.milk parser → projectm-eval VM →      │
                 │        │              │                     warp/shape/wave/blur/comp GLSL passes] │
                 │        │              └─ built-in / shader / image / text / particle / post        │
                 │        ▼                                                                          │
                 │   Compositor (FBO ping-pong RGBA16F, "to buffer" snapshots) ─► screen / export    │
                 │        ▲                                                                          │
                 │   GUI: ImGui layer+knob editor · F1 picker · MD3 hotkeys · HUD                    │
                 └───────────────────────────────────────────────────────────────────────────────────┘
```

**Key decisions (carried from research):**
- **Embedding:** vendor `projectm-eval` for math; native idTech GLSL passes for the
  pipeline; our own FFT feeds `projectm_pcm`-equivalent buffers. No libprojectM renderer.
- **GUI:** ImGui for the rich editor (v1) → `.gui`/SWF re-skin later. Console `vis_*`
  cvars remain the testable substrate under every feature.
- **The shipped effect becomes the substrate:** feedback/warp-mesh/bloom/kaleido are the
  processor layers *and* the MilkDrop pipeline's building blocks — no throwaway work.

---

## 7. Milestones (phased)

Each milestone is shippable and demoable. Ordered to de-risk the hardest pillar (A) early
while reusing shipped infrastructure.

- **M0 — Foundation refactor (P0).** Generalize the shipped modulation matrix into a
  per-parameter routing table; introduce the `Layer` abstraction wrapping the existing
  effects/processors as layer types. No new visuals; parity with today via a default
  single-layer stack. ImGui debug panel exposing layers + routing.
- **M1 — MilkDrop equation presets (P0).** `.milk` parser + vendored `projectm-eval` +
  native warp/wave/shape/border/composite passes (no custom shaders). Texture pack.
  Playlist over `base/presets/milk/`. **Milestone demo:** cycle 100 classic presets with
  SPACE. This is the product-defining moment.
- **M2 — Preset UX parity (P0).** Full MD3 hotkeys (FR-D), soft/hard cuts, lock/shuffle/
  rate, auto-advance on timer/beat, preset browser (`L`). Two-lane beat events wired to
  cuts.
- **M3 — HLSL shader presets (P1).** `warp_`/`comp_` HLSL→GLSL transpiler + uniform/sampler
  preamble → unlocks MilkDrop2/3 + the big shader-heavy packs (Cream, MegaPack).
- **M4 — ZGE layer editor + MIDI (P1).** Full layer stack UI (assets/main tabs, category
  dropdown, reorder/solo/alpha/blend, "to buffer"), Polar spectrum layer, shader-manifest
  format, Shadertoy import (**FR-C10**). MilkDrop preset as one layer among many.
  **MIDI I/O (FR-E5):** MIDI-in as routing sources + MIDI-learn on knobs; MIDI-out (beat/
  CC/clock) for external sync. **Follow-up added 2026-07-18** (direct user report the
  actual coverage falls well short of "every slider"): **FR-C1-EXPAND** (per-layer-
  instance target registry, not today's 8 fixed globals), **FR-C8** (curve/response
  shaping — exponential/log/S-curve/threshold-boolean/quantize/invert, not just linear
  range-map), **FR-C9** (DMX output, reversing this PRD's earlier non-goal).
- **M4.5 — DOOM 3: BFG map flythrough (P1, Pillar F).** Opt-in retail-data picker (FR-F1),
  geometry-only map load via `idRenderWorld` (FR-F2), portal-graph camera flythrough with
  collision-safe spline pathing (FR-F3), audio-reactive camera/lighting via the existing
  routing registry (FR-F4), composability with the processor stack (FR-F5). Independent of
  Pillars A-D — can start in parallel with M1-M3 if capacity allows, since it shares no
  code with the MilkDrop parser/eval path. Demo: point at a real install, watch an
  indefinite audio-reactive tour of a map with bloom/feedback layered on top.
- **M5 — MilkDrop3 mash-ups (P1).** `.milk2` double-presets/mash-ups, 16 waves/shapes,
  500-side shapes, full FFT in shaders.
- **M6 — Video export (P1).** Offline frame-accurate renderer + destination presets via
  FFmpeg encode.
- **M7 — Polish (P2).** `.gui`/SWF re-skin of the editor, ratings persistence, sprites,
  always-on-top/borderless, **Wallpaper/desktop-background mode (FR-B11)**, screen
  recording, NDI output, Release build repair.

---

## 8. Risks & mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| HLSL→GLSL transpile is hard (MilkDrop HLSL ≠ our dialect) | Shader presets blocked | Ship equation-only first (M1); transpiler is its own milestone (M3); start with simplest presets. **Update**: a real, mature reference transpiler exists (`aras-p/hlsl2glslfork`, BSD, used in Unity for years — the library Butterchurn's own MilkDrop shader converter wraps) plus real ground-truth converted shaders (`jberg/butterchurn-presets`, MIT) to validate against — see `plans/PRD-implementation-status.md` M3. |
| `projectm-eval` integration friction (build/link, megabuf locks) | M1 slips | Vendor the self-contained C lib; it has no GL deps; host provides lock callbacks per its README |
| Engine SMP/main-thread constraints on decl/material creation | Stalls/crashes on preset load | Compile eqs off-thread, do material/texture creation main-thread; guard asserts (NFR-3/4) |
| Release build blocker (sub-project configs) | No perf build for heavy presets | Known issue; repair sub-project Release configs (M7); Debug adequate for M0–M2 |
| 130k-preset packs: many broken/incompatible | Bad UX if failures crash | Robust skip-and-log; rating/curation; ship a curated default subset |
| ImGui not present/wired in this build variant | Editor path blocked | **Confirmed NOT present in this fork** (direct search, `plans/PRD-implementation-status.md` M4) — the "RBDOOM port carries it" assumption was checked and found false for this specific fork. Not a blocker in practice: vendor fresh from `ocornut/imgui` (MIT) — mechanical, since real internet access is available this session, not a from-memory reproduction risk. Fall back to `.gui` + cvars remains the option if vendoring is ever undesirable. |
| Map load pulls in gameplay/entity dependencies we don't want (crashes on broken scripts/AI) | M4.5 unstable/blocked | Geometry-only load path (FR-F2), not `idGameLocal::LoadMap`; skip entity spawn/AI entirely |
| Some maps have geometry the portal-graph camera can't safely path (tight corridors, vertical shafts) | Camera gets stuck/clips walls on some maps | Collision-safe spline nudging (FR-F3.3) + a manual per-map override as a P2 escape hatch (FR-F6) |
| Users without a DOOM 3: BFG install can't test M4.5 | Feature invisible without owned data | Strictly optional (FR-F1); every other pillar demos fully without it; document clearly it needs owned retail data |
| Community/ModDB maps target classic DOOM 3, not BFG's asset formats | Some mod maps fail to load or render wrong | Geometry-only load sidesteps *gameplay* incompatibilities (FR-F2); asset-format mismatches are logged and skipped per-map (FR-F6), not promised to all work; document BFG-native mods as the safe-bet target |
| Scope: this is a large multi-quarter effort | Never ships | Milestones are individually shippable; M1 alone is a compelling product |

---

## 9. Open questions

1. **`N` key conflict** (next-effect vs MilkDrop info) — final mapping? (FR-D2 note.)
2. **ImGui availability** in the current dataless build — **answered: not present** (direct search confirmed zero ImGui files/references in `neo/`, contradicting this doc's earlier "RBDOOM port already carries it" assumption, now corrected above). Not vendored yet; see `plans/PRD-implementation-status.md` M4 for the revised plan (vendor fresh from `ocornut/imgui`, MIT, real internet access confirmed available).
3. **Preset storage layout** — `base/presets/milk/<pack>/…` vs flat; how ratings/locks
   persist (sidecar file vs db).
4. **Half-float FBO support** — **answered, with a bigger catch than expected.**
   No `FMT_RGBA16F` existed anywhere in the renderer; added it (`ImageOpts.h`,
   `Image_load.cpp` `BitsForFormat`/`CopyFramebuffer`, `gl_Image.cpp`
   `AllocImage`), so a half-float texture **can** now be allocated and
   `CopyFramebuffer` will honor it instead of hardcoding `GL_RGBA8`. **But this
   alone does not fix MilkDrop-style banding**: `CopyFramebuffer` always reads
   from `GL_BACK` (`qglReadBuffer(GL_BACK)`) — the window's default 8-bit
   backbuffer — so every feedback generation is re-quantized to 8 bits at
   capture time regardless of the destination texture's format. This engine
   has **no FBO render-target abstraction at all** (confirmed by direct
   search); the visualizer draws straight to the default backbuffer every
   frame. A genuine fix needs an actual off-screen half-float FBO the
   visualizer renders *into* instead of the window backbuffer, with a final
   blit-to-8-bit only at present time — a real, standalone piece of engine
   work, not a format-enum tweak. Tracked as a new explicit M1 sub-step (see
   `plans/PRD-implementation-status.md`); the format plumbing landed now
   because it's a correct, low-risk prerequisite either way.
5. **Video export threading** — reuse FFmpeg encode libs already linked; offline vs live
   pipeline sharing.
6. **How much ZGE `.zgeproj` compatibility?** Research says ZGE presets are component
   trees, not equations — likely *not* loaded directly (out of scope), only the *UX model*
   is borrowed. Confirm this stays a non-goal.
7. **Map source format** (FR-F2) — **answered.** Both loose `.proc` files and packed
   `.resources` containers exist and resolve through the same `idFileSystem` read API
   (`OpenFileReadMemory` checks mounted containers, falls back to loose files) — so
   `idRenderWorldLocal::InitFromMap` works identically either way, **once the map's
   container is mounted**. The catch: per-map containers (`base/maps/<name>.resources`)
   are **not** auto-mounted at boot like the shared `_common.resources`/`_ordered.resources`
   — only `idFileSystemLocal::BeginLevelLoad` (today called only from the real game-load
   path) calls `AddResourceFile()` to mount one. **Correction after double-checking the
   public interface directly**: `AddResourceFile` is **not** part of the abstract
   `idFileSystem` interface (`neo/framework/FileSystem.h`) at all — it's
   `idFileSystemLocal`-internal, unreachable from outside code (like
   `visualizer_manager.cpp`) without adding a new public virtual method to `idFileSystem`
   (an engine-interface change, more invasive than first assumed).
   **Superseded correction** (checked `GetFileList`'s implementation body, not just
   `gamedir`'s signature, one pass later): the "sidestep via the public `gamedir`
   parameter" idea below turned out to be equally blocked — `gamedir` only *filters*
   search paths **already registered at boot**; it can't add a previously-unknown
   directory at runtime either. See FR-F1 (revised) for the actual v1 scope: work against
   whichever single game directory is already active (`base/`, or a mod chosen at launch
   via `+set fs_game <moddir>`, idTech4's standard mechanism) — a live in-app "add a new
   source" UX needs the same new `idFileSystem` method the packed-`.resources` case needs,
   folded into one combined P2 follow-up rather than treated as a solved problem.
   Also: `ListFilesTree` only sees the OS filesystem, not container contents — so map
   *discovery* (the MAPS tab, FR-F6) must list `maps/*.resources` filenames directly
   (the engine's own CRC-generation code already does exactly this) rather than expecting
   `.proc`/`.map` files to show up in a directory listing.
8. **Portal-graph coverage** (FR-F3) — **answered: yes, fully public, no gameplay
   bootstrap needed.** `idRenderWorld` (`neo/renderer/RenderWorld.h`) exposes `InitFromMap`,
   `NumAreas`, `NumPortalsInArea`, `GetPortal` (→ `exitPortal_t{areas[2], winding, ...}`),
   `AreasAreConnected` — all public, zero game/session types in the header, and
   `InitFromMap`'s implementation (`RenderWorld_load.cpp`) never touches
   `collisionModelManager`/`gameLocal`/`session`. A render-only precedent already exists
   in this codebase: `neo/ui/RenderWindow.cpp` calls `renderSystem->AllocRenderWorld()` +
   `world->InitFromMap(NULL)` directly, outside `idSessionLocal`/`idGameLocal::LoadMap`
   entirely — confirming FR-F2's "geometry-only, no gameplay" design is not just possible
   but already has a working pattern to follow in this exact fork.

---

## 10. Success metrics

- **Compatibility:** ≥80% of a random Cream-of-the-Crop sample renders recognizably (M1);
  ≥90% including shader presets (M3).
- **Performance:** 60 fps @ 1080p typical preset; ≥45 fps @ 1080p worst-case classic preset.
- **UX:** a MilkDrop user can drive it with muscle memory (hotkey parity, FR-D).
- **Authoring:** a producer can build a 3-layer audio-reactive scene and export a 1080p60
  video without touching a config file (M4 + M6).
- **Map ambience (M4.5):** given a real DOOM 3: BFG install, a 10-minute unattended tour of
  any retail map completes with zero camera stuck-in-geometry incidents and visibly
  audio-reactive lighting/camera motion.
- **Stability:** zero crashes across a 1-hour auto-cycle over a mixed 1000-preset playlist.

---

## Appendix A — MilkDrop 3 hotkey mapping (priority view)

Full table (shipped + candidate) is in `plans/visual-effects-roadmap.md §Keyboard
shortcuts`. Priority for this PRD: **P0** SPACE/BACKSPACE/`` ` ``/R/L/S/O/M (preset nav +
lifecycle, FR-D1/D2); **P1** C/U/P/D/F11 + F2–F9 + CTRL+arrows (look + function row +
transport, FR-D3/D4/D5); **P2** A/Z/F/sprites (mash-up/fullscreen/sprites, FR-D6).

## Appendix B — `.milk` format quick reference

See `docs/research-milkdrop-projectm.md §1–2` for the authoritative breakdown (scalar
params, `per_frame_init_`/`per_frame_`/`per_pixel_`, `[wavecode_NN]`/`[shapecode_NN]`,
`warp_`/`comp_` shaders, variable pools + `q`/`t`/`reg`/`gmegabuf` bridges, audio built-ins,
shader uniform/sampler contract, and the exact 9-stage render pass order).

## Appendix C — Preset & texture sources

Catalogued in `plans/visual-effects-roadmap.md §"Load external presets"`: projectM test
corpus, texture pack, Cream of the Crop, classic projectM, MilkDrop2 originals, En D,
classic-collections bundle, and the 130k MegaPack, with per-pack notes on shader dependence.

## Appendix D — Reference docs

- `docs/research-zge-visualizer.md` — ZGE layer model, 17 categories, routing, export.
- `docs/research-milkdrop-projectm.md` — `.milk` format, pipeline, projectM API, embedding.
- `docs/research-audiorider.md` — multiband + LFO + per-link source/smoothing/gain,
  bipolar/unipolar, cross-modulation, two-lane beat.
- `plans/visualizer-ui.md` — GUI tech decision (ImGui → `.gui` → SWF), screen designs.
- `plans/visual-effects-roadmap.md` — running task log + shipped-feature reference.
- `docs/engine-map.md`, `docs/research-rbdoom.md`, `docs/audit-existing-visualizer-code.md`
  — engine internals, RBDOOM modernizations (incl. ImGui), prior-code audit.

---

## Appendix E — FL Studio ZGE Visualizer UI anatomy (verified from the manual)

Captured from the FL Studio online manual (ZGameEditor Visualizer page + its screenshots:
`ZgameEditorVisualizer.png`, `..._LayerArranger.png`, `..._Wizard.png`,
`..._VideoSettings.png`, `..._Content.png`, `..._Settings.png`). This is the concrete UI
we're cloning. **Note:** the manual's numbered links **01–17** are ZGameEditor *engine*
tutorial videos (Getting Started, Project Tree, Writing Expressions, Import 3DS, …) on
YouTube — background on the underlying engine, *not* the effect categories. The **17
effect categories** are a separate list (below).

### E.1 Screen regions

1. **Top toolbar:** *Rotate* slider · **Window mode** {Attached, Detached, Wallpaper} ·
   **Max rate** {30, 60 Hz} · **Aspect ratio** {16:9, 4:9} · Wizard button · Export button.
2. **Layer strip (the core):** horizontal slots, **left = background → right = foreground**,
   up to 100. Each layer column top-to-bottom:
   - **A/B/C… enable toggle** (header).
   - **Icon row:** effect-preset **stepper ◀▶** · **color swatch** (only if effect exposes
     Hue/Sat/Color) · **move/drag handle** · **layer-menu ▾**.
   - **Content selectors:** *Audio Source* (mixer track) · *Image Source* (asset or
     To-buffer layer) · *Mesh* (from Add Content › Meshes).
   - **Effect knobs:** swap in based on the chosen effect.
   - **Layer menu ▾:** Insert · Clone · Move left/right · Rename · Collapse.
   - **Right-click empty area:** Collapse all · Expand all · Auto-collapse · Layer-
     arrangement editor (Alt+L, text) · Save still image · Show background pattern.
3. **Effect picker** (per layer, grouped): the **17 categories** — Background · Foreground ·
   Blend · Canvas effects · Feedback · Hardware · Image effects · Misc · Object Arrays ·
   Particles · Peak effects · Physics · Postprocess · Scenes · Terrain · Text · Tunnel.
4. **Add Content panel** (tabbed asset library, separate from the layer chain):
   *Images* (Add Pictures/Videos/WebCam/Window/Preset/URL, find-online, filter, video-
   preloading toggles) · *HTML* · *Text* · *Meshes (.3ds)* · *Video cue points*.
   Row actions: Select all · Remove · Remove all · Replace · Remove unused.
5. **Template Wizard:** Presets (thumbnails, Save, **randomize dice**) · Background
   (find-online/Pexels, Browse, *Process* dropdown) · Foreground (color brush, X/Y move
   pad) · Identification (title text w/ move/rotate/font/justify/center/undo) · Audio
   source (+ play/stop) · **"Continue to render and save video."**
6. **Video settings dialog:** presets {YouTube HD, YouTube 4K, Instagram, TikTok} + Advanced
   {resolution, bitrate, uncompressed, 2× supersample, fps 30/60, audio codec default/FLAC,
   audio bitrate, use-final-post-master-audio} · OK / START.
7. **Settings panel:** MIDI in · Reset defaults · NDI output · Antialias (+render targets) ·
   Stereoscopic · DMX · Audio analysis (spectrogram band count, internal FFT precision) ·
   Internal controllers (enable, base offset, level multiplier).

### E.2 Automation model (ZGE's three control types)

Every target control is right-clickable → one of:
1. **Audio Control** — link a *Fruity Peak Controller* (envelope from a mixer track) or
   *Fruity Envelope Controller*; parameter responds to audio, often per-frequency.
2. **Automation Control** — *Edit events* / *Create automation clip* / *Link to controller*.
3. **MIDI Input Control** — MIDI Out channel → matched PORT → notes/CC drive the parameter.

Our analog (Pillar C): (1) = **link to an audio bus** (band/RMS/peak/waveform/beat); (2) =
**LFO/automation source**; (3) = **MIDI in** (P2). Same right-click-to-link gesture.

### E.3 Adopt / adapt / skip mapping

| ZGE element | Decision | Where in PRD |
|---|---|---|
| Layer strip (bg→fg, ≤100, solo/alpha/reorder) | **Adopt** (cap ≤32) | FR-B1 |
| Layer header chrome (toggle/stepper/swatch/drag/menu) | **Adopt** | FR-B8 |
| Layer menu + right-click workspace menu | **Adopt** | FR-B8 |
| Collapse / auto-collapse layers | **Adopt** (needed at scale) | FR-B8 |
| Layer-arrangement text editor (Alt+L) | **Adapt** (P2, our `.cfg`/text form) | FR-B8 |
| Per-layer **Image/Mesh** dropdowns | **Adopt** (Mesh P2) | FR-B9 |
| Per-layer **Audio Source** (mixer track) | **Skip** (no FL mixer w/o a VST; single input + per-knob bus routing) | FR-B9 / Pillar C |
| Effect categories (17), preset stepper | **Adapt** (v1 subset of 9) | FR-B2/B4 |
| Add Content tabs (Images/Text/Meshes/HTML/cue) | **Adapt** (Images+Text+Shaders v1) | FR-B12 |
| Template Wizard | **Adopt** (P1 — best on-ramp) | FR-B10 |
| Video settings dialog (presets + advanced) | **Adopt** | FR-E3 |
| Window modes: Attached/Detached | **Skip** (FL-host concepts; standalone app) | FR-B11 |
| Window mode: Wallpaper | **Defer to M7** (P2) | FR-B11 / M7 |
| Settings: antialias/FFT/internal controllers | **Adopt** | FR-B13 |
| MIDI I/O | **Adopt** (P1, kept) | FR-E5 |
| Settings: NDI | **Adapt** (P2) | FR-B13 / M7 |
| Settings: DMX / Hardware category | **Adopt** (P2, reversed 2026-07-18 — was a non-goal) | FR-C9 |
| Settings: Stereoscopic, Physics, Terrain, Object Arrays, Scenes/Meshes(3D) | **Skip** (non-goals) | §1 non-goals |
| Automation: 3 control types | **Adopt** (→ our routing) | Pillar C / E.2 |
| Layer-editing keyboard set | **Adopt** (editor context) | FR-D7 |
| WebCam / Window / URL / video-preload | **Defer** (P2) | FR-B12 |

**Net-new requirements this pass added:** FR-B8–B13 (layer chrome, content dropdowns,
Wizard, window modes, Add-Content panel, settings panel), FR-D7 (ZGE editor shortcuts +
two-context routing rule), FR-E5 (MIDI I/O, kept), and the expanded FR-E3 export dialog.
**Scoped out this pass:** per-layer FL mixer-track audio (no VST), Attached/Detached window
modes (FL-host concepts). **Deferred to M7:** Wallpaper mode.
