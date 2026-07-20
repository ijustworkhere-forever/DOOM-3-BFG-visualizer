# Plan: Visualizer UI — song / playlist / audio-device / preset selection

Goal (from project brief): a UI where the user can pick a **song**, a **playlist**, or an
**audio device input** (WASAPI loopback "capture mode"), and select a **preset effect** —
plus, per the ZGE research, a path toward a layer/parameter UI later.

## The engine gives us two GUI systems — pick per job

1. **Legacy idUserInterface/idWindow ("old UI")** — text-scripted `.gui` windows drawn
   through `idDeviceContext`. Used for in-game surfaces and the console. Fully
   data-driven: windows, `listDef`, named events, expression registers, `gui::` state
   vars set from C++ (`SetStateString/Int/Float` + `HandleNamedEvent` + `Redraw`).
   `idListWindow` + `idListGUI` already implement scrollable, selectable lists populated
   from C++ — exactly what a song/preset browser needs.
2. **BFG SWF menus (`idSWF` + `idMenuHandler`/`idMenuScreen`/`idMenuWidget`)** — the
   shell/PDA/HUD system. Polished look, gamepad/mouse focus handling, but every screen
   binds to a **pre-authored Flash asset**; we cannot synthesize new vector layouts at
   runtime (only clone/re-skin existing clips and set `material` on placeholders).

**Decision: build the visualizer UI on the legacy `.gui` system first.**
Rationale:
- Zero new binary assets — `.gui` files are plain text we can author freely; SWF screens
  require cloning shipped `.swf` templates whose layouts don't match our needs.
- `idListWindow`/`idListGUI` provide the list UX out of the box.
- The existing (broken) `VisualizerUI.cpp` prototype already pointed this direction; the
  correct pattern is **author a `.gui` file + drive it from C++ state**, not add
  `AddText`/`AddStateVar` methods that don't exist on `idWindow`.
- SWF remains the right choice later for a shell-integrated "Visualizer" main-menu entry;
  the swf `material` hook also lets us embed the visualizer render target inside any menu.

## Screen design (v1)

One fullscreen GUI, `guis/visualizer.gui`, active when the visualizer mode runs
(bound via `game->Shell...` bypass or a console command `visualizer` that pushes the GUI
onto `idUserInterface` fullscreen rendering, same path the console uses):

```
+--------------------------------------------------------------+
| VISUALIZER                                    [Now Playing]  |
|                                                              |
|  SOURCE:  ( ) Song file   ( ) Playlist   ( ) Device capture  |
|  +----------------------------+  +------------------------+  |
|  | Track list (idListWindow)  |  | Preset list            |  |
|  | 01 song_a.ogg              |  | pulse                  |  |
|  | 02 song_b.wav              |  | spectrum_pulse         |  |
|  | ...                        |  | milkdrop_demo          |  |
|  +----------------------------+  +------------------------+  |
|  [Play] [Pause] [Next] [Prev]      Device: <listDef>         |
|                                                              |
|                (visualizer canvas fills background)          |
+--------------------------------------------------------------+
```

- **Canvas**: the GUI's background material samples the visualizer render target
  (or the GUI itself is drawn over the fullscreen visualizer pass — simplest v1: run the
  visualizer as the scene and the GUI as an overlay that fades out when idle).
- **Track list**: `idListWindow` fed by C++ from `idPlaylistManager` (after repair).
  Songs discovered via `fileSystem->ListFilesTree("music", ".wav|.ogg|.mp3|.flac")` —
  loose files in `base/music/` work without resource containers.
- **Playlist**: `.m3u`-style text files in `base/playlists/`, parsed by
  `idPlaylistManager::LoadPlaylist` (implement for real: one path per line, `#` comments).
- **Preset list**: enumerate `base/presets/*.milk` (custom format) via idFileSystem — NOT
  `neo/ui/presets/` (source-tree location was a mistake; presets are game data, move them
  under `base/`).
- **Device selection**: v1 = two entries (Engine output / System loopback [default
  render device]). v1.5 = enumerate WASAPI render endpoints (IMMDeviceEnumerator) for
  loopback of a chosen device, and expose XAudio2 `listDevices`/`s_device` for output.

## C++ architecture

New class `idVisualizerManager` (single owner of visualizer state; NOT inside
`idSoundSystemLocal`):
- owns `idPlaylistManager`, `AudioAnalyzer` (or consumes the global one), current
  `VisualizerPreset`, source mode.
- exposes state to the GUI each frame: `gui->SetStateString("track0..N", ...)`,
  `SetStateInt("trackSel")`, `SetStateFloat("rms")`, etc.
- handles GUI commands returned from `HandleEvent` / named events:
  `play`, `pause`, `next`, `prev`, `selectTrack <n>`, `selectPreset <n>`,
  `setSource <engine|loopback>`.
- console commands for everything the UI does (`vis_play <file>`, `vis_preset <name>`,
  `vis_source loopback`) so features are testable before the GUI exists.

Playback path (v1): play the selected file through the existing sound system —
`PlayShaderDirectly` on an implicitly generated sound shader works for `.wav` today
(`idSoundShader::SetDefaultText` auto-generates `sound <name> { <name>.wav }`), keeping
`SSF_MUSIC`-style global flat playback. OGG/FLAC arrive with the decoder repair
(see PROGRESS repair list) — decode-to-PCM-in-memory first, true streaming later.

## Interaction contract (gui ↔ C++)

- `.gui` uses `notime`d windows + `listDef` with `gui::trackN` text vars.
- Selection: list window sets `gui::trackSel`; `onAction` emits `selectTrack` command
  string; `idVisualizerManager::HandleGuiCommand` consumes it.
- Now-playing + spectrum readouts: manager writes `gui::npTitle`, `gui::band0..5`
  every frame; bars in the GUI bind `scale`/`matcolor` expressions to those vars (the
  GUI expression evaluator reads `gui::` floats directly — no custom drawing code).

## Later (v2, after MilkDrop-style engine exists)

- Shell integration: add a "Visualizer" `idMenuScreen` to the main menu SWF handler,
  with the canvas embedded via a placeholder sprite's `material` var.
- ZGE-style layer strip + per-knob audio routing UI (docs/research-zge-visualizer.md) —
  likely justified only if we outgrow single-preset MilkDrop mode.
- Preset hot-reload (`reloadDecls`-style) and A/B preset blending controls.

## Repair prerequisites (from docs/audit-existing-visualizer-code.md)

1. `idPlaylistManager` compile fixes (value-list semantics, real playlist parsing).
2. `MilkDropParser` include/path fixes + route through idFileSystem; move presets to
   `base/presets/`.
3. Analyzer hooked into `idSoundSystemLocal::Render()` so `gui::band*` values are live.
4. Delete the current `VisualizerUI.cpp` approach (invented idWindow APIs); replace with
   `idVisualizerManager` + authored `guis/visualizer.gui` per this plan.

---

## Implementation addendum (from the full ui/ and menus/ subsystem reads)

**Legacy `.gui` path — concrete mechanics (confirmed):**
- Feed lists through **`idListGUI`** (`uiManager->AllocListGUI()` → `Config(gui, "songList")`
  → `Push/Add(id, text)`), which writes `songList_item_N` state keys that `listDef`
  reads; selection comes back as `songList_sel_0` + the window's `onEnter` command.
- `idListWindow` supports multi-column rows (`\t`-split, `tabstops/tabaligns`) and
  **icon columns** (`TAB_TYPE_ICON` draws a named material — album art / now-playing
  marker for free).
- Device picker: **`choiceDef`** (`idChoiceWindow`) bound to a `gui::` var or cvar —
  exactly a cycling option control. Volume/seek: `sliderDef`.
- Reactive canvas: custom `Redraw()` override on our `idUserInterfaceLocal` subclass
  (draw after `desktop->Redraw()`), or a full-rect material driven by shader parms.
  Do NOT use `renderDef` (idRenderWindow) — BFG dropped offset-viewport support.
- The three arcade minigame windows (GameSSDWindow etc.) prove fully custom-drawing
  windows hosted in `.gui` files are a supported pattern.
- Note: `AddGui()` was already added to `idUserInterfaceManager` (UserInterface.h:151,
  impl UserInterface.cpp:221) by the earlier experiment — keep it; it's sound.

**SWF path (v2) — exact recipes now known** (docs/engine-map.md, menus section):
- Tabbed Song|Playlist|Device|Preset layout maps 1:1 onto the **`idMenuHandler_PDA`**
  pattern (persistent shared DynamicList + scrollbar + NavBar tab strip).
- Song/preset list screen: copy **`MenuScreen_Shell_Dev.cpp`** (DynamicList of buttons →
  `WIDGET_ACTION_PRESS_FOCUSED` → run command). List + detail panel: copy
  `MenuScreen_Shell_Load.cpp`. Device/param sliders: `MenuScreen_Shell_GameOptions.cpp`
  + `idMenuDataSource`.
- `idMenuWidget_DynamicList::SetListData` accepts **raw idStr** rows — filesystem song
  lists and WASAPI device names push straight in.
- Registration: new `shellAreas_t` entry + `BIND_SHELL_SCREEN` + a `WIDGET_ACTION_COMMAND`
  case in Root/Settings; the blocker is **SWF assets** — reuse `menuSettings`/`menuLoad`
  sprite structures rather than authoring new Flash.

**Third path — ImGui (from RBDOOM pre-1.5 port, docs/research-rbdoom.md):**
Debug/param panel (band meters, per-knob routing, preset browser during development)
before any polished UI exists. Recommended to land BEFORE the .gui screens so every
audio/visual feature ships with live controls.

**Revised sequencing:** ImGui debug panel → console commands (`vis_*`) →
`.gui` picker screen (this doc's v1 design) → SWF shell screen (v2).
