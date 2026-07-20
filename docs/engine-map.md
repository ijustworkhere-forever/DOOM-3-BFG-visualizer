# DOOM 3 BFG Engine Map (for the visualizer project)

Working map of every engine subsystem, built from a full-source reading pass.
Each section notes what the subsystem does, its key types, and what matters for the
sound-reactive visualizer. Companion docs: `docs/audit-existing-visualizer-code.md`
(defects in the new code), `plans/` (roadmaps).

## neo/sound — sound system (read first-hand, verified stock except new files)

**Architecture:** `idSoundSystemLocal` (global `soundSystemLocal`) owns `idSoundHardware`
(XAudio2 device + master/submix voices + voice pool) and a list of `idSoundWorldLocal`s;
one world is "playing" at a time (`SetPlayingSoundWorld`). Worlds own `idSoundEmitterLocal`s
(block-allocated, positional), each with up to 16 `idSoundChannel`s. A channel binds a
parsed `idSoundShader` decl (parms: volume dB, min/max distance in meters, shakes, class,
SSF_* flags) to `idSoundSample_XAudio2` buffers and, when audible, an
`idSoundVoice_XAudio2` wrapping an `IXAudio2SourceVoice`.

**Frame flow:** `common` frame → `soundSystem->Render()` → `currentSoundWorld->Update()`
(per-emitter distance/portal occlusion via `ResolveOrigin` walking renderWorld portals,
volume calc, voice-count cushioning) → per-channel `UpdateHardware` (allocate/position
voices, surround matrix via `idSoundVoice_Base::CalculateSurround`) → `hardware.Update()`
(commit XAudio2 ops, recycle zombie voices, VU meter overlay). Sound time is wall-clock
`Sys_Milliseconds`, not game time.

**Sample pipeline:** `.wav`/`.msadpcm` (or generated `.idwav` binary) loaded whole into
1..N buffers (`sampleBuffer_t`); XMA2 only on consoles. 16-bit PCM or MS-ADPCM; streaming
uses 3 queued XAudio2 buffers with `idStreamingVoiceContext` callbacks re-submitting the
next buffer from `OnBufferStart`.

**Audio-reactive hooks that already exist (important for us):**
- `.amp` sidecar files (built by the `neo/amplitude` tool): 60 Hz min/max envelope,
  `idSoundSample::GetAmplitude(timeMS)` → `idSoundEmitter::CurrentAmplitude()` →
  `idSoundWorld::CurrentShakeAmplitude()` (drives screen shakes today).
- XAudio2 **volume-meter effect** per voice (`GetAmplitude()` RMS when `shakes` used) and
  on the **master voice** (`s_showLevelMeter`) — the master-voice effect chain is the
  natural tap point for a real-time FFT of the final mix.
- `listDevices` command + `s_device` cvar — device enumeration/selection (XAudio2 2.7),
  reusable for the device-picker UI.
- `idSoundSystem::ImageForTime(ms, waveform)` — stubbed hook meant to return an image of
  the current waveform/level ("sound level meter window"); a ready-made seam.

**Gotchas:** `s_useCompression=1` means most samples on disk are ADPCM (raw buffer bytes
are NOT PCM — any analyzer reading `sample->buffers` must decode or be restricted to PCM);
`DBtoLinear` uses 2^(db/6); `SSF_MUSIC` sounds mute when the player plays custom music;
sound worlds pause/unpause with accumulated pause-time bookkeeping.

## neo/cm + neo/aas — collision & pathfinding data (agent-verified: fully stock)

Collision: trace-model-vs-polygonal-model queries (`idCollisionModelManager`), axial BSP
over polygons/brushes built from map brushes/patches (binary `.bcm` cache). Query kinds:
translation sweep, rotation sweep (tan(a/2) analytic solvers), contacts, contents.
AAS: `idAASFile` loader/queries for the AI navmesh ("DewmAAS" v1.07) — areas, reachability
graph, clusters/portals; routing logic lives in d3xp/ai.

Visualizer relevance: none directly; only note is `CollisionModel_debug.cpp`'s wireframe
debug-draw path via `renderWorld->DebugPolygon/DebugArrow` — a quick way to draw geometry
overlays. Treat both as inert dependencies.

## neo/renderer — resources: materials, images, models, cinematics (agent-verified stock, two stock-BFG strips)

**Material expression system — the engine's MilkDrop analog.** `idMaterial` compiles
`.mtr` text into flat expression registers + an op list (tiny per-frame VM):
predefined inputs `EXP_REG_TIME`, `EXP_REG_PARM0..11` (per-entity), `EXP_REG_GLOBAL0..7`;
ops include arithmetic, comparisons, `OP_TYPE_TABLE` (idDeclTable lookups — sin/cos
oscillators) and `OP_TYPE_SOUND`. `EvaluateRegisters()` (Material.cpp:2474) runs per
surface per frame; `sound` in a material expression evaluates to
`soundEmitter->CurrentAmplitude()` (Material.cpp:2538) — live per-emitter RMS.
`r_forceSoundOpAmplitude` overrides for testing. Stage keyword `soundmap <waveform>`
creates an `idSndWindow` cinematic whose `ImageForTime` calls
`soundSystem->ImageForTime(ms, waveform)` — the built-in audio→texture primitive
(currently stubbed in the sound system). GLSL program stages exist per-stage
(`newShaderStage_t`: `program`, up to 4 vertexParms, 8 fragmentMaps).

**Image pipeline**: `idImageManager`/`globalImages`, `.bimage` binary cache, TGA+JPEG
loaders only, intrinsic `_currentRender`/`_currentDepth` capture targets (what
post-process materials sample), `UploadScratch` for per-frame CPU→GPU texture streaming
(cinematics/sound meter path). Image programs (`heightmap()`, `add()`, …) are load-time
compositing.

**Models**: `idRenderModelStatic` + ASE/LWO/MA parsers, MD5 skinned, MD3 morph, and
procedural dynamic models — notably `Model_liquid` (real 2D wave-propagation height
field — a ready-made "continuously animating mesh" to imitate for audio-driven surfaces),
beam/sprite/particle models all driven from `renderEntity->shaderParms`.

**Deviations from stock (both intentional strips in the BFG GPL release context):**
`Cinematic.cpp` is stripped to a stub (RoQ/Bink playback removed — `videomap` renders
nothing; `soundmap`/`idSndWindow` survives); `AutoRenderBink` stubbed empty. No
visualizer code anywhere in the renderer.

## neo/renderer — frontend (agent-verified stock; 1 cosmetic anomaly)

Classic frontend/backend split with double-buffered 64 MB frame arenas.
`idRenderWorld::RenderScene` → `R_RenderView` (frustum setup → portal flood
`FindViewLightsAndEntities` → `R_AddLights` → `R_AddModels` → `R_AddInGameGuis` →
sort → subviews) → command buffer (`RC_DRAW_VIEW_3D/GUI`, `RC_COPY_RENDER`,
`RC_POST_PROCESS`) consumed by the backend.

**Visualizer injection seams (ranked):**
1. **`RC_POST_PROCESS`** — `R_RenderPostProcess` runs after 3D+subviews
   (RenderWorld.cpp:861 → RenderSystem.cpp:196 `R_AddDrawPostProcess`; backend
   `RB_PostProcess`); gated by `r_skipPostProcess`. Natural slot for fullscreen
   audio-reactive passes, or add a sibling `RC_VISUALIZER` command in tr_local.h.
2. **GUI-on-surface** — `R_AddInGameGuis`/`R_RenderGuiSurf` runs `gui->Redraw(time)`
   into the shared guiModel: an audio-reactive idUserInterface needs zero renderer changes.
3. **Subview/dynamic-texture feedback** — `DI_REMOTE/MIRROR/XRAY` show per-frame
   texture-fill re-render; the reusable pattern for a feedback canvas.
4. **Per-view shaderParms** — `renderView.shaderParms[]` flows into GLOBAL0..7 registers.
`R_MakeFullScreenTris` (`backEnd.unitSquareSurface`) already exists for fullscreen quads.
Anomaly: `tr_frontend_main.cpp` GPL comment header has stray digits (comment-only).

## neo/d3xp — game core (agent-verified stock)

`idGameLocal` god-object behind the `gameExport_t` API (`GetGameAPI`, version 8;
statically linked in BFG). `RunFrame` ordering: lobby sync → dual timelines (fast/slow,
slow-mo via `slowmoScale`) → set renderview → PVS setup → think pass over
`activeEntities` (TIME_GROUP1 then RunTimeGroup2) → service events → mpGame.Run.
Entities: fixed `entities[4096]` + spawnIds for `idEntityPtr` staleness; first 128 slots
= clients. `gameSoundWorld` global = the engine-provided sound world (game writes
emitters + slow-mo; never reads audio back).

**`gameLocal.globalShaderParms[12]` is the cleanest audio→material bridge**: writers
today are `target_setGlobalShaderParm` and script `setShaderParm`; `Player.cpp:9034` /
`Entity.cpp:1576` copy it into every `renderView->shaderParms` → GLOBAL0..7 registers →
every material referencing `global0..N`. Even network-replicated (SNAP_SHADERPARMS).
Caution: parms 4/5 initialized to 1.0 for portal-sky legacy.

## neo/d3xp — world entities (agent-verified stock)

Taxonomy: `idLight`, `idSound` speakers, movers/doors/plats, triggers, ~35 `idTarget_*`
logic entities, items/moveables/projectiles/weapons, `idSmokeParticles` (singleton,
10k-particle budget, rebuilt-every-tic render callback — the most drivable dynamic
geometry primitive), `idFuncEmitter`/`idBeam` cheap primitives.

**Audio-reactive gold:** `idLight::Present()` sets `renderLight.referenceSound` to the
light's (or `lightParent`'s) sound emitter; light material expressions then modulate from
that emitter's amplitude. `Event_SetSoundHandles` ("soundGroups") pushes one master
emitter onto whole groups of lights — **stock audio-reactive lighting with zero engine
changes** (amplitude only, no bands). `idEarthQuake` uses
`gameSoundWorld->CurrentShakeAmplitude()`. Target entities expose
`SetShaderParm/SetGlobalShaderTime/FadeSoundClass` as the intended logic→visual surface.

## neo/d3xp — entity core (agent-verified stock)

`idEntity` (spawn via spawnArgs → `gameEdit->ParseSpawnArgsToRenderEntity/RefSound`,
think-flag activation lists, idEventDef script bridge, signals, binding/teams, network
snapshots). `idAnimatedEntity` adds idAnimator. AF ragdolls (`idAF` ↔ MD5 joints),
`idBrittleFracture` shattering, `idEntityFx` (.fx timelines), IK solvers, GameEdit
drag/edit tools.

**Key seam:** `idEntity::SetShaderParm(n,v)` (Entity.cpp:1044) → `UpdateVisuals` →
`UpdateEntityDef` pushes 12 per-entity parms to materials; parms 3–11 seed from
spawnArgs. And every entity's `renderEntity.referenceSound = refSound.referenceSound`
(Entity.cpp:1690) — the native sound↔shader coupling (`Fx.cpp:450` lights adopt it).

## neo/idlib — math/geometry/bv (agent-verified stock)

Fixed vectors/matrices/quats/planes/Pluecker, arbitrary `idVecX/idMatX` (full dense LA:
LU/QR/Cholesky/eigen/SVD; **static 1024-float scratch pool → not thread-safe**), `idLCP`,
`idCurve` spline family + `idInterpolate`/`idExtrapolate` (great for band smoothing /
envelope followers / param easing), `idODE`, `idRandom2`. `idMath`: `Sin16/ATan16/Exp16`
fast approximations, `BitReverse`/`CeilPowerOfTwo` (FFT building blocks, unused).
`idComplex` exists (only complex type — natural DFT base). SIMD: CPUID dispatch but only
memcpy/minmax/joint-skinning virtualized; SSE inline elsewhere; no NEON.
geometry/: `idDrawVert` (32B packed, half-float ST), `idRenderMatrix` (row-major, SSE
culling), `idTraceModel`, `idSurface`+patches, `idWinding`. bv/: Bounds/Box/Sphere.
Footgun: `idMat3` column-major vs `idMat4`/`idRenderMatrix` row-major.

## neo/swf — SWF/ActionScript menu runtime (agent-verified stock)

id's from-scratch Flash player: parses `.swf` (or binary `.bswf` v16 + `.tga` atlas
cache in `generated/swf/`), `idSWFSprite` timelines with PlaceObject2/3 display lists,
full AS2 bytecode interpreter (`idSWFScriptFunction_Script::Run`, prototype-chain object
model `idSWFScriptObject`/`idSWFScriptVar`), ear-clipping triangulation of vector shapes,
render via `DrawStretchPic`/`AllocTris` in virtual 640×480 with stereo-depth support,
stencil masks, and **full blend-mode set (add/screen/multiply…) — useful for visuals**.

**C++↔AS binding (how all BFG menus work):** native functions installed via
`Set("name", fn)`; native vars (`_x,_y,_alpha,_rotation,_visible,material,…` on sprites;
`text,textColor,scroll,…` on text) via `SetNative`; game calls `idSWF::Invoke("fn",parms)`,
`SetGlobal`, `GetNestedSprite/Text/Obj`. Text fields bind to `_global` vars
(`editText->variable`), so `swf->SetGlobal("songTitle", s)` fills a field.

**Constraints for our UI:** no API to synthesize new vector shapes at runtime —
`duplicateMovieClip` clones authored clips (how dynamic lists grow), and any sprite's
`material` var can be set to an arbitrary engine material (render target, cinematic) —
the strong hook for embedding the visualizer canvas inside a menu. New pickers = new C++
menu classes driving cloned/reused BFG SWF list templates. SWF-embedded audio is stubbed
(`SWF_Sounds.cpp` empty); menu sounds go through `PlayShaderDirectly`.

## neo/renderer — backend (agent-verified stock + non-stock scaffolding)

Stock GL3.2-core backend: `RB_ExecuteBackEndCommands` dispatches `RB_DrawView`
(depth prepass → stencil-shadow interactions → shader passes → fog → screen-warp →
debug), `RB_CopyRender`, `RB_PostProcess`. `idRenderProgManager` loads Cg-style `.vfp`,
transpiles to GLSL 1.50 (`ConvertCG2GLSL`), delivers uniforms as shared vec4 arrays.
64-bit `GLS_*` state bits; double-buffered 31MB vertex/index ring cache.

**Fullscreen-effect hooks (exact):**
- `RB_PostProcess` (tr_backend_draw.cpp:2840) — resolves screen to `_currentRender`,
  draws `unitSquareSurface` with the postprocess program. **Gated behind `rs_enable`
  (resolution scaling) — relax the guard for the visualizer pass.** No bloom exists in
  stock BFG's tree (MRB_POSTPROCESS is only a log label).
- `SS_POST_PROCESS` material sort (tr_backend_draw.cpp:2591) — heat-haze path: any
  material with post-process sort + a `newShaderStage_t` GLSL program runs fullscreen
  with **up to 512 `RENDERPARM_USER+n` vec4s fed from material expression registers**
  and N textures — a data-driven custom-shader path needing ZERO C++ changes.
- `RB_MotionBlur` (:2645) — reference fullscreen pass sampling scene+depth.

**Non-stock renderer footprint (all currently orphaned):**
- `RenderBackend.h` — malformed abstract multi-API interface (see audit; keep excluded).
- `RenderContext.h` / `RenderTexture.h` — syntactically valid render-to-texture/backend
  scaffolding, no .cpp, referenced nowhere.
- `idRenderProgManager::LoadShaderFromSource(vtxSrc, fragSrc, uniforms)`
  (RenderProgs.cpp:413) — **non-stock runtime-GLSL compile, clearly the intended
  visualizer preset-shader hook**, but returns a bare GLuint, registers no
  `glslProgram_t`, resolves no uniforms, and has no callers. Needs integration with
  `glslPrograms`/`BindShader`/`CommitUniforms` to be usable.

## neo/framework (agent-verified stock; critical frame-ordering facts)

**`idCommonLocal::Frame()` (common_frame.cpp:367) is THE per-frame hook site.** Order:
events → SwapCommandBuffers (waits GPU) → fixed-timestep game frames → background
game thread runs `game->RunFrame()` then `Draw()` (builds render commands incl. console)
→ `RenderCommandBuffers()` → join → **`soundSystem->Render()` LAST (line 663)**.
⇒ Audio for frame N is mixed after frame N's visuals are committed: **the analyzer
must either run early in Frame() (before RunGameAndDraw) on the previous frame's mix,
or accept one frame of latency — one-frame-latency is the clean design.**

Other findings:
- **`idDeclTable` is the engine-intended audio→material/particle bridge**: materials
  and particle parms (`ParseParametric`) read `DECL_TABLE` lookups per frame; but
  `values` is private with no runtime setter — add `SetValue(s)` to stream audio bands
  into named tables (then `sinTable`-style expressions everywhere react).
- **`idDebugGraph`** (DebugGraph.h, via `console->CreateGraph(n)`) — ready-made bar-graph
  widget auto-rendered over the scene: instant FFT spectrum/VU display for debugging.
- New decl types are cheap (`DECL_MAX_TYPES=32`, ~15 used): `DECL_PRESET`/`DECL_PLAYLIST`
  get free listing/reload/completion by registering an allocator.
- Decl text is Huffman-compressed in memory; `FindType` is main-thread-only.
- File system: loose files under `base/` always work (`ListFilesTree` merges dirs +
  resource containers, extension-filtered — ideal for music/preset browsers);
  `OpenExplicitFileRead(OSPath)` = "load a song from anywhere on disk". BFG does NOT
  mount loose .pk4/.zip. zlib is 1.2.3 (old, known CVEs — fine offline).
- Console/cmd/cvar registration trivial (`CONSOLE_COMMAND`, static `idCVar`).
- Doom-classic bridge: `RunDoomClassicFrame()` (common_frame.cpp:709-748) palette-expands
  classic `screens[0]` → `renderSystem->UploadImage("_doomClassic", ...)` each frame —
  the pattern to copy for streaming any CPU-generated visual texture.

## neo/d3xp — player/view (agent-verified stock; the post-process framework)

`idPlayerView` + **`FullscreenFXManager`** (PlayerView.cpp:1693) is the engine's
sanctioned fullscreen post-process stack: each `FullscreenFX` subclass (Helltime,
DoubleVision, EnviroSuit, InfluenceVision, Warp, **Bloom** — yes, script-drivable bloom
exists here via `player->bloomIntensity`) declares `Active()`/`HighQuality()`, gets
framebuffer capture to `_currentRender`/`_accum` and alpha-faded blend-back. **A
visualizer post-process = one new FullscreenFX subclass registered in `Initialize()`.**
Also: **`g_testPostProcess` cvar draws ANY named material fullscreen — the zero-code
path to test a visualizer material today.** `CalculateShake()` already reads
`gameSoundWorld->CurrentShakeAmplitude()` per frame — stock audio→view precedent.
HUD = SWF push pattern (`idMenuHandler_HUD`, player pushes state per tic).

## neo/d3xp — physics, script/gamesys (agent-verified stock)

Physics: idPhysics hierarchy (Static/Actor/Player/Monster/Parametric/RigidBody/AF-LCP),
idClip broadphase, forces, idPush. Never plays audio; surfaces impacts via four
`self->Collide(trace, velocity)` sites (RigidBody/AF/Player/Monster) carrying impulse
magnitude + surface material — natural beat/impact event sources if game-world visuals
ever react to physics.

Script/gamesys: DoomScript VM (compiler → idProgram → idInterpreter → idThread),
idEventDef scheduling, idClass RTTI (CLASS_DECLARATION/EVENT macros — reuse for new
visualizer entity classes), idSaveGame. **Verdict: do NOT run per-frame preset equations
in DoomScript** (floats only, interpreted, narrow script→render channel via
`setShaderParm`); fine as low-frequency orchestration (preset switching, transitions).

## neo/d3xp — menus (agent-verified stock; the SWF menu recipes)

Three layers: `idMenuHandler` (owns idSWF + screens + transition FSM + CommandBar) →
`idMenuScreen` (binds a named top-level SWF sprite; Initialize/Update/Show/Hide/
HandleAction) → `idMenuWidget` (spritePath binding, focus chain, eventActions,
observers, optional `idMenuDataSource`). SWF list convention: items `item0..itemN`,
buttons need frames `up/over/sel_up/...` + `label0.txtVal` — **C++ binds to nothing
without an authored SWF exposing these sprites.**

**Recipes (verified against source):**
- Scrollable text list: `idMenuWidget_DynamicList` + `SetListData(idList<idList<idStr>>)`
  — accepts RAW strings (not just #str_ ids), so song titles/device names push straight in.
- Best templates: `MenuScreen_Shell_Dev.cpp` (self-contained list → console command —
  the song/preset list template), `_Shell_Load.cpp` (list + side detail panel),
  `_Shell_GameOptions.cpp` + `idMenuDataSource` (sliders/toggles — device cycler),
  `idMenuHandler_PDA` (tab strip — maps 1:1 onto Song|Playlist|Device|Preset tabs).
- Registration: `shellAreas_t` enum + `BIND_SHELL_SCREEN` (MenuHandler_Shell.cpp:390-428);
  shell owned by `idGameLocal` (`shellHandler`), shown via `Shell_Show`.

## neo/ui — legacy GUI (agent-verified; non-stock additions cataloged)

Stock system: `idUserInterfaceLocal` (state idDict, `gui::` vars) → `idWindow` tree
(expression-register compiler/evaluator over rect/colors/visible; event scripts:
onAction/onFrame/named events; `transition` animates vars) → widgets: **`idListWindow`**
(multi-column with icon columns, type-ahead; fed via `"<name>_item_N"` state keys —
idiomatically through **`idListGUI`** (`AllocListGUI`→`Config`→`Push/Add`), selection
returned via `"<name>_sel_0"` + onEnter command), `idChoiceWindow` (cvar/`gui::`-bound
option cycler — device picker), `idSliderWindow`, `idEditWindow`, `idBindWindow`.
`idRenderWindow` (3D-scene-in-GUI) is marked NO LONGER SUPPORTED in BFG. Arcade
minigame windows (SSD/BearShoot/BustOut) prove full custom-drawing windows are viable.

Non-stock: `VisualizerUI.*`, `VisualizerPreset.h`, `MilkDropParser.*` (all dead code,
defects in the audit doc), plus small edits — `IsVisualizer()` (UserInterface.h:57),
`AddGui()` (UserInterface.h:151 / UserInterfaceLocal.h:141 / UserInterface.cpp:221-238;
structurally fine). Nothing constructs VisualizerUI; no project file lists it.
**UI plan approach confirmed**: chrome as `.gui` (listDef/choiceDef/sliderDef via
idListGUI + `gui::` state), reactive canvas via custom Redraw override or a fullscreen
material fed by shader parms — not by finishing VisualizerUI as written.

## neo/sys (agent-verified: stock + clean x64 port)

Platform contract `sys_public.h` (Sys_* frees, sysEvent_t queue, idUDP). Win32:
`WinMain` (win_main.cpp:1436) → `common->Init` → `while(1){Win_Frame(); common->Frame();}`
— the whole reusable app skeleton. GL context via wgl fake-window dance, GL 3.2
(`r_useOpenGL32`), vsync incl. adaptive; `R_GetModeListForDisplay` is multi-monitor
aware (fullscreen = 1-based monitor #; -1 = borderless spanning). Input: DirectInput8
buffered kbd/mouse + XInput on a dedicated 250Hz thread. Only deviation from stock:
**a clean 64-bit port** (uintptr_t handles, __cpuid intrinsics, MXCSR FPU control,
RtlCaptureStackBackTrace, SEH cleanup) — no behavior changes.

Session/lobby/matchmaking/savegame (18-state FSM, snapshot delta streaming, host
migration): all stock, **all dead weight for a standalone visualizer**. Strip strategy:
don't delete `idSession` (hard global dependency of common/shell/game) — stub
`idSessionLocal` (always-signed-in local user, GetState()=IDLE, no-op the ~150
matchmaking virtuals; `idLobbyStub` is the template). Keep a simplified savegame path
for settings persistence only.

## neo/idlib — core (agent-verified stock; 1 deviation)

Foundation: aligned tagged allocator (+block/dynamic-block allocators), idStr (20B
inline buffer), idToken→idLexer→idParser text pipeline (all decl/gui/material parsing),
idList/idHashIndex/idStrPool/idDict, idBitMsg, threading wrappers + idParallelJobList,
Win32-only sys defines. Only substantive deviation: `sys/sys_types.h` `nullptr`
modernization guard for modern MSVC (build-compat patch, benign). Stock `MEM_TAG(AUDIO)`
and `MEM_TAG(AMPLITUDE)` exist for tagging analysis buffers; add a VISUALIZER tag in
`sys_alloc_tags.h` if wanted.

## doomclassic + timidity (agent-verified stock BFG embedding)

DOOM 1/2 as a re-entrant C engine: all globals hoisted into `struct Globals` (`::g->`),
4 instances for splitscreen; host-clocked 35Hz tics; WAD IO through idFileSystem;
networking tunneled through idLobby. Renderer writes 8-bit palettized `g->screens[0]`;
palette expanded via `g->XColorMap`; **the modern bridge is
`idCommonLocal::RunDoomClassicFrame()` (neo/framework/common_frame.cpp:709-748) which
palette-expands to RGBA and calls `renderSystem->UploadImage("_doomClassic", ...)`.**

**Classic sound = a second XAudio2 pipeline sharing the SAME `IXAudio2` device**
(`soundSystemLocal.hardware.GetIXAudio2()`, i_sound_win32.cpp): SFX voices with
X3DAudio panning, and music as a **fully pre-rendered Timidity PCM blob (`musicBuffer`)
looping on one source voice — directly readable in memory for offline FFT** of classic
music. A master-voice tap (our plan) captures classic audio too, since both pipelines
share the device. Timidity = stock third-party GUS/MIDI synth rendering to buffer.

---

# Synthesis: how the visualizer maps onto this engine

**Audio in** → master-voice XAudio2 tap (catches game + classic + our music playback) +
WASAPI loopback for external audio ("capture mode") + decoders for files.
**Analysis** → 1024-pt FFT, 7 bands (+MilkDrop bass/mid/treb + _att envelopes), RMS,
beat detect; runs early in `idCommonLocal::Frame()` with one-frame latency (frame
ordering: game→draw→render→sound).
**Feature routing** (per ZGE/AudioRider): source (band/RMS/LFO) → per-link attack/release
smoothing → bipolar/unipolar map → gain → destination.
**Destinations, cheapest first**: `gameLocal.globalShaderParms` (GLOBAL0..7 registers);
named `idDeclTable` values (needs small runtime setter; then ALL materials/particles can
react); `RENDERPARM_USER` uniforms via SS_POST_PROCESS materials; `ImageForTime`
waveform/spectrum texture (`soundmap` stages); `idDebugGraph` bars (debug).
**Render pipeline**: MilkDrop pass set at `RB_PostProcess`/`FullscreenFX` seam
(RGBA16F ping-pong warp/decay → wave/shape overlay → composite); prototype instantly
via `g_testPostProcess` + an SS_POST_PROCESS material.
**Presets**: manifest + GLSL files (rename custom format `.dviz`); finish
`LoadShaderFromSource` for runtime shader compile; projectm-eval later for real .milk.
**UI**: `.gui` + idListGUI pickers (v1) → SWF shell screen (Dev/Load/GameOptions/PDA
templates) later; ImGui (RBDOOM pre-1.5 port) as the debug/param panel.
