# Research: RBDOOM-3-BFG modernizations — what to adopt

Source: RBDOOM-3-BFG README (github.com/RobertBeckebans/RBDOOM-3-BFG). A local clone
exists at `C:\RBDOOM-3-BFG` on the Windows build machine. Our standing decision
(plans/partitioned-coalescing-mountain.md) holds: **cherry-pick, never wholesale-port the
NVRHI renderer.**

## Full modernization inventory, tagged for us

Tags: [easy] cherry-pickable · [mod] moderate integration work · [subsys] subsystem
replacement (avoid for now) · ★ = high value for a music visualizer.

**Build / toolchain**
- CMake build system, VS2022, Linux (GCC/Clang + SDL2), macOS [mod] — our planned
  "CMake as separate future path" mirrors this; use their `neo/CMakeLists.txt` as reference.
- Precompiled-header discipline; astyle formatting; `// RB` change markers [easy] —
  adopt the change-marker convention (e.g. `// VIS`) immediately.

**Renderer core**
- NVRHI abstraction + DX12 + Vulkan [subsys] — entangled with ShaderMake, shader assets,
  device managers. Do not port.
- 64-bit HDR pipeline, linear RGB, ACES filmic tonemapping [mod] ★ — HDR + tonemap +
  bloom is exactly what makes visualizer feedback trails glow. We can implement a scoped
  version (RGBA16F targets + tonemap composite pass) in the stock GL backend without
  NVRHI; our MilkDrop pipeline already requires RGBA16F ping-pong, so the visualizer
  effectively brings its own mini-HDR path.
- PBR (GGX, RMAO), irradiance volumes/env probes, shadow atlas/CSM, SSAO, SSR [subsys] —
  game-visual features; irrelevant to visualizer v1.
- SMAA/TAA, chromatic aberration, blue-noise dithering, **CRT/retro filter modes
  (C64/CPC/Genesis/PSX)** [mod] ★ — the CRT/retro filters and chromatic aberration are
  self-contained fullscreen shader passes; direct candidates for our post-process effect
  library (each is basically one .dviz effect).

**Shaders**
- DXC + SPIR-V cross-compilation, shader permutation system [subsys] — tied to NVRHI.
  We stay with stock GLSL RenderProgs; our effect-file format supplies new shaders.

**Audio**
- OpenAL Soft backend replacing XAudio2 [subsys→later] — valuable if we ever go
  cross-platform/standalone; for now XAudio2 + WASAPI loopback is Windows-native and
  already the plan. Their sound backend abstraction is the reference if we abstract ours.
- OGG support, Bink via libbinkdec/FFmpeg [mod] ★ — precedent for exactly our decoder
  work; check their OGG decoder wiring at `C:\RBDOOM-3-BFG` before finishing ours.

**Assets / filesystem**
- glTF2/OBJ models, PNG/EXR/HDR images [mod] — PNG loading (stb-based) is a nice small
  pick-up for visualizer image layers (stock engine reads only TGA/JPEG). ★ for the
  ZGE-style "Image" layer.
- Virtual filesystem mod priority, `fs_game` mods [easy] — matches our plan to ship
  presets/music under `base/`.
- TrenchBroom mapping, rbdmap BSP compiler, exportFGD etc. [subsys] — mapping tools, skip.

**Tooling / debug**
- **ImGui integration** [mod] ★★ — the single highest-value cherry-pick: an immediate-mode
  debug UI for the visualizer (band meters, preset knobs, layer stack editing) without
  authoring .gui/.swf. RBDOOM shows imgui living happily inside idTech4's frame; port the
  glue (input routing + render hook), render via stock GL.
- RenderDoc markers, Optick profiler [easy] — nice-to-have once shader passes exist.
- Gamepad via SDL2, widescreen fixes [mod] — widescreen/aspect handling worth reviewing;
  stock virtual-640×480 GUI math is aspect-locked.

## Recommended adoption order (visualizer-first)

1. **`// VIS` change markers + astyle discipline** — hygiene, now.
2. **ImGui glue** — debug/control UI for the analyzer + effect params long before the
   polished .gui screens exist; also the fallback if .gui list UX is too limited.
3. **OGG decoder wiring reference** — compare our XA2 decoder boundary to theirs while
   repairing our stb_vorbis path.
4. **PNG (stb_image) loading** — enables image layers in effects.
5. **Post-process shader ports** — CRT/retro filters, chromatic aberration, and their
   bloom/tonemap as .dviz effects once our pass pipeline exists.
6. **CMake (idlib slice first)** — per the existing plan, as a parallel build path only.
7. **OpenAL/cross-platform** — only if/when the standalone (Phase 5, Option B) targets
   non-Windows.

Not adopting: NVRHI/DX12/Vulkan, PBR/GI/shadow systems, DXC/SPIR-V, mapping tools —
each is a subsystem replacement with dependency trees (extern/nvrhi, ShaderMake, shader
assets) absent from this tree, and none serves the visualizer goal.

---

## Deep-dive addendum (full repo/release-history analysis)

**The single most important fact: RBDOOM's history splits at v1.5.0 (April 2023).**
Everything through **v1.4.0 was an OpenGL engine with the stock backend structure** —
PBR, HDR, ACES tonemapping, bloom, SSAO, SMAA, and baked GI all shipped in that era as
GLSL against GL. v1.5.0 rewrote the backend on NVRHI (DX12/Vulkan) + ShaderMake/DXC.
**You do not need NVRHI to get the visuals. Use the v1.4.0 tag as the donor; treat
v1.5+ as reference only** (except the retro/CRT shaders from v1.6, which are pure
fragment shaders and port back cleanly).

Cleanly cherry-pickable (verified NVRHI-free at stock seams):
- stb-based PNG/EXR/HDR loaders → drop into `idImage` load path.
- **ImGui integration from the pre-1.5 branch** — input hook + context setup are
  backend-agnostic; the ~200-line GL draw-data renderer is reusable verbatim.
- Post shaders: bloom, ACES tonemap, chromatic aberration, blue-noise dither, SMAA,
  CRT/retro palettes (C64/CPC/Genesis/PSX) → GLSL + cvars on our post chain.
- OpenAL Soft as an alternate `idSoundHardware` implementation (clean seam swap) —
  only if capture ergonomics or cross-platform ever demand it; **stay on XAudio2 and
  tap the mix buffer for FFT** (agent's recommendation matches our plan).
- glTF2/OBJ loaders, `idRenderLog` RenderDoc markers, Optick, OGG/FFmpeg wiring.

NVRHI-entangled (skip or reimplement in GL): TAA (motion vectors + history), v1.5 ImGui
render path, ShaderMake/DXC, the idRenderBackend rewrite. Toolchain-entangled (skip):
baked GI/probes, TrenchBroom/dmap. Diffuse (not cherry-pickable): CMake migration,
C++ cleanup sweeps.

**Phased adoption (visualizer-value order):**
0. Check out the v1.4.0 tag as donor reference.
1. **ImGui panel first** — every later effect ships with live controls instead of cvars.
2. Audio input core (our own code; XAudio2 tap + FFT — see PLAN).
3. HDR FP16 target + ACES resolve (bloom on LDR looks wrong; do HDR first).
4. Bloom + chromatic aberration + dither, driven by ImGui + audio uniforms.
5. stb image loaders + SMAA.
6. Retro/CRT aesthetic modes as preset-selectable looks.
Deferred: RenderDoc markers/Optick, glTF2, CMake. Never: NVRHI/DX12/Vulkan/TAA/GI/mapping tools.
