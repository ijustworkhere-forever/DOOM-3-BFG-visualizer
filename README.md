# idVIS — a MilkDrop/ZGE-style sound-reactive visualizer on the DOOM 3: BFG engine

This repo is a fork of id Software's **DOOM 3: BFG Edition GPL source release**
repurposed as a standalone, dataless music visualizer: point it at an audio
source (local files, a playlist, or WASAPI loopback of anything playing on the
system) and it renders MilkDrop/projectM-style reactive visuals, driven by an
FFT audio analyzer feeding the idTech4 renderer.

It borrows three things from three different tools and fuses them:

1. **The preset ecosystem of MilkDrop / projectM** — the goal is to load and
   run real `.milk` presets, not a fixed set of hand-coded effects.
2. **The authoring model of FL Studio's ZGameEditor Visualizer** — a layer
   stack of source/processor slots, every parameter linkable to an audio
   source with smoothing and range.
3. **The engine underneath it** — since this is built on an actual game
   engine, one layer type can fly a camera through licensed DOOM 3: BFG maps
   (retail or community mods) as a living 3D backdrop.

The full product vision, requirements, and design decisions live in
[`plans/PRD-zge-milkdrop-visualizer.md`](plans/PRD-zge-milkdrop-visualizer.md).
**Live implementation status** (what's actually landed vs. planned, per
milestone) is tracked in
[`plans/PRD-implementation-status.md`](plans/PRD-implementation-status.md) —
that file is the source of truth for "does X work yet," updated as work lands.

## Status

Actively in development. Milestones M0, M3, M4, and M7 have landed on
`master`; see the implementation status doc linked above for the full
breakdown of what's implemented per milestone and what still needs a Windows
build/runtime pass to verify. Development happens cross-platform (editing on
macOS, building/testing on Windows, since the engine targets Win32/DirectX
audio + Win32 windowing) — anything marked `[needs Windows build/test]` in the
status doc has been written and reviewed but not yet compiled or run.

## Building

- Open `neo/doom3.sln` in Visual Studio 2022 (the project files target the
  `v143` platform toolset) and build the `Debug|x64` or `Release|x64`
  configuration.
- No external SDKs are required to build the visualizer-specific code — the
  HLSL→GLSL shader transpiler (`neo/external/hlsl2glslfork/`), the ImGui UI
  (`neo/external/imgui/`), and the NDI network video output
  (`neo/external/ndi/`) are all vendored directly in this repo.
- NDI output additionally needs the free
  [NDI Runtime](https://ndi.video/tools/) redistributable installed on the
  machine you *run* the build on (not needed to build); without it,
  `vis_ndiEnable` logs a warning and no-ops instead of failing to start.
- Game data is not included (per the original id Software release terms —
  see `README.txt`/`COPYING.txt`); this visualizer boots dataless and does not
  require owning DOOM 3: BFG, except for the optional map-flythrough layer
  type, which does.

## Running

Once built, the key console commands (bind a key or type in the in-game
console) are documented inline as they're added; the current set spans audio
playback/source selection, visualizer effect/layer control, MIDI I/O,
recording/video export, wallpaper mode, and NDI output. Search
`neo/sound/visualizer_manager.cpp` for `CONSOLE_COMMAND` and `idCVar` to see
the full current list, or check the relevant milestone section in
`plans/PRD-implementation-status.md` for what each feature does and how to
invoke it.

## Repo layout

- `neo/` — the engine source (idTech4/BFG), including all visualizer-specific
  additions (`neo/sound/visualizer_manager.*`, `neo/external/*` vendored
  libraries, `neo/sys/win32/win_*` platform integration shims).
- `plans/` — product requirements docs and the live implementation-status
  tracker.
- `docs/` — research and architecture notes (engine subsystem map, MilkDrop/
  projectM format research, ZGE Visualizer research) that informed the PRD.
- `PLAN.md` / `PROGRESS.md` — earlier, lower-level planning/progress notes
  from before `plans/PRD-*.md` existed; kept for history, superseded by the
  PRD + status doc above for current planning.
- `README.txt` / `COPYING.txt` — the original id Software release notes and
  GPL license text; unmodified, still the license of record for the base
  engine and its third-party-licensed components (JPEG, zlib, Timidity,
  etc. — see `README.txt` for the itemized list).

## License

The base engine is GPL-licensed by id Software, with several third-party
components under their own compatible licenses (see `README.txt`). Code
vendored for the visualizer work carries its own upstream license, noted at
the top of each vendored directory/file (`neo/external/imgui` is MIT,
`neo/external/hlsl2glslfork` is BSD-style + MojoShader's zlib license,
`neo/external/ndi` headers are MIT-per-file).
