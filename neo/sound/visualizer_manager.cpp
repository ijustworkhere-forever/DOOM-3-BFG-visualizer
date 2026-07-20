#include "../idlib/precompiled.h"
#pragma hdrstop
#include <windows.h>	// InitOnceExecuteOnce, for VisInitModTargets -- see its own comment
#include "visualizer_manager.h"
#include "visualizer_data.h"
#include "snd_local.h"
#include "MilkPreset.h"             // PRD M1: .milk preset parser (parse-only for now)
#include "MilkEvaluator.h"          // PRD M1 step 3: projectm-eval context + variable binding
#include "MilkShaderTranspiler.h"   // PRD M3: warp_/comp_ HLSL -> GLSL (hlsl2glslfork)
#include "../framework/KeyInput.h"  // idKeyInput::IsDown, for CTRL+arrow modifier checks (PRD M2 hotkeys)
#include "../renderer/RenderWorld.h" // idRenderWorld: geometry-only map load (PRD M4.5, Pillar F)
#include "../sys/win32/win_midi.h"  // g_midiInput: MIDI CC routing sources (PRD FR-E5, M4)
#include "../renderer/OpenGL/qgl.h" // qglGenFramebuffers/etc + GL_FRAMEBUFFER* constants (PRD M1 step 8)
#include "../renderer/Image.h"      // idImage/idImageManager/globalImages (PRD M1 step 8 FBO test)
#include "../renderer/Material.h"   // idMaterial::GetImageWidth/Height for layer aspect
#include "VisVideoEncoder.h"        // PRD FR-E3/M6: TGA sequence -> real video file
#include "../sys/win32/win_wallpaper.h"  // PRD M7: desktop wallpaper mode (Progman/WorkerW reparenting)
#include "../sys/win32/win_ndi.h"        // PRD M7: NDI network video output
#include "../sys/win32/win_imgui.h"      // PRD M4: ImGui init/frame lifecycle (Init/Shutdown live in win_glimp.cpp)
#include "../external/imgui/imgui.h"     // PRD M4: layer-editor UI content -- safe to include directly (no windows.h dependency)

// From the renderer (tr_local.h) - forward-declared to avoid pulling the heavy
// renderer-internal header into this sound TU. Layout matches exactly, and the
// free function links by name. Used to enumerate monitors/resolutions for the
// DISPLAY tab.
struct vidMode_t { int width; int height; int displayHz; };
extern bool R_GetModeListForDisplay( const int displayNum, idList<vidMode_t> & modeList );

static void VisApplyVideoMode();   // defined below; used by the DISPLAY console commands
static void VisCycleDisplayMode(); // defined below; PRD FR-D6 F hotkey (windowed -> fullscreen monitor 1..N -> windowed)
static void VisUpdateMidiOutput( float dtSec ); // defined below; PRD FR-E5 MIDI out, called from Frame()
static void VisUpdateDmxOutput( float dtSec );  // defined below; PRD FR-C9 Art-Net DMX out, called from Frame()
static int  VisDmxChannelTarget( int channel0 );// defined below; PRD FR-C9: read a DMX channel's assigned target index (for the GUI list)
static void VisSendNDIFrame();                  // defined below; PRD M7 NDI output, called from Draw2D

/*
========================
PRD M1 step 8: minimal off-screen FBO wrapper

The actual fix for MilkDrop-style feedback banding needs the visualizer to
render INTO a half-float off-screen target instead of the window's 8-bit
default backbuffer (see plans/PRD-implementation-status.md M1 step 8 for the
full architectural finding: CopyFramebuffer always reads GL_BACK, so no
amount of FMT_RGBA16F alone fixes banding without an actual FBO to draw
into). This class is that FBO: attach an existing idImage (allocated with
FMT_RGBA16F via AllocImage) as the color target, bind/unbind around a draw
pass.

Layering note, stated plainly rather than glossed over: every other raw
qgl-prefixed / GL-prefixed call in this engine lives in neo/renderer/ (Image_load.cpp,
gl_Image.cpp, RenderSystem_init.cpp); this is the first time this sound-
subsystem file reaches across that layering line. Done here anyway because
building a matching abstraction on idImage/idRenderSystem first (the
"proper" placement) is extra design work this class doesn't strictly need
to prove the round-trip works -- moving it into the renderer proper is a
documented follow-up, not a functional blocker. Availability is checked by
testing the qgl* function pointers directly for NULL (RenderSystem_init.cpp
leaves them NULL if GL_ARB_framebuffer_object isn't present) rather than
pulling in the heavy, renderer-internal tr_local.h just to read
glConfig.framebufferObjectAvailable -- same "forward-declare instead of a
heavy include" discipline as vidMode_t/R_GetModeListForDisplay above.
========================
*/
class idVisFBO {
public:
    idVisFBO() : m_fbo( 0 ) {}
    ~idVisFBO() { Destroy(); }

    bool Create( idImage * colorImage ) {
        Destroy();
        if ( colorImage == NULL || qglGenFramebuffers == NULL || qglFramebufferTexture2D == NULL ||
             qglCheckFramebufferStatus == NULL || qglBindFramebuffer == NULL ) {
            return false;   // extension not available on this GL -- see the class comment
        }
        qglGenFramebuffers( 1, &m_fbo );
        qglBindFramebuffer( GL_FRAMEBUFFER, m_fbo );
        qglFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorImage->GetTexNum(), 0 );
        const GLenum status = qglCheckFramebufferStatus( GL_FRAMEBUFFER );
        qglBindFramebuffer( GL_FRAMEBUFFER, 0 );   // unbind -- back to the default framebuffer
        if ( status != GL_FRAMEBUFFER_COMPLETE ) {
            idLib::Warning( "idVisFBO::Create: incomplete framebuffer (status 0x%x)", (unsigned int)status );
            Destroy();
            return false;
        }
        return true;
    }

    void Destroy() {
        if ( m_fbo != 0 && qglDeleteFramebuffers != NULL ) {
            qglDeleteFramebuffers( 1, &m_fbo );
        }
        m_fbo = 0;
    }

    bool IsValid() const { return m_fbo != 0; }

    void Bind() {
        if ( m_fbo != 0 && qglBindFramebuffer != NULL ) {
            qglBindFramebuffer( GL_FRAMEBUFFER, m_fbo );
        }
    }
    static void Unbind() {
        if ( qglBindFramebuffer != NULL ) {
            qglBindFramebuffer( GL_FRAMEBUFFER, 0 );
        }
    }

private:
    GLuint m_fbo;
};

idVisualizerManager g_visualizerManager;

idCVar vis_autoAdvance( "vis_autoAdvance", "1", CVAR_BOOL | CVAR_ARCHIVE, "advance to the next playlist track when the current one ends" );
idCVar vis_show( "vis_show", "1", CVAR_BOOL | CVAR_ARCHIVE, "draw the visualizer overlay" );
idCVar vis_hud( "vis_hud", "1", CVAR_BOOL | CVAR_ARCHIVE, "show the now-playing / preset status line and change banner" );
idCVar vis_bands( "vis_bands", "7", CVAR_INTEGER | CVAR_ARCHIVE, "number of spectrum bands (e.g. 3, 5, 7, 9)", 1, 64 );
idCVar vis_effect( "vis_effect", "0", CVAR_INTEGER | CVAR_ARCHIVE, "visual effect: 0 bars, 1 radial, 2 scope, 3 ring, 4 particles, 5 spectrogram, 6 starfield, 7 phase scope", 0, 7 );
idCVar vis_fullscreen( "vis_fullscreen", "0", CVAR_BOOL | CVAR_ARCHIVE, "draw the effect over the whole window's client area instead of a bottom panel -- NOT the OS display/window mode, see vis_display/VisCycleDisplayMode (F hotkey) for that" );
idCVar vis_feedback( "vis_feedback", "0", CVAR_BOOL | CVAR_ARCHIVE, "MilkDrop-style feedback trail (implies fullscreen)" );
idCVar vis_feedbackDecay( "vis_feedbackDecay", "0.94", CVAR_FLOAT | CVAR_ARCHIVE, "feedback brightness retained per frame (trail length)", 0.5f, 0.995f );
idCVar vis_feedbackZoom( "vis_feedbackZoom", "1.010", CVAR_FLOAT | CVAR_ARCHIVE, "feedback zoom per frame (>1 = outward warp)", 0.95f, 1.1f );
idCVar vis_mod( "vis_mod", "1", CVAR_BOOL | CVAR_ARCHIVE, "enable audio->visual parameter routing (scale/hue/bright/zoom/rotate)" );
idCVar vis_lfoPeriod( "vis_lfoPeriod", "8.0", CVAR_FLOAT | CVAR_ARCHIVE, "seconds per cycle of the LFO modulation source", 0.25f, 120.0f );
idCVar vis_autoLoopback( "vis_autoLoopback", "1", CVAR_BOOL | CVAR_ARCHIVE, "arm WASAPI loopback at boot so any system audio drives the visualizer with no commands" );
idCVar vis_backgroundAudio( "vis_backgroundAudio", "1", CVAR_BOOL | CVAR_ARCHIVE, "keep audio + reactivity running when the window loses focus (don't mute on deactivate)" );
idCVar vis_layer( "vis_layer", "0", CVAR_BOOL | CVAR_ARCHIVE, "draw a sound-reactive image layer (see the IMAGES tab)" );
idCVar vis_layerList( "vis_layerList", "", CVAR_ARCHIVE, "semicolon-separated image files for reactive layers (managed by the IMAGES tab)" );
idCVar vis_layerScale( "vis_layerScale", "1.0", CVAR_FLOAT | CVAR_ARCHIVE, "base size multiplier for the image layer", 0.05f, 4.0f );
idCVar vis_layerAlpha( "vis_layerAlpha", "0.8", CVAR_FLOAT | CVAR_ARCHIVE, "base opacity of the image layer", 0.0f, 1.0f );
// PRD Pillar C follow-up (direct user report: "all images, effects, layers
// have scale but also x/y coordinates so things can be moved around in an
// almost xyz style"). -1..1 normalized offset (fraction of the visualizer's
// half-width/half-height), matching the existing mousex/mousey normalization
// convention, so it stays meaningful at any resolution. 0,0 = old hardcoded
// screen-center behavior, byte-for-byte unchanged for anyone who never touches these.
idCVar vis_effectX( "vis_effectX", "0", CVAR_FLOAT | CVAR_ARCHIVE, "horizontal position of the current single Effect (-1 left edge .. 0 center .. 1 right edge)", -1.0f, 1.0f );
idCVar vis_effectY( "vis_effectY", "0", CVAR_FLOAT | CVAR_ARCHIVE, "vertical position of the current single Effect (-1 top edge .. 0 center .. 1 bottom edge)", -1.0f, 1.0f );
idCVar vis_layerX( "vis_layerX", "0", CVAR_FLOAT | CVAR_ARCHIVE, "horizontal position of the image layer (-1 left edge .. 0 center .. 1 right edge)", -1.0f, 1.0f );
idCVar vis_layerY( "vis_layerY", "0", CVAR_FLOAT | CVAR_ARCHIVE, "vertical position of the image layer (-1 top edge .. 0 center .. 1 bottom edge)", -1.0f, 1.0f );
idCVar vis_layerColorize( "vis_layerColorize", "0", CVAR_BOOL | CVAR_ARCHIVE, "tint the image layer with vis_layerHue (below) instead of its natural colors" );
// PRD Pillar C follow-up (direct user report: "there is no way to change the
// hue with a slider and tint with routed hue exists but i'm not clear on how
// it works or how to use it"). Previously "Tint with routed hue" reused the
// GLOBAL VMT_HUE target -- the same LFO-driven rainbow-cycle value that
// drives Bars/Radial/etc's color, with no slider anywhere near the Image
// Layer panel and no obvious relationship to it. This is a dedicated,
// directly-adjustable, directly-bindable hue just for the image layer.
idCVar vis_layerHue( "vis_layerHue", "0", CVAR_FLOAT | CVAR_ARCHIVE, "image layer tint hue (0..1), used when vis_layerColorize is on -- drag the slider or right-click it to bind to an audio band/MIDI/mouse", 0, 1 );
idCVar vis_warp( "vis_warp", "0", CVAR_INTEGER | CVAR_ARCHIVE, "feedback warp: 0 off, 1 mesh, 2 hi-res mesh, 3 fragment-shader (spike); implies vis_feedback", 0, 3 );
idCVar vis_warpMode( "vis_warpMode", "0", CVAR_INTEGER | CVAR_ARCHIVE, "warp shape: 0 ripple, 1 swirl, 2 tunnel, 3 fisheye", 0, 3 );
idCVar vis_warpAmount( "vis_warpAmount", "0.06", CVAR_FLOAT | CVAR_ARCHIVE, "feedback warp displacement depth", 0.0f, 0.40f );
idCVar vis_warpFreq( "vis_warpFreq", "8.0", CVAR_FLOAT | CVAR_ARCHIVE, "feedback warp ripple frequency (rings across the screen)", 1.0f, 30.0f );
idCVar vis_warpSpeed( "vis_warpSpeed", "1.5", CVAR_FLOAT | CVAR_ARCHIVE, "feedback warp ripple flow speed", 0.0f, 8.0f );
idCVar vis_kaleido( "vis_kaleido", "0", CVAR_INTEGER | CVAR_ARCHIVE, "kaleidoscope symmetry on the warp mesh: 0 off, else N mirrored wedges", 0, 16 );
idCVar vis_bloom( "vis_bloom", "0", CVAR_FLOAT | CVAR_ARCHIVE, "bloom/glow post-process intensity (0 = off)", 0.0f, 1.0f );
idCVar vis_bloomThreshold( "vis_bloomThreshold", "0.55", CVAR_FLOAT | CVAR_ARCHIVE, "brightness above which pixels bloom", 0.0f, 1.0f );
idCVar vis_bloomRadius( "vis_bloomRadius", "0.6", CVAR_FLOAT | CVAR_ARCHIVE, "bloom blur radius", 0.0f, 1.0f );
idCVar vis_bloomBeat( "vis_bloomBeat", "0", CVAR_FLOAT | CVAR_ARCHIVE, "how much the beat/bass pulses the bloom intensity", 0.0f, 1.0f );
idCVar vis_presetCycle( "vis_presetCycle", "0", CVAR_BOOL | CVAR_ARCHIVE, "auto-cycle through base/presets on a timer" );
idCVar vis_presetCycleSecs( "vis_presetCycleSecs", "30", CVAR_FLOAT | CVAR_ARCHIVE, "seconds between preset auto-switches", 2.0f, 600.0f );
idCVar vis_presetCycleOnBeat( "vis_presetCycleOnBeat", "1", CVAR_BOOL | CVAR_ARCHIVE, "when cycling, wait for the next beat after the timer so switches land musically" );
idCVar vis_presetCycleShuffle( "vis_presetCycleShuffle", "0", CVAR_BOOL | CVAR_ARCHIVE, "cycle presets in a shuffled order instead of sequentially" );
idCVar vis_milkSoftCutSecs( "vis_milkSoftCutSecs", "2.5", CVAR_FLOAT | CVAR_ARCHIVE, "PRD FR-A5: crossfade duration in seconds for a soft-cut .milk preset auto-advance (hard cuts, beat-triggered via vis_presetCycleOnBeat, are instant)", 0.1f, 30.0f );
// PRD FR-D1 follow-up: lets .milk presets be browsed/loaded from a custom
// external pack (e.g. a downloaded community preset library), not just the
// bundled presets/milk/ directory. The pack's own root directory must first
// be registered as a search path (vis_presetSearchPath -- an alias for
// vis_mapSearchPath/RegisterModSearchPath, same underlying mechanism, just
// named for discoverability from the preset side); this cvar is the
// relative subdirectory INSIDE any registered search path that actually
// holds the .milk files -- e.g. a pack unpacked as
// "<install>/milkdrop_presets/presets-cream-of-the-crop-master/<artist>/..."
// needs vis_presetSearchPath milkdrop_presets, then this cvar set to
// "presets-cream-of-the-crop-master" (ListFilesTree recurses, so artist/
// style subfolders under it are found automatically). Every existing
// presets/milk scan site now reads this cvar instead of a hardcoded literal,
// so changing it re-points the PRESETS tab / vis_milkList/vis_milkPreset
// random-pick sources all at once.
idCVar vis_milkPresetSubdir( "vis_milkPresetSubdir", "presets/milk", CVAR_ARCHIVE, "relative subdirectory (within any registered search path) to scan for .milk presets -- change to browse a custom preset pack registered via vis_presetSearchPath" );
// tracks the most recently registered custom milkdrop search-path root (set
// by both vis_presetSearchPath and the GUI's "Register" button) so
// vis_milkList/the GUI Scan button can self-heal the common "registered the
// FULL path all the way down to the pack folder, but left vis_milkPresetSubdir
// set to that same folder name" double-nesting mistake -- see their own
// comments for the full story.
static idStr s_lastMilkSearchPath;

// PRD follow-up (direct, repeated user request: "every time i launch the
// app i need to re-register paths for maps and milkdrop presets ... is
// there a way we can save these to a config file"). idFileSystem::
// RegisterModSearchPath itself has no persistence of its own -- it's a
// pure runtime "add this directory" call, forgotten the moment the process
// exits. These two CVAR_ARCHIVE cvars remember every directory ever
// registered via vis_mapSearchPath/vis_presetSearchPath or their GUI
// "Register" buttons (semicolon-separated, same list format vis_layerList
// already uses elsewhere in this file), and VisReapplySavedSearchPaths
// (called once at startup from Frame()) re-registers all of them
// automatically, so the user never has to retype/reclick them again.
idCVar vis_mapSearchPathSaved( "vis_mapSearchPathSaved", "", CVAR_ARCHIVE, "semicolon-separated map/mod search-path directories registered via vis_mapSearchPath or the Map Flythrough panel's Register button -- auto-reapplied at every launch" );
idCVar vis_presetSearchPathSaved( "vis_presetSearchPathSaved", "", CVAR_ARCHIVE, "semicolon-separated MilkDrop preset search-path directories registered via vis_presetSearchPath or the MilkDrop Presets panel's Register button -- auto-reapplied at every launch" );

// Fixed real bug (direct user report, twice): registering the FULL absolute
// path all the way down to the pack folder itself (e.g.
// "C:\...\milkdrop_presets\presets-cream-of-the-crop-master") while
// vis_milkPresetSubdir was STILL set to that same folder name resolves to a
// nonexistent double-nested directory. The obvious-looking fix -- clear
// vis_milkPresetSubdir and rescan the registered root directly via
// ListFilesTree("", ...)/ListFilesTree(".", ...) -- runs into TWO separate
// PRE-EXISTING engine bugs in FileSystem.cpp's GetFileListTree, confirmed by
// direct testing and code reading, not guessed:
//   1. relativePath="" makes GetFileList's "fullRelativePath" concatenation
//      produce a LEADING-SLASH child name ("/Dancer"), which the engine's
//      own IsOSPath() then misreads as an already-absolute OS path,
//      silently breaking BuildOSPath's resolution for every recursed call.
//   2. relativePath="." has the same problem one level removed: every
//      recursed child name becomes "./Dancer", and GetFileListTree's own
//      `if ( folders[i][0] == '.' ) continue;` guard (meant to skip "."/".."
//      pseudo-entries) then wrongly excludes EVERY subdirectory, since the
//      check tests the full concatenated name's first character, not just
//      the entry's own basename.
// Given both "empty root" sentinels are broken, this takes the safer,
// surgical path instead: if the registered root's own LAST path component
// matches vis_milkPresetSubdir exactly, the user has literally registered
// container+subdir already joined together -- so re-register just the
// PARENT directory as the search path (keeping subdir as-is), which reduces
// back to the exact non-empty-relativePath container+subdir shape already
// proven working all session, no risky root-scan needed at all.
static void VisTryFixMilkDoubleNesting() {
    if ( s_lastMilkSearchPath.IsEmpty() || vis_milkPresetSubdir.GetString()[0] == '\0' ) {
        return;
    }
    idStr parent = s_lastMilkSearchPath;
    parent.StripFilename();
    if ( parent.Length() >= s_lastMilkSearchPath.Length() ) {
        return;   // no path separator found -- nothing to strip
    }
    idStr lastComponent = s_lastMilkSearchPath.Right( s_lastMilkSearchPath.Length() - parent.Length() );
    lastComponent.StripLeading( '/' );
    lastComponent.StripLeading( '\\' );
    if ( idStr::Icmp( lastComponent.c_str(), vis_milkPresetSubdir.GetString() ) != 0 ) {
        return;   // not the double-nesting shape -- leave it alone, some other reason found 0
    }
    idLib::Printf( "vis_milkList/Scan: registered root '%s' already ends in the subdir '%s' -- re-registering '%s' as the search path instead\n",
        s_lastMilkSearchPath.c_str(), vis_milkPresetSubdir.GetString(), parent.c_str() );
    fileSystem->RegisterModSearchPath( parent.c_str() );
    s_lastMilkSearchPath = parent;
}

idCVar vis_palette( "vis_palette", "0", CVAR_INTEGER | CVAR_ARCHIVE, "color palette: 0 rainbow, 1 fire, 2 ocean, 3 synthwave, 4 mono", 0, 4 );
idCVar vis_hueShiftGlobal( "vis_hueShiftGlobal", "0", CVAR_FLOAT | CVAR_ARCHIVE, "global hue offset added to all colors", 0, 1 );
// PRD FR-E5: "trigger preset/effect/palette changes on notes" -- consumed by
// the note-on edge-detect block inlined in Frame() (see there for why this
// is inline rather than a free function: AdvancePreset() is private).
idCVar vis_midiNoteAdvance( "vis_midiNoteAdvance", "0", CVAR_BOOL | CVAR_ARCHIVE, "PRD FR-E5: any incoming MIDI note-on immediately advances the preset (milk hard-cut, or the .cfg cycle's AdvancePreset)" );
idCVar vis_midiNotePalette( "vis_midiNotePalette", "0", CVAR_BOOL | CVAR_ARCHIVE, "PRD FR-E5: any incoming MIDI note-on also cycles vis_palette" );
// idMidiInput only exposes current-state polling (GetNoteOn), not an event
// queue, so the note-on trigger block edge-detects the on-transition itself
// against this previous-frame snapshot -- a held note must not re-trigger
// every single frame.
static bool s_midiNoteOnPrev[16][128];

// Cached 2D solid material (white TGA + blend + vertexColor). DrawFilled's
// built-in _white material has no 'blend' stage and doesn't render in the GUI
// overlay, so we draw every rect through this instead.
//
// CRITICAL: declManager->FindMaterial must be called on the MAIN thread (decl
// loading asserts otherwise). Draw2D runs on the SMP draw-worker thread, so we
// resolve the material in Frame() (called from soundSystem->Render on the main
// thread) and only USE the cached pointer while drawing.
static const idMaterial * s_visSolid = NULL;
static const idMaterial * s_visSolidAdd = NULL;    // PRD M5 wavecode: additive-blend variant (bAdditive=1 waves)
static const idMaterial * s_visFeedback = NULL;   // samples the captured prev frame
// PRD M1 step 8: two full-precision (FMT_RGBA16F) ping-pong feedback targets,
// fixing the banding the single-buffer s_visFeedback/"visualizer/feedbackrt"
// scheme couldn't -- CaptureRenderToImage always re-quantizes to the window's
// 8-bit backbuffer regardless of the destination texture's own format (see
// PRD-implementation-status.md M1 step 8), so no amount of format-only fixing
// helps there. Here, each frame renders (via BeginOffscreenRender/
// EndOffscreenRender) directly into whichever of A/B isn't currently being
// SAMPLED, at full precision, with no read-back step at all -- the rendered
// texture already IS next frame's feedback source. s_visFeedback/
// "visualizer/feedbackrt" stay exactly as they were, still feeding the
// experimental wq>=3 custom-fragment-shader warp path (visualizer/warp's
// fragmentMap is bound to that image name at .mtr parse time and can't be
// swapped per-frame without a deeper shader-side change) -- a deliberate,
// documented scope boundary, not an oversight.
static const idMaterial * s_visFeedbackA = NULL;
static const idMaterial * s_visFeedbackB = NULL;
static const idMaterial * s_visFeedbackActive = NULL;   // this frame's SAMPLE source (set in Draw2D)
static const char * s_visFeedbackActiveImageName = NULL;   // PRD M3: same image, as a name (for EnqueueVisMilkWarpDraw)
static bool  s_milkPingPongParity = false;   // false: write A / read B; true: write B / read A
static const idMaterial * s_visWarp = NULL;       // custom-shader warp (spike; may be NULL)
static const idMaterial * s_visBloom = NULL;      // bloom post-process (may be NULL)
static std::vector<const idMaterial *> s_layerMats;   // resolved reactive image layers
static idStr s_layerListCached;                       // vis_layerList value they were resolved for
static float s_aspectFix = 1.0f;                      // x-scale to keep circles round on wide displays
static bool s_feedbackCaptured = false;           // have we captured at least one frame?

// transient on-screen banner (preset loaded / track changed); fades over ~2.5s
static idStr s_bannerText;
static int   s_bannerMs = 0;
static const int VIS_BANNER_MS = 5000;      // banner visible ~5s, then fades out
static void VisSetBanner( const char * text ) {
    s_bannerText = text;
    s_bannerMs = Sys_Milliseconds();
}

// PRD FR-D4/M7: MilkDrop preset ratings (F6 hotkey), persisted across runs.
// Keyed by the .milk preset's own name (idMilkPreset::GetName(), the
// filename with path/extension already stripped -- stable across reloads
// of the same file, same identity real MilkDrop's own rating system keys
// on). Lazily loaded on first use rather than at startup, since most runs
// never touch ratings at all; idDict's own binary WriteToFileHandle/
// ReadFromFileHandle round-trip (already used elsewhere in the engine for
// savegame-style persistence) is reused as-is rather than hand-rolling a
// text parser for what's just a flat name->rating map.
static idDict s_milkRatings;
static bool   s_milkRatingsLoaded = false;
static const char * const VIS_MILK_RATINGS_PATH = "presets/milk/ratings.bin";

static void VisEnsureMilkRatingsLoaded() {
    if ( s_milkRatingsLoaded ) {
        return;
    }
    s_milkRatingsLoaded = true;
    idFile * f = fileSystem->OpenFileRead( VIS_MILK_RATINGS_PATH );
    if ( f == NULL ) {
        return;   // no ratings saved yet -- NFR-3, not an error
    }
    s_milkRatings.ReadFromFileHandle( f );
    fileSystem->CloseFile( f );
}

static void VisSaveMilkRatings() {
    idFile * f = fileSystem->OpenFileWrite( VIS_MILK_RATINGS_PATH );
    if ( f == NULL ) {
        idLib::Warning( "visualizer: could not save milk preset ratings to '%s'", VIS_MILK_RATINGS_PATH );
        return;
    }
    s_milkRatings.WriteToFileHandle( f );
    fileSystem->CloseFile( f );
}

static int VisGetMilkRating( const char * presetName ) {
    VisEnsureMilkRatingsLoaded();
    return idMath::ClampInt( 0, 5, s_milkRatings.GetInt( presetName, 0 ) );
}

static void VisSetMilkRating( const char * presetName, int rating ) {
    VisEnsureMilkRatingsLoaded();
    s_milkRatings.SetInt( presetName, idMath::ClampInt( 0, 5, rating ) );
    VisSaveMilkRatings();
}

/*
========================
Audio -> visual parameter routing (ZGE/AudioRider "knob" model)

Each visual TARGET (scale/hue/bright/zoom/rotate) is driven by one audio SOURCE
through a simple affine route:  value = base + source * amount.  The default
routes make the visualizer musical out of the box; `vis_route <target> <source>
<amount> [base]` re-patches any knob live.  All routes are evaluated once per
frame in Frame() on the MAIN thread (analyzer values are updated there) and the
results cached in s_mod[]/s_rotAngle so the draw-worker thread only reads floats.
========================
*/
enum visSource_t {
    VMS_NONE = 0, VMS_BASS, VMS_MID, VMS_TREB,
    VMS_BASSATT, VMS_MIDATT, VMS_TREBATT,
    VMS_RMS, VMS_BEAT, VMS_LEVEL, VMS_LFO,
    // PRD Pillar C follow-up (direct, repeated user request: "i should be
    // able to right click on a slider ... and bind it to ... mouse location
    // x/y, all of these things are documented as functionalities of
    // zgameeditor"). Raw normalized [0,1] cursor position within the game
    // window's client area, tracked from SE_MOUSE_ABSOLUTE events in
    // MenuProcessEvent (see s_mouseNormX/Y below) -- no per-route extra data
    // needed, so unlike band/midicc-param/midinote these are plain fixed
    // enum sources, same as bass/mid/treb.
    VMS_MOUSEX, VMS_MOUSEY,
    // PRD FR-E5/M4: MIDI input as first-class routing sources. midicc1-4
    // are fixed channel-0 shortcuts (CC1 = mod wheel by convention on most
    // controllers), kept for backward compatibility with any existing
    // vis_route lines. VMS_MIDICC_PARAM is the fuller "any CC on any
    // channel" source, previously documented as a follow-up -- parsed from
    // a "midicc<channel>_<cc>" source string (e.g. "midicc3_74") by
    // VisParseMidiCCSource, with the actual channel/cc numbers stored in
    // the route itself (visRoute_t::midiChannel/midiCC) since a single enum
    // value can't carry them.
    VMS_MIDICC1, VMS_MIDICC2, VMS_MIDICC3, VMS_MIDICC4,
    VMS_MIDICC_PARAM,
    // PRD FR-E5: "use note velocity as an envelope" -- the note-side
    // counterpart of VMS_MIDICC_PARAM, same "channel/number stored in the
    // route itself" pattern (parsed from "midinote<channel>_<note>" by
    // VisParseMidiNoteSource, since a single enum value can't carry them
    // either). Reads as the held note's velocity while the key is down,
    // 0 while up (idMidiInput doesn't reset velocity to 0 on note-off, so
    // this gates on GetNoteOn itself rather than trusting a stale GetNoteVelocity).
    VMS_MIDINOTE_VEL,
    // PRD Pillar C follow-up (direct user report: "i do not see how i can
    // link an effect to an audio band... the way a user can in
    // zgameeditor for fl studio"): VMS_BASS/MID/TREB only ever sampled the
    // fixed 3-way MilkDrop-style split, never one of the user's actual
    // configured vis_bands (1-64) spectrum bands. Same "placeholder enum
    // value + extra int stored in the route" pattern as VMS_MIDICC_PARAM
    // above (a single enum value can't carry the band index) -- parsed
    // from a "bandN" source string (e.g. "band3") by VisParseBandSource,
    // with the index stored in visRoute_t::bandIndex. Not exposed in the
    // GUI's fixed-source combo loop (which stops before VMS_MIDICC_PARAM);
    // instead the GUI appends one extra Selectable per currently-configured
    // band, regenerated every frame since vis_bands can change live.
    VMS_BAND,
    VMS_COUNT
};
static const char * s_sourceNames[VMS_COUNT] = {
    "none", "bass", "mid", "treb", "bassatt", "midatt", "trebatt",
    "rms", "beat", "level", "lfo",
    "mousex", "mousey",
    "midicc1", "midicc2", "midicc3", "midicc4",
    "midicc:param",  // never typed directly -- see VisParseMidiCCSource
    "midinote:vel",  // never typed directly -- see VisParseMidiNoteSource
    "band:param"     // never typed directly -- see VisParseBandSource
};

// PRD Pillar C follow-up: last-seen normalized mouse position, updated from
// SE_MOUSE_ABSOLUTE in MenuProcessEvent. Default (0.5, 0.5) (screen center)
// so an unrouted-but-curious `vis_route <target> mousex ...` before the
// first mouse move ever reaches this window reads a sane mid-range value
// instead of 0.
static float s_mouseNormX = 0.5f;
static float s_mouseNormY = 0.5f;

// Parses a "band<N>" source string (e.g. "band3" = spectrum band index 3)
// for the per-band routing source. Bare digits only, no separator needed
// since there's just one number to extract (unlike midicc/midinote's
// channel_number pairs).
static bool VisParseBandSource( const char * str, int & bandIndex ) {
    if ( idStr::Icmpn( str, "band", 4 ) != 0 ) {
        return false;
    }
    const char * rest = str + 4;
    if ( rest[0] == '\0' ) {
        return false;
    }
    for ( const char * p = rest; *p != '\0'; p++ ) {
        if ( !isdigit( (unsigned char)*p ) ) {
            return false;
        }
    }
    const int idx = atoi( rest );
    if ( idx < 0 || idx >= 64 ) {   // matches vis_bands' own 1-64 range
        return false;
    }
    bandIndex = idx;
    return true;
}

// Parses a "midicc<channel>_<ccNumber>" source string (e.g. "midicc3_74" =
// channel 3, CC 74) for the general MIDI CC routing source. Bare
// "midicc1".."midicc4" (no underscore) are NOT matched here -- they fall
// through to the existing fixed-name lookup in VisFindName, unaffected.
static bool VisParseMidiCCSource( const char * str, int & channel, int & ccNumber ) {
    if ( idStr::Icmpn( str, "midicc", 6 ) != 0 ) {
        return false;
    }
    const char * rest = str + 6;
    const char * underscore = strchr( rest, '_' );
    if ( underscore == NULL ) {
        return false;
    }
    char chBuf[8];
    const int chLen = (int)( underscore - rest );
    if ( chLen <= 0 || chLen >= (int)sizeof( chBuf ) ) {
        return false;
    }
    for ( int i = 0; i < chLen; i++ ) {
        if ( !isdigit( (unsigned char)rest[i] ) ) {
            return false;
        }
    }
    memcpy( chBuf, rest, chLen );
    chBuf[chLen] = '\0';
    const char * ccStr = underscore + 1;
    if ( ccStr[0] == '\0' ) {
        return false;
    }
    for ( const char * p = ccStr; *p != '\0'; p++ ) {
        if ( !isdigit( (unsigned char)*p ) ) {
            return false;
        }
    }
    const int ch = atoi( chBuf );
    const int cc = atoi( ccStr );
    if ( ch < 0 || ch > 15 || cc < 0 || cc > 127 ) {
        return false;
    }
    channel = ch;
    ccNumber = cc;
    return true;
}

// Parses a "midinote<channel>_<noteNumber>" source string (e.g. "midinote0_60"
// = channel 0, note 60/middle C) for the general MIDI note-velocity routing
// source -- same shape as VisParseMidiCCSource above, just for notes instead
// of CCs.
static bool VisParseMidiNoteSource( const char * str, int & channel, int & noteNumber ) {
    if ( idStr::Icmpn( str, "midinote", 8 ) != 0 ) {
        return false;
    }
    const char * rest = str + 8;
    const char * underscore = strchr( rest, '_' );
    if ( underscore == NULL ) {
        return false;
    }
    char chBuf[8];
    const int chLen = (int)( underscore - rest );
    if ( chLen <= 0 || chLen >= (int)sizeof( chBuf ) ) {
        return false;
    }
    for ( int i = 0; i < chLen; i++ ) {
        if ( !isdigit( (unsigned char)rest[i] ) ) {
            return false;
        }
    }
    memcpy( chBuf, rest, chLen );
    chBuf[chLen] = '\0';
    const char * noteStr = underscore + 1;
    if ( noteStr[0] == '\0' ) {
        return false;
    }
    for ( const char * p = noteStr; *p != '\0'; p++ ) {
        if ( !isdigit( (unsigned char)*p ) ) {
            return false;
        }
    }
    const int ch = atoi( chBuf );
    const int note = atoi( noteStr );
    if ( ch < 0 || ch > 15 || note < 0 || note > 127 ) {
        return false;
    }
    channel = ch;
    noteNumber = note;
    return true;
}

// PRD M0: modulation targets are a REGISTRY, not a fixed enum, so any future
// layer/effect can add its own routable knob (name + default + clamp/wrap)
// without touching this core code. VMT_* below are resolved INDICES into the
// registry (looked up once at startup by name), not compile-time constants —
// every existing `s_mod[VMT_SCALE]`-style call site keeps working unchanged.
struct visModTargetDef_t {
    const char * name;
    float        defaultBase;
    float        clampMin, clampMax;   // clampMin > clampMax => no clamp
    bool         wrap01;               // hue-style wrap to [0,1)
    // PRD FR-C1-EXPAND: which layer-stack slot this target belongs to, or -1
    // for a process-wide global (every one of the 8 original built-ins). This
    // is the "owner handle" the PRD calls for so two instances of the same
    // effect in the stack don't fight over one shared value -- purely
    // descriptive metadata (the value still lives at index t in s_mod like any
    // other target); slot code resolves its own index via s_slotTarget below.
    int          ownerSlot;
};

// PRD follow-up (direct, repeated, urgent user report: "there is still no
// way to bind audio band outputs to parameters like hue shift ... which
// has been my ask this whole time and is an important feature").
//
// ROOT CAUSE, finally found via raw fopen/fprintf tracing (bypassing engine
// logging entirely so it works even before `common` exists): s_targetDefs
// (previously idList<T>, then this VisFixedList<T,N>) was being populated
// correctly -- VisInitModTargetsOnce() genuinely ran and genuinely filled it
// to Num()==41, confirmed by a log line immediately before that function's
// `return TRUE` -- but a SECOND log line, placed in the container's own
// default constructor, then fired AFTER that, at the exact same address,
// resetting count back to 0. That is C++'s static-initialization-order
// fiasco: something reaches into this registry (via VisInitModTargets(),
// called from many places, some of them very early in engine/window/
// renderer bring-up) before this translation unit's own dynamic
// initializers -- including the container's constructor -- have run, so the
// constructor's `count = 0` clobbers real data that was already, legitimately
// written to that same static storage moments earlier. This is exactly why
// every PLAIN SCALAR static in this file (the VMT_* ints, the s_milk*/s_vis*
// bools and floats, dozens of them) was NEVER affected: they have no
// user-provided constructor, so they are only ever zero-initialized, and
// zero-initialization of static storage is guaranteed
// to happen before ANY code runs (no ordering hazard possible). The fix is
// to give VisFixedList that exact same guarantee: no user-provided
// constructor at all. `count` (and `data`) rely purely on static
// zero-initialization, so there is no dynamic-initializer left to run late
// and stomp on already-populated data, no matter how early something
// triggers the registry.
template< typename T, int N >
struct VisFixedList {
    T   data[N];
    int count;
    int Num() const { return count; }
    void SetNum( int n ) {
        if ( n > count ) {
            for ( int i = count; i < n && i < N; i++ ) {
                data[i] = T();
            }
        }
        count = ( n < N ) ? n : N;
    }
    int Append( const T & item ) {
        if ( count >= N ) {
            idLib::Warning( "VisFixedList::Append: capacity (%d) exceeded -- dropping registration", N );
            return count - 1;
        }
        data[count] = item;
        return count++;
    }
    T &       operator[]( int i )       { return data[i]; }
    const T & operator[]( int i ) const { return data[i]; }
};

static VisFixedList<visModTargetDef_t, 128> s_targetDefs;

static int VisRegisterModTargetOwned( const char * name, float defaultBase, float clampMin, float clampMax, bool wrap01, int ownerSlot ) {
    visModTargetDef_t def;
    def.name = name;
    def.defaultBase = defaultBase;
    def.clampMin = clampMin;
    def.clampMax = clampMax;
    def.wrap01 = wrap01;
    def.ownerSlot = ownerSlot;
    const int idx = s_targetDefs.Append( def );
    return idx;
}

// Backward-compatible global (ownerSlot = -1) registration -- every existing
// call site keeps this exact signature, so the 8 original built-ins register
// at the same indices as before.
static int VisRegisterModTarget( const char * name, float defaultBase, float clampMin, float clampMax, bool wrap01 ) {
    return VisRegisterModTargetOwned( name, defaultBase, clampMin, clampMax, wrap01, -1 );
}

static int VisFindModTarget( const char * name ) {
    for ( int i = 0; i < s_targetDefs.Num(); i++ ) {
        if ( idStr::Icmp( s_targetDefs[i].name, name ) == 0 ) {
            return i;
        }
    }
    return -1;
}

// PRD FR-C8: per-link curve/response shaping. The shape is a pure function of
// the (nominally 0..1) sampled+range-mapped source value, applied as a stage
// BETWEEN range-map and the final gain(amount)/offset(base) affine step (see
// VisUpdateModulation). `invert` (1-x) is a SEPARATE composable flag applied
// AFTER the shape function, not an exclusive mode -- exactly as the FR asks
// ("Invert ... composable with any of the above"). Direct user request:
// "curves or smoothing ... so that 1 to 100% can become a boolean or tapered."
enum visShape_t {
    VSHAPE_LINEAR = 0,   // identity (default). shapeParam unused.
    VSHAPE_EXP,          // x^k. shapeParam = exponent k (>0; k>1 = punchier low end).
    VSHAPE_LOG,          // log(1+k*x)/log(1+k). shapeParam = k (>0; perceptual compression).
    VSHAPE_SCURVE,       // smoothstep 3x^2-2x^3 soft knee. shapeParam unused.
    VSHAPE_THRESHOLD,    // boolean 0/1. shapeParam = threshold, shapeParam2 = hysteresis half-band.
    VSHAPE_QUANTIZE,     // N discrete levels. shapeParam = level count N (>=2).
    VSHAPE_COUNT
};
static const char * s_shapeNames[VSHAPE_COUNT] = {
    "linear", "exp", "log", "scurve", "threshold", "quantize"
};

// shape/invert/shapeParam/shapeParam2 are the FR-C8 additions; they are trailing
// members so the aggregate initializers below (which specify only source/amount/
// base positionally) keep compiling and default the rest to linear/no-invert.
// shapeState is transient per-frame hysteresis memory (last threshold output),
// never serialized.
struct visRoute_t {
    int source; float amount; float base;
    int midiChannel; int midiCC; int midiNote; int bandIndex;
    int shape; bool invert; float shapeParam; float shapeParam2; float shapeState;
};
// PRD follow-up: same VisFixedList swap as s_targetDefs above, and for the
// same reason -- these two are always resized together with s_targetDefs
// (see VisInitModTargetsOnce), so they must share its reliability
// characteristics, not idList's.
static VisFixedList<visRoute_t, 128> s_routes;
static VisFixedList<float, 128>      s_mod;

// PRD FR-C1-EXPAND: per-layer-stack-slot modulation targets. Mirrors the
// per-instance state pattern already used for the stack (visStackParticleState_t
// et al). One registry target per (slot, param) pair -- registered by name
// (slot<N>.<param>) so they route via `vis_route`/the generic GUI loop with no
// per-target special-casing. VIS_MOD_STACK_SLOTS must equal VIS_MAX_STACK_LAYERS
// (compile_time_assert'd where that constant is defined, further down the file);
// it's duplicated here only because VIS_MAX_STACK_LAYERS isn't in scope this
// early (it's declared near the stack code it belongs to).
static const int VIS_MOD_STACK_SLOTS = 4;
enum {
    VSP_OPACITY = 0,       // multiplier on visStackLayer_t.opacity at composite time
    VSP_PARTICLE_SPAWN,    // multiplier on the per-beat particle spawn count
    VSP_PARTICLE_SCALE,    // multiplier on drawn particle size
    VSP_STAR_SPEED,        // multiplier on starfield warp speed
    VSP_SPECTRO_BRIGHT,    // multiplier on spectrogram brightness
    // PRD Pillar C follow-up (direct user report: "all images, effects,
    // layers have scale but also x/y coordinates so things can be moved
    // around in an almost xyz style"). Additive offset on top of
    // visStackLayer_t.posX/posY, -1..1, unlike the multipliers above.
    VSP_POSX,
    VSP_POSY,
    VSP_COUNT
};
// resolved registry indices for each (slot, param); filled in VisInitModTargetsOnce.
static int   s_slotTarget[VIS_MOD_STACK_SLOTS][VSP_COUNT];
// persistent backing storage for the target names -- the registry keeps the
// char* by reference (VisRegisterModTargetOwned does NOT copy), and va()'s
// buffer is transient, so the built names must outlive registration. idStr set
// once and never reassigned => c_str() stays stable for the process lifetime.
static idStr s_slotTargetName[VIS_MOD_STACK_SLOTS][VSP_COUNT];

// Built-in targets, registered once in the same order as the old fixed enum
// so existing `vis_route`/preset .cfg lines and the resolved indices below are
// unaffected. Layers added later just call VisRegisterModTarget of their own.
static int VMT_SCALE, VMT_HUE, VMT_BRIGHT, VMT_ZOOM, VMT_ROTATE;
// PRD Pillar C follow-up (direct user report: "we...can[not] bind a
// signal/input to the options like warp amount or layer scale or hue"):
// these three ADD a routable modulation offset on top of their own cvar's
// value (defaultBase 0.0, no clamp -- sum semantics, matching FR-C1's "links
// can stack on a knob (sum/replace)"), rather than replacing the cvar
// entirely, so vis_warpAmount/vis_layerScale/vis_hueShiftGlobal keep working
// exactly as before for anyone who never routes anything to them.
static int VMT_WARPAMOUNT_MOD, VMT_LAYERSCALE_MOD, VMT_HUESHIFT_MOD;
// PRD FR-F4: map flythrough camera as real routing targets (closes the direct
// "camera not reactive" report). Multipliers with a neutral default of 1.0, so
// an unrouted camera behaves byte-for-byte like the old fixed constants.
static int VMT_CAM_SPEED, VMT_CAM_FOV;
// PRD Pillar C (direct, repeated user request: "we still cannot bind audio
// band levels to effect options like warp amount or bloom threshold ...
// this is the main goal of trying to implement ZgameEditor style editing").
// Same additive-offset-on-top-of-the-cvar pattern as warpamountmod/
// layerscalemod/hueshiftmod above -- every one of these is a no-clamp 0.0
// default, so an unrouted slider behaves byte-for-byte like before.
static int VMT_BLOOM_MOD, VMT_BLOOMTHRESH_MOD, VMT_BLOOMRADIUS_MOD;
static int VMT_FEEDBACKDECAY_MOD, VMT_KALEIDO_MOD, VMT_WARPFREQ_MOD, VMT_WARPSPEED_MOD;
// PRD Pillar C follow-up (direct user report: "image layer still does not
// have a way to bind it to an input of audio band or midi or any other" /
// "there is no way to change the hue with a slider"). vis_layerScale already
// had layerscalemod; these two round out the Image Layer panel.
static int VMT_LAYERALPHA_MOD, VMT_LAYERHUE_MOD;
// PRD follow-up: map-flythrough wireframe/background tint hues.
static int VMT_MAPWIREHUE_MOD, VMT_MAPBGHUE_MOD;
// PRD Pillar C follow-up (direct user report: "all images, effects, layers
// have scale but also x/y coordinates so things can be moved around in an
// almost xyz style"). Additive offset on top of vis_effectX/Y and
// vis_layerX/Y respectively -- same no-clamp pattern as every other _MOD
// above, so an unrouted position behaves byte-for-byte like the plain cvar.
static int VMT_EFFECTX_MOD, VMT_EFFECTY_MOD, VMT_LAYERX_MOD, VMT_LAYERY_MOD;

// Fixed real bug: this used to be a plain `bool s_modTargetsInit` guard,
// which is a classic double-checked-locking race under this engine's SMP
// threading -- one thread could see the (non-atomic) flag already true
// and read s_mod while ANOTHER thread was still in the middle of resizing/
// populating it (s_modTargetsInit was set true at the top, before the
// slower SetNum + loop work below it even ran).
//
// Fixed real bug, round 2: std::call_once looked like the textbook-correct
// fix, but direct instrumentation (fopen-based logging, bypassing this
// engine's own broken-at-boot savepath logging) proved it does NOT
// actually provide cross-thread visibility here -- one thread's call_once
// ran the lambda and left s_targetDefs.Num()==5, then a SECOND thread's
// call_once returned (correctly recognizing "already done") but still
// read s_targetDefs.Num()==0 at the SAME address. This engine creates
// threads via its own idSysThread wrapper (see idlib/Thread.cpp), not
// std::thread/_beginthreadex, and something about that combination broke
// call_once's synchronizes-with guarantee in practice, root cause not
// pursued further given a bulletproof alternative exists: Win32's
// InitOnceExecuteOnce is backed directly by the NT kernel, not the C++
// runtime's abstraction over it, and works correctly regardless of how
// the calling thread was created.
static INIT_ONCE s_modTargetsInitOnce = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK VisInitModTargetsOnce( PINIT_ONCE, PVOID, PVOID * ) {
    VMT_SCALE  = VisRegisterModTarget( "scale",  1.0f, 0.0f, 3.0f, false );
    VMT_HUE    = VisRegisterModTarget( "hue",    0.0f, 1.0f, 0.0f, true );   // no clamp, wraps
    VMT_BRIGHT = VisRegisterModTarget( "bright", 1.0f, 0.0f, 1.0f, false );
    VMT_ZOOM   = VisRegisterModTarget( "zoom",   0.0f, 1.0f, 0.0f, false );  // no clamp
    VMT_ROTATE = VisRegisterModTarget( "rotate", 0.0f, 1.0f, 0.0f, false );  // no clamp (rad/sec)
    // additive offsets on top of vis_warpAmount / vis_layerScale / vis_hueShiftGlobal
    VMT_WARPAMOUNT_MOD = VisRegisterModTarget( "warpamountmod", 0.0f, 0.0f, 0.0f, false ); // no clamp
    VMT_LAYERSCALE_MOD = VisRegisterModTarget( "layerscalemod", 0.0f, 0.0f, 0.0f, false ); // no clamp
    VMT_HUESHIFT_MOD   = VisRegisterModTarget( "hueshiftmod",   0.0f, 0.0f, 0.0f, false ); // no clamp

    // PRD FR-F4: camera fly-speed / FOV as multipliers (1.0 = unchanged). These
    // stay GLOBAL (ownerSlot -1) -- there's one flythrough camera. Speed clamps
    // to 0..8x, FOV to 0.2..3x, so an extreme route can't stall or invert them.
    VMT_CAM_SPEED = VisRegisterModTarget( "camspeed", 1.0f, 0.0f, 8.0f, false );
    VMT_CAM_FOV   = VisRegisterModTarget( "camfov",   1.0f, 0.2f, 3.0f, false );

    // PRD Pillar C: the remaining Effect-panel sliders the user can now
    // right-click and bind directly (VisBindableSliderFloat). Same additive-
    // offset, no-clamp pattern as warpamountmod/layerscalemod/hueshiftmod.
    VMT_BLOOM_MOD         = VisRegisterModTarget( "bloommod",          0.0f, 0.0f, 0.0f, false );
    VMT_BLOOMTHRESH_MOD   = VisRegisterModTarget( "bloomthresholdmod", 0.0f, 0.0f, 0.0f, false );
    VMT_BLOOMRADIUS_MOD   = VisRegisterModTarget( "bloomradiusmod",    0.0f, 0.0f, 0.0f, false );
    VMT_FEEDBACKDECAY_MOD = VisRegisterModTarget( "feedbackdecaymod",  0.0f, 0.0f, 0.0f, false );
    VMT_KALEIDO_MOD       = VisRegisterModTarget( "kaleidomod",        0.0f, 0.0f, 0.0f, false );
    VMT_WARPFREQ_MOD      = VisRegisterModTarget( "warpfreqmod",       0.0f, 0.0f, 0.0f, false );
    VMT_WARPSPEED_MOD     = VisRegisterModTarget( "warpspeedmod",      0.0f, 0.0f, 0.0f, false );
    // PRD Pillar C: Image Layer panel bindability (vis_layerScale already had
    // layerscalemod). layerhuemod wraps to [0,1) like the other hue targets.
    VMT_LAYERALPHA_MOD    = VisRegisterModTarget( "layeralphamod",     0.0f, 0.0f, 0.0f, false );
    VMT_LAYERHUE_MOD      = VisRegisterModTarget( "layerhuemod",       0.0f, 1.0f, 0.0f, true );
    // PRD follow-up: map-flythrough wireframe/background tint hues, both wrap to [0,1).
    VMT_MAPWIREHUE_MOD    = VisRegisterModTarget( "mapwirehuemod",     0.0f, 1.0f, 0.0f, true );
    VMT_MAPBGHUE_MOD      = VisRegisterModTarget( "mapbghuemod",       0.0f, 1.0f, 0.0f, true );
    // PRD Pillar C follow-up: position (x/y) rounds out scale as a routable
    // knob, ZGE-style. -1..1 clamp matches vis_effectX/Y's own range.
    VMT_EFFECTX_MOD = VisRegisterModTarget( "effectxmod", 0.0f, -1.0f, 1.0f, false );
    VMT_EFFECTY_MOD = VisRegisterModTarget( "effectymod", 0.0f, -1.0f, 1.0f, false );
    VMT_LAYERX_MOD  = VisRegisterModTarget( "layerxmod",  0.0f, -1.0f, 1.0f, false );
    VMT_LAYERY_MOD  = VisRegisterModTarget( "layerymod",  0.0f, -1.0f, 1.0f, false );

    // PRD FR-C1-EXPAND: per-stack-slot targets. Registered owned (ownerSlot =
    // slot) so two Particle (or any) layers in the stack route independently.
    // The first 5 (opacity..spectro) are neutral-1.0 multipliers with source
    // NONE by default => zero behavioural change until a user actually routes
    // one. posx/posy (PRD Pillar C follow-up: "all images, effects, layers
    // have scale but also x/y coordinates") are a different shape -- a
    // neutral-0.0 ADDITIVE offset on top of visStackLayer_t.posX/posY,
    // clamped -1..1 -- so they need their own default-base/clamp-min columns
    // rather than reusing the multiplier defaults above.
    {
        static const char * const kSlotParamSuffix[VSP_COUNT]      = { "opacity", "pspawn", "pscale", "starspeed", "spectro", "posx",  "posy"  };
        static const float        kSlotParamDefaultBase[VSP_COUNT] = { 1.0f,      1.0f,     1.0f,     1.0f,        1.0f,      0.0f,    0.0f    };
        static const float        kSlotParamClampMin[VSP_COUNT]    = { 0.0f,      0.0f,     0.0f,     0.0f,        0.0f,     -1.0f,   -1.0f    };
        static const float        kSlotParamMax[VSP_COUNT]         = { 1.0f,      8.0f,     8.0f,     8.0f,        4.0f,      1.0f,    1.0f    };
        for ( int slot = 0; slot < VIS_MOD_STACK_SLOTS; slot++ ) {
            for ( int k = 0; k < VSP_COUNT; k++ ) {
                s_slotTargetName[slot][k] = va( "slot%d.%s", slot, kSlotParamSuffix[k] );
                s_slotTarget[slot][k] = VisRegisterModTargetOwned(
                    s_slotTargetName[slot][k].c_str(), kSlotParamDefaultBase[k], kSlotParamClampMin[k], kSlotParamMax[k], false, slot );
            }
        }
    }

    s_routes.SetNum( s_targetDefs.Num() );
    s_mod.SetNum( s_targetDefs.Num() );
    for ( int i = 0; i < s_targetDefs.Num(); i++ ) {
        s_routes[i].source = VMS_NONE;
        s_routes[i].amount = 0.0f;
        s_routes[i].base   = s_targetDefs[i].defaultBase;
        // PRD FR-C8 defaults: linear/no-invert = a byte-for-byte no-op vs. the
        // pre-shape behavior. shapeParam=1.0 is a neutral start (x^1=x) so a
        // route switched to exp before its param is touched still passes x
        // through; shapeParam2 is a small default hysteresis half-band.
        s_routes[i].shape       = VSHAPE_LINEAR;
        s_routes[i].invert      = false;
        s_routes[i].shapeParam  = 1.0f;
        s_routes[i].shapeParam2 = 0.05f;
        s_routes[i].shapeState  = 0.0f;
        s_mod[i]            = s_targetDefs[i].defaultBase;
    }
    // default routes (same musical defaults as before the refactor)
    s_routes[VMT_SCALE]  = { VMS_BASSATT, 0.40f, 0.90f };
    s_routes[VMT_HUE]    = { VMS_LFO,     1.00f, 0.00f };
    s_routes[VMT_BRIGHT] = { VMS_BEAT,    0.20f, 0.80f };
    s_routes[VMT_ZOOM]   = { VMS_BASSATT, 0.03f, 0.00f };
    s_routes[VMT_ROTATE] = { VMS_MID,     0.60f, 0.10f };
    return TRUE;
}

static void VisInitModTargets() {
    InitOnceExecuteOnce( &s_modTargetsInitOnce, VisInitModTargetsOnce, NULL, NULL );
}

// Round 3 (historical): at the time, even InitOnceExecuteOnce looked like it
// failed to make one thread's writes visible to another -- s_targetDefs.Num()
// read 5 on one thread and 0 on another at the identical address. The actual
// root cause (found later, see the VisFixedList comment above) was never a
// threading/visibility problem at all: the container's own constructor was
// running AFTER VisInitModTargetsOnce had already populated it (a static-
// initialization-order fiasco), silently resetting count back to 0 -- which
// looks exactly like a stale/torn cross-thread read if you don't know to
// suspect the constructor. That's fixed now (VisFixedList has no
// user-provided constructor, so there's no dynamic initializer left to run
// late). VisMod() below is kept regardless, on its own merits: every read
// goes through a bounds-checked accessor that cannot index out of range no
// matter what an unregistered/removed target's index looks like, so a bad
// index degrades to "this effect reads 0 for its modulation this frame"
// rather than crashing.
static ID_INLINE float VisMod( int target ) {
    if ( target < 0 || target >= s_mod.Num() ) {
        // Fixed real bug: this used to unconditionally return 0.0f here,
        // which was "safe" in the sense of not indexing out of range, but
        // 0.0f is a TERRIBLE fallback for a multiplicative factor -- every
        // caller that does `size * VisMod(VMT_SCALE)` or
        // `alpha * VisMod(VMT_BRIGHT)` (DrawEffectBars, DrawEffectRadial,
        // etc, all of them) collapses to a zero-sized, fully transparent
        // shape when this fires, which is INDISTINGUISHABLE from "nothing
        // is rendering" -- confirmed live: this is why the visualizer
        // showed nothing but a black/gray screen even though Draw2D was
        // provably running its full draw path every frame with valid
        // audio data. It's the same stuck-empty-view symptom as the
        // "unknown target" warnings from vis_route/preset loading (see the
        // round-3 comment above) hitting every VisMod call too, just with
        // a much less obvious symptom (invisible effects, not a printed
        // warning). Registration order is fixed (see VisInitModTargetsOnce:
        // scale, hue, bright, zoom, rotate, in that order, always), so
        // hardcode each built-in target's own neutral default here --
        // scale/bright default to 1.0 (a no-op multiplier), hue/zoom/rotate
        // default to 0.0 (a no-op offset) -- so a stuck-empty view on this
        // thread degrades to "effects render at their unmodulated resting
        // size/brightness" instead of "effects render as literally
        // nothing." Anything beyond the 5 built-ins (a layer's own
        // registered target) still has no safe default to fall back to,
        // so those keep the original 0.0f.
        switch ( target ) {
            case 0: return 1.0f;   // VMT_SCALE
            case 2: return 1.0f;   // VMT_BRIGHT
            default: return 0.0f;  // VMT_HUE / VMT_ZOOM / VMT_ROTATE / unknown
        }
    }
    return s_mod[target];
}

// Like VisMod, but for targets whose neutral resting value is a caller-supplied
// number rather than one of VisMod's hardcoded scale/bright/... defaults. Used
// for the camera and per-slot multipliers (fallback 1.0), so a stuck-empty
// s_mod view (see VisMod's long comment) degrades to "no modulation this frame"
// instead of collapsing the multiplied quantity to zero.
static ID_INLINE float VisModOr( int target, float fallback ) {
    return ( target >= 0 && target < s_mod.Num() ) ? s_mod[target] : fallback;
}

// Resolve a per-stack-slot target's current value (PRD FR-C1-EXPAND). slot < 0
// is the legacy single-effect path (no per-slot routing) and every param
// returns its neutral 1.0 multiplier, so that path is byte-for-byte unchanged.
static ID_INLINE float VisSlotMod( int slot, int paramKind ) {
    if ( slot < 0 || slot >= VIS_MOD_STACK_SLOTS || paramKind < 0 || paramKind >= VSP_COUNT ) {
        return 1.0f;
    }
    return VisModOr( s_slotTarget[slot][paramKind], 1.0f );
}

// Same idea as VisSlotMod, but for the posx/posy targets -- an ADDITIVE
// offset whose neutral value is 0.0 (no position change), not a multiplier's
// 1.0. slot < 0 (the legacy single-effect path never calls this -- it uses
// VMT_EFFECTX_MOD/VMT_EFFECTY_MOD directly) falls back to 0.0 too.
static ID_INLINE float VisSlotPosOffset( int slot, int paramKind ) {
    if ( slot < 0 || slot >= VIS_MOD_STACK_SLOTS || paramKind < 0 || paramKind >= VSP_COUNT ) {
        return 0.0f;
    }
    return VisModOr( s_slotTarget[slot][paramKind], 0.0f );
}

// PRD Pillar C follow-up (direct user report: "all images, effects, layers
// have scale but also x/y coordinates so things can be moved around in an
// almost xyz style"). Set once per effect draw (legacy single-effect path in
// Draw2D, or VisRenderStackLayerEffect for a stack slot) immediately before
// dispatching to the DrawEffect* function, which reads these instead of
// hardcoding VIS_W*0.5f/VIS_H*0.5f -- so every effect becomes positionable
// with one shared mechanism instead of a bespoke offset per effect. Already
// in real pixel units (VIS_W/VIS_H space), not the raw -1..1 cvar range.
static float s_curOffX = 0.0f;
static float s_curOffY = 0.0f;

static float s_rotAngle = 0.0f;   // integrated radial rotation (radians)
static float s_rotDelta = 0.0f;   // this-frame rotation increment (for feedback)
static float s_lfoPhase = 0.0f;   // 0..1 sawtooth
static float s_warpPhase = 0.0f;  // integrated feedback-warp ripple phase
static int   s_modLastMs = 0;

// formats a route's source for display -- VMS_MIDICC_PARAM/VMS_MIDINOTE_VEL
// store their channel/cc-or-note in the route itself (their s_sourceNames
// entries are just placeholders, never meant to be printed).
static idStr VisFormatRouteSource( const visRoute_t & route ) {
    if ( route.source == VMS_MIDICC_PARAM ) {
        return va( "midicc%d_%d", route.midiChannel, route.midiCC );
    }
    if ( route.source == VMS_MIDINOTE_VEL ) {
        return va( "midinote%d_%d", route.midiChannel, route.midiNote );
    }
    if ( route.source == VMS_BAND ) {
        return va( "band%d", route.bandIndex );
    }
    return s_sourceNames[ route.source ];
}

static float VisSampleSource( const visRoute_t & route ) {
    switch ( route.source ) {
        case VMS_BASS:    return g_audioAnalyzer.GetBass();
        case VMS_MID:     return g_audioAnalyzer.GetMid();
        case VMS_TREB:    return g_audioAnalyzer.GetTreb();
        case VMS_BASSATT: return g_audioAnalyzer.GetBassAtt();
        case VMS_MIDATT:  return g_audioAnalyzer.GetMidAtt();
        case VMS_TREBATT: return g_audioAnalyzer.GetTrebAtt();
        case VMS_RMS:     return g_audioAnalyzer.GetRMS();
        case VMS_BEAT:    return g_audioAnalyzer.GetBeat() ? 1.0f : 0.0f;
        case VMS_LEVEL:   return ( g_audioAnalyzer.GetBass() + g_audioAnalyzer.GetMid() + g_audioAnalyzer.GetTreb() ) * ( 1.0f / 3.0f );
        case VMS_LFO:     return s_lfoPhase;
        case VMS_MOUSEX:  return s_mouseNormX;
        case VMS_MOUSEY:  return s_mouseNormY;
        case VMS_MIDICC1: return g_midiInput.GetCC( 0, 1 );
        case VMS_MIDICC2: return g_midiInput.GetCC( 0, 2 );
        case VMS_MIDICC3: return g_midiInput.GetCC( 0, 3 );
        case VMS_MIDICC4: return g_midiInput.GetCC( 0, 4 );
        // PRD FR-E5: general "any CC on any channel" source -- the follow-up
        // the M4 write-up flagged as not attempted; landed now via
        // VisParseMidiCCSource (vis_route) storing the parsed channel/cc
        // directly in the route, since VMS_MIDICC_PARAM alone can't carry them.
        case VMS_MIDICC_PARAM: return g_midiInput.GetCC( route.midiChannel, route.midiCC );
        // PRD FR-E5: note velocity as an envelope -- 0 while the key is up,
        // gated on GetNoteOn since GetNoteVelocity alone doesn't reset to 0
        // on note-off (see idMidiInput's own threading/semantics comment).
        case VMS_MIDINOTE_VEL: return g_midiInput.GetNoteOn( route.midiChannel, route.midiNote )
            ? g_midiInput.GetNoteVelocity( route.midiChannel, route.midiNote ) : 0.0f;
        // PRD Pillar C follow-up: route one of the user's actual configured
        // vis_bands spectrum bands (not the fixed bass/mid/treb split) --
        // GetBandLevel is bounds-checked (returns 0 for a stale/out-of-range
        // index, e.g. if vis_bands was lowered after this route was set).
        case VMS_BAND:    return g_audioAnalyzer.GetBandLevel( route.bandIndex );
        default:          return 0.0f;
    }
}

// PRD FR-C8: response-shaping stage. `x` is the sampled (nominally 0..1)
// source value; returns the shaped value with `invert` (1-x) composed last.
// Threshold reads/updates route.shapeState for hysteresis, so the route is
// taken by non-const reference. Params are clamped to safe ranges here so a
// stale/degenerate shapeParam (e.g. 0 exponent, <2 quantize levels) can never
// produce a NaN/Inf or divide-by-zero -- the shaped output stays finite.
static float VisApplyShape( visRoute_t & route, float x ) {
    float y;
    switch ( route.shape ) {
        case VSHAPE_EXP: {
            // x^k: k>1 compresses the low end (punchier), k<1 expands it.
            const float k  = idMath::ClampFloat( 0.01f, 16.0f, route.shapeParam );
            const float xc = ( x < 0.0f ) ? 0.0f : x;   // Pow undefined for negative base
            y = idMath::Pow( xc, k );
            break;
        }
        case VSHAPE_LOG: {
            // log(1+k*x)/log(1+k): perceptual/loudness-style compression.
            const float k  = idMath::ClampFloat( 0.01f, 100.0f, route.shapeParam );
            const float xc = idMath::ClampFloat( 0.0f, 1.0f, x );
            y = idMath::Log( 1.0f + k * xc ) / idMath::Log( 1.0f + k );
            break;
        }
        case VSHAPE_SCURVE: {
            // smoothstep soft knee on [0,1] -- avoids harsh on/off snapping.
            const float xc = idMath::ClampFloat( 0.0f, 1.0f, x );
            y = xc * xc * ( 3.0f - 2.0f * xc );
            break;
        }
        case VSHAPE_THRESHOLD: {
            // "1 to 100% becomes a boolean": output is 0 or 1. A hysteresis
            // half-band around the threshold prevents chatter near the edge --
            // rise to 1 above thr+band, fall to 0 below thr-band, otherwise
            // hold the previous output.
            const float thr  = route.shapeParam;
            const float band = idMath::ClampFloat( 0.0f, 1.0f, route.shapeParam2 );
            if ( x >= thr + band ) {
                route.shapeState = 1.0f;
            } else if ( x <= thr - band ) {
                route.shapeState = 0.0f;
            }
            y = route.shapeState;
            break;
        }
        case VSHAPE_QUANTIZE: {
            // Snap to N evenly spaced levels across [0,1] for stepped/strobing looks.
            const int   levels = (int)idMath::ClampFloat( 2.0f, 256.0f, route.shapeParam );
            const float xc     = idMath::ClampFloat( 0.0f, 1.0f, x );
            const float step   = 1.0f / (float)( levels - 1 );
            y = idMath::Floor( xc / step + 0.5f ) * step;
            break;
        }
        case VSHAPE_LINEAR:
        default:
            y = x;
            break;
    }
    if ( route.invert ) {
        y = 1.0f - y;
    }
    return y;
}

// Evaluate every route once per frame (main thread). dtSec is the frame delta.
static void VisUpdateModulation( float dtSec ) {
    VisInitModTargets();

    // s_targetDefs/s_routes/s_mod are always resized together in
    // VisInitModTargetsOnce, so they're always the same size -- Min() here is
    // just cheap insurance, not a workaround for anything.
    const int modCount = Min( Min( s_targetDefs.Num(), s_mod.Num() ), s_routes.Num() );

    if ( !vis_mod.GetBool() ) {
        for ( int t = 0; t < modCount; t++ ) {
            s_mod[t] = s_targetDefs[t].defaultBase;
        }
        s_rotAngle = 0.0f;
        s_rotDelta = 0.0f;
        return;
    }

    // advance the LFO (sawtooth 0..1)
    const float period = idMath::ClampFloat( 0.05f, 600.0f, vis_lfoPeriod.GetFloat() );
    s_lfoPhase += dtSec / period;
    s_lfoPhase -= idMath::Floor( s_lfoPhase );

    for ( int t = 0; t < modCount; t++ ) {
        // Sample -> (range-map, n/a today) -> FR-C8 shape -> gain/offset.
        // The shape stage is a pure function of the sampled source value and
        // runs BEFORE the final `base + x*amount` affine step, so amount/base
        // still act as gain/offset on the shaped signal.
        float srcVal = VisSampleSource( s_routes[t] );
        srcVal = VisApplyShape( s_routes[t], srcVal );
        s_mod[t] = s_routes[t].base + srcVal * s_routes[t].amount;
        const visModTargetDef_t & def = s_targetDefs[t];
        if ( def.wrap01 ) {
            s_mod[t] = s_mod[t] - idMath::Floor( s_mod[t] );   // wrap to 0..1 (hue)
        } else if ( def.clampMin < def.clampMax ) {
            s_mod[t] = idMath::ClampFloat( def.clampMin, def.clampMax, s_mod[t] );
        }
    }

    // rotate target is an angular velocity (rad/sec): integrate for the radial
    // absolute angle and keep the per-frame delta for the feedback spin.
    s_rotDelta = VisMod(VMT_ROTATE) * dtSec;
    s_rotAngle += s_rotDelta;
    if ( s_rotAngle > idMath::TWO_PI )  s_rotAngle -= idMath::TWO_PI;
    if ( s_rotAngle < -idMath::TWO_PI ) s_rotAngle += idMath::TWO_PI;
}

static void VisFillRect( const idVec4 & color, float x, float y, float w, float h ) {
    if ( s_visSolid == NULL ) {
        return; // not resolved yet (first frame) - skip rather than touch decls off-thread
    }
    renderSystem->SetColor( color );
    renderSystem->DrawStretchPic( x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f, s_visSolid );
}

// arbitrary-corner filled quad (for radial bars, oscilloscope segments, warps)
static void VisFillQuad( const idVec4 & color,
                         float x0, float y0, float x1, float y1,
                         float x2, float y2, float x3, float y3 ) {
    if ( s_visSolid == NULL ) {
        return;
    }
    renderSystem->SetColor( color );
    renderSystem->DrawStretchPic(
        idVec4( x0, y0, 0.0f, 0.0f ), idVec4( x1, y1, 1.0f, 0.0f ),
        idVec4( x2, y2, 1.0f, 1.0f ), idVec4( x3, y3, 0.0f, 1.0f ), s_visSolid );
}

// a thick line segment as a quad (oscilloscope / vector effects)
static void VisLine( const idVec4 & color, float ax, float ay, float bx, float by, float thick ) {
    float dx = bx - ax;
    float dy = by - ay;
    const float len = idMath::Sqrt( dx * dx + dy * dy );
    if ( len < 0.0001f ) {
        return;
    }
    // perpendicular, scaled to half-thickness
    const float px = -dy / len * thick * 0.5f;
    const float py =  dx / len * thick * 0.5f;
    VisFillQuad( color, ax + px, ay + py, bx + px, by + py, bx - px, by - py, ax - px, ay - py );
}

// HSV->RGB with s=v=1; h in [0,1). Cheap rainbow across the spectrum.
// Global palette: remap the incoming hue into a per-palette sub-range and add
// the global hue offset, so a single cvar recolors every effect + image layer.
static idVec4 HueColor( float h, float alpha ) {
    float hue;
    switch ( vis_palette.GetInteger() ) {
        case 1:  hue = 0.0f + h * 0.15f; break;   // fire: red -> orange -> yellow
        case 2:  hue = 0.45f + h * 0.20f; break;  // ocean: green/cyan -> blue
        case 3:  hue = 0.75f + h * 0.25f; break;  // synthwave: magenta -> pink -> red wrap
        case 4:  hue = 0.55f; break;              // mono: single hue
        default: hue = h; break;                  // 0 rainbow: full spectrum (unchanged)
    }
    hue = hue + vis_hueShiftGlobal.GetFloat() + VisMod( VMT_HUESHIFT_MOD );
    hue = hue - idMath::Floor( hue );             // wrap to 0..1
    h = ( hue - idMath::Floor( hue ) ) * 6.0f;
    const int i = (int)h;
    const float f = h - i;
    const float q = 1.0f - f;
    switch ( i ) {
        case 0:  return idVec4( 1.0f, f,    0.0f, alpha );
        case 1:  return idVec4( q,    1.0f, 0.0f, alpha );
        case 2:  return idVec4( 0.0f, 1.0f, f,    alpha );
        case 3:  return idVec4( 0.0f, q,    1.0f, alpha );
        case 4:  return idVec4( f,    0.0f, 1.0f, alpha );
        default: return idVec4( 1.0f, 0.0f, q,    alpha );
    }
}

static const float VIS_W = 640.0f;
static const float VIS_H = 480.0f;

// Draw the captured previous frame back to the screen, zoomed and rotated about
// the center and dimmed by `dim`. rot is the PER-FRAME rotation increment so the
// spin accumulates through the feedback loop. Produces the MilkDrop warp/trail.
static void DrawFeedbackFrame( float zoom, float rot, float dim ) {
    if ( s_visFeedbackActive == NULL ) {
        return;
    }
    const float cx = VIS_W * 0.5f;
    const float cy = VIS_H * 0.5f;
    const float hw = cx * zoom;
    const float hh = cy * zoom;
    const float c = idMath::Cos( rot );
    const float s = idMath::Sin( rot );
    // local corner offsets: TL, TR, BR, BL
    const float lx[4] = { -hw,  hw,  hw, -hw };
    const float ly[4] = { -hh, -hh,  hh,  hh };
    idVec4 corner[4];
    const float sc[4] = { 0.0f, 1.0f, 1.0f, 0.0f };  // texcoord s per corner
    const float tc[4] = { 0.0f, 0.0f, 1.0f, 1.0f };  // texcoord t per corner
    for ( int i = 0; i < 4; i++ ) {
        corner[i].x = cx + lx[i] * c - ly[i] * s;
        corner[i].y = cy + lx[i] * s + ly[i] * c;
        corner[i].z = sc[i];
        corner[i].w = tc[i];
    }
    renderSystem->SetColor( idVec4( dim, dim, dim, 1.0f ) );
    renderSystem->DrawStretchPic( corner[0], corner[1], corner[2], corner[3], s_visFeedbackActive );
}

// MilkDrop-style warp mesh: a regular screen grid whose per-vertex TEXCOORDS are
// displaced by a warp function (inverse zoom + rotation + a radial ripple that
// flows with s_warpPhase). Sampling the previous frame through these coords and
// re-capturing accumulates flowing warp/liquid feedback. The GPU bilinearly
// interpolates the sampled image across each cell - a real per-pixel-ish warp
// without a custom fragment shader (which the dataless renderprog path can't
// take safely).
static const int VIS_WARP_MAX_GX = 48;   // max grid columns (hi-res)
static const int VIS_WARP_MAX_GY = 36;
static void DrawFeedbackWarpMesh( int gcols, int grows, int mode,
                                  float zoom, float rot, float dim,
                                  float amount, float freq, float phase ) {
    if ( s_visFeedbackActive == NULL ) {
        return;
    }
    if ( gcols > VIS_WARP_MAX_GX ) gcols = VIS_WARP_MAX_GX;
    if ( grows > VIS_WARP_MAX_GY ) grows = VIS_WARP_MAX_GY;
    const int NX = gcols + 1;
    const int NY = grows + 1;
    static idVec2 pos[ ( VIS_WARP_MAX_GX + 1 ) * ( VIS_WARP_MAX_GY + 1 ) ];   // screen xy
    static idVec2 tex[ ( VIS_WARP_MAX_GX + 1 ) * ( VIS_WARP_MAX_GY + 1 ) ];   // warped st
    const float invZoom = ( zoom != 0.0f ) ? ( 1.0f / zoom ) : 1.0f;
    const float c = idMath::Cos( rot );
    const float s = idMath::Sin( rot );
    for ( int j = 0; j < NY; j++ ) {
        for ( int i = 0; i < NX; i++ ) {
            const int idx = j * NX + i;
            const float gx = (float)i / (float)gcols;   // 0..1
            const float gy = (float)j / (float)grows;
            pos[idx].Set( gx * VIS_W, gy * VIS_H );

            // warp is computed in centered coords (screen center = image center)
            const float u = gx - 0.5f;
            const float v = gy - 0.5f;
            const float r = idMath::Sqrt( u * u + v * v );
            float wu = u;
            float wv = v;
            switch ( mode ) {
                case 1: {   // swirl - rotate by an angle that varies with radius
                    const float sw = amount * 6.0f * ( 0.6f - idMath::ClampFloat( 0.0f, 0.6f, r ) )
                                   + amount * idMath::Sin( r * freq - phase );
                    const float cc = idMath::Cos( sw ), ss = idMath::Sin( sw );
                    wu = ( u * cc - v * ss ) * invZoom;
                    wv = ( u * ss + v * cc ) * invZoom;
                    break;
                }
                case 2: {   // tunnel - nonlinear radius remap (gamma), spins with rot
                    const float rr = idMath::Pow( r, 1.0f + amount * 3.0f * ( 1.0f + idMath::Sin( r * freq - phase ) * 0.5f ) );
                    const float scale = ( r > 1e-4f ) ? ( invZoom * rr / r ) : invZoom;
                    wu = u * scale;
                    wv = v * scale;
                    break;
                }
                case 3: {   // fisheye - barrel distortion
                    const float scale = invZoom * ( 1.0f + amount * 3.0f * ( r * r - 0.12f ) );
                    wu = u * scale;
                    wv = v * scale;
                    break;
                }
                default: {  // 0 ripple - radial displacement
                    const float ripple = 1.0f + amount * idMath::Sin( r * freq - phase );
                    wu = u * invZoom * ripple;
                    wv = v * invZoom * ripple;
                    break;
                }
            }
            float tu = wu * c - wv * s + 0.5f;
            float tv = wu * s + wv * c + 0.5f;

            // kaleidoscope: fold the sample coord into N mirrored angular wedges so
            // the feedback becomes radially symmetric (mandala look). Bindable via
            // VMT_KALEIDO_MOD -- rounded to the nearest int since the fold count is
            // discrete, then clamped back into the slider's own 0..16 range.
            const int kaleido = idMath::ClampInt( 0, 16, vis_kaleido.GetInteger() + (int)idMath::Rint( VisModOr( VMT_KALEIDO_MOD, 0.0f ) ) );
            const float ku = tu - 0.5f;
            const float kv = tv - 0.5f;
            const float kr = idMath::Sqrt( ku * ku + kv * kv );
            // guard ATan(0,0) (asserts) at the exact center vertex
            if ( kaleido >= 2 && kr > 1e-5f ) {
                const float seg = idMath::TWO_PI / (float)kaleido;
                float ka = idMath::ATan( kv, ku );
                ka -= seg * idMath::Floor( ka / seg );   // into [0, seg)
                if ( ka > seg * 0.5f ) {
                    ka = seg - ka;                        // mirror the wedge
                }
                tu = 0.5f + kr * idMath::Cos( ka );
                tv = 0.5f + kr * idMath::Sin( ka );
            }
            tex[idx].Set( tu, tv );
        }
    }

    renderSystem->SetColor( idVec4( dim, dim, dim, 1.0f ) );
    for ( int j = 0; j < grows; j++ ) {
        for ( int i = 0; i < gcols; i++ ) {
            const int tl = j * NX + i;
            const int tr = tl + 1;
            const int bl = tl + NX;
            const int br = bl + 1;
            renderSystem->DrawStretchPic(
                idVec4( pos[tl].x, pos[tl].y, tex[tl].x, tex[tl].y ),
                idVec4( pos[tr].x, pos[tr].y, tex[tr].x, tex[tr].y ),
                idVec4( pos[br].x, pos[br].y, tex[br].x, tex[br].y ),
                idVec4( pos[bl].x, pos[bl].y, tex[bl].x, tex[bl].y ),
                s_visFeedbackActive );
        }
    }
}

/*
========================
PRD M1 step 4: MilkDrop preset-driven warp mesh

The active preset (if any) that's driving the warp mesh in place of the
built-in ripple/swirl/tunnel/fisheye modes. Loaded/compiled on the MAIN
thread (VisLoadMilkPreset, from a console command); per_pixel_ is ALSO
evaluated on the main thread, in VisUpdateMilkFrame (called from Frame(),
right next to VisUpdateModulation) -- NOT inside Draw2D/DrawMilkWarpMesh,
which run on the draw-worker thread. This matters for more than the
project's usual "resolve on main thread, draw-worker only reads" rule: the
HostLocks.c lock callbacks are no-ops, which is only safe if projectm-eval
Execute() is never called concurrently from two threads at once (it shares
process-wide gmegabuf/reg00-99 state across every idMilkEvaluator instance).
Keeping ALL Execute() calls on the main thread and only handing the
draw-worker thread pre-computed floats (s_milkTex[]) sidesteps that risk
entirely, exactly like s_mod[] already does for the routing matrix.

Known limitation, documented rather than silently approximated: this
implements the zoom/rot/dx/dy/cx/cy/sx/sy affine transform per_pixel_ code
controls (well-documented, unambiguous), but NOT classic MilkDrop's
sinusoidal "warp" shader-noise ripple term -- that lives in the warp PIXEL
SHADER in real MilkDrop, which is M3 (HLSL shader presets) scope, not this
step's per_pixel_ EQUATION integration.
========================
*/
static const int MILK_DEFAULT_GX = 32;   // MilkDrop's own default mesh resolution
static const int MILK_DEFAULT_GY = 24;

static idMilkPreset       s_milkPreset;
static idMilkEvaluator     s_milkEval;
static struct projectm_eval_code * s_milkInitCode = NULL;
static struct projectm_eval_code * s_milkFrameCode = NULL;
static struct projectm_eval_code * s_milkPixelCode = NULL;
static bool                s_milkActive = false;
static idVec2               s_milkTex[ ( MILK_DEFAULT_GX + 1 ) * ( MILK_DEFAULT_GY + 1 ) ];   // cached warped UVs

// PRD M3: true once the active preset's warp_ shader has been transpiled
// and successfully enqueued for backend compilation -- DrawMilkWarpMesh
// uses this to pick the per-pixel custom-shader path (a full-screen quad,
// RB_VisMilkWarpDraw) instead of the CPU-mesh path above when set. This
// only reflects "the transpile+enqueue succeeded", not "the backend
// compile/link also succeeded" -- RB_VisMilkWarpDraw itself silently no-ops
// if the backend compile failed (see its comment in tr_backend_draw.cpp),
// so a bad preset shader degrades to a black quad rather than falling back
// to the CPU mesh; this is an accepted simplification (NFR-3: tolerate and
// move on) since the two paths can't both be attempted for the same frame
// without either double-drawing or plumbing a compile-result callback back
// to the front end across the render command boundary.
static bool                s_milkWarpShaderReady = false;

// PRD M3: one-time hand-off from preset load (main thread, console-command
// context) to DrawMilkWarpMesh (draw-worker thread, per-frame render-command
// context) -- see the fix comment above VisLoadMilkPreset's transpile block
// for why this indirection exists (the direct-call version silently never
// reached the backend). Benign single-writer-then-single-reader: written
// once at preset load, consumed (and cleared) on DrawMilkWarpMesh's very
// next call.
static bool                s_milkWarpPendingCompile = false;
static idStr                s_milkWarpPendingVertexGLSL;
static idStr                s_milkWarpPendingFragGLSL;
static idStr                s_milkWarpPendingCompFragGLSL;   // PRD M3: comp_ pass, empty if preset has no comp_ shader
// PRD M3/M5: the vertex passthrough is a FIXED shader, identical for every
// preset (and for both A and B mash-up sides) -- transpiled once and cached
// here rather than re-running the hlsl2glslfork worker thread per preset
// load (or once per side).
static idStr                s_milkVertexGLSL;
static bool                 s_milkVertexGLSLReady = false;

// PRD M3: per-frame uniform values for the compiled custom warp shader,
// computed once on the main thread (VisUpdateMilkFrame, same thread that
// already reads g_audioAnalyzer/s_milkEval for the CPU-mesh path) and read
// by DrawMilkWarpMesh (draw-worker thread) -- same benign single-writer/
// single-reader-per-frame pattern s_milkTex[] above already relies on.
static visMilkWarpUniforms_t s_milkWarpUniforms;

// PRD M3 fidelity fix: real seeded RNG for rand_frame/rand_preset instead of
// the old naive sin()-based placeholders (idMath::Fabs(idMath::Sin(...)) --
// deterministic from just a frame counter or a preset name's string length,
// not actually random). Same plain LCG style already used elsewhere in this
// file (VisPickRandomMilkPreset, BuildCycleList's shuffle), not a different
// RNG type. rand_frame advances every frame (persistent LCG state);
// rand_preset is (re)seeded once per preset load from wall-clock time xor'd
// with the preset path's hash, then held constant for that preset's whole
// lifetime -- matching real MilkDrop's own rand_preset semantics.
static unsigned int s_milkRandFrameLCG = 0;
static float        s_milkRandPresetValue = 0.0f;
static float        s_milkRandPresetValueB = 0.0f;

static float VisNextMilkRandFrame() {
    s_milkRandFrameLCG = s_milkRandFrameLCG * 1664525u + 1013904223u;   // LCG
    return ( s_milkRandFrameLCG >> 8 ) * ( 1.0f / 16777216.0f );        // -> [0,1)
}

static float VisSeedMilkRandPreset( const char * path ) {
    unsigned int seed = (unsigned int)( Sys_Milliseconds() ^ ( idStr::Hash( path ) * 2654435761u ) );
    seed = seed * 1664525u + 1013904223u;   // LCG
    return ( seed >> 8 ) * ( 1.0f / 16777216.0f );   // -> [0,1)
}

// PRD M5: a second preset ("B") for MilkDrop3-style mash-ups (blend two full
// presets). Mirrors the primary ("A") state above exactly -- a genuine
// N-preset refactor of the existing single-instance functions would be a
// riskier rewrite of already-shipped, working code, so this v1 duplicates
// just the warp-mesh path (the visually dominant part of a mash-up) rather
// than every A-side feature (waveform/video-echo stay A-only for now,
// documented below). s_milkMashMix 0 = pure A (mash-up inactive), 1 = pure
// B; VisUpdateMilkFrame/DrawMilkWarpMesh both check s_milkActiveB.
static idMilkPreset       s_milkPresetB;
static idMilkEvaluator     s_milkEvalB;
static struct projectm_eval_code * s_milkInitCodeB = NULL;
static struct projectm_eval_code * s_milkFrameCodeB = NULL;
static struct projectm_eval_code * s_milkPixelCodeB = NULL;
static bool                s_milkActiveB = false;
static idVec2               s_milkTexB[ ( MILK_DEFAULT_GX + 1 ) * ( MILK_DEFAULT_GY + 1 ) ];
static float                s_milkMashMix = 0.0f;

// PRD FR-A5: preset auto-advance soft-cut state -- reuses the mash-up B
// side above as the crossfade primitive (load the next preset as B, ramp
// s_milkMashMix 0->1 over vis_milkSoftCutSecs, promote B to A on
// completion). s_milkPresetBPath is needed because idMilkPreset doesn't
// retain its own source path (idMilkPreset::GetName() is just the
// stripped basename, used for display/ratings) -- "promoting" B to A
// means a fresh VisLoadMilkPreset(path) call using this stashed path, not
// an in-memory struct copy (that would leave s_milkEval's compiled
// bytecode, shader-transpile state, and the A-only wave/echo caches all
// stale).
static bool                 s_milkCutInProgress = false;
static idStr                 s_milkPresetBPath;

// PRD M5: B side's own shader-driven warp_/comp_ path -- mirrors the A-side
// pending-compile mechanism (s_milkWarpPendingCompile etc.) above exactly,
// just routed to the backend's separate B-slot programs via
// EnqueueVisMilkShaderCompile's isMashB flag. Lets two INDEPENDENT custom
// shaders (not just two CPU-mesh presets) mash up together.
static bool                 s_milkWarpBShaderReady = false;
static bool                 s_milkWarpBPendingCompile = false;
static idStr                s_milkWarpBPendingVertexGLSL;
static idStr                s_milkWarpBPendingFragGLSL;
static idStr                s_milkWarpBPendingCompFragGLSL;
static visMilkWarpUniforms_t s_milkWarpUniformsB;

// Waveform/video-echo params: fixed per preset (not per-frame), so cached
// once in VisLoadMilkPreset (main thread) rather than re-read from
// s_milkPreset.GetParams() every draw call. This matters for more than
// efficiency: DrawMilkWaveform/DrawMilkVideoEcho run on the draw-worker
// thread, and idDict (s_milkPreset's params) is NOT thread-safe -- a
// vis_milkPreset reload on the main thread calls idDict::Clear() during
// Load(), which would race a concurrent GetFloat() from the worker thread.
// Caching plain floats at load time sidesteps that entirely, matching the
// same "resolve on main thread, draw-worker only reads" rule s_milkTex[]
// and s_mod[] already follow.
struct milkWaveParams_t { float r, g, b, a; float scale, alphaMul; float cx, cy; };
struct milkEchoParams_t { float alpha; float zoom; int orient; };
static milkWaveParams_t s_milkWaveParams;
static milkEchoParams_t s_milkEchoParams;
static float s_milkElapsedSec = 0.0f;   // seconds since the active preset loaded, for the "progress" variable

/*
========================
PRD M5: custom wavecode (up to 16 preset-defined custom waveforms)

Real MilkDrop's wavecode_N_per_point runs once PER AUDIO SAMPLE POINT, using
locals (sample/value1/value2/x/y/r/g/b/a) that must NOT be shared with the
main per_pixel_ warp-mesh context, but the q1..q64 BRIDGE variables and
gmegabuf/reg00-99 SHOULD still cross into/out of it (confirmed against
docs/research-milkdrop-projectm.md and, this round, against the vendored
projectm-eval source directly: TreeVariables.c's register_variable always
mallocs fresh per-context storage for ordinary names like q1 -- two contexts
can NEVER literally share that memory -- but gmegabuf/reg00-99 resolve
through a global/global_variables pointer supplied at context_create time,
which idMilkEvaluator::Init already points at the SAME process-wide statics
for every instance, so those two already cross automatically for free).

Design: one idMilkEvaluator PER WAVE (so its own per_frame_/per_point_ code
shares x/y/etc locals with EACH OTHER within that wave, matching real
MilkDrop, but not with the main context or other waves), with q1..q64
manually copied in from the main context before the per-point loop runs each
frame and copied back out after -- the only mechanism this vendored library
actually supports for bridging separate contexts. sample/value1/value2/x/y/
r/g/b/a need no MilkEvaluator.h/.cpp changes at all: GetVariable() already
does the same find-or-create lookup the compiler itself uses, so it works
for any name a wave's code happens to reference.
========================
*/
static const int MILK_MAX_WAVES = 16;           // MilkDrop3's documented cap
static const int MILK_WAVE_MAX_SAMPLES = 256;   // safety cap on per-point Executes/frame -- real
                                                  // MilkDrop's native waveform can afford ~512+ points
                                                  // because it's not re-running a compiled EEL2
                                                  // expression per point; ours is, so this is deliberately
                                                  // more conservative than real MilkDrop's own default.
struct milkWaveRuntime_t {
    idMilkEvaluator                eval;
    struct projectm_eval_code *    frameCode;
    struct projectm_eval_code *    pointCode;
    PRJM_EVAL_F *   qSample;
    PRJM_EVAL_F *   qValue1;
    PRJM_EVAL_F *   qValue2;
    PRJM_EVAL_F *   qX;
    PRJM_EVAL_F *   qY;
    PRJM_EVAL_F *   qR;
    PRJM_EVAL_F *   qG;
    PRJM_EVAL_F *   qB;
    PRJM_EVAL_F *   qA;
    int             numSamples;
    bool            additive;
    bool            useDots;
};
struct milkWavePoint_t { float x, y, r, g, b, a; };
static milkWaveRuntime_t s_milkWaveRt[MILK_MAX_WAVES];
static int               s_milkNumWaveRt = 0;   // how many of the above are actually active this preset
// draw-worker-safe cache: positions/colors computed on the main thread in
// VisUpdateMilkFrame, drawn on the draw-worker thread in DrawMilkWavecode --
// same split every other Milk* draw function in this file already follows.
static milkWavePoint_t   s_milkWavePoints[MILK_MAX_WAVES][MILK_WAVE_MAX_SAMPLES];
static int               s_milkWavePointCount[MILK_MAX_WAVES];

static void VisUnloadMilkWaves() {
    for ( int i = 0; i < MILK_MAX_WAVES; i++ ) {
        milkWaveRuntime_t & wr = s_milkWaveRt[i];
        if ( wr.frameCode != NULL ) { wr.eval.FreeCode( wr.frameCode ); wr.frameCode = NULL; }
        if ( wr.pointCode != NULL ) { wr.eval.FreeCode( wr.pointCode ); wr.pointCode = NULL; }
        wr.eval.Shutdown();
        s_milkWavePointCount[i] = 0;
    }
    s_milkNumWaveRt = 0;
}

// Compiles every wave the preset defines (up to MILK_MAX_WAVES) and caches
// its GetVariable() pointers + static params. Called from VisLoadMilkPreset
// (main thread), same as the rest of preset load.
static void VisLoadMilkWaves( const idMilkPreset & preset ) {
    VisUnloadMilkWaves();
    s_milkNumWaveRt = idMath::ClampInt( 0, MILK_MAX_WAVES, preset.NumWaves() );
    for ( int i = 0; i < s_milkNumWaveRt; i++ ) {
        milkWaveRuntime_t & wr = s_milkWaveRt[i];
        const milkWaveCode_t & w = preset.GetWave( i );
        if ( !wr.eval.Init() ) {
            idLib::Warning( "visualizer: wave %d evaluator init failed", w.index );
            continue;
        }
        wr.frameCode = wr.eval.Compile( w.perFrame.c_str() );
        wr.pointCode = wr.eval.Compile( w.perPoint.c_str() );
        // find-or-create -- guaranteed non-NULL, whether or not this wave's
        // own code happens to reference each name.
        wr.qSample = wr.eval.GetVariable( "sample" );
        wr.qValue1 = wr.eval.GetVariable( "value1" );
        wr.qValue2 = wr.eval.GetVariable( "value2" );
        wr.qX      = wr.eval.GetVariable( "x" );
        wr.qY      = wr.eval.GetVariable( "y" );
        wr.qR      = wr.eval.GetVariable( "r" );
        wr.qG      = wr.eval.GetVariable( "g" );
        wr.qB      = wr.eval.GetVariable( "b" );
        wr.qA      = wr.eval.GetVariable( "a" );
        wr.numSamples = idMath::ClampInt( 8, MILK_WAVE_MAX_SAMPLES, w.params.GetInt( "samples", 256 ) );
        wr.additive   = w.params.GetBool( "badditive", false );
        wr.useDots    = w.params.GetBool( "busedots", false );
        s_milkWavePointCount[i] = 0;
    }
}

/*
========================
PRD M5: custom shapecode (up to 16 preset-defined shapes)

A strict subset of the wavecode design just above: real MilkDrop's
shapecode_N_per_frame runs once per frame (no per-point loop -- shapes are
drawn as a single N-sided polygon, not sampled per audio point), so the same
"one idMilkEvaluator per instance, q1..q64 manually bridged in/out each
frame" design applies directly with no per-point complexity at all.
x/y/rad/ang/sides/r/g/b/a/r2/g2/b2/a2/enabled all resolve via GetVariable()
the same way wavecode's did -- defaults come from the shape's own static
params (shapecode_N_x=, etc), reset every frame before Execute() exactly
like VisComputeMilkWarpMesh's zoom/rot/dx/dy pattern, then overridable by
the preset's own per_frame_ code.
========================
*/
static const int MILK_MAX_SHAPES = 16;          // MilkDrop3's documented cap
static const int MILK_SHAPE_MAX_SIDES = 100;    // spec allows up to 500-sided "circles"; capped here for
                                                  // practical triangle-fan draw cost, documented deliberately
                                                  // lower rather than silently truncated without a note.
struct milkShapeRuntime_t {
    idMilkEvaluator                eval;
    struct projectm_eval_code *    frameCode;
    PRJM_EVAL_F *   qX;
    PRJM_EVAL_F *   qY;
    PRJM_EVAL_F *   qRad;
    PRJM_EVAL_F *   qAng;
    PRJM_EVAL_F *   qSides;
    PRJM_EVAL_F *   qEnabled;
    PRJM_EVAL_F *   qR;
    PRJM_EVAL_F *   qG;
    PRJM_EVAL_F *   qB;
    PRJM_EVAL_F *   qA;
    PRJM_EVAL_F *   qR2;
    PRJM_EVAL_F *   qG2;
    PRJM_EVAL_F *   qB2;
    PRJM_EVAL_F *   qA2;
    bool            additive;
    bool            thickOutline;
};
struct milkShapeCache_t {
    bool  enabled;
    float cx, cy, rad, ang;
    int   sides;
    float r, g, b, a;
    bool  additive, thickOutline;
};
static milkShapeRuntime_t s_milkShapeRt[MILK_MAX_SHAPES];
static int                s_milkNumShapeRt = 0;
// draw-worker-safe cache, same split as the wave/warp-mesh caches above.
static milkShapeCache_t   s_milkShapeCache[MILK_MAX_SHAPES];

static void VisUnloadMilkShapes() {
    for ( int i = 0; i < MILK_MAX_SHAPES; i++ ) {
        milkShapeRuntime_t & sr = s_milkShapeRt[i];
        if ( sr.frameCode != NULL ) { sr.eval.FreeCode( sr.frameCode ); sr.frameCode = NULL; }
        sr.eval.Shutdown();
        s_milkShapeCache[i].enabled = false;
    }
    s_milkNumShapeRt = 0;
}

static void VisLoadMilkShapes( const idMilkPreset & preset ) {
    VisUnloadMilkShapes();
    s_milkNumShapeRt = idMath::ClampInt( 0, MILK_MAX_SHAPES, preset.NumShapes() );
    for ( int i = 0; i < s_milkNumShapeRt; i++ ) {
        milkShapeRuntime_t & sr = s_milkShapeRt[i];
        const milkShapeCode_t & s = preset.GetShape( i );
        if ( !sr.eval.Init() ) {
            idLib::Warning( "visualizer: shape %d evaluator init failed", s.index );
            continue;
        }
        sr.frameCode = sr.eval.Compile( s.perFrame.c_str() );
        sr.qX       = sr.eval.GetVariable( "x" );
        sr.qY       = sr.eval.GetVariable( "y" );
        sr.qRad     = sr.eval.GetVariable( "rad" );
        sr.qAng     = sr.eval.GetVariable( "ang" );
        sr.qSides   = sr.eval.GetVariable( "sides" );
        sr.qEnabled = sr.eval.GetVariable( "enabled" );
        sr.qR  = sr.eval.GetVariable( "r" );
        sr.qG  = sr.eval.GetVariable( "g" );
        sr.qB  = sr.eval.GetVariable( "b" );
        sr.qA  = sr.eval.GetVariable( "a" );
        sr.qR2 = sr.eval.GetVariable( "r2" );
        sr.qG2 = sr.eval.GetVariable( "g2" );
        sr.qB2 = sr.eval.GetVariable( "b2" );
        sr.qA2 = sr.eval.GetVariable( "a2" );
        sr.additive     = s.params.GetBool( "additive", false );
        sr.thickOutline = s.params.GetBool( "thickoutline", false );
        s_milkShapeCache[i].enabled = false;
    }
}

static void VisUnloadMilkPresetB();   // forward decl: PRD M5 mash-up B side, defined below
static idStr VisStripShaderBody( const idStr & raw );   // forward decl: PRD M3, defined below
static void VisUnloadShaderToy();     // forward decl: PRD FR-C10 ShaderToy import, defined below

static void VisUnloadMilkPreset() {
    if ( s_milkInitCode != NULL )  { s_milkEval.FreeCode( s_milkInitCode );  s_milkInitCode = NULL; }
    if ( s_milkFrameCode != NULL ) { s_milkEval.FreeCode( s_milkFrameCode ); s_milkFrameCode = NULL; }
    if ( s_milkPixelCode != NULL ) { s_milkEval.FreeCode( s_milkPixelCode ); s_milkPixelCode = NULL; }
    s_milkEval.Shutdown();
    s_milkActive = false;
    s_milkWarpShaderReady = false;
    VisUnloadMilkWaves();
    VisUnloadMilkShapes();
    VisUnloadMilkPresetB();   // a mash-up needs A active -- if A goes away, so does B
}

// console-callable: load a .milk preset and make it the active warp driver.
// Tolerant of failure (NFR-3): logs and leaves milk mode inactive rather
// than half-applying a broken preset.
static bool VisLoadMilkPreset( const char * path ) {
    VisUnloadMilkPreset();
    // PRD FR-C10: a ShaderToy shader and a .milk warp_ shader both drive the
    // single backend custom-shader "A" program slot (s_milkWarpProgId) -- they
    // are mutually exclusive, so loading a .milk preset clears any active
    // ShaderToy layer (and vice-versa, see VisLoadShaderToyFile).
    VisUnloadShaderToy();
    if ( !s_milkPreset.Load( path ) ) {
        return false;
    }
    if ( !s_milkEval.Init() ) {
        idLib::Warning( "visualizer: milk evaluator init failed" );
        return false;
    }
    s_milkInitCode  = s_milkEval.Compile( s_milkPreset.GetPerFrameInit().c_str() );
    s_milkFrameCode = s_milkEval.Compile( s_milkPreset.GetPerFrame().c_str() );
    s_milkPixelCode = s_milkEval.Compile( s_milkPreset.GetPerPixel().c_str() );
    s_milkElapsedSec = 0.0f;
    s_milkRandPresetValue = VisSeedMilkRandPreset( path );
    s_milkEval.UpdateVariables( 0.0f, MILK_DEFAULT_GX, MILK_DEFAULT_GY, (int)VIS_W, (int)VIS_H, 0.0f );
    if ( s_milkInitCode != NULL ) {
        s_milkEval.Execute( s_milkInitCode );
    }
    // PRD M3: transpile the preset's own warp_ (and, if present, comp_)
    // shader -- DrawMilkWarpMesh below switches to the resulting
    // full-screen-quad custom-shader path when warp_ transpiles
    // successfully, falling back to the CPU-mesh path (which never uses a
    // preset-specific shader) otherwise. comp_ (MilkDrop's separate
    // post-composite shader, run as a second full-screen pass on the
    // already-warped image) rides along as an optional extra pass -- warp_
    // alone is still enough to activate the custom-shader path; comp_ just
    // adds polish on top when both are present and both transpile.
    //
    // Fixed real bug: this used to call renderSystem->EnqueueVisMilkShaderCompile()
    // directly, right here. That silently never reached the backend -- this
    // function runs from console-command processing (VisLoadMilkPreset is
    // called synchronously from vis_milkPreset/the +startup-command path),
    // not from Draw2D's per-frame render-command context that every OTHER
    // Enqueue* call in this codebase (BeginOffscreenRender, EnqueueImGuiRender,
    // etc.) is issued from. Confirmed via a dedicated main-thread-safe
    // compile-status poll (RB_GetMilkWarpCompileStatus, see Frame() below):
    // it stayed 0 (never attempted) across an entire run, proving
    // RB_VisMilkShaderCompile was never even invoked, not just that it
    // failed. Now just stashes the transpiled GLSL + a pending flag;
    // DrawMilkWarpMesh (draw-worker thread, the same context every other
    // working Enqueue* call in this file already uses) issues the actual
    // one-time EnqueueVisMilkShaderCompile call on its next invocation.
    s_milkWarpShaderReady = false;
    s_milkWarpPendingCompFragGLSL.Clear();
    const idStr warpBody = VisStripShaderBody( s_milkPreset.GetWarpShader() );
    if ( !warpBody.IsEmpty() ) {
        if ( !s_milkVertexGLSLReady ) {
            idStr vertexErr;
            s_milkVertexGLSLReady = idMilkShaderTranspiler::TranspileVertexPassthrough( s_milkVertexGLSL, vertexErr );
            if ( !s_milkVertexGLSLReady ) {
                idLib::Warning( "visualizer: milk warp shader vertex passthrough transpile failed: %s", vertexErr.c_str() );
            }
        }
        idStr fragGLSL, fragErr;
        if ( s_milkVertexGLSLReady && idMilkShaderTranspiler::Transpile( warpBody.c_str(), "milk_warp", fragGLSL, fragErr ) ) {
            s_milkWarpPendingFragGLSL = fragGLSL;
            s_milkWarpPendingVertexGLSL = s_milkVertexGLSL;
            s_milkWarpShaderReady = true;

            // comp_ is optional polish riding on top of a working warp_ --
            // a comp_ transpile failure only loses the extra pass, not the
            // whole custom-shader feature (same NFR-3 tolerance as every
            // other preset-parsing failure mode in this file).
            const idStr compBody = VisStripShaderBody( s_milkPreset.GetCompShader() );
            if ( !compBody.IsEmpty() ) {
                idStr compGLSL, compErr;
                if ( idMilkShaderTranspiler::Transpile( compBody.c_str(), "milk_comp", compGLSL, compErr ) ) {
                    s_milkWarpPendingCompFragGLSL = compGLSL;
                } else {
                    idLib::Warning( "visualizer: milk preset '%s' comp_ shader transpile failed, warp_ still active without it: %s",
                        s_milkPreset.GetName().c_str(), compErr.c_str() );
                }
            }

            s_milkWarpPendingCompile = true;
        } else if ( s_milkVertexGLSLReady ) {
            idLib::Warning( "visualizer: milk preset '%s' warp_ shader transpile failed, using CPU-mesh warp instead: %s",
                s_milkPreset.GetName().c_str(), fragErr.c_str() );
        }
    }

    // cache the fixed (non-per-frame) waveform/video-echo scalar params here,
    // on the main thread, so DrawMilkWaveform/DrawMilkVideoEcho (draw-worker
    // thread) never touch s_milkPreset.GetParams() (idDict, not thread-safe)
    // directly -- see the comment above the struct definitions.
    {
        const idDict & params = s_milkPreset.GetParams();
        s_milkWaveParams.r = idMath::ClampFloat( 0.0f, 1.0f, params.GetFloat( "wave_r", 1.0f ) );
        s_milkWaveParams.g = idMath::ClampFloat( 0.0f, 1.0f, params.GetFloat( "wave_g", 1.0f ) );
        s_milkWaveParams.b = idMath::ClampFloat( 0.0f, 1.0f, params.GetFloat( "wave_b", 1.0f ) );
        s_milkWaveParams.a = idMath::ClampFloat( 0.0f, 1.0f, params.GetFloat( "wave_a", 1.0f ) );
        s_milkWaveParams.scale    = params.GetFloat( "fWaveScale", 1.0f );
        s_milkWaveParams.alphaMul = params.GetFloat( "fWaveAlpha", 1.0f );
        s_milkWaveParams.cx = params.GetFloat( "wave_x", 0.5f ) * VIS_W;
        s_milkWaveParams.cy = params.GetFloat( "wave_y", 0.5f ) * VIS_H;

        s_milkEchoParams.alpha  = idMath::ClampFloat( 0.0f, 1.0f, params.GetFloat( "fVideoEchoAlpha", 0.0f ) );
        s_milkEchoParams.zoom   = idMath::ClampFloat( 0.1f, 4.0f, params.GetFloat( "fVideoEchoZoom", 2.0f ) );
        s_milkEchoParams.orient = idMath::ClampInt( 0, 3, params.GetInt( "nVideoEchoOrientation", 0 ) );
    }

    VisLoadMilkWaves( s_milkPreset );
    VisLoadMilkShapes( s_milkPreset );

    // Fixed real bug: this used to be set right after the per_pixel_ compile,
    // BEFORE the warp_ transpile block above ran -- a preset with a working
    // warp_/comp_ shader but no per_pixel_ code (the shader-driven-only case,
    // confirmed live via a hand-authored warp_+comp_-only test preset) left
    // s_milkActive false forever, so DrawMilkWarpMesh's caller (gated on
    // s_milkActive) never even ran, leaving the whole custom-shader path
    // dead code for such presets even though it had already compiled and
    // linked successfully.
    s_milkActive = ( s_milkPixelCode != NULL ) || s_milkWarpShaderReady;

    idLib::Printf( "visualizer: milk preset '%s' -- init %s, frame %s, pixel %s%s\n",
        s_milkPreset.GetName().c_str(),
        s_milkInitCode  != NULL ? "ok" : ( s_milkPreset.GetPerFrameInit().IsEmpty() ? "-" : "FAILED" ),
        s_milkFrameCode != NULL ? "ok" : ( s_milkPreset.GetPerFrame().IsEmpty()     ? "-" : "FAILED" ),
        s_milkPixelCode != NULL ? "ok" : ( s_milkPreset.GetPerPixel().IsEmpty()     ? "-" : "FAILED" ),
        s_milkActive ? "" : " (no per_pixel_ and no working warp_ shader -> milk mode inactive, mesh unaffected)" );
    return true;
}

static void VisUnloadMilkPresetB() {
    if ( s_milkInitCodeB != NULL )  { s_milkEvalB.FreeCode( s_milkInitCodeB );  s_milkInitCodeB = NULL; }
    if ( s_milkFrameCodeB != NULL ) { s_milkEvalB.FreeCode( s_milkFrameCodeB ); s_milkFrameCodeB = NULL; }
    if ( s_milkPixelCodeB != NULL ) { s_milkEvalB.FreeCode( s_milkPixelCodeB ); s_milkPixelCodeB = NULL; }
    s_milkEvalB.Shutdown();
    s_milkActiveB = false;
    s_milkWarpBShaderReady = false;
    s_milkWarpBPendingCompFragGLSL.Clear();
    // PRD FR-A5: every path that removes the B side (manual mash-up clear,
    // a fresh VisLoadMilkPresetB call resetting before it loads, A itself
    // going away) funnels through here -- cancel any in-flight soft cut
    // too, so a stale s_milkPresetBPath can't get "promoted" later via
    // VisUpdateMilkFrame's ramp for a B side that no longer exists.
    s_milkCutInProgress = false;
}

// PRD M5: load a second preset as the mash-up "B" side. Requires preset A
// to already be active (a mash-up blends TWO active presets, not a lone
// preset) -- tolerant of failure (NFR-3) exactly like VisLoadMilkPreset.
static bool VisLoadMilkPresetB( const char * path ) {
    VisUnloadMilkPresetB();
    if ( !s_milkActive ) {
        idLib::Printf( "vis_milkMash: load preset A first (vis_milkPreset <path>)\n" );
        return false;
    }
    if ( !s_milkPresetB.Load( path ) ) {
        return false;
    }
    if ( !s_milkEvalB.Init() ) {
        idLib::Warning( "visualizer: milk evaluator (B) init failed" );
        return false;
    }
    s_milkInitCodeB  = s_milkEvalB.Compile( s_milkPresetB.GetPerFrameInit().c_str() );
    s_milkFrameCodeB = s_milkEvalB.Compile( s_milkPresetB.GetPerFrame().c_str() );
    s_milkPixelCodeB = s_milkEvalB.Compile( s_milkPresetB.GetPerPixel().c_str() );
    s_milkRandPresetValueB = VisSeedMilkRandPreset( path );
    s_milkEvalB.UpdateVariables( 0.0f, MILK_DEFAULT_GX, MILK_DEFAULT_GY, (int)VIS_W, (int)VIS_H, 0.0f );
    if ( s_milkInitCodeB != NULL ) {
        s_milkEvalB.Execute( s_milkInitCodeB );
    }

    // PRD M5: transpile B's own warp_ (and, if present, comp_) shader --
    // mirrors VisLoadMilkPreset's A-side block exactly, just stashed into the
    // B-slot pending state so DrawMilkWarpMesh's draw-worker-thread compile
    // (the correct render-command context -- see the fix comment on the
    // A-side block above) issues a SEPARATE EnqueueVisMilkShaderCompile call
    // with isMashB=true.
    s_milkWarpBShaderReady = false;
    s_milkWarpBPendingCompFragGLSL.Clear();
    const idStr warpBodyB = VisStripShaderBody( s_milkPresetB.GetWarpShader() );
    if ( !warpBodyB.IsEmpty() ) {
        if ( !s_milkVertexGLSLReady ) {
            idStr vertexErr;
            s_milkVertexGLSLReady = idMilkShaderTranspiler::TranspileVertexPassthrough( s_milkVertexGLSL, vertexErr );
            if ( !s_milkVertexGLSLReady ) {
                idLib::Warning( "visualizer: milk warp shader vertex passthrough transpile failed: %s", vertexErr.c_str() );
            }
        }
        idStr fragGLSLB, fragErrB;
        if ( s_milkVertexGLSLReady && idMilkShaderTranspiler::Transpile( warpBodyB.c_str(), "milk_warp_b", fragGLSLB, fragErrB ) ) {
            s_milkWarpBPendingFragGLSL = fragGLSLB;
            s_milkWarpBPendingVertexGLSL = s_milkVertexGLSL;
            s_milkWarpBShaderReady = true;

            const idStr compBodyB = VisStripShaderBody( s_milkPresetB.GetCompShader() );
            if ( !compBodyB.IsEmpty() ) {
                idStr compGLSLB, compErrB;
                if ( idMilkShaderTranspiler::Transpile( compBodyB.c_str(), "milk_comp_b", compGLSLB, compErrB ) ) {
                    s_milkWarpBPendingCompFragGLSL = compGLSLB;
                } else {
                    idLib::Warning( "visualizer: mash-up preset B '%s' comp_ shader transpile failed, warp_ still active without it: %s",
                        s_milkPresetB.GetName().c_str(), compErrB.c_str() );
                }
            }

            s_milkWarpBPendingCompile = true;
        } else if ( s_milkVertexGLSLReady ) {
            idLib::Warning( "visualizer: mash-up preset B '%s' warp_ shader transpile failed, using CPU-mesh warp instead: %s",
                s_milkPresetB.GetName().c_str(), fragErrB.c_str() );
        }
    }

    // Fixed real bug (same class as A's own s_milkActive, see that fix's
    // comment): must account for a working warp_/comp_ shader, not just
    // per_pixel_ code -- otherwise a shader-driven-only B preset would never
    // actually take part in the mash-up.
    s_milkActiveB = ( s_milkPixelCodeB != NULL ) || s_milkWarpBShaderReady;
    idLib::Printf( "visualizer: mash-up preset B '%s' -- init %s, frame %s, pixel %s%s\n",
        s_milkPresetB.GetName().c_str(),
        s_milkInitCodeB  != NULL ? "ok" : ( s_milkPresetB.GetPerFrameInit().IsEmpty() ? "-" : "FAILED" ),
        s_milkFrameCodeB != NULL ? "ok" : ( s_milkPresetB.GetPerFrame().IsEmpty()     ? "-" : "FAILED" ),
        s_milkPixelCodeB != NULL ? "ok" : ( s_milkPresetB.GetPerPixel().IsEmpty()     ? "-" : "FAILED" ),
        s_milkActiveB ? "" : " (no per_pixel_ and no working warp_ shader -> B side inactive, mash-up unaffected)" );
    return true;
}

// Runs per_frame_ once, then per_pixel_ once per warp-mesh vertex, caching
// the resulting sample UVs into s_milkTex[] -- called from Frame() (main
// thread) alongside VisUpdateModulation, BEFORE Draw2D (draw-worker thread)
// ever reads s_milkTex. See the threading note in the block comment above.
// Shared per-vertex warp-mesh math, factored out so the PRD M5 mash-up "B"
// side can reuse the exact same, already-tested logic rather than a risky
// hand-duplicated copy (a single source of truth for both A and B).
static void VisComputeMilkWarpMesh( idMilkEvaluator & eval, const idDict & params,
                                     struct projectm_eval_code * pixelCode, idVec2 * outTex ) {
    PRJM_EVAL_F * zoomVar = eval.GetVariable( "zoom" );
    PRJM_EVAL_F * rotVar  = eval.GetVariable( "rot" );
    PRJM_EVAL_F * dxVar   = eval.GetVariable( "dx" );
    PRJM_EVAL_F * dyVar   = eval.GetVariable( "dy" );
    PRJM_EVAL_F * sxVar   = eval.GetVariable( "sx" );
    PRJM_EVAL_F * syVar   = eval.GetVariable( "sy" );
    PRJM_EVAL_F * cxVar   = eval.GetVariable( "cx" );
    PRJM_EVAL_F * cyVar   = eval.GetVariable( "cy" );

    const int NX = MILK_DEFAULT_GX + 1;
    const int NY = MILK_DEFAULT_GY + 1;
    for ( int j = 0; j < NY; j++ ) {
        for ( int i = 0; i < NX; i++ ) {
            const int idx = j * NX + i;
            const float gx = (float)i / (float)MILK_DEFAULT_GX;
            const float gy = (float)j / (float)MILK_DEFAULT_GY;
            const float u = gx - 0.5f;
            const float v = gy - 0.5f;
            const float rad = idMath::Sqrt( u * u + v * v ) * 2.0f;   // 0 center .. ~1.41 corners
            const float ang = ( idMath::Fabs( u ) > 1e-6f || idMath::Fabs( v ) > 1e-6f ) ? idMath::ATan( v, u ) : 0.0f;

            // per_pixel_ semantics: each vertex starts from the preset's
            // scalar defaults, then optionally overrides them -- so reset
            // every var to that default before Execute(), every vertex.
            if ( zoomVar != NULL ) *zoomVar = params.GetFloat( "zoom", 1.0f );
            if ( rotVar  != NULL ) *rotVar  = params.GetFloat( "rot", 0.0f );
            if ( dxVar   != NULL ) *dxVar   = params.GetFloat( "dx", 0.0f );
            if ( dyVar   != NULL ) *dyVar   = params.GetFloat( "dy", 0.0f );
            if ( sxVar   != NULL ) *sxVar   = params.GetFloat( "sx", 1.0f );
            if ( syVar   != NULL ) *syVar   = params.GetFloat( "sy", 1.0f );
            if ( cxVar   != NULL ) *cxVar   = params.GetFloat( "cx", 0.5f );
            if ( cyVar   != NULL ) *cyVar   = params.GetFloat( "cy", 0.5f );

            eval.SetPixelVars( gx, gy, rad, ang );
            if ( pixelCode != NULL ) {
                eval.Execute( pixelCode );
            }

            const float zoom = ( zoomVar != NULL && *zoomVar != 0.0f ) ? (float)*zoomVar : 1.0f;
            const float rot  = ( rotVar  != NULL ) ? (float)*rotVar : 0.0f;
            const float dx   = ( dxVar   != NULL ) ? (float)*dxVar : 0.0f;
            const float dy   = ( dyVar   != NULL ) ? (float)*dyVar : 0.0f;
            const float sx   = ( sxVar != NULL && *sxVar != 0.0f ) ? (float)*sxVar : 1.0f;
            const float sy   = ( syVar != NULL && *syVar != 0.0f ) ? (float)*syVar : 1.0f;
            const float cx   = ( cxVar  != NULL ) ? (float)*cxVar : 0.5f;
            const float cy   = ( cyVar  != NULL ) ? (float)*cyVar : 0.5f;

            // sample the PREVIOUS frame around (cx,cy), scaled by zoom*sx/sy,
            // rotated by rot, offset by dx/dy (docs/research-milkdrop-projectm.md
            // section 2's "warp pass": per-vertex eqs produce each vertex's
            // sample UV into the prior frame's canvas).
            const float su = ( gx - cx ) / ( zoom * sx );
            const float sv = ( gy - cy ) / ( zoom * sy );
            const float c = idMath::Cos( rot ), s = idMath::Sin( rot );
            const float ru = su * c - sv * s;
            const float rv = su * s + sv * c;
            outTex[idx].Set( cx + ru - dx, cy + rv - dy );
        }
    }
}

// PRD M5: evaluate each active wave's per_frame_ once, then per_point_ once
// per sample point, caching x/y/r/g/b/a into s_milkWavePoints[] for the
// draw-worker thread (DrawMilkWavecode). Called from VisUpdateMilkFrame
// (main thread) right after the main context's per_frame_, so this frame's
// q-values are current before being bridged into each wave.
static void VisUpdateMilkWaves( float dtSec, float progress ) {
    const float * wave = g_audioAnalyzer.GetWaveform();
    for ( int w = 0; w < s_milkNumWaveRt; w++ ) {
        milkWaveRuntime_t & wr = s_milkWaveRt[w];
        if ( !wr.eval.IsInitialized() ) {
            s_milkWavePointCount[w] = 0;
            continue;
        }
        wr.eval.UpdateVariables( dtSec, MILK_DEFAULT_GX, MILK_DEFAULT_GY, (int)VIS_W, (int)VIS_H, progress );
        // bridge q1..q64 IN from the main context -- the only mechanism this
        // vendored library supports for sharing state across two separate
        // contexts (see the block comment above the struct definitions).
        for ( int q = 0; q < MILK_NUM_Q_VARS; q++ ) {
            wr.eval.SetQ( q, s_milkEval.GetQ( q ) );
        }
        if ( wr.frameCode != NULL ) {
            wr.eval.Execute( wr.frameCode );
        }

        const int n = wr.numSamples;
        for ( int i = 0; i < n; i++ ) {
            const float frac = ( n > 1 ) ? ( (float)i / (float)( n - 1 ) ) : 0.0f;
            const int sampleIdx = idMath::ClampInt( 0, FFT_SIZE - 1, (int)( frac * ( FFT_SIZE - 1 ) ) );
            const float v = idMath::ClampFloat( -1.0f, 1.0f, wave[sampleIdx] );

            // per-point defaults, overridable by the wave's own code -- same
            // "reset to default every iteration, then let the preset's code
            // override" convention VisComputeMilkWarpMesh already follows.
            if ( wr.qSample != NULL ) *wr.qSample = frac;
            if ( wr.qValue1 != NULL ) *wr.qValue1 = v;
            if ( wr.qValue2 != NULL ) *wr.qValue2 = v;    // mono waveform only -- no true stereo channel split available here
            if ( wr.qX != NULL ) *wr.qX = frac;              // 0..1 left to right, matching the built-in waveform's own layout
            if ( wr.qY != NULL ) *wr.qY = 0.5f + v * 0.5f;   // 0..1, centered
            if ( wr.qR != NULL ) *wr.qR = s_milkWaveParams.r;
            if ( wr.qG != NULL ) *wr.qG = s_milkWaveParams.g;
            if ( wr.qB != NULL ) *wr.qB = s_milkWaveParams.b;
            if ( wr.qA != NULL ) *wr.qA = s_milkWaveParams.a;

            if ( wr.pointCode != NULL ) {
                wr.eval.Execute( wr.pointCode );
            }

            milkWavePoint_t & p = s_milkWavePoints[w][i];
            p.x = ( wr.qX != NULL ) ? idMath::ClampFloat( 0.0f, 1.0f, (float)*wr.qX ) * VIS_W : frac * VIS_W;
            p.y = ( wr.qY != NULL ) ? idMath::ClampFloat( 0.0f, 1.0f, (float)*wr.qY ) * VIS_H : VIS_H * 0.5f;
            p.r = ( wr.qR != NULL ) ? idMath::ClampFloat( 0.0f, 1.0f, (float)*wr.qR ) : s_milkWaveParams.r;
            p.g = ( wr.qG != NULL ) ? idMath::ClampFloat( 0.0f, 1.0f, (float)*wr.qG ) : s_milkWaveParams.g;
            p.b = ( wr.qB != NULL ) ? idMath::ClampFloat( 0.0f, 1.0f, (float)*wr.qB ) : s_milkWaveParams.b;
            p.a = ( wr.qA != NULL ) ? idMath::ClampFloat( 0.0f, 1.0f, (float)*wr.qA ) : s_milkWaveParams.a;
        }
        s_milkWavePointCount[w] = n;

        // bridge q1..q64 back OUT so any writes this wave's code made persist
        // into next frame's per_frame_/per_pixel_, matching real MilkDrop's
        // single shared-VM q semantics as closely as this library allows.
        for ( int q = 0; q < MILK_NUM_Q_VARS; q++ ) {
            s_milkEval.SetQ( q, wr.eval.GetQ( q ) );
        }
    }
}

// PRD M5: evaluate each active shape's per_frame_ once (no per-point loop --
// see the block comment above the shape struct definitions) and cache the
// result for DrawMilkShapecode (draw-worker thread). Called from
// VisUpdateMilkFrame (main thread) alongside VisUpdateMilkWaves.
static void VisUpdateMilkShapes( float dtSec, float progress ) {
    for ( int i = 0; i < s_milkNumShapeRt; i++ ) {
        milkShapeRuntime_t & sr = s_milkShapeRt[i];
        milkShapeCache_t & cache = s_milkShapeCache[i];
        if ( !sr.eval.IsInitialized() ) {
            cache.enabled = false;
            continue;
        }
        const idDict & params = s_milkPreset.GetShape( i ).params;

        sr.eval.UpdateVariables( dtSec, MILK_DEFAULT_GX, MILK_DEFAULT_GY, (int)VIS_W, (int)VIS_H, progress );
        for ( int q = 0; q < MILK_NUM_Q_VARS; q++ ) {
            sr.eval.SetQ( q, s_milkEval.GetQ( q ) );
        }

        // defaults from the shape's own static params, reset every frame
        // before Execute() -- same "reset to default, let the preset's code
        // override" convention VisComputeMilkWarpMesh already follows.
        if ( sr.qX != NULL )       *sr.qX = params.GetFloat( "x", 0.5f );
        if ( sr.qY != NULL )       *sr.qY = params.GetFloat( "y", 0.5f );
        if ( sr.qRad != NULL )     *sr.qRad = params.GetFloat( "rad", 0.1f );
        if ( sr.qAng != NULL )     *sr.qAng = params.GetFloat( "ang", 0.0f );
        if ( sr.qSides != NULL )   *sr.qSides = params.GetFloat( "sides", 4.0f );
        if ( sr.qEnabled != NULL ) *sr.qEnabled = params.GetBool( "enabled", true ) ? 1.0 : 0.0;
        if ( sr.qR != NULL )       *sr.qR = params.GetFloat( "r", 1.0f );
        if ( sr.qG != NULL )       *sr.qG = params.GetFloat( "g", 1.0f );
        if ( sr.qB != NULL )       *sr.qB = params.GetFloat( "b", 1.0f );
        if ( sr.qA != NULL )       *sr.qA = params.GetFloat( "a", 1.0f );
        if ( sr.qR2 != NULL )      *sr.qR2 = params.GetFloat( "r2", params.GetFloat( "r", 1.0f ) );
        if ( sr.qG2 != NULL )      *sr.qG2 = params.GetFloat( "g2", params.GetFloat( "g", 1.0f ) );
        if ( sr.qB2 != NULL )      *sr.qB2 = params.GetFloat( "b2", params.GetFloat( "b", 1.0f ) );
        if ( sr.qA2 != NULL )      *sr.qA2 = params.GetFloat( "a2", params.GetFloat( "a", 1.0f ) );

        if ( sr.frameCode != NULL ) {
            sr.eval.Execute( sr.frameCode );
        }

        // bridge q1..q64 back OUT, same rationale as the wave loop above.
        for ( int q = 0; q < MILK_NUM_Q_VARS; q++ ) {
            s_milkEval.SetQ( q, sr.eval.GetQ( q ) );
        }

        cache.enabled = ( sr.qEnabled == NULL ) || ( *sr.qEnabled > 0.5 );
        if ( !cache.enabled ) {
            continue;
        }
        cache.cx   = idMath::ClampFloat( -0.5f, 1.5f, ( sr.qX != NULL )   ? (float)*sr.qX   : 0.5f ) * VIS_W;
        cache.cy   = idMath::ClampFloat( -0.5f, 1.5f, ( sr.qY != NULL )   ? (float)*sr.qY   : 0.5f ) * VIS_H;
        cache.rad  = idMath::ClampFloat(  0.0f, 4.0f, ( sr.qRad != NULL ) ? (float)*sr.qRad : 0.1f ) * VIS_H * 0.5f;
        cache.ang  = ( sr.qAng != NULL ) ? (float)*sr.qAng : 0.0f;
        cache.sides = idMath::ClampInt( 3, MILK_SHAPE_MAX_SIDES, ( sr.qSides != NULL ) ? (int)*sr.qSides : 4 );
        cache.r = idMath::ClampFloat( 0.0f, 1.0f, ( sr.qR != NULL ) ? (float)*sr.qR : 1.0f );
        cache.g = idMath::ClampFloat( 0.0f, 1.0f, ( sr.qG != NULL ) ? (float)*sr.qG : 1.0f );
        cache.b = idMath::ClampFloat( 0.0f, 1.0f, ( sr.qB != NULL ) ? (float)*sr.qB : 1.0f );
        cache.a = idMath::ClampFloat( 0.0f, 1.0f, ( sr.qA != NULL ) ? (float)*sr.qA : 1.0f );
        cache.additive     = sr.additive;
        cache.thickOutline = sr.thickOutline;
    }
}

static void VisUpdateMilkFrame( float dtSec ) {
    if ( !s_milkActive ) {
        return;
    }
    s_milkElapsedSec += dtSec;
    // "progress" = fraction of the way to the next scheduled preset switch
    // (real MilkDrop semantics -- NOT audio playback position; this engine
    // has no track-seek/position API at all, see the roadmap's CTRL+up/down
    // note, so that reading isn't available to compute here even if wanted).
    const float cycleSecs = idMath::ClampFloat( 2.0f, 600.0f, vis_presetCycleSecs.GetFloat() );
    const float progress = idMath::ClampFloat( 0.0f, 1.0f, s_milkElapsedSec / cycleSecs );
    s_milkEval.UpdateVariables( dtSec, MILK_DEFAULT_GX, MILK_DEFAULT_GY, (int)VIS_W, (int)VIS_H, progress );
    if ( s_milkFrameCode != NULL ) {
        s_milkEval.Execute( s_milkFrameCode );
    }
    VisComputeMilkWarpMesh( s_milkEval, s_milkPreset.GetParams(), s_milkPixelCode, s_milkTex );
    VisUpdateMilkWaves( dtSec, progress );
    VisUpdateMilkShapes( dtSec, progress );

    // PRD M3: refresh the custom warp shader's per-frame uniforms (see
    // s_milkWarpUniforms's own comment above) -- computed here regardless
    // of whether a shader is actually compiled/ready this frame, same
    // "always compute, only conditionally used" simplicity as s_milkTex[].
    {
        static int s_milkFrameCounter = 0;
        s_milkFrameCounter++;
        s_milkWarpUniforms.time = s_milkElapsedSec;
        s_milkWarpUniforms.frame = (float)s_milkFrameCounter;
        s_milkWarpUniforms.fps = ( dtSec > 0.0001f ) ? ( 1.0f / dtSec ) : 60.0f;
        s_milkWarpUniforms.progress = progress;
        s_milkWarpUniforms.bass = g_audioAnalyzer.GetBass();
        s_milkWarpUniforms.mid = g_audioAnalyzer.GetMid();
        s_milkWarpUniforms.treb = g_audioAnalyzer.GetTreb();
        s_milkWarpUniforms.bassAtt = g_audioAnalyzer.GetBassAtt();
        s_milkWarpUniforms.midAtt = g_audioAnalyzer.GetMidAtt();
        s_milkWarpUniforms.trebAtt = g_audioAnalyzer.GetTrebAtt();
        // MilkDrop `vol`/`vol_att`: overall (attenuated) volume = mean of the
        // three bands, matching this file's own VMS_LEVEL definition.
        s_milkWarpUniforms.vol = ( s_milkWarpUniforms.bass + s_milkWarpUniforms.mid + s_milkWarpUniforms.treb ) * ( 1.0f / 3.0f );
        s_milkWarpUniforms.volAtt = ( s_milkWarpUniforms.bassAtt + s_milkWarpUniforms.midAtt + s_milkWarpUniforms.trebAtt ) * ( 1.0f / 3.0f );
        // MilkDrop `roam_cos`/`roam_sin`: four independent slowly-drifting
        // per-frame values. Real MilkDrop derives them from a slow noise of
        // time; approximated here with four slow, mutually-incommensurate
        // cos/sin rates so each component roams independently (good enough for
        // the swizzles presets take -- .x/.y/.zww/.zxy).
        static const float roamRate[4] = { 0.071f, 0.113f, 0.173f, 0.237f };
        for ( int r = 0; r < 4; r++ ) {
            const float phase = s_milkElapsedSec * roamRate[r] + (float)r * 1.7f;
            s_milkWarpUniforms.roamCos[r] = idMath::Cos( phase );
            s_milkWarpUniforms.roamSin[r] = idMath::Sin( phase );
        }
        // standard MilkDrop convention: the smaller screen dimension gets
        // aspect 1.0, the larger one is scaled down by the ratio.
        s_milkWarpUniforms.aspectX = ( VIS_W > VIS_H ) ? ( VIS_H / VIS_W ) : 1.0f;
        s_milkWarpUniforms.aspectY = ( VIS_H > VIS_W ) ? ( VIS_W / VIS_H ) : 1.0f;
        for ( int q = 0; q < 32; q++ ) {
            s_milkWarpUniforms.q[q] = (float)s_milkEval.GetQ( q );
        }
        s_milkWarpUniforms.randFrame = VisNextMilkRandFrame();
        s_milkWarpUniforms.randPreset = s_milkRandPresetValue;
    }

    // PRD M5: mash-up "B" side, computed with the exact same shared logic,
    // into its own tex cache -- DrawMilkWarpMesh blends the two.
    if ( s_milkActiveB ) {
        s_milkEvalB.UpdateVariables( dtSec, MILK_DEFAULT_GX, MILK_DEFAULT_GY, (int)VIS_W, (int)VIS_H, progress );
        if ( s_milkFrameCodeB != NULL ) {
            s_milkEvalB.Execute( s_milkFrameCodeB );
        }
        VisComputeMilkWarpMesh( s_milkEvalB, s_milkPresetB.GetParams(), s_milkPixelCodeB, s_milkTexB );

        // PRD M5: B's own shader-uniform snapshot -- everything global (time/
        // frame/fps/progress/aspect/audio bands) is identical to A's, only
        // q[]/randPreset (per-preset state) differ, so this reuses
        // s_milkWarpUniforms's already-computed globals rather than
        // recomputing them.
        s_milkWarpUniformsB = s_milkWarpUniforms;
        for ( int q = 0; q < 32; q++ ) {
            s_milkWarpUniformsB.q[q] = (float)s_milkEvalB.GetQ( q );
        }
        s_milkWarpUniformsB.randPreset = s_milkRandPresetValueB;
    }

    // PRD FR-A5: advance the soft-cut ramp, then promote B to A once it
    // completes. Runs on the main thread (this function's own documented
    // contract, same as every other per_frame_/per_pixel_ update here) --
    // the exact context VisLoadMilkPreset/VisLoadMilkPresetB already
    // require (console-command processing), so calling it from here is no
    // new cross-thread concern.
    if ( s_milkCutInProgress ) {
        const float softCutSecs = idMath::ClampFloat( 0.1f, 30.0f, vis_milkSoftCutSecs.GetFloat() );
        s_milkMashMix += dtSec / softCutSecs;
        if ( s_milkMashMix >= 1.0f ) {
            const idStr nextPath = s_milkPresetBPath;   // VisLoadMilkPreset below unloads B (and clears s_milkPresetBPath's owner state) as a side effect -- copy the path first
            s_milkCutInProgress = false;
            s_milkMashMix = 0.0f;
            VisLoadMilkPreset( nextPath.c_str() );   // also calls VisUnloadMilkPresetB() internally -- B is fully torn down as part of this
        }
    }
}

/*
===========================================================================
PRD FR-C10: ShaderToy import.

ShaderToy shaders are ALREADY plain GLSL (unlike MilkDrop warp_/comp_ HLSL,
which needs the M3 hlsl2glslfork transpiler), so there is nothing to transpile
here. The only work is adapting ShaderToy's fixed wrapper convention to this
engine's EXISTING custom-shader plumbing: a ShaderToy shader defines
    void mainImage( out vec4 fragColor, in vec2 fragCoord )
and expects a small fixed uniform set (iResolution/iTime/iFrame/iMouse/
iChannel0-3/...). VisBuildShaderToyFragGLSL below wraps that body in a real
main() that supplies those uniforms from values this engine already computes,
then feeds the result through the UNCHANGED milk warp compile/draw path
(EnqueueVisMilkShaderCompile + EnqueueVisMilkWarpDraw) -- the closest existing
analog for "take raw shader text at runtime and get it compiled + drawn as a
full-screen layer." No new backend command or GLSL program slot is added.

Integration point: a ShaderToy shader is a full-screen warp-driver, exactly
like an active .milk warp_ shader, so it plugs in as a vis_effect-style mode
that is mutually exclusive with a .milk preset (both occupy the single backend
"A" custom-shader program slot, s_milkWarpProgId -- see the backend
RB_VisMilkShaderCompile). It composites into the same feedback/bloom/
kaleidoscope pipeline every other warp path already feeds.

Mapping honored by the generated wrapper (verified against the backend's
RB_SetMilkCommonUniforms / RB_DrawMilkFullscreenPass uniform set):
  iChannel0        -> sampler_main (this app's feedback / previous-frame buffer)
  iResolution.xy   -> texsize.xy   (backend sets texsize = {w,h,1/w,1/h})
  fragCoord        -> xlv_TEXCOORD0 * texsize.xy  (the 0..1 varying * pixel size)
  iTime            -> time         (real per-frame seconds, VisUpdateShaderToyFrame)
  iTimeDelta       -> 1.0/fps      iFrame -> int(frame)
  output           -> gl_FragData[0] (NOT gl_FragColor -- the milk program's MRT convention)

KNOWN LIMITATIONS (documented, not faked):
  - iChannel1-3 alias the feedback buffer (only iChannel0 is meaningful);
    a shader expecting a specific texture in 1-3 renders wrong-but-harmless.
  - iMouse/iDate are neutral (zero). No multi-buffer ("Buf A".. / Common tab),
    no ES3-only features (texelFetch, integer samplers, textureGrad).
  - Bare-ID fetch (e.g. "MsjBDR") needs a ShaderToy API key for programmatic
    download; only the local-file paste-and-save workflow is implemented (see
    the vis_shadertoyLoad console command). No fake key is baked in.
===========================================================================
*/
static bool  s_shadertoyActive = false;
static idStr s_shadertoyName;
static bool  s_shadertoyPendingCompile = false;
static idStr s_shadertoyPendingVertexGLSL;
static idStr s_shadertoyPendingFragGLSL;

static void VisUnloadShaderToy() {
    s_shadertoyActive = false;
    s_shadertoyPendingCompile = false;
    s_shadertoyName.Clear();
    // NOTE: the backend "A" program slot is left as-is; a subsequent
    // VisLoadMilkPreset/VisLoadShaderToyFile overwrites it via its own
    // EnqueueVisMilkShaderCompile, and EnqueueVisMilkWarpDraw is simply no
    // longer issued for this layer once s_shadertoyActive is false.
}

// Wrap a raw ShaderToy Image-pass fragment body (its mainImage function plus
// any helpers it defines) into a GLSL-1.10 fragment shader that fits the
// EXISTING milk warp program layout, so it compiles + draws through the
// unchanged EnqueueVisMilkShaderCompile / EnqueueVisMilkWarpDraw path. See the
// block comment above for the full uniform mapping and its verification.
static idStr VisBuildShaderToyFragGLSL( const idStr & body ) {
    idStr out;
    out += "// PRD FR-C10 ShaderToy import adapter (generated) -- GLSL 1.10, milk warp program layout\n";
    out += "varying vec2 xlv_TEXCOORD0;\n";
    out += "uniform sampler2D sampler_main;\n";   // ShaderToy iChannel0 == feedback (previous-frame) buffer
    out += "uniform vec4 texsize;\n";             // backend sets {w, h, 1/w, 1/h} of the sampled image
    out += "uniform float time;\n";
    out += "uniform float frame;\n";
    out += "uniform float fps;\n";
    out += "uniform float milkOutputAlpha;\n";
    out += "// ShaderToy -> GLSL 1.10 compatibility shims + uniform adapter\n";
    out += "#define texture texture2D\n";               // modern texture(s,uv) -> texture2D(s,uv)
    out += "#define textureLod(s,uv,lod) texture2D(s,uv)\n";
    out += "#define iChannel0 sampler_main\n";
    out += "#define iChannel1 sampler_main\n";           // KNOWN LIMITATION: 1-3 alias the feedback buffer
    out += "#define iChannel2 sampler_main\n";
    out += "#define iChannel3 sampler_main\n";
    out += "#define iTime time\n";
    out += "#define iTimeDelta (1.0/max(fps,1.0))\n";
    out += "#define iFrameRate fps\n";
    out += "#define iFrame int(frame)\n";
    out += "#define iResolution vec3( texsize.x, texsize.y, ( texsize.y > 0.0 ) ? ( texsize.x / texsize.y ) : 1.0 )\n";
    out += "// ShaderToy uniforms with no engine source yet -- neutral values, set in main()\n";
    out += "vec4  iMouse;\n";
    out += "vec4  iDate;\n";
    out += "vec3  iChannelResolution[4];\n";
    out += "float iChannelTime[4];\n";
    out += "// ---- begin imported ShaderToy source ----\n";
    out += body;
    out += "\n// ---- end imported ShaderToy source ----\n";
    out += "void main() {\n";
    out += "    iMouse = vec4( 0.0, 0.0, 0.0, 0.0 );\n";
    out += "    iDate  = vec4( 0.0, 0.0, 0.0, 0.0 );\n";
    out += "    iChannelResolution[0] = vec3( texsize.x, texsize.y, 1.0 );\n";
    out += "    iChannelResolution[1] = iChannelResolution[0];\n";
    out += "    iChannelResolution[2] = iChannelResolution[0];\n";
    out += "    iChannelResolution[3] = iChannelResolution[0];\n";
    out += "    iChannelTime[0] = time; iChannelTime[1] = time; iChannelTime[2] = time; iChannelTime[3] = time;\n";
    // copy the varying into a plain local BEFORE use -- documented GLSL-1.10
    // driver bug (see kMilkPassthroughFragmentGLSL in tr_backend_draw.cpp):
    // a varying used directly in an expression argument can be mishandled.
    out += "    vec2 stUV = xlv_TEXCOORD0;\n";
    out += "    vec4 fragColor = vec4( 0.0, 0.0, 0.0, 1.0 );\n";
    out += "    mainImage( fragColor, stUV * texsize.xy );\n";
    out += "    gl_FragData[0] = vec4( fragColor.rgb, milkOutputAlpha );\n";
    out += "}\n";
    return out;
}

// console-callable: load a ShaderToy Image-pass fragment shader from a local
// file, wrap it, and queue it for compile on the backend "A" custom-shader
// program slot. Tolerant of failure (NFR-3): logs and leaves the ShaderToy
// layer inactive rather than crashing. The actual GL compile + its ok/failed
// log ride the SAME RB_GetMilkWarpCompileStatus poll the .milk warp path uses
// (Frame() above); a shader with GLSL errors makes EnqueueVisMilkWarpDraw
// no-op (s_milkWarpProgId stays 0), leaving just the feedback trail.
static bool VisLoadShaderToyFile( const char * path ) {
    // mutually exclusive with a .milk warp shader -- see VisLoadMilkPreset.
    VisUnloadMilkPreset();
    VisUnloadShaderToy();

    void * buffer = NULL;
    const int len = fileSystem->ReadFile( path, &buffer, NULL );
    if ( len <= 0 || buffer == NULL ) {
        idLib::Warning( "vis_shadertoyLoad: could not read '%s' (path is relative to a registered search path, like vis_milkPreset)", path );
        return false;
    }
    idStr raw( (const char *)buffer );
    fileSystem->FreeFile( buffer );

    // Strip any pasted "#version ..." directive: the backend compiles this as
    // GLSL 1.10 and does not prepend a version, so a ShaderToy-editor
    // "#version 300 es" line would only conflict. (Genuine ES3-only features
    // stay a documented gap, not something a stripped directive can rescue.)
    idStr body;
    const char * p = raw.c_str();
    const char * lineStart = p;
    for ( ; ; p++ ) {
        if ( *p != '\n' && *p != '\0' ) {
            continue;
        }
        idStr line( lineStart );
        line.CapLength( (int)( p - lineStart ) );
        idStr trimmed = line;
        trimmed.StripLeading( ' ' );
        trimmed.StripLeading( '\t' );
        if ( idStr::Icmpn( trimmed.c_str(), "#version", 8 ) != 0 ) {
            body += line;
            body += "\n";
        }
        if ( *p == '\0' ) {
            break;
        }
        lineStart = p + 1;
    }

    if ( body.Find( "mainImage" ) == -1 ) {
        idLib::Warning( "vis_shadertoyLoad: '%s' has no mainImage() -- not a ShaderToy Image-pass shader?", path );
        return false;
    }

    // reuse the SAME full-screen-quad vertex passthrough the .milk warp path
    // uses (varying xlv_TEXCOORD0, position untouched) -- transpiled once.
    if ( !s_milkVertexGLSLReady ) {
        idStr vertexErr;
        s_milkVertexGLSLReady = idMilkShaderTranspiler::TranspileVertexPassthrough( s_milkVertexGLSL, vertexErr );
        if ( !s_milkVertexGLSLReady ) {
            idLib::Warning( "vis_shadertoyLoad: vertex passthrough transpile failed: %s", vertexErr.c_str() );
            return false;
        }
    }

    s_shadertoyPendingVertexGLSL = s_milkVertexGLSL;
    s_shadertoyPendingFragGLSL = VisBuildShaderToyFragGLSL( body );
    s_shadertoyPendingCompile = true;
    s_shadertoyActive = true;
    s_shadertoyName = path;
    idLib::Printf( "vis_shadertoyLoad: '%s' wrapped + queued for compile (watch console for 'milk custom warp shader compiled+linked ok' or a failure line)\n", path );
    return true;
}

// Draw-worker thread: issue the one-time backend compile (from THIS context,
// per the same fix documented on VisLoadMilkPreset's transpile block) then the
// full-screen custom-shader draw. Reuses the .milk warp draw entirely; only
// difference from DrawMilkWarpMesh is there is no CPU-mesh fallback (a
// ShaderToy layer is shader-only by definition).
static void DrawShaderToyLayer() {
    if ( s_shadertoyPendingCompile ) {
        renderSystem->EnqueueVisMilkShaderCompile( s_shadertoyPendingVertexGLSL.c_str(), s_shadertoyPendingFragGLSL.c_str(), NULL, false );
        s_shadertoyPendingCompile = false;
    }
    if ( s_visFeedbackActive == NULL || s_visFeedbackActiveImageName == NULL ) {
        return;
    }
    // s_milkWarpUniforms carries time/frame/fps (populated by
    // VisUpdateShaderToyFrame); the backend binds the feedback image to
    // sampler_main (iChannel0) and computes texsize itself. If the last
    // compile failed, this call safely no-ops in the backend.
    renderSystem->EnqueueVisMilkWarpDraw( s_visFeedbackActiveImageName, s_milkWarpUniforms );
}

// Main thread: refresh the ShaderToy layer's per-frame uniforms. Reuses
// s_milkWarpUniforms (milk + ShaderToy are mutually exclusive, so the struct
// is never contended) since EnqueueVisMilkWarpDraw reads exactly that struct.
static void VisUpdateShaderToyFrame( float dtSec ) {
    if ( !s_shadertoyActive ) {
        return;
    }
    static int   s_shadertoyFrameCounter = 0;
    static float s_shadertoyElapsedSec = 0.0f;
    s_shadertoyFrameCounter++;
    s_shadertoyElapsedSec += dtSec;
    s_milkWarpUniforms.time  = s_shadertoyElapsedSec;
    s_milkWarpUniforms.frame = (float)s_shadertoyFrameCounter;
    s_milkWarpUniforms.fps   = ( dtSec > 0.0001f ) ? ( 1.0f / dtSec ) : 60.0f;
    // TODO(FR-C1-EXPAND): once the modulation/routing system lands (owned by a
    // parallel agent), expose iTime scaling / a synthetic iMouse as routable
    // targets here. Not wired now -- do not touch VisRegisterModTarget/vis_route.
    s_milkWarpUniforms.bass    = g_audioAnalyzer.GetBass();
    s_milkWarpUniforms.mid     = g_audioAnalyzer.GetMid();
    s_milkWarpUniforms.treb    = g_audioAnalyzer.GetTreb();
    s_milkWarpUniforms.bassAtt = g_audioAnalyzer.GetBassAtt();
    s_milkWarpUniforms.midAtt  = g_audioAnalyzer.GetMidAtt();
    s_milkWarpUniforms.trebAtt = g_audioAnalyzer.GetTrebAtt();
    s_milkWarpUniforms.aspectX = ( VIS_W > VIS_H ) ? ( VIS_H / VIS_W ) : 1.0f;
    s_milkWarpUniforms.aspectY = ( VIS_H > VIS_W ) ? ( VIS_W / VIS_H ) : 1.0f;
}

// Draw-worker thread: emit the warp mesh using s_milkTex[] computed above
// on the main thread this same frame. Mirrors DrawFeedbackWarpMesh's
// position-grid + quad-emission exactly; only the per-vertex UV source
// differs (cached MilkDrop output vs. inline hardcoded math).
static void DrawMilkWarpMesh( float dim ) {
    // PRD M3: issue the one-time backend shader compile from THIS thread/
    // context (draw-worker, per-frame render-command) -- see the fix
    // comment above VisLoadMilkPreset's transpile block. Checked before any
    // early return below so a preset load right before ping-pong/feedback
    // isn't ready yet (s_visFeedbackActive still NULL) doesn't miss this.
    if ( s_milkWarpPendingCompile ) {
        renderSystem->EnqueueVisMilkShaderCompile( s_milkWarpPendingVertexGLSL.c_str(), s_milkWarpPendingFragGLSL.c_str(),
            s_milkWarpPendingCompFragGLSL.IsEmpty() ? NULL : s_milkWarpPendingCompFragGLSL.c_str(), false );
        s_milkWarpPendingCompile = false;
    }
    // PRD M5: B's own pending compile, same draw-worker-thread rule as A's
    // just above, routed to the backend's separate B-slot programs.
    if ( s_milkWarpBPendingCompile ) {
        renderSystem->EnqueueVisMilkShaderCompile( s_milkWarpBPendingVertexGLSL.c_str(), s_milkWarpBPendingFragGLSL.c_str(),
            s_milkWarpBPendingCompFragGLSL.IsEmpty() ? NULL : s_milkWarpBPendingCompFragGLSL.c_str(), true );
        s_milkWarpBPendingCompile = false;
    }

    if ( s_visFeedbackActive == NULL ) {
        return;
    }

    // PRD M3: the preset's own custom warp_ shader takes over entirely when
    // one transpiled+compiled successfully -- a full-screen quad computing
    // per-pixel warp in the shader itself, instead of the CPU-computed
    // coarse mesh below. dim (the engine's feedback-decay multiplier the
    // CPU-mesh path applies via vertex color) is deliberately NOT forced
    // onto this path's output: a custom shader already controls its own
    // old-vs-new blend via its own per_frame_ variables and GetPixel(uv)
    // sampling (real MilkDrop semantics), so an extra multiply here would
    // just double-decay real presets that already do this themselves.
    if ( s_milkWarpShaderReady && s_visFeedbackActiveImageName != NULL ) {
        renderSystem->EnqueueVisMilkWarpDraw( s_visFeedbackActiveImageName, s_milkWarpUniforms );
        // PRD M5: mash-up "B" side -- its OWN compiled warp_/comp_ shader,
        // alpha-blended on top at s_milkMashMix (the same "draw A opaque,
        // draw B translucent on top" compositing the CPU-mesh mash-up path
        // below already uses). Two arbitrary custom shaders don't need a
        // more sophisticated blend rule than plain alpha compositing -- each
        // renders its own full pass, standard blending composites them.
        if ( s_milkActiveB && s_milkWarpBShaderReady && s_milkMashMix > 0.01f && s_visFeedbackActiveImageName != NULL ) {
            renderSystem->EnqueueVisMilkMashDraw( s_visFeedbackActiveImageName, s_milkWarpUniformsB, s_milkMashMix );
        }
        return;
    }

    const int gcols = MILK_DEFAULT_GX;
    const int grows = MILK_DEFAULT_GY;
    const int NX = gcols + 1;
    const int NY = grows + 1;
    static idVec2 pos[ ( MILK_DEFAULT_GX + 1 ) * ( MILK_DEFAULT_GY + 1 ) ];
    static bool posInit = false;
    if ( !posInit ) {
        posInit = true;
        for ( int j = 0; j < NY; j++ ) {
            for ( int i = 0; i < NX; i++ ) {
                pos[j * NX + i].Set( ( (float)i / (float)gcols ) * VIS_W, ( (float)j / (float)grows ) * VIS_H );
            }
        }
    }

    renderSystem->SetColor( idVec4( dim, dim, dim, 1.0f ) );
    for ( int j = 0; j < grows; j++ ) {
        for ( int i = 0; i < gcols; i++ ) {
            const int tl = j * NX + i;
            const int tr = tl + 1;
            const int bl = tl + NX;
            const int br = bl + 1;
            renderSystem->DrawStretchPic(
                idVec4( pos[tl].x, pos[tl].y, s_milkTex[tl].x, s_milkTex[tl].y ),
                idVec4( pos[tr].x, pos[tr].y, s_milkTex[tr].x, s_milkTex[tr].y ),
                idVec4( pos[br].x, pos[br].y, s_milkTex[br].x, s_milkTex[br].y ),
                idVec4( pos[bl].x, pos[bl].y, s_milkTex[bl].x, s_milkTex[bl].y ),
                s_visFeedbackActive );
        }
    }

    // PRD M5: mash-up "B" side, drawn on top at s_milkMashMix alpha -- the
    // same alpha-blend mechanism DrawMilkVideoEcho already proves works on
    // this material (a translucent second copy composited over the first).
    // Reuses the identical position grid since both meshes share the same
    // screen-space layout; only the sampled UVs (s_milkTexB vs s_milkTex)
    // differ.
    if ( s_milkActiveB && s_milkMashMix > 0.01f ) {
        const float mix = idMath::ClampFloat( 0.0f, 1.0f, s_milkMashMix );
        renderSystem->SetColor( idVec4( dim, dim, dim, mix ) );
        for ( int j = 0; j < grows; j++ ) {
            for ( int i = 0; i < gcols; i++ ) {
                const int tl = j * NX + i;
                const int tr = tl + 1;
                const int bl = tl + NX;
                const int br = bl + 1;
                renderSystem->DrawStretchPic(
                    idVec4( pos[tl].x, pos[tl].y, s_milkTexB[tl].x, s_milkTexB[tl].y ),
                    idVec4( pos[tr].x, pos[tr].y, s_milkTexB[tr].x, s_milkTexB[tr].y ),
                    idVec4( pos[br].x, pos[br].y, s_milkTexB[br].x, s_milkTexB[br].y ),
                    idVec4( pos[bl].x, pos[bl].y, s_milkTexB[bl].x, s_milkTexB[bl].y ),
                    s_visFeedbackActive );
            }
        }
    }
}

/*
========================
PRD M1 step 5: MilkDrop preset's built-in waveform + video-echo composite

Both driven purely by the preset's own SCALAR params (idMilkPreset::GetParams,
already parsed since step 2) -- no new evaluator work needed, since these
aren't per_frame_/per_pixel_ equation outputs, just constants the preset sets
directly (wave_r/g/b/a/x/y, fWaveScale, fWaveAlpha, fVideoEchoZoom/Alpha,
nVideoEchoOrientation).

Deliberately NOT implemented here, and documented rather than silently
skipped: custom "wavecode" (per-wave per_point_ equation-driven waveforms,
idMilkPreset::GetWave) and "shapecode" (idMilkPreset::GetShape) -- both need
a per-sample/per-shape evaluation loop (an evaluator execution per audio
sample point, not per screen vertex), which is real additional scope beyond
this step's "use what's already parsed as scalars" increment. Presets that
lean on custom wavecode for their waveform will show nothing extra here
(falling back to whatever the warp mesh alone produces), not a crash.
========================
*/
static void DrawMilkWaveform() {
    if ( !s_milkActive || s_visSolid == NULL ) {
        return;
    }
    // read the cache populated in VisLoadMilkPreset (main thread), NOT
    // s_milkPreset.GetParams() directly -- idDict isn't thread-safe and
    // this function runs on the draw-worker thread.
    const milkWaveParams_t & wp = s_milkWaveParams;
    const float finalAlpha = idMath::ClampFloat( 0.0f, 1.0f, wp.a * wp.alphaMul );
    if ( finalAlpha < 0.01f ) {
        return;   // preset explicitly hid the waveform (e.g. wave_a=0, as several real fixtures do)
    }
    const float cx = wp.cx;
    const float cy = wp.cy;

    const float * wave = g_audioAnalyzer.GetWaveform();
    const float amp = VIS_H * 0.35f * wp.scale;
    const idVec4 color( wp.r, wp.g, wp.b, finalAlpha );
    const int step = FFT_SIZE / 320;   // ~320 points across the width, matching DrawEffectScope
    const float left = cx - VIS_W * 0.5f;
    float prevX = left;
    float prevY = cy - idMath::ClampFloat( -1.0f, 1.0f, wave[0] ) * amp;
    for ( int s = step; s < FFT_SIZE; s += step ) {
        const float x = left + VIS_W * (float)s / (float)FFT_SIZE;
        const float y = cy - idMath::ClampFloat( -1.0f, 1.0f, wave[s] ) * amp;
        VisLine( color, prevX, prevY, x, y, 2.0f );
        prevX = x;
        prevY = y;
    }
}

static void DrawMilkVideoEcho() {
    if ( !s_milkActive || s_visFeedbackActive == NULL ) {
        return;
    }
    // read the cache populated in VisLoadMilkPreset (main thread) -- see the
    // thread-safety note above DrawMilkWaveform.
    const float alpha = s_milkEchoParams.alpha;
    if ( alpha < 0.01f ) {
        return;   // most presets don't use video echo -- the common case, cheap to skip
    }
    const float zoom = s_milkEchoParams.zoom;
    const int orient = s_milkEchoParams.orient;   // 0 none,1 flip-x,2 flip-y,3 both

    const float cx = VIS_W * 0.5f;
    const float cy = VIS_H * 0.5f;
    const float hw = cx * zoom;
    const float hh = cy * zoom;
    // texcoord s/t per corner (TL,TR,BR,BL), flipped per nVideoEchoOrientation
    float sc[4] = { 0.0f, 1.0f, 1.0f, 0.0f };
    float tc[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    if ( orient == 1 || orient == 3 ) { float t; t=sc[0]; sc[0]=sc[1]; sc[1]=t; t=sc[3]; sc[3]=sc[2]; sc[2]=t; }
    if ( orient == 2 || orient == 3 ) { float t; t=tc[0]; tc[0]=tc[3]; tc[3]=t; t=tc[1]; tc[1]=tc[2]; tc[2]=t; }

    renderSystem->SetColor( idVec4( 1.0f, 1.0f, 1.0f, alpha ) );
    renderSystem->DrawStretchPic(
        idVec4( cx - hw, cy - hh, sc[0], tc[0] ), idVec4( cx + hw, cy - hh, sc[1], tc[1] ),
        idVec4( cx + hw, cy + hh, sc[2], tc[2] ), idVec4( cx - hw, cy + hh, sc[3], tc[3] ),
        s_visFeedbackActive );
}

// PRD M5: draw each active wave's cached per-point positions (computed on
// the main thread in VisUpdateMilkWaves) as connected line segments, or
// dots if the preset set bUseDots. Honest facsimile, stated plainly: real
// MilkDrop interpolates color smoothly per-vertex in its own renderer; this
// draws each segment/dot in its LEADING point's color rather than a true
// per-vertex gradient -- close enough for a custom-wave facsimile, not exact
// parity.
static void DrawMilkWavecode() {
    if ( !s_milkActive || s_visSolid == NULL ) {
        return;
    }
    for ( int w = 0; w < s_milkNumWaveRt; w++ ) {
        const int n = s_milkWavePointCount[w];
        if ( n < 1 ) {
            continue;
        }
        const milkWaveRuntime_t & wr = s_milkWaveRt[w];
        const idMaterial * mat = ( wr.additive && s_visSolidAdd != NULL ) ? s_visSolidAdd : s_visSolid;
        const milkWavePoint_t * pts = s_milkWavePoints[w];

        if ( wr.useDots ) {
            const float sz = 3.0f;
            for ( int i = 0; i < n; i++ ) {
                const milkWavePoint_t & p = pts[i];
                renderSystem->SetColor( idVec4( p.r, p.g, p.b, p.a ) );
                renderSystem->DrawStretchPic( p.x - sz * 0.5f, p.y - sz * 0.5f, sz, sz, 0.0f, 0.0f, 1.0f, 1.0f, mat );
            }
        } else {
            for ( int i = 0; i < n - 1; i++ ) {
                const milkWavePoint_t & a = pts[i];
                const milkWavePoint_t & b = pts[i + 1];
                const float dx = b.x - a.x;
                const float dy = b.y - a.y;
                const float len = idMath::Sqrt( dx * dx + dy * dy );
                if ( len < 0.0001f ) {
                    continue;
                }
                const float thick = 2.0f;
                const float px = -dy / len * thick * 0.5f;
                const float py =  dx / len * thick * 0.5f;
                renderSystem->SetColor( idVec4( a.r, a.g, a.b, a.a ) );
                renderSystem->DrawStretchPic(
                    idVec4( a.x + px, a.y + py, 0.0f, 0.0f ), idVec4( b.x + px, b.y + py, 1.0f, 0.0f ),
                    idVec4( b.x - px, b.y - py, 1.0f, 1.0f ), idVec4( a.x - px, a.y - py, 0.0f, 1.0f ), mat );
            }
        }
    }
}

// PRD M5: draw each active shape's cached polygon (computed on the main
// thread in VisUpdateMilkShapes) as a triangle fan from its center.
// Honest facsimile, stated plainly: real MilkDrop can interpolate a radial
// gradient between the inner (r/g/b/a) and outer (r2/g2/b2/a2) colors; this
// draws a single solid fill (r/g/b/a) instead, since idRenderSystem's 2D
// draw calls set one color for the whole call, not per-vertex -- r2/g2/b2/a2
// are still parsed/evaluated (see VisLoadMilkShapes/VisUpdateMilkShapes) so
// a true gradient is a small follow-up, not a re-plumb, once/if per-vertex
// color becomes available. "textured" shapes (sampling a preset-supplied
// image) are also not attempted here -- always drawn as a solid fill.
static void DrawMilkShapecode() {
    if ( !s_milkActive || s_visSolid == NULL ) {
        return;
    }
    for ( int i = 0; i < s_milkNumShapeRt; i++ ) {
        const milkShapeCache_t & cache = s_milkShapeCache[i];
        if ( !cache.enabled || cache.rad < 0.001f ) {
            continue;
        }
        const idMaterial * mat = ( cache.additive && s_visSolidAdd != NULL ) ? s_visSolidAdd : s_visSolid;
        renderSystem->SetColor( idVec4( cache.r, cache.g, cache.b, cache.a ) );

        const int n = cache.sides;
        idVec2 prev( cache.cx + cache.rad * idMath::Cos( cache.ang ), cache.cy + cache.rad * idMath::Sin( cache.ang ) );
        for ( int s = 1; s <= n; s++ ) {
            const float ang2 = cache.ang + idMath::TWO_PI * (float)s / (float)n;
            const idVec2 cur( cache.cx + cache.rad * idMath::Cos( ang2 ), cache.cy + cache.rad * idMath::Sin( ang2 ) );
            renderSystem->DrawStretchTri(
                idVec2( cache.cx, cache.cy ), prev, cur,
                idVec2( 0.0f, 0.0f ), idVec2( 0.0f, 0.0f ), idVec2( 0.0f, 0.0f ), mat );
            if ( cache.thickOutline ) {
                VisLine( idVec4( cache.r, cache.g, cache.b, cache.a ), prev.x, prev.y, cur.x, cur.y, 2.0f );
            }
            prev = cur;
        }
    }
}

// Fixed real bug: base/materials/visualizer_boot.mtr's "visualizer/feedbackA"/
// "feedbackB" materials reference "map visualizer/feedbackrtA"/"feedbackrtB",
// and a fragmentMap stage references "visualizer/feedbackrt" directly -- all
// three are purely runtime targets (two filled in by BeginOffscreenRender
// each frame, one by CaptureRenderToImage), never meant to exist as files.
// Confirmed verbatim by the user: "Couldn't load image: visualizer/feedbackrtA
// : visualizer/feedbackrtA#__0200", flooding every frame.
//
// Root cause, nailed down via pointer-address diagnostic logging on a live
// build (three earlier attempts fixed real-but-partial bugs without curing
// the flood): idImageManager::GetImageWithParameters -- what a material's
// "map"/"fragmentMap" stage actually resolves an image name through, see
// ImageManager.cpp -- only reuses an existing image by name if its
// filter/repeat/usage ALSO match what the stage is requesting; on a
// mismatch it skips that entry and creates a brand new, independent
// idImage via a raw AllocImage call, with no generatorFunction. This
// generator originally called GenerateImage with TF_LINEAR/TR_CLAMP
// (reasonable for a render target sampled bilinear+clamped), but the
// "map"/"fragmentMap" stages here have no clamp/nearest keyword, so they
// request idImage's engine defaults instead -- TF_DEFAULT/TR_REPEAT. That
// mismatch meant every material parse of these stages created a fresh,
// generator-less duplicate image under the identical name, which Bind()
// then tried (and failed) to load from disk, every time. Matching the
// generator's filter/repeat/usage to the engine defaults these stages
// actually ask for makes GetImageWithParameters find and reuse THIS image
// instead of duplicating it. BeginOffscreenRender still unconditionally
// re-derives the real filter (TF_LINEAR/TR_CLAMP) via its own per-frame
// image->AllocImage(opts, TF_LINEAR, TR_CLAMP) call regardless of what's
// set here, so actual rendering behavior is unaffected -- this only
// changes what the FIRST registration looks like to other lookups.
//
// (Earlier, now-superseded fix attempts: a bare globalImages->AllocImage
// pre-registration left IsLoaded() false; registering via
// ImageFromFunction from idVisualizerManager's constructor created its own
// orphaned object since idImageManager::GetImageWithParameters's mismatch
// path always wins regardless of registration timing; ImageFromFunction's
// "image already exists" branch not attaching a missing generator was a
// real bug too, fixed separately in ImageManager.cpp, but insufficient on
// its own since a filter/repeat mismatch means that branch never gets a
// chance to find this image in the first place.)
static void VisFeedbackRTGenerator( idImage * image ) {
    byte data[8][8][4];
    memset( data, 0, sizeof( data ) );
    image->GenerateImage( (byte *)data, 8, 8, TF_DEFAULT, TR_REPEAT, TD_DEFAULT );
}

idVisualizerManager::idVisualizerManager() :
    m_playing( false ),
    m_trackEndTime( 0 ),
    m_sourceArmed( false ),
    m_nextArmMs( 0 ),
    m_nextDeviceCheckMs( 0 ),
    m_menuOpen( false ),
    m_menuTab( 0 ),
    m_menuSel( 0 ),
    m_menuScroll( 0 ),
    m_cycleActive( false ),
    m_cycleIndex( 0 ),
    m_cycleNextMs( 0 ),
    m_cyclePending( false ) {
    VisInitModTargets();   // registry must be populated before any Draw2D/Frame call
}

/*
========================
idVisualizerManager::TrackPathToShaderName

PlayShaderDirectly resolves names through the decl system: plain names get
".wav" appended by the implicit shader generator, while names containing
".ogg" are kept as-is. So strip a ".wav" extension but keep ".ogg".
========================
*/
void idVisualizerManager::TrackPathToShaderName( const char * path, idStr & out ) {
    out = path;
    out.BackSlashesToSlashes();
    idStr ext;
    out.ExtractFileExtension( ext );
    if ( ext.Icmp( "wav" ) == 0 || ext.Icmp( "msadpcm" ) == 0 ) {
        out.StripFileExtension();
    }
}

void idVisualizerManager::StartCurrentTrack() {
    const char * track = m_playlist.GetCurrentTrack();
    if ( track == NULL ) {
        m_playing = false;
        m_nowPlaying.Clear();
        return;
    }

    idStr shaderName;
    TrackPathToShaderName( track, shaderName );

    if ( soundSystemLocal.currentSoundWorld == NULL ) {
        idLib::Warning( "visualizer: no active sound world" );
        return;
    }

    const int lengthMS = soundSystemLocal.currentSoundWorld->PlayShaderDirectly( shaderName.c_str() );
    m_nowPlaying = shaderName;
    m_playing = ( lengthMS != 0 );
    m_trackEndTime = ( lengthMS > 0 ) ? Sys_Milliseconds() + lengthMS : 0;

    if ( m_playing ) {
        idLib::Printf( "visualizer: playing %s (%d.%01ds)\n", shaderName.c_str(), lengthMS / 1000, ( lengthMS % 1000 ) / 100 );
        idStr disp = shaderName;
        disp.StripPath();
        disp.StripFileExtension();
        VisSetBanner( disp.c_str() );
    } else {
        idLib::Warning( "visualizer: failed to start %s", shaderName.c_str() );
    }
}

void idVisualizerManager::Play( const char * path ) {
    if ( path == NULL || path[0] == '\0' ) {
        return;
    }
    if ( m_playlist.LoadPlaylist( path ) <= 0 ) {
        idLib::Warning( "visualizer: nothing to play in '%s'", path );
        return;
    }
    StartCurrentTrack();
}

void idVisualizerManager::Stop() {
    if ( soundSystemLocal.currentSoundWorld != NULL ) {
        // PlayShaderDirectly(NULL) stops the local sound channel
        soundSystemLocal.currentSoundWorld->PlayShaderDirectly( NULL );
    }
    m_playing = false;
    m_trackEndTime = 0;
    m_nowPlaying.Clear();
}

void idVisualizerManager::Next() {
    if ( m_playlist.GetNumTracks() == 0 ) {
        return;
    }
    m_playlist.PlayNext();
    StartCurrentTrack();
}

void idVisualizerManager::Prev() {
    if ( m_playlist.GetNumTracks() == 0 ) {
        return;
    }
    m_playlist.PlayPrev();
    StartCurrentTrack();
}

void idVisualizerManager::SetSourceMode( AudioSourceMode mode ) {
    g_audioAnalyzer.SetSourceMode( mode );
}

/*
========================
idVisualizerManager::Draw2D

Immediate-mode spectrum bars in 640x480 virtual coords, drawn from
idCommonLocal::Draw() every frame (works with no map loaded). One log-spaced
band per bar, height = auto-gained level, rainbow across the spectrum, with a
bass-driven background pulse.
========================
*/

// Effect 0: classic spectrum bars rising from a baseline.
static void DrawEffectBars( int n ) {
    const float baseY = VIS_H - 20.0f + s_curOffY;
    const float maxBarH = VIS_H * 0.75f;
    const float slotW = VIS_W / (float)n;
    const float gap = ( n <= 24 ) ? 2.0f : 1.0f;
    const float scale = VisMod(VMT_SCALE);
    const float hueShift = VisMod(VMT_HUE);
    const float alpha = 0.9f * VisMod(VMT_BRIGHT);
    for ( int i = 0; i < n; i++ ) {
        const float level = idMath::ClampFloat( 0.0f, 1.2f, g_audioAnalyzer.GetBandLevel( i ) * scale );
        const float barH = idMath::ClampFloat( 0.0f, maxBarH, level * maxBarH );
        const float x = i * slotW + gap * 0.5f + s_curOffX;
        const float w = slotW - gap;
        const float hue = ( ( n > 1 ) ? ( 0.66f * (float)i / (float)( n - 1 ) ) : 0.0f ) + hueShift;
        VisFillRect( HueColor( hue, alpha ), x, baseY - barH, w, barH );
    }
    VisFillRect( idVec4( 0.3f, 0.3f, 0.3f, 0.6f ), s_curOffX, baseY, VIS_W, 2.0f );
}

// Effect 1: radial spectrum - bands as spokes around a center ring (Polar-style).
static void DrawEffectRadial( int n ) {
    const float cx = VIS_W * 0.5f + s_curOffX;
    const float cy = VIS_H * 0.5f + s_curOffY;
    const float r0 = 40.0f;                        // inner ring radius
    const float rMax = 150.0f;                     // max spoke length
    const float beat = idMath::ClampFloat( 0.0f, 1.0f, g_audioAnalyzer.GetBass() * 0.5f );
    const float ring = r0 * ( 1.0f + beat * 0.3f ); // ring pulses with bass
    const float scale = VisMod(VMT_SCALE);
    const float hueShift = VisMod(VMT_HUE);
    const float alpha = 0.9f * VisMod(VMT_BRIGHT);
    for ( int i = 0; i < n; i++ ) {
        const float level = idMath::ClampFloat( 0.0f, 1.2f, g_audioAnalyzer.GetBandLevel( i ) * scale );
        const float len = level * rMax;
        const float ang = idMath::TWO_PI * (float)i / (float)n - idMath::HALF_PI + s_rotAngle;
        const float halfW = ( idMath::PI / (float)n ) * 0.7f;
        const float aL = ang - halfW;
        const float aR = ang + halfW;
        const float rOut = ring + len;
        const float ilx = cx + ring * idMath::Cos( aL ) * s_aspectFix, ily = cy + ring * idMath::Sin( aL );
        const float irx = cx + ring * idMath::Cos( aR ) * s_aspectFix, iry = cy + ring * idMath::Sin( aR );
        const float olx = cx + rOut * idMath::Cos( aL ) * s_aspectFix, oly = cy + rOut * idMath::Sin( aL );
        const float orx = cx + rOut * idMath::Cos( aR ) * s_aspectFix, ory = cy + rOut * idMath::Sin( aR );
        const float hue = ( ( n > 1 ) ? ( (float)i / (float)n ) : 0.0f ) + hueShift;
        VisFillQuad( HueColor( hue, alpha ), olx, oly, orx, ory, irx, iry, ilx, ily );
    }
}

// Effect 2: oscilloscope - the raw waveform drawn as a connected line.
static void DrawEffectScope() {
    const float * wave = g_audioAnalyzer.GetWaveform();
    const float cy = VIS_H * 0.5f + s_curOffY;
    const float amp = VIS_H * 0.42f * VisMod(VMT_SCALE);
    const float hueShift = VisMod(VMT_HUE);
    const float alpha = 0.95f * VisMod(VMT_BRIGHT);
    const int step = FFT_SIZE / 320;               // ~320 points across the screen
    float prevX = 0.0f;
    float prevY = cy - idMath::ClampFloat( -1.0f, 1.0f, wave[0] ) * amp;
    for ( int s = step; s < FFT_SIZE; s += step ) {
        const float x = VIS_W * (float)s / (float)FFT_SIZE + s_curOffX;
        const float y = cy - idMath::ClampFloat( -1.0f, 1.0f, wave[s] ) * amp;
        const float hue = 0.33f + 0.33f * ( wave[s] * 0.5f + 0.5f ) + hueShift; // green->cyan by level
        VisLine( HueColor( hue, alpha ), prevX, prevY, x, y, 2.5f );
        prevX = x;
        prevY = y;
    }
}

// Effect 3: waveform wrapped into a ring - the scope bent into a circle, spinning.
static void DrawEffectRing() {
    const float * wave = g_audioAnalyzer.GetWaveform();
    const float cx = VIS_W * 0.5f + s_curOffX;
    const float cy = VIS_H * 0.5f + s_curOffY;
    const float baseR = 110.0f;
    const float amp = 90.0f * VisMod(VMT_SCALE);
    const float hueShift = VisMod(VMT_HUE);
    const float alpha = 0.95f * VisMod(VMT_BRIGHT);
    const int N = 180;                     // points around the circle
    const int step = FFT_SIZE / N;
    float px = 0.0f, py = 0.0f;
    for ( int i = 0; i <= N; i++ ) {
        const int s = ( i % N ) * step;
        const float w = idMath::ClampFloat( -1.0f, 1.0f, wave[s] );
        const float ang = idMath::TWO_PI * (float)i / (float)N + s_rotAngle;
        const float r = baseR + w * amp;
        const float x = cx + r * idMath::Cos( ang ) * s_aspectFix;
        const float y = cy + r * idMath::Sin( ang );
        if ( i > 0 ) {
            const float hue = (float)i / (float)N + hueShift;
            VisLine( HueColor( hue, alpha ), px, py, x, y, 2.5f );
        }
        px = x;
        py = y;
    }
}

// Effect 4: particles - dots spawned on each beat, pushed outward by band energy,
// faded over their life. Persistent state; updated once per frame in Draw2D.
struct VisParticle { float x, y, vx, vy, life, maxLife, hue; };
static const int VIS_MAX_PARTICLES = 300;
static VisParticle s_particles[VIS_MAX_PARTICLES];
static int s_partLastMs = 0;
static unsigned int s_partRng = 2463534242u;
static float VisRand01() {
    s_partRng = s_partRng * 1664525u + 1013904223u;
    return (float)( ( s_partRng >> 8 ) & 0xFFFFFF ) / (float)0x1000000;
}

// per-instance particle pool for the real add/remove layer stack (see
// visStackLayer_t, defined further below once its sibling star/spectrogram
// state structs are also in scope).
struct visStackParticleState_t {
    VisParticle particles[VIS_MAX_PARTICLES];
    int lastMs;
};

// PRD Pillar C follow-up (real add/remove layer stack): an independent stack
// slot running Particles needs its OWN particle pool + spawn timer instead of
// sharing this single global one (else two simultaneous Particle layers would
// corrupt/double-advance each other's dots). extState==NULL (every existing
// caller -- the legacy single vis_effect path) keeps using the original
// module-static pool byte-for-byte; VisRenderLayerStack is the only caller
// that ever passes a real pointer. VisRand01()/s_partRng stay a single shared
// stream across all instances -- harmless (just correlates their randomness
// slightly), not worth per-instance RNG state too.
static void DrawEffectParticles( visStackParticleState_t * extState = NULL, int slot = -1 ) {
    VisParticle * particles = extState ? extState->particles : s_particles;
    int & lastMs = extState ? extState->lastMs : s_partLastMs;
    // PRD FR-C1-EXPAND per-slot routing (1.0 / no-op on the legacy path).
    const float spawnMul = VisSlotMod( slot, VSP_PARTICLE_SPAWN );
    const float scaleMul = VisSlotMod( slot, VSP_PARTICLE_SCALE );

    const int nowMs = Sys_Milliseconds();
    float dt = ( lastMs > 0 ) ? ( nowMs - lastMs ) * 0.001f : 0.0f;
    dt = idMath::ClampFloat( 0.0f, 0.1f, dt );
    lastMs = nowMs;

    const float cx = VIS_W * 0.5f + s_curOffX;
    const float cy = VIS_H * 0.5f + s_curOffY;
    const float hueShift = VisMod(VMT_HUE);
    const float brightness = VisMod(VMT_BRIGHT);

    // spawn a burst on each beat; speed scales with bass
    if ( g_audioAnalyzer.GetBeat() ) {
        const float speed = 120.0f + 240.0f * idMath::ClampFloat( 0.0f, 2.0f, g_audioAnalyzer.GetBassAtt() );
        const int spawnCap = idMath::ClampInt( 0, VIS_MAX_PARTICLES, (int)( 40.0f * spawnMul ) );
        int spawned = 0;
        for ( int i = 0; i < VIS_MAX_PARTICLES && spawned < spawnCap; i++ ) {
            if ( particles[i].life > 0.0f ) {
                continue;
            }
            const float ang = VisRand01() * idMath::TWO_PI;
            const float sp = speed * ( 0.5f + VisRand01() );
            particles[i].x = cx;
            particles[i].y = cy;
            particles[i].vx = idMath::Cos( ang ) * sp;
            particles[i].vy = idMath::Sin( ang ) * sp;
            particles[i].maxLife = 0.8f + VisRand01() * 0.8f;
            particles[i].life = particles[i].maxLife;
            particles[i].hue = VisRand01();
            spawned++;
        }
    }

    // integrate + draw
    for ( int i = 0; i < VIS_MAX_PARTICLES; i++ ) {
        VisParticle & p = particles[i];
        if ( p.life <= 0.0f ) {
            continue;
        }
        p.x += p.vx * dt;
        p.y += p.vy * dt;
        p.vx *= 0.98f;                 // gentle drag
        p.vy *= 0.98f;
        p.life -= dt;
        const float t = ( p.maxLife > 0.0f ) ? ( p.life / p.maxLife ) : 0.0f;
        const float a = idMath::ClampFloat( 0.0f, 1.0f, t ) * brightness;
        const float sz = ( 2.0f + 3.0f * t ) * scaleMul;
        const float dx = cx + ( p.x - cx ) * s_aspectFix;   // keep the burst circular
        VisFillRect( HueColor( p.hue + hueShift, a ), dx - sz * 0.5f, p.y - sz * 0.5f, sz, sz );
    }
}

// Effect 5: spectrogram - a scrolling time (x) vs frequency (y) heatmap. Each
// frame appends one column of log-spaced band energy on the right; older columns
// scroll left. Per-row peak-follower auto-gain keeps it readable at any volume.
static const int SPECTRO_COLS = 80;
static const int SPECTRO_ROWS = 32;
static float s_spectro[SPECTRO_COLS][SPECTRO_ROWS];
static float s_spectroPeak[SPECTRO_ROWS] = { 0.0f };
static int   s_spectroHead = 0;     // column index to write next
static bool  s_spectroInit = false;

// per-instance scrolling history for the real add/remove layer stack (see
// visStackParticleState_t above for why -- shared history would visibly
// tear/duplicate between two simultaneous Spectrogram layers).
struct visStackSpectroState_t {
    float spectro[SPECTRO_COLS][SPECTRO_ROWS];
    float spectroPeak[SPECTRO_ROWS];
    int   spectroHead;
    bool  init;
};

static void DrawEffectSpectrogram( visStackSpectroState_t * extState = NULL, int slot = -1 ) {
    float ( *spectro )[SPECTRO_ROWS] = extState ? extState->spectro : s_spectro;
    float * spectroPeak = extState ? extState->spectroPeak : s_spectroPeak;
    int & spectroHead = extState ? extState->spectroHead : s_spectroHead;
    bool & spectroInit = extState ? extState->init : s_spectroInit;

    const int sr = g_audioAnalyzer.GetSampleRate();
    if ( !spectroInit ) {
        for ( int c = 0; c < SPECTRO_COLS; c++ ) {
            for ( int r = 0; r < SPECTRO_ROWS; r++ ) {
                spectro[c][r] = 0.0f;
            }
        }
        spectroInit = true;
    }

    // build a new column: SPECTRO_ROWS log-spaced bins from ~40 Hz to ~16 kHz
    if ( sr > 0 ) {
        const float loHz = 40.0f;
        const float hiHz = ( sr * 0.45f < 16000.0f ) ? sr * 0.45f : 16000.0f;
        const float logLo = idMath::Log( loHz );
        const float logHi = idMath::Log( hiHz );
        for ( int r = 0; r < SPECTRO_ROWS; r++ ) {
            const float frac = (float)r / (float)( SPECTRO_ROWS - 1 );
            const float hz = idMath::Exp( logLo + ( logHi - logLo ) * frac );
            int bin = (int)( hz * (float)FFT_SIZE / (float)sr );
            if ( bin < 1 ) bin = 1;
            if ( bin > FFT_SIZE / 2 - 1 ) bin = FFT_SIZE / 2 - 1;
            const float mag = g_audioAnalyzer.GetSmoothedMagnitude( bin );
            // per-row auto-gain (slow-decaying peak follower)
            spectroPeak[r] = ( mag > spectroPeak[r] ) ? mag : ( spectroPeak[r] * 0.999f );
            const float norm = ( spectroPeak[r] > 1e-6f ) ? ( mag / spectroPeak[r] ) : 0.0f;
            spectro[spectroHead][r] = idMath::ClampFloat( 0.0f, 1.0f, norm );
        }
        spectroHead = ( spectroHead + 1 ) % SPECTRO_COLS;
    }

    // draw: newest column on the right, low frequencies at the bottom
    const float bright = VisMod(VMT_BRIGHT) * VisSlotMod( slot, VSP_SPECTRO_BRIGHT );
    const float cellW = VIS_W / (float)SPECTRO_COLS;
    const float top = VIS_H * 0.08f + s_curOffY;
    const float cellH = ( VIS_H * 0.86f ) / (float)SPECTRO_ROWS;
    for ( int x = 0; x < SPECTRO_COLS; x++ ) {
        const int col = ( spectroHead - 1 - x + 2 * SPECTRO_COLS ) % SPECTRO_COLS;
        const float px = VIS_W - ( x + 1 ) * cellW + s_curOffX;
        for ( int r = 0; r < SPECTRO_ROWS; r++ ) {
            const float v = spectro[col][r];
            if ( v < 0.02f ) {
                continue;
            }
            const float py = top + ( SPECTRO_ROWS - 1 - r ) * cellH;
            // heat ramp: low intensity -> blue, high -> red
            const float hue = 0.66f - 0.66f * v;
            VisFillRect( HueColor( hue, v * bright ), px, py, cellW + 0.5f, cellH + 0.5f );
        }
    }
}

// Effect 6: starfield / warp tunnel - stars stream outward from the center toward
// the edges, speed scaled by bass, so it reads as flying through space. Each star
// has an angle + radius + speed; each frame the radius advances outward and the
// star respawns at the center with a new random angle when it passes the edge.
// Beats boost the speed and brightness. Persistent state; updated once per frame.
struct VisStar { float ang, rad, prevRad, speed; float hue; bool alive; };
static const int VIS_MAX_STARS = 240;
static VisStar s_stars[VIS_MAX_STARS];
static int s_starLastMs = 0;

// per-instance star field for the real add/remove layer stack (see
// visStackParticleState_t above for the shared-state rationale).
struct visStackStarState_t {
    VisStar stars[VIS_MAX_STARS];
    int lastMs;
};

static void DrawEffectStarfield( visStackStarState_t * extState = NULL, int slot = -1 ) {
    VisStar * stars = extState ? extState->stars : s_stars;
    int & lastMs = extState ? extState->lastMs : s_starLastMs;

    const int nowMs = Sys_Milliseconds();
    float dt = ( lastMs > 0 ) ? ( nowMs - lastMs ) * 0.001f : 0.0f;
    dt = idMath::ClampFloat( 0.0f, 0.1f, dt );
    lastMs = nowMs;

    const float cx = VIS_W * 0.5f + s_curOffX;
    const float cy = VIS_H * 0.5f + s_curOffY;
    const float hueShift = VisMod(VMT_HUE);
    const float brightness = VisMod(VMT_BRIGHT);
    // edge is the travel radius before a star respawns -- derived from the
    // CANVAS half-dimensions (not cx/cy, which now include the position
    // offset), so an off-center starfield still despawns at a sensible
    // canvas-relative distance instead of one that shrinks/grows with position.
    const float edge = idMath::Sqrt( ( VIS_W * 0.5f ) * ( VIS_W * 0.5f ) + ( VIS_H * 0.5f ) * ( VIS_H * 0.5f ) );

    // warp speed rides the bass; a beat kicks it (and the brightness) harder
    const float bass = idMath::ClampFloat( 0.0f, 2.0f, g_audioAnalyzer.GetBassAtt() );
    const bool beat = g_audioAnalyzer.GetBeat();
    const float warp = ( 60.0f + 260.0f * bass + ( beat ? 200.0f : 0.0f ) ) * VisSlotMod( slot, VSP_STAR_SPEED );
    const float beatBoost = beat ? 1.3f : 1.0f;

    for ( int i = 0; i < VIS_MAX_STARS; i++ ) {
        VisStar & st = stars[i];
        if ( !st.alive ) {
            // spawn near the center with a random angle + per-star speed factor
            st.ang = VisRand01() * idMath::TWO_PI;
            st.rad = 2.0f + VisRand01() * 8.0f;
            st.prevRad = st.rad;
            st.speed = 0.5f + VisRand01() * 1.5f;
            st.hue = VisRand01();
            st.alive = true;
        }
        st.prevRad = st.rad;
        st.rad += warp * st.speed * dt;
        if ( st.rad >= edge ) {
            st.alive = false;                 // passed the edge - respawn next frame
            continue;
        }
        // near the edge stars are brighter, faster-looking, and thicker
        const float t = idMath::ClampFloat( 0.0f, 1.0f, st.rad / edge );
        const float ca = idMath::Cos( st.ang );
        const float sa = idMath::Sin( st.ang );
        const float x0 = cx + st.prevRad * ca * s_aspectFix;
        const float y0 = cy + st.prevRad * sa;
        const float x1 = cx + st.rad * ca * s_aspectFix;
        const float y1 = cy + st.rad * sa;
        const float a = idMath::ClampFloat( 0.0f, 1.0f, ( 0.25f + 0.75f * t ) * brightness * beatBoost );
        const float thick = 1.0f + 2.5f * t;
        const float hue = st.hue + t * 0.2f + hueShift;   // shift toward the tunnel color as it flies out
        VisLine( HueColor( hue, a ), x0, y0, x1, y1, thick );
    }
}

// Sound-reactive image layers: each active image is drawn centered, scaled by the
// routed SCALE, spun by the routed rotation, and alpha/hue modulated - ZGE-style.
// Corrected for display aspect (round on wide screens) and per-image aspect (no
// stretch). Multiple layers get alternating spin + nested scale so they're distinct.
// Drawn into the composite before the feedback capture, so they trail/warp too.
// Effect 7: phase scope / Lissajous - plot the waveform against a delayed copy of
// itself (x = wave[i], y = wave[i+delay]), rotating, so it draws flowing analog-
// scope loops that are very different from the linear oscilloscope.
static void DrawEffectPhaseScope() {
    const float * wave = g_audioAnalyzer.GetWaveform();
    const float cx = VIS_W * 0.5f + s_curOffX;
    const float cy = VIS_H * 0.5f + s_curOffY;
    const float amp = VIS_H * 0.4f * idMath::ClampFloat( 0.1f, 3.0f, VisMod(VMT_SCALE) );
    const float hueShift = VisMod(VMT_HUE);
    const float alpha = 0.9f * VisMod(VMT_BRIGHT);
    const int delay = 48;              // samples between the x and y taps
    const int step = 2;
    const float c = idMath::Cos( s_rotAngle );
    const float s = idMath::Sin( s_rotAngle );
    float px = 0.0f, py = 0.0f;
    bool first = true;
    for ( int i = 0; i + delay < FFT_SIZE; i += step ) {
        const float xv = idMath::ClampFloat( -1.0f, 1.0f, wave[i] );
        const float yv = idMath::ClampFloat( -1.0f, 1.0f, wave[i + delay] );
        const float rx = xv * c - yv * s;   // rotate the point
        const float ry = xv * s + yv * c;
        const float x = cx + rx * amp * s_aspectFix;
        const float y = cy + ry * amp;
        if ( !first ) {
            const float hue = 0.5f + 0.5f * rx + hueShift;   // hue by position
            VisLine( HueColor( hue, alpha ), px, py, x, y, 2.0f );
        }
        px = x;
        py = y;
        first = false;
    }
}

static void DrawVisLayer() {
    if ( !vis_layer.GetBool() || s_layerMats.empty() ) {
        return;
    }
    // PRD Pillar C follow-up: image layer position, own dedicated cvars (not
    // s_curOffX/Y -- this isn't part of the single-effect/layer-stack
    // dispatch those are scoped to, it's always its own separate composite).
    const float cx = VIS_W * 0.5f + idMath::ClampFloat( -1.0f, 1.0f, vis_layerX.GetFloat() + VisMod( VMT_LAYERX_MOD ) ) * VIS_W * 0.5f;
    const float cy = VIS_H * 0.5f + idMath::ClampFloat( -1.0f, 1.0f, vis_layerY.GetFloat() + VisMod( VMT_LAYERY_MOD ) ) * VIS_H * 0.5f;
    const float baseAlpha = vis_layerAlpha.GetFloat();
    const bool colorize = vis_layerColorize.GetBool();
    const float sc[4] = { 0.0f, 1.0f, 1.0f, 0.0f };
    const float tc[4] = { 0.0f, 0.0f, 1.0f, 1.0f };

    for ( size_t k = 0; k < s_layerMats.size(); k++ ) {
        const idMaterial * mat = s_layerMats[k];
        if ( mat == NULL ) {
            continue;
        }
        // per-image aspect (width/height); default square if unknown
        const int iw = mat->GetImageWidth();
        const int ih = mat->GetImageHeight();
        const float imgAspect = ( ih > 0 ) ? ( (float)iw / (float)ih ) : 1.0f;

        // nested size + alternating spin per layer so stacked layers read distinctly
        const float layerScale = 1.0f - 0.15f * (float)k;
        const float sz = 256.0f * ( vis_layerScale.GetFloat() + VisMod( VMT_LAYERSCALE_MOD ) )
                       * idMath::ClampFloat( 0.05f, 4.0f, VisMod(VMT_SCALE) ) * ( layerScale > 0.2f ? layerScale : 0.2f );
        const float hh = sz * 0.5f;
        const float hw = hh * imgAspect * s_aspectFix;   // image aspect + display aspect
        const float rot = ( k & 1 ) ? -s_rotAngle : s_rotAngle;
        const float c = idMath::Cos( rot );
        const float s = idMath::Sin( rot );
        const float lx[4] = { -hw,  hw,  hw, -hw };
        const float ly[4] = { -hh, -hh,  hh,  hh };
        idVec4 corner[4];
        for ( int i = 0; i < 4; i++ ) {
            corner[i].x = cx + lx[i] * c - ly[i] * s;
            corner[i].y = cy + lx[i] * s + ly[i] * c;
            corner[i].z = sc[i];
            corner[i].w = tc[i];
        }
        // PRD Pillar C follow-up (direct user report: "there is no way to
        // change the hue with a slider and tint with routed hue exists but
        // i'm not clear on how it works or how to use it"): the tint now
        // comes from vis_layerHue/VMT_LAYERHUE_MOD -- a real, dedicated,
        // directly-adjustable-and-bindable slider on THIS panel -- instead
        // of silently reusing the unrelated global VMT_HUE (the LFO-driven
        // rainbow-cycle value that drives Bars/Radial/etc's own color,
        // which had no slider anywhere near this panel and no obvious
        // connection to it). Alpha is likewise now bindable (layeralphamod).
        const float a = idMath::ClampFloat( 0.0f, 1.0f, ( baseAlpha + VisModOr( VMT_LAYERALPHA_MOD, 0.0f ) ) * VisMod(VMT_BRIGHT) );
        const float layerHue = vis_layerHue.GetFloat() + VisModOr( VMT_LAYERHUE_MOD, 0.0f );
        const idVec4 tint = colorize ? HueColor( layerHue + 0.12f * (float)k, a ) : idVec4( 1.0f, 1.0f, 1.0f, a );
        renderSystem->SetColor( tint );
        renderSystem->DrawStretchPic( corner[0], corner[1], corner[2], corner[3], mat );
    }
}

// ===========================================================================
// PRD Pillar C: real ZGE-style add/remove layer stack (direct user report:
// "zge allow you to add layers and remove layers. we do not seem to have
// that functionality"). Previously vis_effect selected exactly ONE of the 8
// source effects; s_layerTypes (see the vis_layers console command further
// below) was only a read-only catalog of that fact, not a real multi-
// instance stack.
//
// Each active slot renders its own effect into its own runtime image via
// BeginOffscreenRender/EndOffscreenRender (the two calls the rest of this
// file already uses for the ping-pong feedback pair), then all slots are
// composited back onto the real backbuffer in stack order via ordinary
// alpha/additive-blend DrawStretchPic calls -- see VisRenderLayerStack.
//
// Deliberate scope boundary: BeginOffscreenRender rejects a nested Begin
// while one is already open (confirmed by reading idRenderSystemLocal::
// BeginOffscreenRender directly), and the feedback/warp/milk ping-pong path
// already holds one open for the ENTIRE effect-draw span. So the stack only
// takes over when that ping-pong redirect is NOT active this frame (see the
// pingPongActive check in Draw2D) -- with feedback/warp/milk-preset mode on,
// Draw2D falls back to the original single vis_effect switch, unchanged.
// Combining the two is a real future enhancement, not an oversight.
//
// Extracted from Draw2D's own non-feedback backdrop draw (the `else` half of
// its `if (feedback) {...} else {...}`) so VisRenderLayerStack can redraw the
// exact same backdrop after its own per-slot capture loop stomps it (each
// slot's EndOffscreenRender opaquely blits over the whole backbuffer). Edge
// case, deliberately left as-is: if feedback is nominally on but not yet
// ping-pong-ready (pingPongActive false for a reason other than feedback
// being off, e.g. the first frame or an overlay just closed), the stack path
// still redraws this plain panel rather than the feedback-trail frame that
// would otherwise have been there -- a one-frame, self-correcting visual
// inconsistency, not a crash or persistent regression.
// forward decl: defined near VisRenderMilkMapScene (PRD M4.5 FR-F3 camera/
// scene functions are defined later in this file), needed here to fix a
// direct user report -- a successfully loaded flythrough map ("Load
// selected map" -> vis_mapLoad, confirmed loading + building a valid camera
// tour) never actually appeared on screen. Root cause: Draw2D() renders the
// map's 3D geometry FIRST (as the base layer), but this backdrop fill
// (called right after, in the non-feedback branch, and AGAIN by
// VisRenderLayerStack when a layer stack is active) painted a 90%-opaque
// dark rectangle directly over those already-rendered pixels every frame,
// completely smothering the map.
static bool VisMapSceneActive();
static void VisRenderMilkMapScene();   // also forward-declared again near its own definition; needed here too since VisRenderLayerStack (below) must call it back in after the layer-stack capture loop stomps it

static void VisDrawEffectPanelBackdrop() {
    if ( VisMapSceneActive() ) {
        return;   // let the map's own rendered geometry show through instead
    }
    const bool full = vis_fullscreen.GetBool();
    const float panelTop = full ? 0.0f : ( VIS_H - 20.0f - VIS_H * 0.75f - 6.0f );
    const float panelH = full ? VIS_H : ( VIS_H - panelTop );
    VisFillRect( idVec4( 0.05f, 0.05f, 0.08f, 0.9f ), 0, panelTop, VIS_W, panelH );

    const float bass = idMath::ClampFloat( 0.0f, 1.0f, g_audioAnalyzer.GetBass() * 0.15f );
    if ( bass > 0.001f ) {
        VisFillRect( idVec4( 0.20f, 0.06f, 0.30f, bass ), 0, panelTop, VIS_W, panelH );
    }
}

static const int VIS_MAX_STACK_LAYERS = 4;
// PRD FR-C1-EXPAND: the per-slot modulation-target array (s_slotTarget, declared
// up near the registry before this constant is in scope) is sized by
// VIS_MOD_STACK_SLOTS -- keep the two in lockstep or per-slot routing silently
// covers the wrong number of slots.
compile_time_assert( VIS_MAX_STACK_LAYERS == VIS_MOD_STACK_SLOTS );

// PRD Pillar C follow-up (direct user report: "the zge layers only have
// additive but need all the modes, additive, subtractive, inverse and the
// rest"). One material per (slot, mode) pair, resolved once in Frame() like
// every other material in this file. Names/suffixes mirror the new
// Material.cpp "blend" keywords + .mtr blocks exactly.
enum { VISBLEND_NORMAL = 0, VISBLEND_ADDITIVE, VISBLEND_SUBTRACT, VISBLEND_MULTIPLY, VISBLEND_SCREEN, VISBLEND_DARKEN, VISBLEND_LIGHTEN, VISBLEND_INVERT, VISBLEND_COUNT };
static const char * const s_visBlendModeNames[VISBLEND_COUNT]     = { "Normal", "Additive", "Subtractive", "Multiply", "Screen", "Darken", "Lighten", "Invert" };
static const char * const s_visBlendModeMtrSuffix[VISBLEND_COUNT] = { "", "Add", "Subtract", "Multiply", "Screen", "Darken", "Lighten", "Invert" };
static const idMaterial * s_visLayerSlotMat[VIS_MAX_STACK_LAYERS][VISBLEND_COUNT];

// PRD follow-up (direct user report: "we need blend modes for the map
// flythrough layer maybe one mode for wireframe and one for background").
// Plain-solid-fill counterpart of s_visLayerSlotMat above, for
// VisFillRectBlend -- "visualizer/solidAdditive" (VISBLEND_ADDITIVE) is a
// pre-existing material with an irregular name (predates this convention,
// already used elsewhere for wavecode additive draws), so this is an
// explicit name table rather than string-concatenated like
// s_visBlendModeMtrSuffix above.
static const char * const s_visSolidBlendMatNames[VISBLEND_COUNT] = {
    "visualizer/solid", "visualizer/solidAdditive", "visualizer/solidSubtract", "visualizer/solidMultiply",
    "visualizer/solidScreen", "visualizer/solidDarken", "visualizer/solidLighten", "visualizer/solidInvert"
};
static const idMaterial * s_visSolidBlendMat[VISBLEND_COUNT];

// Same as VisFillRect (defined much earlier in this file) but picks one of
// the VISBLEND_* solid-fill materials above -- placed here rather than next
// to VisFillRect since it needs VISBLEND_COUNT/s_visSolidBlendMat, which
// aren't declared until this point in the file.
static void VisFillRectBlend( const idVec4 & color, float x, float y, float w, float h, int blendMode ) {
    const int mode = ( blendMode >= 0 && blendMode < VISBLEND_COUNT ) ? blendMode : VISBLEND_NORMAL;
    if ( s_visSolidBlendMat[mode] == NULL ) {
        return;
    }
    renderSystem->SetColor( color );
    renderSystem->DrawStretchPic( x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f, s_visSolidBlendMat[mode] );
}

struct visStackLayer_t {
    int    effectType;   // 0-7, same meaning as vis_effect (Bars..PhaseScope)
    float  opacity;       // 0..1, applied at composite time (SetColor alpha)
    int    blendMode;      // VISBLEND_* -- was a bool "additive" flag; 0/1 keep the exact same meaning for backward compatibility with old presets/console lines
    bool   enabled;        // disabled slots keep their state but don't render
    // PRD Pillar C follow-up (direct user report: "all images, effects,
    // layers have scale but also x/y coordinates so things can be moved
    // around in an almost xyz style"). -1..1, same normalized convention as
    // vis_effectX/Y -- this slot's OWN base position, summed with whatever's
    // routed to its posx/posy targets (see VisSlotPosOffset).
    float  posX, posY;
    // per-instance state for the 3 effects that hold persistent buffers --
    // Bars/Radial/Scope/Ring/PhaseScope have none (verified: no static
    // locals in any of the five), so they need nothing here.
    visStackParticleState_t particleState;
    visStackStarState_t     starState;
    visStackSpectroState_t  spectroState;
};
static idList<visStackLayer_t> s_layerStack;

static idStr VisStackSlotImageName( int slot ) {
    return va( "_visLayerSlot%d", slot );
}

// zero-initializes a brand new slot's state (particle life/star alive/
// spectro init all need to start at their "empty" value, not whatever
// garbage idList's growth memory happens to hold).
static void VisStackInitLayer( visStackLayer_t & layer, int effectType, float opacity, int blendMode, bool enabled, float posX = 0.0f, float posY = 0.0f ) {
    memset( &layer, 0, sizeof( layer ) );
    layer.effectType = effectType;
    layer.opacity = opacity;
    layer.blendMode = blendMode;
    layer.enabled = enabled;
    layer.posX = posX;
    layer.posY = posY;
}

static bool VisStackAddLayer( int effectType, float opacity = 1.0f, int blendMode = VISBLEND_NORMAL, bool enabled = true, float posX = 0.0f, float posY = 0.0f ) {
    if ( s_layerStack.Num() >= VIS_MAX_STACK_LAYERS ) {
        idLib::Printf( "vis_stackAdd: already at the %d-layer cap\n", VIS_MAX_STACK_LAYERS );
        return false;
    }
    if ( effectType < 0 || effectType > 7 ) {
        idLib::Printf( "vis_stackAdd: effect type must be 0-7 (bars..phase scope)\n" );
        return false;
    }
    if ( blendMode < 0 || blendMode >= VISBLEND_COUNT ) {
        idLib::Printf( "vis_stackAdd: blend mode must be 0-%d (Normal/Additive/Subtractive/Multiply/Screen/Darken/Lighten/Invert)\n", VISBLEND_COUNT - 1 );
        return false;
    }
    visStackLayer_t layer;
    VisStackInitLayer( layer, effectType, idMath::ClampFloat( 0.0f, 1.0f, opacity ), blendMode, enabled,
                        idMath::ClampFloat( -1.0f, 1.0f, posX ), idMath::ClampFloat( -1.0f, 1.0f, posY ) );
    s_layerStack.Append( layer );
    return true;
}

static bool VisStackRemoveLayer( int index ) {
    if ( index < 0 || index >= s_layerStack.Num() ) {
        idLib::Printf( "vis_stackRemove: index out of range (0-%d)\n", s_layerStack.Num() - 1 );
        return false;
    }
    s_layerStack.RemoveIndex( index );
    return true;
}

static bool VisStackMoveLayer( int index, int newIndex ) {
    if ( index < 0 || index >= s_layerStack.Num() || newIndex < 0 || newIndex >= s_layerStack.Num() ) {
        return false;
    }
    if ( index == newIndex ) {
        return true;
    }
    const visStackLayer_t moved = s_layerStack[index];
    s_layerStack.RemoveIndex( index );
    s_layerStack.Insert( moved, newIndex );
    return true;
}

static void VisStackClear() {
    s_layerStack.Clear();
}

static void VisRegisterLayerStackRTGenerators() {
    static bool s_registered = false;
    if ( s_registered ) {
        return;
    }
    s_registered = true;
    for ( int i = 0; i < VIS_MAX_STACK_LAYERS; i++ ) {
        globalImages->ImageFromFunction( VisStackSlotImageName( i ).c_str(), VisFeedbackRTGenerator );
    }
}

// dispatches one slot's effect into whatever offscreen target is currently
// bound (the caller has already done BeginOffscreenRender before this runs).
static void VisRenderStackLayerEffect( visStackLayer_t & layer, int n, int slot ) {
    // PRD Pillar C follow-up: this slot's own base position plus whatever's
    // routed to its posx/posy targets, clamped -1..1 then scaled into
    // VIS_W/VIS_H pixel space -- every DrawEffect* below reads s_curOffX/Y
    // instead of hardcoding screen-center.
    s_curOffX = idMath::ClampFloat( -1.0f, 1.0f, layer.posX + VisSlotPosOffset( slot, VSP_POSX ) ) * VIS_W * 0.5f;
    s_curOffY = idMath::ClampFloat( -1.0f, 1.0f, layer.posY + VisSlotPosOffset( slot, VSP_POSY ) ) * VIS_H * 0.5f;
    switch ( layer.effectType ) {
        case 0:  DrawEffectBars( n );                                break;
        case 1:  DrawEffectRadial( n );                              break;
        case 2:  DrawEffectScope();                                  break;
        case 3:  DrawEffectRing();                                   break;
        case 4:  DrawEffectParticles( &layer.particleState, slot );  break;
        case 5:  DrawEffectSpectrogram( &layer.spectroState, slot );  break;
        case 6:  DrawEffectStarfield( &layer.starState, slot );      break;
        case 7:  DrawEffectPhaseScope();                             break;
        default: DrawEffectBars( n );                                break;
    }
}

// The real compositor: captures each enabled slot into its own image, then
// redraws the shared background panel (stomped by each slot capture's own
// auto-blit -- see the EndOffscreenRender comment below) and layers each
// slot's captured texture back on top of it in stack order.
//
// `restoreBase` (direct user report: "i tried adding a layer stack layer and
// it did not seem to do anything" -- with a MilkDrop preset also active): the
// caller passes false when it has ALREADY put the correct base content in the
// backbuffer immediately before this call (the ping-pong-active call site in
// Draw2D, right after EndOffscreenRender's feedback blit) -- restoring the
// map/panel backdrop AGAIN here would either be redundant (map case) or
// actively wrong (panel case: it would paint the near-opaque backdrop OVER
// the MilkDrop feedback trail that's the whole point of that call site).
static void VisRenderLayerStack( int n, bool restoreBase = true ) {
    VisRegisterLayerStackRTGenerators();

    for ( int i = 0; i < s_layerStack.Num(); i++ ) {
        visStackLayer_t & layer = s_layerStack[i];
        if ( !layer.enabled ) {
            continue;
        }
        renderSystem->BeginOffscreenRender( VisStackSlotImageName( i ).c_str(), (int)VIS_W, (int)VIS_H );
        VisRenderStackLayerEffect( layer, n, i );
        // EndOffscreenRender always blits this slot's own image onto the
        // REAL backbuffer (opaque, GLS_SRCBLEND_ONE|GLS_DSTBLEND_ZERO --
        // confirmed by reading RB_VisFboEnd directly), overwriting whatever
        // the previous slot (or the panel background drawn before this loop
        // started) put there. That's fine -- it's wasted work, not a
        // correctness problem, because the manual composite pass below
        // redraws the real backbuffer from scratch afterward using each
        // slot's IMAGE (which does hold that slot's correct captured
        // content regardless of the wasted blit).
        renderSystem->EndOffscreenRender();
    }

    // restore whatever base layer the capture loop above just stomped:
    // the loaded flythrough map's 3D scene if one is active (direct user
    // report: loaded a map with a layer stack active and never saw it --
    // each slot's EndOffscreenRender opaquely blits over the ENTIRE
    // backbuffer, erasing the map render Draw2D() did earlier this same
    // frame), otherwise the plain panel backdrop (identical to Draw2D's own
    // non-feedback panel-fill). Skipped when restoreBase is false -- the
    // caller already put the right base (MilkDrop feedback trail, or the map
    // re-rendered on top of it) in the backbuffer.
    if ( restoreBase ) {
        if ( VisMapSceneActive() ) {
            VisRenderMilkMapScene();
        } else {
            VisDrawEffectPanelBackdrop();
        }
    }

    for ( int i = 0; i < s_layerStack.Num(); i++ ) {
        const visStackLayer_t & layer = s_layerStack[i];
        if ( !layer.enabled ) {
            continue;
        }
        const int mode = ( layer.blendMode >= 0 && layer.blendMode < VISBLEND_COUNT ) ? layer.blendMode : VISBLEND_NORMAL;
        const idMaterial * mat = s_visLayerSlotMat[i][mode];
        if ( mat == NULL ) {
            continue;   // not resolved yet (first frame) -- skip rather than touch decls off-thread
        }
        // PRD FR-C1-EXPAND: per-slot routed opacity multiplies the slot's own
        // opacity (neutral 1.0 when unrouted => identical to before).
        const float slotAlpha = idMath::ClampFloat( 0.0f, 1.0f, layer.opacity * VisSlotMod( i, VSP_OPACITY ) );
        renderSystem->SetColor( idVec4( 1.0f, 1.0f, 1.0f, slotAlpha ) );
        renderSystem->DrawStretchPic( 0.0f, 0.0f, VIS_W, VIS_H, 0.0f, 0.0f, 1.0f, 1.0f, mat );
    }
}
// ===========================================================================

// forward decls: PRD M4.5 FR-F3 camera/scene functions are defined later in
// this file (near the vis_map* console commands they're tested through),
// but Draw2D()/Frame() (below) need to call them.
static void VisUpdateMilkCamera( float dtSec );
static void VisRenderMilkMapScene();
static void VisUpdateMapRenderColors();   // PRD follow-up: bindable wireframe/background tint hues
static void VisRebuildMilkTour();

// PRD FR-E4 / M6: recording state (see the capture call at the end of Draw2D).
static bool  s_milkRecording = false;
static int   s_milkRecordFrame = 0;
static idStr s_milkRecordDir;

// PRD FR-D3/roadmap F11: "toggle all effects" master switch. The design
// decision deferred in earlier passes -- save-and-restore rather than
// zero-and-forget, so toggling back on returns exactly what was there
// before, not just some default state.
static bool  s_milkEffectsMasterOff = false;
static bool  s_savedVisFeedback = false;
static int   s_savedVisWarp = 0;
static float s_savedVisBloom = 0.0f;

// Extracted from the K_F11 case below so both the hotkey and the new
// ImGui "All effects off" button (direct user report: "we need all of the
// options that exist in the ingame engine menu to exist in the imgui
// menu") drive the exact same save-and-restore logic, not a duplicated copy.
static void VisToggleEffectsMasterOff() {
    if ( !s_milkEffectsMasterOff ) {
        s_savedVisFeedback = vis_feedback.GetBool();
        s_savedVisWarp     = vis_warp.GetInteger();
        s_savedVisBloom    = vis_bloom.GetFloat();
        vis_feedback.SetBool( false );
        vis_warp.SetInteger( 0 );
        vis_bloom.SetFloat( 0.0f );
        s_milkEffectsMasterOff = true;
    } else {
        // Fixed real bug: restoring unconditionally could clobber a manual
        // change (console, EFFECTS-tab menu, ImGui) made to these cvars
        // while the master-off toggle was active with a stale snapshot.
        // Only restore if they're still exactly at the all-off state this
        // same toggle forced them into; otherwise treat the external
        // change as an intentional override and just clear the flag.
        if ( !vis_feedback.GetBool() && vis_warp.GetInteger() == 0 && vis_bloom.GetFloat() == 0.0f ) {
            vis_feedback.SetBool( s_savedVisFeedback );
            vis_warp.SetInteger( s_savedVisWarp );
            vis_bloom.SetFloat( s_savedVisBloom );
        }
        s_milkEffectsMasterOff = false;
    }
}

// Fixed real bug: registering these from idVisualizerManager's constructor
// (as an earlier version of this fix did) turned out to be WRONG, not just
// racy -- proven via pointer-address diagnostic logging on a live build.
// idVisualizerManager is a namespace-scope global, so its constructor runs
// during C++ static initialization, before main() even starts, which is
// BEFORE idImageManager::Init() (called from deep in the renderer's OpenGL
// init sequence) and well before the boot-time material precache ever
// touches "map visualizer/feedbackrtA" etc. Registering that early doesn't
// let us win the race against the precache -- it loses it in a worse way:
// idImageManager::GetImageWithParameters (which material "map" stage
// resolution goes through) only reuses an existing image by name if
// filter/repeat/usage ALSO match; our generator hadn't run yet at that
// point (ActuallyLoadImage no-ops before the renderer exists), so our
// image was still sitting at idImage's default-constructed filter/repeat/
// usage, not the TF_LINEAR/TR_CLAMP our generator actually wants. When the
// precache's own request didn't match, it silently AllocImage'd a second,
// completely separate, generator-less idImage under the identical name --
// and since new hash entries are found first, that second object is the
// one Bind()/BeginOffscreenRender have used ever since, while ours sat
// orphaned in memory, never referenced again. Confirmed directly: logging
// each object's pointer showed the constructor's ImageFromFunction call
// and BeginOffscreenRender's GetImage call returning two different
// addresses for the exact same literal name string.
//
// Fix: register lazily, on the first Draw2D() call instead. By the time
// gameplay is rendering frames at all, engine boot (including the material
// precache) has unconditionally already finished, so whatever bare
// placeholder the precache created under these names already exists in
// the image table -- ImageFromFunction's "already exists" branch (see
// ImageManager.cpp) now attaches our generator to THAT SAME object instead
// of racing to create a separate one first.
static void VisRegisterFeedbackRTGenerators() {
    static bool s_registered = false;
    if ( s_registered ) {
        return;
    }
    s_registered = true;
    globalImages->ImageFromFunction( "visualizer/feedbackrt", VisFeedbackRTGenerator );
    globalImages->ImageFromFunction( "visualizer/feedbackrtA", VisFeedbackRTGenerator );
    globalImages->ImageFromFunction( "visualizer/feedbackrtB", VisFeedbackRTGenerator );
    // "visualizer/bloomrt": a fourth runtime-only target, referenced via
    // "fragmentMap 0 visualizer/bloomrt" in the bloom material and filled
    // in each frame by renderSystem->CaptureRenderToImage -- same
    // Couldn't-load-image warning source as the three above, same fix.
    globalImages->ImageFromFunction( "visualizer/bloomrt", VisFeedbackRTGenerator );
    // PRD M3: intermediate target for the two-pass warp_->comp_ pipeline --
    // see RB_VisMilkCompDraw's comment in tr_backend_draw.cpp for why a
    // preset's comp_ shader (MilkDrop's separate post-composite pass, which
    // operates on the ALREADY-warped image) needs its own render target
    // distinct from the ping-pong feedbackrtA/B pair.
    globalImages->ImageFromFunction( "visualizer/milkwarpintermediate", VisFeedbackRTGenerator );
}

void idVisualizerManager::Draw2D() {
    VisRegisterFeedbackRTGenerators();
    VisInitModTargets();

    if ( !vis_show.GetBool() ) {
        MenuDraw2D();
        return;
    }

    // keep the analyzer's band count in sync with the cvar
    if ( g_audioAnalyzer.GetBandCount() != vis_bands.GetInteger() ) {
        g_audioAnalyzer.SetBandCount( vis_bands.GetInteger() );
    }

    const int n = g_audioAnalyzer.GetBandCount();
    if ( n < 1 ) {
        MenuDraw2D();
        return;
    }

    // PRD M4.5: if a map is loaded, render its 3D geometry as the base layer
    // FIRST, so the feedback trail/bloom/kaleidoscope processors below
    // composite on top of it (FR-F5), exactly like they already composite
    // on top of the 2D effects.
    VisRenderMilkMapScene();

    const bool feedback = ( vis_feedback.GetBool() || vis_warp.GetBool() || s_milkActive || s_shadertoyActive ) && s_visFeedback != NULL;

    // PRD M1 step 8: decide overlay-open state up front (moved earlier from
    // its old position right before the CaptureRenderToImage call below) so
    // the new offscreen ping-pong redirect can be gated on it too -- a menu/
    // console covering the screen must freeze the trail, not render menu
    // pixels into next frame's feedback source.
    const bool overlayOpenNow = m_menuOpen || ( console != NULL && console->Active() );
    const bool pingPongReady  = ( s_visFeedbackA != NULL && s_visFeedbackB != NULL );
    const bool pingPongActive = feedback && pingPongReady && !overlayOpenNow;

    if ( pingPongActive ) {
        // this frame SAMPLES whichever buffer holds LAST frame's write, and
        // WRITES into the other one -- see the s_visFeedbackA/B declaration
        // comment for the full precision rationale.
        s_visFeedbackActive = s_milkPingPongParity ? s_visFeedbackA : s_visFeedbackB;
        s_visFeedbackActiveImageName = s_milkPingPongParity ? "visualizer/feedbackrtA" : "visualizer/feedbackrtB";
        const char * writeImageName = s_milkPingPongParity ? "visualizer/feedbackrtB" : "visualizer/feedbackrtA";
        renderSystem->BeginOffscreenRender( writeImageName, renderSystem->GetWidth(), renderSystem->GetHeight() );
    } else if ( s_visFeedbackActive == NULL ) {
        // bootstrap fallback: before ping-pong has ever run (A/B not yet
        // resolved) or while frozen behind a menu, fall back to the old
        // single-buffer material rather than sampling nothing.
        s_visFeedbackActive = s_visFeedback;
        s_visFeedbackActiveImageName = "visualizer/feedbackrt";
    }

    if ( feedback ) {
        // MilkDrop feedback: redraw the previous captured frame, zoomed about the
        // center and dimmed, so the effect leaves warping trails. No opaque
        // backdrop (that would erase the trail).
        if ( s_feedbackCaptured ) {
            // zoom = base cvar + audio-routed push; rotation spins per-frame so it
            // accumulates through the loop; decay dims the retained trail.
            const float z = idMath::ClampFloat( 0.90f, 1.15f, vis_feedbackZoom.GetFloat() + VisMod(VMT_ZOOM) );
            const float d = idMath::ClampFloat( 0.0f, 1.0f, vis_feedbackDecay.GetFloat() + VisModOr( VMT_FEEDBACKDECAY_MOD, 0.0f ) );
            const int wq = vis_warp.GetInteger();
            if ( s_milkActive ) {
                // PRD M1 step 4: an active .milk preset takes over the warp
                // mesh entirely, bypassing vis_warp's own mode selection --
                // loading a preset "just works" without extra cvar setup.
                DrawMilkWarpMesh( d );
                // PRD M5: custom shapes drawn onto the warped canvas first --
                // real MilkDrop treats shapecode as background decoration,
                // beneath the waveform/wavecode layers below.
                DrawMilkShapecode();
                // PRD M1 step 5: built-in waveform onto the warped canvas,
                // then the video-echo composite as the final step (matches
                // the research doc's pass order: waves before composite).
                DrawMilkWaveform();
                DrawMilkVideoEcho();
                // PRD M5: custom wavecode (preset-defined waves), on top of
                // everything above -- matches real MilkDrop's own layering
                // (built-in waveform first, custom waves drawn after).
                DrawMilkWavecode();
            } else if ( s_shadertoyActive ) {
                // PRD FR-C10: an imported ShaderToy shader is the full-screen
                // warp driver this frame (mutually exclusive with a .milk
                // preset -- see the ShaderToy block above), sampling the
                // previous frame via iChannel0/sampler_main just like the milk
                // warp_ path. No dim multiply for the same reason the milk
                // warp path skips it: the shader controls its own feedback.
                DrawShaderToyLayer();
            } else if ( wq >= 3 && s_visWarp != NULL ) {
                // Fragment-shader warp: the warp params ride in on the vertex color
                // (the 2D path sets it via SetColor), so the shader is animated +
                // audio-reactive. r=amount, g=phase(0..1), b=mode(0..1), a=dim.
                // (shader multiplies amount by 0.5, so pre-scale by 2.)
                const float eff = ( vis_warpAmount.GetFloat() + VisMod( VMT_WARPAMOUNT_MOD ) ) * ( 1.0f + idMath::ClampFloat( 0.0f, 2.0f, g_audioAnalyzer.GetBassAtt() ) );
                const float pr = idMath::ClampFloat( 0.0f, 1.0f, eff * 2.0f );
                const float pg = s_warpPhase - idMath::Floor( s_warpPhase );
                const float pb = idMath::ClampFloat( 0.0f, 1.0f, vis_warpMode.GetInteger() / 3.0f );
                renderSystem->SetColor( idVec4( pr, pg, pb, d ) );
                renderSystem->DrawStretchPic( 0.0f, 0.0f, VIS_W, VIS_H, 0.0f, 0.0f, 1.0f, 1.0f, s_visWarp );
            } else if ( wq > 0 ) {
                // ripple depth swells with bass; ties the warp to the music.
                const float amt = ( vis_warpAmount.GetFloat() + VisMod( VMT_WARPAMOUNT_MOD ) ) * ( 1.0f + idMath::ClampFloat( 0.0f, 2.0f, g_audioAnalyzer.GetBassAtt() ) );
                const int gcols = ( wq >= 2 ) ? 48 : 24;   // wq==3 w/ null shader falls back to hi-res
                const int grows = ( wq >= 2 ) ? 36 : 18;
                DrawFeedbackWarpMesh( gcols, grows, vis_warpMode.GetInteger(),
                                      z, s_rotDelta, d, amt, vis_warpFreq.GetFloat() + VisModOr( VMT_WARPFREQ_MOD, 0.0f ), s_warpPhase );
            } else {
                DrawFeedbackFrame( z, s_rotDelta, d );
            }
        } else {
            // first frame: clear to black so the trail starts clean
            VisFillRect( idVec4( 0.0f, 0.0f, 0.0f, 1.0f ), 0, 0, VIS_W, VIS_H );
        }
    } else {
        // backdrop: full-screen or a bottom panel depending on vis_fullscreen
        VisDrawEffectPanelBackdrop();
    }

    // PRD Pillar C: real add/remove layer stack takes over from the single
    // vis_effect switch once the user has added at least one stack layer --
    // see VisRenderLayerStack's own comment for why this only applies when
    // the feedback/warp/milk ping-pong redirect isn't already open.
    if ( !pingPongActive && s_layerStack.Num() > 0 ) {
        VisRenderLayerStack( n );
    } else {
        // Direct user report: with a map loaded, switching vis_effect (next-
        // effect hotkey/GUI) appeared to do nothing -- traced to this branch
        // having been wrongly gated on !VisMapSceneActive() (matching
        // VisRenderLayerStack's base-restore rule, which does NOT apply
        // here). Re-checked every DrawEffect* function below directly: none
        // of them clear/fill the whole screen -- Bars/Radial/Ring/Scope draw
        // alpha-blended bars/quads/lines, Spectrogram and Starfield only
        // fill the specific cells/stars that have nonzero values -- so none
        // of them can hide the map underneath the way the old opaque panel
        // backdrop did. Safe to always run this switch again.
        // PRD Pillar C follow-up: the legacy single-effect path's own
        // position, same s_curOffX/Y mechanism the layer stack uses.
        s_curOffX = idMath::ClampFloat( -1.0f, 1.0f, vis_effectX.GetFloat() + VisMod( VMT_EFFECTX_MOD ) ) * VIS_W * 0.5f;
        s_curOffY = idMath::ClampFloat( -1.0f, 1.0f, vis_effectY.GetFloat() + VisMod( VMT_EFFECTY_MOD ) ) * VIS_H * 0.5f;
        switch ( vis_effect.GetInteger() ) {
            case 1:  DrawEffectRadial( n );    break;
            case 2:  DrawEffectScope();        break;
            case 3:  DrawEffectRing();         break;
            case 4:  DrawEffectParticles();    break;
            case 5:  DrawEffectSpectrogram();  break;
            case 6:  DrawEffectStarfield();    break;
            case 7:  DrawEffectPhaseScope();   break;
            default: DrawEffectBars( n );      break;
        }
    }

    DrawVisLayer();   // reactive image layer, composited before the capture

    if ( pingPongActive ) {
        renderSystem->EndOffscreenRender();   // blits this frame's write buffer to the backbuffer
        s_milkPingPongParity = !s_milkPingPongParity;
        // Direct user report: a map loaded alongside an active MilkDrop
        // preset never showed up, even after the earlier panel-backdrop/
        // layer-stack fixes elsewhere in this file. Root cause: this
        // EndOffscreenRender call unconditionally blits the ping-pong
        // feedback buffer over the ENTIRE backbuffer -- discarding
        // VisRenderMilkMapScene()'s render from the top of this function,
        // which happens before BeginOffscreenRender ever redirects drawing
        // away from the backbuffer in the first place. Same "restore the
        // base after a capture stomps it" pattern VisRenderLayerStack
        // already uses for its own capture loop.
        VisRenderMilkMapScene();
        // Direct user report: "i tried adding an layer stack layer and it
        // did not seem to do anything" -- with a MilkDrop preset also
        // active, s_layerStack.Num()>0 was true but the whole stack was
        // skipped below (pingPongActive gates it off entirely, since its
        // per-slot BeginOffscreenRender calls would conflict with the
        // ping-pong redirect that was still open at that point in the
        // frame). Now that EndOffscreenRender above has closed the
        // ping-pong redirect, it's safe to run the stack's own capture
        // loop here. restoreBase=false: the feedback trail (or the map
        // just re-rendered above) is already the correct base in the
        // backbuffer -- restoring it again would either be redundant or
        // (the panel-backdrop case) paint over the feedback trail.
        if ( s_layerStack.Num() > 0 ) {
            VisRenderLayerStack( n, false );
        }
    }

    if ( feedback ) {
        // Freeze the trail while the picker menu or the console is open, so their
        // pixels can never be captured into the feedback loop. On the frame the
        // overlay closes, wipe the trail (s_feedbackCaptured=false) so any frame
        // captured during an SMP thread skew can't leave a fading menu ghost.
        static bool s_prevOverlayOpen = false;
        if ( s_prevOverlayOpen && !overlayOpenNow ) {
            s_feedbackCaptured = false;   // overlay just closed - restart trail clean
        }
        s_prevOverlayOpen = overlayOpenNow;
        if ( !overlayOpenNow ) {
            // still fed for the experimental wq>=3 custom-fragment-shader warp
            // path, whose visualizer/warp material has "visualizer/feedbackrt"
            // baked into its fragmentMap at .mtr parse time -- see the
            // s_visFeedbackA/B declaration comment for why that path wasn't
            // folded into the new ping-pong scheme this round.
            renderSystem->CaptureRenderToImage( "visualizer/feedbackrt", false );
            s_feedbackCaptured = true;
        }
    }

    // Bloom post-process: capture the composited effect+trail and add a blurred,
    // bright-passed glow over it. After the feedback capture (so the glow isn't
    // fed back / doesn't compound) and before the HUD (so text isn't bloomed).
    // PRD Pillar C: bloom/threshold/radius are now bindable (VMT_BLOOM_MOD/
    // VMT_BLOOMTHRESH_MOD/VMT_BLOOMRADIUS_MOD), same additive-offset pattern
    // as warpamountmod. The master gate reads the MODULATED bloom amount, not
    // just the raw cvar, so routing something to "bloommod" can turn bloom on
    // even while the vis_bloom slider itself is at 0.
    const float bloomBase = idMath::ClampFloat( 0.0f, 1.0f, vis_bloom.GetFloat() + VisModOr( VMT_BLOOM_MOD, 0.0f ) );
    if ( bloomBase > 0.01f && s_visBloom != NULL ) {
        renderSystem->CaptureRenderToImage( "visualizer/bloomrt", false );
        float bloomI = idMath::ClampFloat( 0.0f, 1.0f, bloomBase * ( 1.0f + vis_bloomBeat.GetFloat() * idMath::ClampFloat( 0.0f, 2.0f, g_audioAnalyzer.GetBassAtt() ) ) );
        renderSystem->SetColor( idVec4( bloomI, idMath::ClampFloat( 0.0f, 1.0f, vis_bloomThreshold.GetFloat() + VisModOr( VMT_BLOOMTHRESH_MOD, 0.0f ) ),
                                        idMath::ClampFloat( 0.0f, 1.0f, vis_bloomRadius.GetFloat() + VisModOr( VMT_BLOOMRADIUS_MOD, 0.0f ) ), 1.0f ) );
        renderSystem->DrawStretchPic( 0.0f, 0.0f, VIS_W, VIS_H, 0.0f, 0.0f, 1.0f, 1.0f, s_visBloom );
    }

    // Title card: a transient banner (track / preset name) shown only for a few
    // seconds when something loads, then fades out. No persistent overlay - it
    // detracts from the effect. Drawn after the feedback capture so it never
    // smears into the trail, and hidden while the picker menu is up.
    if ( vis_hud.GetBool() && !m_menuOpen && s_bannerText.Length() > 0 ) {
        const int elapsed = Sys_Milliseconds() - s_bannerMs;
        if ( elapsed >= 0 && elapsed < VIS_BANNER_MS ) {
            // hold full opacity for the first 60%, fade over the last 40%
            const float t = (float)elapsed / (float)VIS_BANNER_MS;
            const float a = ( t < 0.6f ) ? 1.0f : ( 1.0f - ( t - 0.6f ) / 0.4f );
            const int w = s_bannerText.Length() * SMALLCHAR_WIDTH;
            renderSystem->DrawSmallStringExt( ( (int)VIS_W - w ) / 2, (int)( VIS_H * 0.12f ),
                                              s_bannerText.c_str(),
                                              idVec4( 0.35f, 0.85f, 1.0f, a ), true );
        }
    }

    // PRD FR-E4 / M6: simple image-sequence screen recording, one TGA per
    // frame. Placed after the banner but before the picker menu, same
    // "exclude the overlay" philosophy the feedback/bloom captures above
    // already follow. This is the simpler FR-E4 "screen recording"
    // alternative the PRD calls out; assembling frames into an actual video
    // file (via the FFmpeg libs already linked for audio decode) is the
    // bigger FR-E3 follow-up, not attempted here.
    //
    // Fixed real bug: this used to call renderSystem->CaptureRenderToFile()
    // (raw qglReadPixels) directly from here -- but Draw2D runs on the SMP
    // draw-worker thread, not the thread that owns the GL context. Same
    // confirmed-crash bug as VisSendNDIFrame just above; identical fix
    // (EnqueueCaptureToFile, deferred to the backend thread via
    // RB_VisCapture in RenderSystem.cpp).
    if ( s_milkRecording && !m_menuOpen ) {
        const idStr filename = va( "%s/frame_%06d.tga", s_milkRecordDir.c_str(), s_milkRecordFrame );
        renderSystem->EnqueueCaptureToFile( filename.c_str(), false );
        s_milkRecordFrame++;
    }

    // PRD M7: NDI network video output, same "exclude the overlay"
    // placement as the recording capture just above.
    if ( !m_menuOpen ) {
        VisSendNDIFrame();
    }

    MenuDraw2D();   // picker overlay draws on top of the effect (excluded from
                    // the feedback capture above, so it never smears)
}

idCVar vis_imguiEditor( "vis_imguiEditor", "0", CVAR_BOOL | CVAR_ARCHIVE, "PRD M4: show the ImGui layer/routing editor panel" );
// read-only-ish mirror (same pattern as vis_wallpaperMode/vis_ndiActive):
// true only while the mouse cursor is physically positioned over the
// separate ImGui tool window (win_imgui.cpp), updated every frame from
// Vis_ImGuiSetToolWindowVisible via WindowFromPoint against the live cursor
// position -- deliberately NOT a GetForegroundWindow()/focus-based check
// (see that function's own comment for why focus-transfer timing between
// two top-level windows of the same process proved unreliable in practice).
// common_frame.cpp's mouse-grab condition reads this by name so the cursor
// hides over the visualization display but stays visible while the mouse is
// actually over the tool window, rather than for the whole time it's merely
// open somewhere on screen.
idCVar vis_imguiEditorFocused( "vis_imguiEditorFocused", "0", CVAR_BOOL, "read-only-ish mirror of whether the mouse is over the ImGui layer-editor tool window" );

// PRD Pillar C/FR-E5 "MIDI-learn": "right-click any knob -> Link to source"
// -- the ZGE-style binding gesture. idMidiInput only exposes current-state
// polling (GetCC), not an event queue, so learning "which CC did the user
// just move" means arming a snapshot of every channel/CC pair's value at
// the moment learn mode starts, then each frame comparing the live value
// against that snapshot -- the first CC to move enough binds the armed
// target to VMS_MIDICC_PARAM at that exact channel/CC and clears learn
// mode. Same 16x128 scan shape as the note-on trigger scan in Frame()
// already uses, and only actually walks the full grid while a learn is
// in progress (early-out otherwise).
static int   s_midiLearnTarget = -1;   // index into s_targetDefs/s_routes, -1 = not learning
static float s_midiLearnSnapshot[16][128];

static void VisArmMidiLearn( int target ) {
    s_midiLearnTarget = target;
    for ( int ch = 0; ch < 16; ch++ ) {
        for ( int cc = 0; cc < 128; cc++ ) {
            s_midiLearnSnapshot[ch][cc] = g_midiInput.GetCC( ch, cc );
        }
    }
}

// Called every frame from Frame() (not gated on the GUI being open -- the
// popup that armed this may already be closed by the time the user
// actually twists the physical knob).
static void VisUpdateMidiLearn() {
    if ( s_midiLearnTarget < 0 || s_midiLearnTarget >= s_targetDefs.Num() ) {
        s_midiLearnTarget = -1;
        return;
    }
    for ( int ch = 0; ch < 16; ch++ ) {
        for ( int cc = 0; cc < 128; cc++ ) {
            const float v = g_midiInput.GetCC( ch, cc );
            if ( idMath::Fabs( v - s_midiLearnSnapshot[ch][cc] ) > 0.05f ) {
                s_routes[s_midiLearnTarget].source = VMS_MIDICC_PARAM;
                s_routes[s_midiLearnTarget].midiChannel = ch;
                s_routes[s_midiLearnTarget].midiCC = cc;
                idLib::Printf( "MIDI-learn: %s <- midicc%d_%d\n", s_targetDefs[s_midiLearnTarget].name, ch, cc );
                s_midiLearnTarget = -1;
                return;
            }
        }
    }
}

// Console-command equivalent of the GUI's right-click "MIDI Learn" gesture
// -- every GUI action in this file has a command backing it (matches the
// existing vis_route/vis_routes pattern), and this also makes MIDI-learn
// testable/scriptable without a mouse.
CONSOLE_COMMAND( vis_midiLearn, "arm MIDI-learn for a routing target: vis_midiLearn <target> (e.g. scale/hue/bright/zoom/rotate) -- next moved CC binds to it", NULL ) {
    VisInitModTargets();
    if ( args.Argc() != 2 ) {
        idLib::Printf( "usage: vis_midiLearn <target>\n" );
        return;
    }
    const int t = VisFindModTarget( args.Argv( 1 ) );
    if ( t < 0 ) {
        idLib::Printf( "unknown target '%s'\n", args.Argv( 1 ) );
        return;
    }
    VisArmMidiLearn( t );
    idLib::Printf( "vis_midiLearn: listening for a CC to bind to '%s'...\n", args.Argv( 1 ) );
}

// PRD M4: the layer-editor UI content itself -- built each frame (main
// thread, between Vis_ImGuiNewFrame() and Vis_ImGuiEndFrame(), see
// win_imgui.h's threading note for why this must stay single-threaded). A
// real, functional v1: live band-level bars/BPM readout, the master effect
// toggles, and the modulation routing table (the same state vis_route/
// vis_routes already manipulate via typed commands, just with widgets).
// Walks s_targetDefs rather than a fixed list, so any layer that calls
// VisRegisterModTarget shows up here automatically too.
// small helper: run a console command from GUI code the exact same way a
// typed command would (BufferCommandText), rather than calling the static
// implementation functions directly -- keeps GUI-triggered actions on the
// one already-correct, already-thread-safe code path every command already
// uses (matches this file's own established pattern, e.g. VisMilkAdvancePreset
// callers), instead of duplicating logic or risking calling something from
// the wrong context.
static void VisGuiRunCommand( const char * cmd ) {
    cmdSystem->BufferCommandText( CMD_EXEC_APPEND, va( "%s\n", cmd ) );
}

// Forward declarations: these cvars/helper are DEFINED further down this
// file (wallpaper/NDI near their own feature blocks, MIDI-out near
// VisUpdateMidiOutput, VisAppendFiles near the menu-scan code) but the GUI
// below reads/displays them too -- same plain (non-static) idCVar linkage
// every other cvar in this file already has, just declared here first so
// the compiler has a type/name to check this earlier use against.
extern idCVar vis_wallpaperMode;
extern idCVar vis_ndiActive;
extern idCVar vis_ndiSourceName;
extern idCVar vis_midiOutBeatNote;
extern idCVar vis_midiOutCC;
extern idCVar vis_midiOutClock;
extern idCVar vis_midiOutChannel;
extern idCVar vis_midiOutNote;
extern idCVar vis_dmxEnable;      // PRD FR-C9: Art-Net DMX output cvars (defined near VisUpdateDmxOutput)
extern idCVar vis_dmxTargetIP;
extern idCVar vis_dmxUniverse;
extern idCVar vis_mapRenderMode;   // PRD follow-up: map-flythrough render mode combo, exposed in the GUI for the first time
extern idCVar vis_mapWireHue;
extern idCVar vis_mapBgHue;
extern idCVar vis_mapBgTint;
extern idCVar vis_mapWireBlend;
extern idCVar vis_mapBgBlend;
extern idCVar vis_mapWireOnTop;
static void VisAppendFiles( idStrList & out, const char * dir, const char * ext );
static bool VisLayerListHas( const char * list, const char * name );
static idStr VisLayerListToggle( const char * list, const char * name );
static void VisRememberSearchPath( idCVar & cvar, const char * dir );   // PRD follow-up: persist Register button/vis_*SearchPath directories
static void VisReapplySavedSearchPaths();   // PRD follow-up: re-registers them all at startup (called once from Frame())
static bool VisPickRandomMilkPreset( idStr & out );   // used by the new mash-up mix controls
static void VisToggleEffectsMasterOff();              // used by the new "all effects off" button

// enumerate monitor native resolutions the same way BuildDisplayItems() does
// (that method + its m_displayItems result are private to idVisualizerManager,
// so the GUI -- a free function -- rebuilds an equivalent local list rather
// than reaching into the class, matching this section's existing
// imageBrowseList/presetBrowseList self-contained-scan pattern).
static const int s_visGuiWindowedPresets[][2] = { { 1280, 720 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 } };
// mirrors the DISP_FS_MONITOR/DISP_WINDOWED anonymous enum defined later in
// this file (visDisplayItem_t::kind) -- can't forward-declare an anonymous
// enum, so these are separately-named equivalents for the GUI's local list.
static const int kVisGuiDispFsMonitor = 0;
static const int kVisGuiDispWindowed  = 1;

// PRD Pillar C (direct, repeated user request: "i keep asking for this ...
// i should be able to right click on a slider or dropdown and bind it to an
// audio band or midi input or mouse location x/y ... documented as
// functionalities of zgameeditor"). Shared per-target route controls --
// source picker (with the existing MIDI-learn right-click), amount/base,
// FR-C8 shape/param, invert. Extracted verbatim from the Modulation routing
// table's per-row loop body so BOTH that table AND the new right-click
// popup on each bindable slider below draw the exact same controls -- one
// target, one place its binding UI is implemented.
static void VisDrawModRouteCoreControls( int t ) {
    const int src = s_routes[t].source;
    const idStr preview = ( s_midiLearnTarget == t ) ? idStr( "listening..." ) : VisFormatRouteSource( s_routes[t] );
    ImGui::SetNextItemWidth( 140 );
    if ( ImGui::BeginCombo( "##source", preview.c_str() ) ) {
        for ( int s = 0; s < VMS_MIDICC_PARAM; s++ ) {   // exclude the "midicc:param"/"midinote:vel"/"band:param" placeholders -- only meaningful via vis_route's text parser
            if ( ImGui::Selectable( s_sourceNames[s], s == src ) ) {
                s_routes[t].source = s;
            }
        }
        const int bandCount = g_audioAnalyzer.GetBandCount();
        for ( int b = 0; b < bandCount; b++ ) {
            ImGui::PushID( 1000 + b );
            const bool isThisBand = ( src == VMS_BAND && s_routes[t].bandIndex == b );
            if ( ImGui::Selectable( va( "band %d", b ), isThisBand ) ) {
                s_routes[t].source = VMS_BAND;
                s_routes[t].bandIndex = b;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if ( ImGui::BeginPopupContextItem( "##learnctx" ) ) {
        if ( ImGui::Selectable( "MIDI Learn (move a CC knob next)" ) ) {
            VisArmMidiLearn( t );
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth( 80 );
    ImGui::DragFloat( "amount", &s_routes[t].amount, 0.01f );
    ImGui::SameLine();
    ImGui::SetNextItemWidth( 80 );
    ImGui::DragFloat( "base", &s_routes[t].base, 0.01f );

    ImGui::SameLine();
    ImGui::SetNextItemWidth( 90 );
    const int shapeIdx = ( s_routes[t].shape >= 0 && s_routes[t].shape < VSHAPE_COUNT ) ? s_routes[t].shape : VSHAPE_LINEAR;
    if ( ImGui::BeginCombo( "##shape", s_shapeNames[shapeIdx] ) ) {
        for ( int sh = 0; sh < VSHAPE_COUNT; sh++ ) {
            if ( ImGui::Selectable( s_shapeNames[sh], sh == shapeIdx ) ) {
                s_routes[t].shape = sh;
            }
        }
        ImGui::EndCombo();
    }
    if ( shapeIdx == VSHAPE_EXP || shapeIdx == VSHAPE_LOG ||
         shapeIdx == VSHAPE_THRESHOLD || shapeIdx == VSHAPE_QUANTIZE ) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth( 70 );
        ImGui::DragFloat( "##shapeParam", &s_routes[t].shapeParam, 0.01f );
        if ( shapeIdx == VSHAPE_THRESHOLD ) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth( 70 );
            ImGui::DragFloat( "##shapeParam2", &s_routes[t].shapeParam2, 0.01f );
        }
    }
    ImGui::SameLine();
    ImGui::Checkbox( "inv", &s_routes[t].invert );
}

// PRD Pillar C: the actual ZGE-style universal-binding gesture -- right-
// click ANY bindable slider (not just a row buried in the separate
// Modulation routing panel) to bind it to an audio band, MIDI CC/note, or
// mouse X/Y. Draws the real ImGui::SliderFloat unchanged (so dragging it
// still edits the underlying cvar/value exactly as before -- the modulation
// is a separate additive offset on top, same pattern already proven by
// warpamountmod/layerscalemod/hueshiftmod), then -- since every call site
// below passes an already-registered target (resolved once at startup in
// VisInitModTargetsOnce, so this is never -1 in practice) -- shows a live
// "-> value" readout while a route is actually driving it, plus the
// right-click popup with VisDrawModRouteCoreControls scoped to just this
// one target.
// resetValue (direct user report: "i'd like to be able to type in a value on
// some of these parameters... i cant just drag it to 0") defaults to 0.0f,
// the right neutral for most sliders here (Hue shift, Position X/Y, Warp/
// Bloom amounts) -- callers whose real default isn't 0 (e.g. Layer scale's
// 1.0) pass their own. Typing an exact value doesn't need new code: this
// ImGui build already turns Ctrl+Click OR a double-click on any slider into
// a text-entry box (see SliderBehavior's `double_clicked`/`g.IO.KeyCtrl`
// check in imgui_widgets.cpp) -- that's a *different* gesture from "reset",
// so Reset gets its own explicit button rather than also overloading
// double-click, which would fight ImGui's own built-in text-entry trigger.
static bool VisBindableSliderFloat( const char * label, int target, float * v, float vMin, float vMax, float resetValue = 0.0f ) {
    const bool changed = ImGui::SliderFloat( label, v, vMin, vMax );
    if ( target >= 0 && target < s_routes.Num() ) {
        // Fixed real bug: BeginPopupContextItem(NULL) attaches to whatever
        // ImGui item was submitted MOST RECENTLY -- it MUST run immediately
        // after the slider, before any other item, or a later one silently
        // steals the right-click target.
        const bool bound = s_routes[target].source != VMS_NONE;
        if ( ImGui::BeginPopupContextItem() ) {
            ImGui::Text( "Bind '%s' to a modulation source", label );
            ImGui::Separator();
            VisDrawModRouteCoreControls( target );
            ImGui::EndPopup();
        }
        // Direct user report: right-click still doesn't open the bind popup
        // even after the ordering fix above. Root cause not conclusively
        // isolated (Win32/ImGui mouse-input plumbing reviewed and looks
        // correct; remote synthetic-click testing -- both physical
        // SetCursorPos+mouse_event and message-queue PostMessage -- could not
        // reproduce a working right-click either, which is itself inconclusive
        // since precisely timed synthetic input has real limitations this
        // session's remote/scripted workflow can't fully rule out). Rather
        // than keep guessing at the exact input-plumbing issue, this adds a
        // guaranteed-reliable LEFT-CLICK fallback: an explicit small button
        // that opens the identical bind popup via a normal button click --
        // ImGui buttons are unambiguously well-tested, so this works
        // regardless of whatever the right-click issue turns out to be.
        // Right-click stays wired above in case it's actually fine for real
        // human input and this was purely a remote-testing artifact.
        //
        // Fixed real bug: this button was first added on the SAME line as
        // the slider (via SameLine()), right after the slider's own trailing
        // label -- this panel's window doesn't wrap, and several rows'
        // labels already run close to (or past) the fixed 500px window
        // width (confirmed by a real screenshot: "Effect fills whole window
        // (not the OS displ..." was already visibly clipped before this
        // change), so the button was pushed off-screen and unreachable/
        // invisible on every row that has a label of any real length --
        // including "Hue shift". Drawing it on its OWN line (no leading
        // SameLine) guarantees it stays within the window regardless of how
        // long the slider's own label is.
        ImGui::PushID( target );
        if ( ImGui::SmallButton( bound ? "bound" : "bind" ) ) {
            ImGui::OpenPopup( "##bindbtnpopup" );
        }
        if ( ImGui::BeginPopup( "##bindbtnpopup" ) ) {
            ImGui::Text( "Bind '%s' to a modulation source", label );
            ImGui::Separator();
            VisDrawModRouteCoreControls( target );
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if ( ImGui::SmallButton( "Reset" ) ) { *v = resetValue; }
        ImGui::PopID();
        if ( bound ) {
            ImGui::SameLine();
            ImGui::TextColored( ImVec4( 0.3f, 0.9f, 0.3f, 1.0f ), "-> %.3f", *v + VisMod( target ) );
        }
    }
    return changed;
}

// Int-slider counterpart of VisBindableSliderFloat above (e.g. Kaleidoscope's
// fold count) -- modulation is still tracked/shaped as a float internally
// (VisMod), just rounded to the nearest int for the live readout, matching
// how the actual consumption site applies it (idMath::Rint + clamp).
static bool VisBindableSliderInt( const char * label, int target, int * v, int vMin, int vMax, int resetValue = 0 ) {
    const bool changed = ImGui::SliderInt( label, v, vMin, vMax );
    if ( target >= 0 && target < s_routes.Num() ) {
        // See VisBindableSliderFloat's comment -- same fix, same reason.
        const bool bound = s_routes[target].source != VMS_NONE;
        if ( ImGui::BeginPopupContextItem() ) {
            ImGui::Text( "Bind '%s' to a modulation source", label );
            ImGui::Separator();
            VisDrawModRouteCoreControls( target );
            ImGui::EndPopup();
        }
        // See VisBindableSliderFloat's comment -- same fix (own line, not
        // SameLine(), so it can't be clipped off the window's right edge).
        ImGui::PushID( target );
        if ( ImGui::SmallButton( bound ? "bound" : "bind" ) ) {
            ImGui::OpenPopup( "##bindbtnpopup" );
        }
        if ( ImGui::BeginPopup( "##bindbtnpopup" ) ) {
            ImGui::Text( "Bind '%s' to a modulation source", label );
            ImGui::Separator();
            VisDrawModRouteCoreControls( target );
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if ( ImGui::SmallButton( "Reset" ) ) { *v = resetValue; }
        ImGui::PopID();
        if ( bound ) {
            const int modded = idMath::ClampInt( vMin, vMax, *v + (int)idMath::Rint( VisMod( target ) ) );
            ImGui::SameLine();
            ImGui::TextColored( ImVec4( 0.3f, 0.9f, 0.3f, 1.0f ), "-> %d", modded );
        }
    }
    return changed;
}

static void VisDrawImGuiLayerEditor() {
    if ( !vis_imguiEditor.GetBool() ) {
        return;
    }
    VisInitModTargets();

    // PRD (post-M4 follow-up): the layer editor now lives in its own real
    // top-level OS window (win_imgui.cpp), which already provides a real
    // title bar/minimize/maximize/close/resize border -- ImGui's OWN
    // window chrome (its default title bar, resize grip, movable
    // positioning) would just be a second, redundant, confusing layer of
    // "window within a window" nested inside that (a direct user report:
    // "why is the imGUI not stretching to the window it's in... why is
    // [the game window] in a normal windows GUI while our Visualizer Layer
    // Editor is not"). Pin this ImGui panel to (0,0) at exactly the real
    // OS window's current client size every frame (io.DisplaySize is
    // already that size -- ImGui_ImplWin32_NewFrame reads it from
    // s_imguiHwnd's client rect each frame) and strip all of ImGui's own
    // chrome, so it reads as one seamless window rather than two nested ones.
    ImGui::SetNextWindowPos( ImVec2( 0.0f, 0.0f ) );
    ImGui::SetNextWindowSize( ImGui::GetIO().DisplaySize );
    if ( !ImGui::Begin( "Visualizer Layer Editor", NULL,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings ) ) {
        ImGui::End();
        return;
    }

    if ( ImGui::CollapsingHeader( "Effect", ImGuiTreeNodeFlags_DefaultOpen ) ) {
        bool show = vis_show.GetBool();
        if ( ImGui::Checkbox( "vis_show", &show ) ) { vis_show.SetBool( show ); }
        ImGui::SameLine();
        bool hud = vis_hud.GetBool();
        if ( ImGui::Checkbox( "HUD", &hud ) ) { vis_hud.SetBool( hud ); }
        ImGui::SameLine();
        bool fullscreen = vis_fullscreen.GetBool();
        // vis_fullscreen controls the EFFECT'S OWN draw area (whole client
        // rect vs. a smaller bottom panel), NOT whether the game WINDOW
        // itself is fullscreen vs windowed -- those are unrelated concepts
        // (window/display mode is vis_display/vis_displays/VisCycleDisplayMode,
        // the "F" hotkey, exposed as its own button in the Display section
        // below). Named explicitly here to avoid exactly the confusion a
        // bare "Fullscreen" label caused (direct user report: "i do not
        // know what effect -> fullscreen does because it doesn't control
        // if the display is full screen or not").
        if ( ImGui::Checkbox( "Effect fills whole window (not the OS display mode)", &fullscreen ) ) { vis_fullscreen.SetBool( fullscreen ); }

        int effect = vis_effect.GetInteger();
        static const char * effectNames[] = { "Bars", "Radial", "Scope", "Ring", "Particles", "Spectrogram", "Starfield", "Phase Scope" };
        // "##combo" disambiguates this widget's ID from the "Effect"
        // CollapsingHeader just opened above -- both are direct children of
        // the same window/ID scope, so without a suffix they'd hash to the
        // identical ID and ImGui's own conflicting-ID detector (on by
        // default) would fire the moment either was hovered (confirmed via
        // a direct user report: "mousing over the effect dropdown... 2
        // visible items with conflicting id"). The label text shown next to
        // the combo is still just "Effect" -- ImGui only hashes/hides
        // everything from "##" onward, it doesn't affect what's displayed.
        if ( ImGui::Combo( "Effect##combo", &effect, effectNames, 8 ) ) { vis_effect.SetInteger( effect ); }
        ImGui::SameLine();
        if ( ImGui::Button( "Next##effect" ) ) { VisGuiRunCommand( "vis_nextEffect" ); }

        int bands = vis_bands.GetInteger();
        if ( ImGui::SliderInt( "Spectrum bands", &bands, 1, 64 ) ) { vis_bands.SetInteger( bands ); }

        float beatSens = g_audioAnalyzer.GetBeatSensitivity();
        if ( ImGui::SliderFloat( "Beat sensitivity", &beatSens, 1.0f, 4.0f ) ) { g_audioAnalyzer.SetBeatSensitivity( beatSens ); }

        int palette = vis_palette.GetInteger();
        static const char * paletteNames[] = { "Rainbow", "Fire", "Ocean", "Synthwave", "Mono" };
        if ( ImGui::Combo( "Palette", &palette, paletteNames, 5 ) ) { vis_palette.SetInteger( palette ); }
        float hueShift = vis_hueShiftGlobal.GetFloat();
        // PRD Pillar C (direct, repeated user request: "we still cannot bind
        // audio band levels to effect options like warp amount or bloom
        // threshold ... this is the main goal of trying to implement
        // ZgameEditor style editing ... i should be able to right click on
        // a slider ... and bind it to an audio band or midi input or mouse
        // location x/y"). Every slider below is now a VisBindableSliderFloat/
        // Int -- right-click it to open the exact same source/shape/amount/
        // base/invert controls as the Modulation routing panel, scoped to
        // that one parameter, without hunting for a separate row. A green
        // "-> value" readout appears next to any slider that's actually
        // routed, so it's visually obvious which ones are live.
        if ( VisBindableSliderFloat( "Hue shift", VMT_HUESHIFT_MOD, &hueShift, 0.0f, 1.0f ) ) { vis_hueShiftGlobal.SetFloat( hueShift ); }

        // PRD Pillar C follow-up (direct user report: "all images, effects,
        // layers have scale but also x/y coordinates so things can be moved
        // around in an almost xyz style"). -1..1: left/top edge .. center .. right/bottom edge.
        float effectX = vis_effectX.GetFloat();
        if ( VisBindableSliderFloat( "Position X", VMT_EFFECTX_MOD, &effectX, -1.0f, 1.0f ) ) { vis_effectX.SetFloat( effectX ); }
        float effectY = vis_effectY.GetFloat();
        if ( VisBindableSliderFloat( "Position Y", VMT_EFFECTY_MOD, &effectY, -1.0f, 1.0f ) ) { vis_effectY.SetFloat( effectY ); }

        ImGui::Separator();
        bool feedback = vis_feedback.GetBool();
        if ( ImGui::Checkbox( "Feedback trail", &feedback ) ) { vis_feedback.SetBool( feedback ); }
        float feedbackDecay = vis_feedbackDecay.GetFloat();
        if ( VisBindableSliderFloat( "Feedback decay", VMT_FEEDBACKDECAY_MOD, &feedbackDecay, 0.5f, 0.995f, 0.94f ) ) { vis_feedbackDecay.SetFloat( feedbackDecay ); }
        float feedbackZoom = vis_feedbackZoom.GetFloat();
        if ( VisBindableSliderFloat( "Feedback zoom", VMT_ZOOM, &feedbackZoom, 0.95f, 1.1f, 1.010f ) ) { vis_feedbackZoom.SetFloat( feedbackZoom ); }

        int warp = vis_warp.GetInteger();
        if ( ImGui::SliderInt( "Warp mode", &warp, 0, 3 ) ) { vis_warp.SetInteger( warp ); }
        int warpMode = vis_warpMode.GetInteger();
        static const char * warpModeNames[] = { "Ripple", "Swirl", "Tunnel", "Fisheye" };
        if ( ImGui::Combo( "Feedback warp shape", &warpMode, warpModeNames, 4 ) ) { vis_warpMode.SetInteger( warpMode ); }
        float warpAmount = vis_warpAmount.GetFloat();
        if ( VisBindableSliderFloat( "Warp amount", VMT_WARPAMOUNT_MOD, &warpAmount, 0.0f, 0.40f, 0.06f ) ) { vis_warpAmount.SetFloat( warpAmount ); }
        float warpFreq = vis_warpFreq.GetFloat();
        if ( VisBindableSliderFloat( "Warp frequency", VMT_WARPFREQ_MOD, &warpFreq, 1.0f, 30.0f, 8.0f ) ) { vis_warpFreq.SetFloat( warpFreq ); }
        float warpSpeed = vis_warpSpeed.GetFloat();
        if ( VisBindableSliderFloat( "Warp speed", VMT_WARPSPEED_MOD, &warpSpeed, 0.0f, 8.0f, 1.5f ) ) { vis_warpSpeed.SetFloat( warpSpeed ); }
        int kaleido = vis_kaleido.GetInteger();
        if ( VisBindableSliderInt( "Kaleidoscope", VMT_KALEIDO_MOD, &kaleido, 0, 16, 0 ) ) { vis_kaleido.SetInteger( kaleido ); }

        ImGui::Separator();
        float bloom = vis_bloom.GetFloat();
        if ( VisBindableSliderFloat( "Bloom", VMT_BLOOM_MOD, &bloom, 0.0f, 1.0f ) ) { vis_bloom.SetFloat( bloom ); }
        float bloomThreshold = vis_bloomThreshold.GetFloat();
        if ( VisBindableSliderFloat( "Bloom threshold", VMT_BLOOMTHRESH_MOD, &bloomThreshold, 0.0f, 1.0f, 0.55f ) ) { vis_bloomThreshold.SetFloat( bloomThreshold ); }
        float bloomRadius = vis_bloomRadius.GetFloat();
        if ( VisBindableSliderFloat( "Bloom radius", VMT_BLOOMRADIUS_MOD, &bloomRadius, 0.0f, 1.0f, 0.6f ) ) { vis_bloomRadius.SetFloat( bloomRadius ); }
        float bloomBeat = vis_bloomBeat.GetFloat();
        if ( ImGui::SliderFloat( "Bloom beat pulse", &bloomBeat, 0.0f, 1.0f ) ) { vis_bloomBeat.SetFloat( bloomBeat ); }

        ImGui::Separator();
        // F1-menu parity: the F11 hotkey's master effects-off toggle had no
        // GUI/console equivalent at all until now (direct user report: "we
        // need all of the options that exist in the ingame engine menu to
        // exist in the imgui menu").
        if ( ImGui::Button( s_milkEffectsMasterOff ? "Restore effects (F11)" : "All effects off (F11)" ) ) {
            VisGuiRunCommand( "vis_effectsOff" );
        }
    }

    // PRD Pillar C real add/remove layer stack (direct user report: "zge
    // allow you to add layers and remove layers. we do not seem to have that
    // functionality"). Empty stack (the default) leaves the single "Effect"
    // combo above in full control, unchanged -- this only takes over once at
    // least one slot has been added. See VisRenderLayerStack's own comment
    // for why it doesn't combine with Feedback trail/Warp/MilkDrop mode yet.
    if ( ImGui::CollapsingHeader( "Layer Stack (ZGE-style add/remove)" ) ) {
        // local copy -- the "Effect" section's effectNames above is scoped to
        // that CollapsingHeader's own { } block, not visible here
        static const char * stackEffectNames[] = { "Bars", "Radial", "Scope", "Ring", "Particles", "Spectrogram", "Starfield", "Phase Scope" };
        // Fixed real bug (direct user report: "layer stack adding a layer
        // just seems to cover the existing visualization"): "Add Layer"
        // always added effectType 0 (Bars) at opacity 1.0/Normal, no matter
        // what the single Effect combo above was currently showing -- and
        // that combo goes fully INACTIVE the instant the stack is non-empty
        // (see the switch in Draw2D). So adding the very first layer always
        // hard-replaced whatever the user was looking at with a fresh Bars
        // view, which reads exactly like "the new layer just covers
        // everything" -- it's not a blend/opacity bug, it's a mode switch
        // the user didn't ask for. Seeding the first layer with the
        // currently-selected effect makes adding it a no-visible-change
        // continuation: opacity/blend only start doing something once a
        // SECOND layer is added on top, which is when they're actually
        // meaningful.
        ImGui::TextWrapped( "Empty stack = plain single Effect above. Adding your first layer here SWITCHES from that single Effect to this independent stack (seeded with whatever the Effect combo above is currently showing, so nothing changes visually right away) -- add more layers on top and adjust their own Opacity/Blend to mix them in. Active only when Feedback trail / Warp / MilkDrop preset mode is off." );
        ImGui::Text( "%d / %d slots", s_layerStack.Num(), VIS_MAX_STACK_LAYERS );
        ImGui::SameLine();
        if ( s_layerStack.Num() < VIS_MAX_STACK_LAYERS ) {
            if ( ImGui::Button( "Add Layer" ) ) {
                const int seedEffect = ( s_layerStack.Num() == 0 ) ? vis_effect.GetInteger() : 0;
                VisGuiRunCommand( va( "vis_stackAdd %d", seedEffect ) );
            }
        } else {
            ImGui::TextDisabled( "Add Layer (cap reached)" );
        }
        ImGui::SameLine();
        if ( ImGui::Button( "Clear Stack" ) ) { VisGuiRunCommand( "vis_stackClear" ); }

        for ( int i = 0; i < s_layerStack.Num(); i++ ) {
            visStackLayer_t & layer = s_layerStack[i];
            ImGui::PushID( i );
            ImGui::Separator();

            bool enabled = layer.enabled;
            if ( ImGui::Checkbox( "##enabled", &enabled ) ) { layer.enabled = enabled; }
            ImGui::SameLine();
            ImGui::SetNextItemWidth( 130 );
            ImGui::Combo( "##effectType", &layer.effectType, stackEffectNames, 8 );
            ImGui::SameLine();
            if ( ImGui::Button( "Up" ) && i > 0 ) { VisStackMoveLayer( i, i - 1 ); }
            ImGui::SameLine();
            if ( ImGui::Button( "Down" ) && i < s_layerStack.Num() - 1 ) { VisStackMoveLayer( i, i + 1 ); }
            ImGui::SameLine();
            if ( ImGui::Button( "Remove" ) ) { VisStackRemoveLayer( i ); ImGui::PopID(); break; }

            ImGui::SetNextItemWidth( 140 );
            ImGui::SliderFloat( "Opacity", &layer.opacity, 0.0f, 1.0f );
            ImGui::SameLine();
            // PRD Pillar C follow-up (direct user report: "the zge layers
            // only have additive but need all the modes, additive,
            // subtractive, inverse and the rest") -- was a bool "Additive"
            // checkbox, now a full blend-mode combo backed by real
            // glBlendEquation support (see GLState.h/GL_State/Material.cpp).
            ImGui::SetNextItemWidth( 130 );
            ImGui::Combo( "Blend##blendmode", &layer.blendMode, s_visBlendModeNames, VISBLEND_COUNT );

            // PRD Pillar C follow-up (direct user report: "all images,
            // effects, layers have scale but also x/y coordinates so things
            // can be moved around in an almost xyz style"). Bound per-slot
            // (s_slotTarget[i][VSP_POSX/POSY]) so two layers can each be
            // routed to a different source independently, same as this
            // slot's other per-slot targets (opacity/pspawn/pscale/...).
            ImGui::SetNextItemWidth( 140 );
            VisBindableSliderFloat( "Position X", s_slotTarget[i][VSP_POSX], &layer.posX, -1.0f, 1.0f );
            ImGui::SameLine();
            ImGui::SetNextItemWidth( 140 );
            VisBindableSliderFloat( "Position Y", s_slotTarget[i][VSP_POSY], &layer.posY, -1.0f, 1.0f );

            ImGui::PopID();
        }
    }

    if ( ImGui::CollapsingHeader( "Image Layer" ) ) {
        bool layerOn = vis_layer.GetBool();
        if ( ImGui::Checkbox( "Enable image layer", &layerOn ) ) { vis_layer.SetBool( layerOn ); }
        // PRD Pillar C follow-up (direct user report: "image layer still does
        // not have a way to bind it to an input of audio band or midi or any
        // other"): scale/alpha/hue are all right-click-bindable now, same as
        // the Effect panel.
        float layerScale = vis_layerScale.GetFloat();
        if ( VisBindableSliderFloat( "Layer scale", VMT_LAYERSCALE_MOD, &layerScale, 0.05f, 4.0f, 1.0f ) ) { vis_layerScale.SetFloat( layerScale ); }
        float layerAlpha = vis_layerAlpha.GetFloat();
        if ( VisBindableSliderFloat( "Layer alpha", VMT_LAYERALPHA_MOD, &layerAlpha, 0.0f, 1.0f, 0.8f ) ) { vis_layerAlpha.SetFloat( layerAlpha ); }
        bool layerColorize = vis_layerColorize.GetBool();
        if ( ImGui::Checkbox( "Tint with hue below", &layerColorize ) ) { vis_layerColorize.SetBool( layerColorize ); }
        float layerHue = vis_layerHue.GetFloat();
        if ( VisBindableSliderFloat( "Layer hue", VMT_LAYERHUE_MOD, &layerHue, 0.0f, 1.0f ) ) { vis_layerHue.SetFloat( layerHue ); }
        float layerX = vis_layerX.GetFloat();
        if ( VisBindableSliderFloat( "Layer position X", VMT_LAYERX_MOD, &layerX, -1.0f, 1.0f ) ) { vis_layerX.SetFloat( layerX ); }
        float layerY = vis_layerY.GetFloat();
        if ( VisBindableSliderFloat( "Layer position Y", VMT_LAYERY_MOD, &layerY, -1.0f, 1.0f ) ) { vis_layerY.SetFloat( layerY ); }
        if ( !layerColorize ) {
            ImGui::SameLine();
            ImGui::TextDisabled( "(check 'Tint with hue below' to use this)" );
        }
        ImGui::SameLine();
        // F1-menu parity: the IMAGES tab's row 0 ("[ clear all layers ]")
        // had no GUI equivalent -- only unchecking every image one at a time.
        if ( ImGui::Button( "Clear all" ) ) {
            vis_layerList.SetString( "" );
            vis_layer.SetBool( false );
        }

        // PRD (post-M4 follow-up, direct user report: "image layer doesn't
        // allow you to select an image"): this section had scale/alpha/tint
        // knobs but no way to actually PICK which image(s) drive the layer
        // -- that only existed in the F1 overlay's IMAGES tab. Reuses the
        // exact same images/ scan + vis_layerList ';'-separated toggle
        // mechanism that tab already uses (VisLayerListHas/Toggle), so
        // ticking a box here and ticking it in the IMAGES tab stay in sync
        // (same underlying cvar).
        ImGui::Separator();
        ImGui::TextUnformatted( "Active image(s) (images/*.tga/.png/.jpg):" );
        static idStrList imageBrowseList;
        static bool imageListScanned = false;
        if ( !imageListScanned || ImGui::Button( "Rescan images/" ) ) {
            imageBrowseList.Clear();
            VisAppendFiles( imageBrowseList, "images", ".tga" );
            VisAppendFiles( imageBrowseList, "images", ".png" );
            VisAppendFiles( imageBrowseList, "images", ".jpg" );
            imageListScanned = true;
        }
        if ( imageBrowseList.Num() == 0 ) {
            ImGui::TextDisabled( "(none found under images/)" );
        } else {
            ImGui::BeginChild( "##imagebrowsebox", ImVec2( 0, 100 ), true );
            for ( int i = 0; i < imageBrowseList.Num(); i++ ) {
                const char * path = imageBrowseList[i].c_str();
                bool active = VisLayerListHas( vis_layerList.GetString(), path );
                ImGui::PushID( i );
                if ( ImGui::Checkbox( "##imgactive", &active ) ) {
                    vis_layerList.SetString( VisLayerListToggle( vis_layerList.GetString(), path ).c_str() );
                }
                ImGui::SameLine();
                idStr label = imageBrowseList[i];
                label.StripPath();
                ImGui::TextUnformatted( label.c_str() );
                ImGui::PopID();
            }
            ImGui::EndChild();
        }
    }

    if ( ImGui::CollapsingHeader( "Audio Source & Playback" ) ) {
        static char playBuf[256] = "";
        ImGui::InputTextWithHint( "##playpath", "music/track.ogg or playlists/list.m3u", playBuf, sizeof( playBuf ) );
        ImGui::SameLine();
        if ( ImGui::Button( "Play" ) ) { VisGuiRunCommand( va( "vis_play \"%s\"", playBuf ) ); }
        if ( ImGui::Button( "Stop" ) ) { VisGuiRunCommand( "vis_stop" ); }
        ImGui::SameLine();
        if ( ImGui::Button( "Prev" ) ) { VisGuiRunCommand( "vis_prev" ); }
        ImGui::SameLine();
        if ( ImGui::Button( "Next##track" ) ) { VisGuiRunCommand( "vis_next" ); }
        ImGui::SameLine();
        if ( ImGui::Button( "Status (console)" ) ) { VisGuiRunCommand( "vis_status" ); }

        // F1-menu parity (direct user report: "we need all of the options
        // that exist in the ingame engine menu to exist in the imgui
        // menu"): the MUSIC/PLAYLISTS tabs let you browse+click a file;
        // this panel previously only had a blind text field. Mirrors
        // OpenMenu()'s exact scan (base/music/*, base/playlists/*.m3u(8)).
        ImGui::Separator();
        static idStrList musicBrowseList, playlistBrowseList;
        static bool audioListsScanned = false;
        if ( !audioListsScanned || ImGui::Button( "Rescan music/ + playlists/" ) ) {
            musicBrowseList.Clear();
            static const char * const kMusicExts[] = { ".wav", ".ogg", ".mp3", ".flac", ".m4a", ".aac", ".opus", ".wma" };
            for ( int e = 0; e < 8; e++ ) {
                VisAppendFiles( musicBrowseList, "music", kMusicExts[e] );
            }
            playlistBrowseList.Clear();
            VisAppendFiles( playlistBrowseList, "playlists", ".m3u" );
            VisAppendFiles( playlistBrowseList, "playlists", ".m3u8" );
            audioListsScanned = true;
        }
        ImGui::TextDisabled( "%d music file(s), %d playlist(s)", musicBrowseList.Num(), playlistBrowseList.Num() );
        if ( musicBrowseList.Num() + playlistBrowseList.Num() > 0 ) {
            ImGui::BeginChild( "##audiobrowsebox", ImVec2( 0, 100 ), true );
            for ( int i = 0; i < musicBrowseList.Num(); i++ ) {
                idStr label = musicBrowseList[i];
                label.StripPath();
                ImGui::PushID( i );
                if ( ImGui::Selectable( label.c_str() ) ) {
                    VisGuiRunCommand( va( "vis_play \"%s\"", musicBrowseList[i].c_str() ) );
                }
                ImGui::PopID();
            }
            for ( int i = 0; i < playlistBrowseList.Num(); i++ ) {
                idStr label = playlistBrowseList[i];
                label.StripPath();
                label = va( "[playlist] %s", label.c_str() );
                ImGui::PushID( 100000 + i );
                if ( ImGui::Selectable( label.c_str() ) ) {
                    VisGuiRunCommand( va( "vis_play \"%s\"", playlistBrowseList[i].c_str() ) );
                }
                ImGui::PopID();
            }
            ImGui::EndChild();
        }

        bool autoAdvance = vis_autoAdvance.GetBool();
        if ( ImGui::Checkbox( "Auto-advance playlist", &autoAdvance ) ) { vis_autoAdvance.SetBool( autoAdvance ); }
        bool autoLoopback = vis_autoLoopback.GetBool();
        if ( ImGui::Checkbox( "Auto-arm system loopback", &autoLoopback ) ) { vis_autoLoopback.SetBool( autoLoopback ); }
        bool bgAudio = vis_backgroundAudio.GetBool();
        if ( ImGui::Checkbox( "Keep reactive when unfocused", &bgAudio ) ) { vis_backgroundAudio.SetBool( bgAudio ); }

        ImGui::Text( "Source:" );
        ImGui::SameLine();
        if ( ImGui::Button( "Engine" ) ) { VisGuiRunCommand( "vis_source engine" ); }
        ImGui::SameLine();
        if ( ImGui::Button( "System loopback" ) ) { VisGuiRunCommand( "vis_source loopback" ); }
        ImGui::SameLine();
        ImGui::TextDisabled( "(%s)", g_audioAnalyzer.GetSourceMode() == AudioSourceMode::WASAPI_LOOPBACK ? "loopback" : "engine" );

        // F1-menu parity: the DEVICES tab shows named, clickable devices with
        // a live "* active" marker -- this panel previously only had a blind
        // numeric index field. Mirrors OpenMenu()'s exact list construction
        // (synthetic "System default" entry first, then EnumerateDevices),
        // and calls SetCaptureDevice directly, exactly like MenuActivate does.
        static std::vector<AudioDeviceInfo> deviceBrowseList;
        static bool deviceListScanned = false;
        if ( !deviceListScanned || ImGui::Button( "Rescan capture devices" ) ) {
            std::vector<AudioDeviceInfo> devs;
            AudioAnalyzer::EnumerateDevices( devs );
            deviceBrowseList.clear();
            AudioDeviceInfo def;
            def.name = "System default output (loopback)";
            def.id = "";
            def.isCapture = false;
            deviceBrowseList.push_back( def );
            for ( size_t i = 0; i < devs.size(); i++ ) {
                deviceBrowseList.push_back( devs[i] );
            }
            deviceListScanned = true;
        }
        ImGui::BeginChild( "##devicebrowsebox", ImVec2( 0, 90 ), true );
        for ( size_t i = 0; i < deviceBrowseList.size(); i++ ) {
            const AudioDeviceInfo & d = deviceBrowseList[i];
            const bool active = ( d.id == g_audioAnalyzer.GetCaptureDeviceId() &&
                                  d.isCapture == g_audioAnalyzer.GetCaptureIsCapture() &&
                                  g_audioAnalyzer.GetSourceMode() == AudioSourceMode::WASAPI_LOOPBACK );
            const char * tag = d.name.empty() ? "" : ( d.isCapture ? " [in]" : " [out]" );
            idStr label = va( "%s%s%s", d.name.c_str(), tag, active ? "  *" : "" );
            ImGui::PushID( (int)i );
            if ( ImGui::Selectable( label.c_str(), active ) ) {
                g_audioAnalyzer.SetCaptureDevice( d.id, d.isCapture );
            }
            ImGui::PopID();
        }
        ImGui::EndChild();

        ImGui::Separator();
        static char presetSaveBuf[256] = "";
        ImGui::InputTextWithHint( "##presetsavename", "preset name (.cfg)", presetSaveBuf, sizeof( presetSaveBuf ) );
        ImGui::SameLine();
        if ( ImGui::Button( "Save current as .cfg" ) ) { VisGuiRunCommand( va( "vis_presetSave \"%s\"", presetSaveBuf ) ); }
        if ( ImGui::Button( "Load .cfg preset" ) ) { VisGuiRunCommand( va( "vis_presetLoad \"%s\"", presetSaveBuf ) ); }
    }

    if ( ImGui::CollapsingHeader( "Preset Cycling (.cfg + .milk)" ) ) {
        bool cycle = vis_presetCycle.GetBool();
        if ( ImGui::Checkbox( "Auto-cycle presets", &cycle ) ) { vis_presetCycle.SetBool( cycle ); }
        float cycleSecs = vis_presetCycleSecs.GetFloat();
        if ( ImGui::SliderFloat( "Cycle interval (s)", &cycleSecs, 2.0f, 600.0f ) ) { vis_presetCycleSecs.SetFloat( cycleSecs ); }
        bool onBeat = vis_presetCycleOnBeat.GetBool();
        if ( ImGui::Checkbox( "Cut on next beat (hard cut)", &onBeat ) ) { vis_presetCycleOnBeat.SetBool( onBeat ); }
        bool shuffle = vis_presetCycleShuffle.GetBool();
        if ( ImGui::Checkbox( "Shuffle order", &shuffle ) ) { vis_presetCycleShuffle.SetBool( shuffle ); }
        float softCutSecs = vis_milkSoftCutSecs.GetFloat();
        if ( ImGui::SliderFloat( "Soft-cut crossfade (s)", &softCutSecs, 0.1f, 30.0f ) ) { vis_milkSoftCutSecs.SetFloat( softCutSecs ); }

        // F1-menu parity: SPACE/BACKSPACE step the .cfg cycle list -- distinct
        // from the Soft/Hard cut buttons below, which only drive .milk state.
        if ( ImGui::Button( "Prev preset (.cfg)" ) ) { VisGuiRunCommand( "vis_presetPrev" ); }
        ImGui::SameLine();
        if ( ImGui::Button( "Next preset (.cfg)" ) ) { VisGuiRunCommand( "vis_presetNext" ); }

        if ( ImGui::Button( "Soft cut now" ) ) { VisGuiRunCommand( "vis_milkCut" ); }
        ImGui::SameLine();
        if ( ImGui::Button( "Hard cut now" ) ) { VisGuiRunCommand( "vis_milkCut hard" ); }
        ImGui::SameLine();
        if ( ImGui::Button( "Clear mash-up" ) ) { VisGuiRunCommand( "vis_milkMashClear" ); }

        if ( s_milkActive ) {
            ImGui::Text( "Current .milk preset: %s (rating %d/5)", s_milkPreset.GetName().c_str(), VisGetMilkRating( s_milkPreset.GetName().c_str() ) );
            for ( int r = 0; r <= 5; r++ ) {
                ImGui::SameLine();
                ImGui::PushID( r );
                if ( ImGui::SmallButton( va( "%d", r ) ) ) { VisSetMilkRating( s_milkPreset.GetName().c_str(), r ); }
                ImGui::PopID();
            }
            if ( s_milkCutInProgress ) {
                ImGui::ProgressBar( idMath::ClampFloat( 0.0f, 1.0f, s_milkMashMix ), ImVec2( -1, 0 ), "soft-cut mix" );
            }
            // F1-menu parity: K_A/K_Z nudge the mash-up mix toward B/A by 0.1
            // per press (K_A also starts a random-B mash-up if none is active
            // yet); this panel previously only had the "Clear mash-up" endpoint.
            if ( ImGui::Button( "Mash toward B (A)" ) ) {
                if ( !s_milkActiveB ) {
                    idStr nextPath;
                    if ( VisPickRandomMilkPreset( nextPath ) ) {
                        if ( VisLoadMilkPresetB( nextPath.c_str() ) && s_milkActiveB ) {
                            s_milkMashMix = 0.5f;
                        }
                    }
                } else {
                    s_milkMashMix = idMath::ClampFloat( 0.0f, 1.0f, s_milkMashMix + 0.1f );
                }
            }
            ImGui::SameLine();
            if ( ImGui::Button( "Mash toward A (Z)" ) && s_milkActiveB ) {
                s_milkMashMix -= 0.1f;
                if ( s_milkMashMix <= 0.0f ) {
                    VisUnloadMilkPresetB();
                    s_milkMashMix = 0.0f;
                }
            }
        } else {
            ImGui::TextDisabled( "No .milk preset currently active" );
        }

        // F1-menu parity: the PRESETS tab's row 0 ("[ + save current as new
        // preset ]") and its browsable presets/*.cfg list had no ImGui
        // equivalent -- this panel could only load a .cfg by typing its
        // exact bare name blind.
        ImGui::Separator();
        if ( ImGui::Button( "Save current as next custom-NN" ) ) { VisGuiRunCommand( "vis_presetSaveCustom" ); }
        static idStrList cfgPresetBrowseList;
        static bool cfgPresetListScanned = false;
        if ( !cfgPresetListScanned || ImGui::Button( "Rescan presets/*.cfg" ) ) {
            cfgPresetBrowseList.Clear();
            VisAppendFiles( cfgPresetBrowseList, "presets", ".cfg" );
            cfgPresetListScanned = true;
        }
        if ( cfgPresetBrowseList.Num() > 0 ) {
            ImGui::BeginChild( "##cfgpresetbrowsebox", ImVec2( 0, 100 ), true );
            for ( int i = 0; i < cfgPresetBrowseList.Num(); i++ ) {
                idStr label = cfgPresetBrowseList[i];
                label.StripPath();
                ImGui::PushID( i );
                if ( ImGui::Selectable( label.c_str() ) ) {
                    VisGuiRunCommand( va( "vis_presetLoadPath \"%s\"", cfgPresetBrowseList[i].c_str() ) );
                }
                ImGui::PopID();
            }
            ImGui::EndChild();
        }
    }

    if ( ImGui::CollapsingHeader( "MilkDrop Presets (custom paths)" ) ) {
        ImGui::TextWrapped( "Load .milk presets from an external pack, not just the bundled presets/milk/." );
        // PRD follow-up (direct user report: registered an absolute path
        // like "C:\DOOM-3-BFG\milkdrop_presets\presets-cream-of-the-crop-
        // master" here directly and got 0 results scanning). Two things
        // fixed this: (1) FileSystem.cpp's RegisterModSearchPath/BuildOSPath
        // now correctly handle an absolute path typed into EITHER field
        // below (previously silently produced a bogus nested path), so
        // pasting a full path now works too; (2) the resolved-directory
        // preview + Scan's console printf (below) make it possible to see
        // exactly what got searched either way, instead of a bare "0 found."
        ImGui::TextWrapped( "Register EITHER a folder name relative to your install (e.g. \"milkdrop_presets\") OR a full absolute path -- both now work." );
        static char presetPathBuf[256] = "";
        ImGui::InputTextWithHint( "##presetsearchpath", "milkdrop_presets  OR  C:\\path\\to\\milkdrop_presets", presetPathBuf, sizeof( presetPathBuf ) );
        ImGui::SameLine();
        if ( ImGui::Button( "Register##preset" ) ) {
            fileSystem->RegisterModSearchPath( presetPathBuf );
            s_lastMilkSearchPath = presetPathBuf;
            VisRememberSearchPath( vis_presetSearchPathSaved, presetPathBuf );   // PRD follow-up: remember across launches
        }

        char subdirBuf[256];
        idStr::snPrintf( subdirBuf, sizeof( subdirBuf ), "%s", vis_milkPresetSubdir.GetString() );
        ImGui::InputTextWithHint( "##presetsubdir", "subfolder inside it, e.g. presets-cream-of-the-crop-master", subdirBuf, sizeof( subdirBuf ) );
        if ( idStr::Cmp( subdirBuf, vis_milkPresetSubdir.GetString() ) != 0 ) {
            vis_milkPresetSubdir.SetString( subdirBuf );
        }
        if ( presetPathBuf[0] != '\0' ) {
            ImGui::TextDisabled( "will search: %s", fileSystem->BuildOSPath( cvarSystem->GetCVarString( "fs_basepath" ), presetPathBuf, vis_milkPresetSubdir.GetString() ) );
        }

        static idStrList presetBrowseList;
        static int presetBrowseSel = -1;
        if ( ImGui::Button( "Scan" ) ) {
            presetBrowseList.Clear();
            VisAppendFiles( presetBrowseList, vis_milkPresetSubdir.GetString(), ".milk" );
            // PRD follow-up (direct user report: registered the FULL
            // absolute path all the way down to the pack folder itself
            // -- "C:\...\milkdrop_presets\presets-cream-of-the-crop-master"
            // -- but left the Subdir field set from an earlier attempt at
            // the same value, so the two concatenate into a nonexistent
            // double-nested path and this still finds 0). Self-heal via
            // VisTryFixMilkDoubleNesting (see its own comment for why a
            // naive "rescan the root directly" retry doesn't work -- two
            // separate pre-existing engine bugs in GetFileListTree break
            // both the empty-string and "." root sentinels).
            if ( presetBrowseList.Num() == 0 ) {
                VisTryFixMilkDoubleNesting();
                presetBrowseList.Clear();
                VisAppendFiles( presetBrowseList, vis_milkPresetSubdir.GetString(), ".milk" );
            }
            presetBrowseSel = -1;
            idLib::Printf( "MilkDrop preset scan: searched '%s' (recursive) for .milk -- %d found\n",
                fileSystem->BuildOSPath( cvarSystem->GetCVarString( "fs_basepath" ), presetPathBuf, vis_milkPresetSubdir.GetString() ), presetBrowseList.Num() );
        }
        ImGui::SameLine();
        ImGui::TextDisabled( "%d found", presetBrowseList.Num() );

        if ( presetBrowseList.Num() > 0 ) {
            ImGui::BeginChild( "##presetbrowsebox", ImVec2( 0, 120 ), true );
            for ( int i = 0; i < presetBrowseList.Num(); i++ ) {
                idStr label = presetBrowseList[i];
                label.StripPath();
                ImGui::PushID( i );
                if ( ImGui::Selectable( label.c_str(), presetBrowseSel == i ) ) {
                    presetBrowseSel = i;
                }
                ImGui::PopID();
            }
            ImGui::EndChild();
            if ( presetBrowseSel >= 0 && presetBrowseSel < presetBrowseList.Num() ) {
                if ( ImGui::Button( "Load selected preset" ) ) {
                    VisGuiRunCommand( va( "vis_milkPreset \"%s\"", presetBrowseList[presetBrowseSel].c_str() ) );
                }
                ImGui::SameLine();
                if ( ImGui::Button( "Mash in selected preset" ) ) {
                    VisGuiRunCommand( va( "vis_milkMash \"%s\"", presetBrowseList[presetBrowseSel].c_str() ) );
                }
            }
        }
    }

    if ( ImGui::CollapsingHeader( "Map Flythrough (M4.5)" ) ) {
        ImGui::TextWrapped( "Load a DOOM 3 map's geometry-only render world (no entities/AI) for ambient camera flythrough, from a custom path." );
        // PRD follow-up (direct user report: registered an absolute path
        // like "C:\DOOM-3-BFG\map_pack\base" here directly and got 0 maps
        // scanning, despite a real maps/ subfolder existing under it).
        // FileSystem.cpp's RegisterModSearchPath/BuildOSPath now correctly
        // handle an absolute path typed into this field (previously always
        // concatenated it as a RELATIVE segment under fs_basepath, producing
        // a bogus nested path); the resolved-directory preview + Scan's
        // console printf below make the actual searched path visible either
        // way, instead of a bare "0 found."
        ImGui::TextWrapped( "Register EITHER a folder name relative to your install (e.g. \"map_pack/base\") OR a full absolute path -- both now work." );
        static char mapPathBuf[256] = "";
        ImGui::InputTextWithHint( "##mapsearchpath", "map_pack/base  OR  C:\\path\\to\\map_pack\\base", mapPathBuf, sizeof( mapPathBuf ) );
        ImGui::SameLine();
        if ( ImGui::Button( "Register##map" ) ) {
            fileSystem->RegisterModSearchPath( mapPathBuf );
            VisRememberSearchPath( vis_mapSearchPathSaved, mapPathBuf );   // PRD follow-up: remember across launches
        }
        if ( mapPathBuf[0] != '\0' ) {
            ImGui::TextDisabled( "will search: %s", fileSystem->BuildOSPath( cvarSystem->GetCVarString( "fs_basepath" ), mapPathBuf, "maps" ) );
        }

        static idStrList mapBrowseList;
        static int mapBrowseSel = -1;
        if ( ImGui::Button( "Scan for maps" ) ) {
            mapBrowseList.Clear();
            idFileList * looseFiles = fileSystem->ListFilesTree( "maps", ".map", true );
            if ( looseFiles != NULL ) {
                for ( int i = 0; i < looseFiles->GetNumFiles(); i++ ) {
                    mapBrowseList.Append( looseFiles->GetFile( i ) );
                }
                fileSystem->FreeFileList( looseFiles );
            }
            idFileList * resFiles = fileSystem->ListFilesTree( "maps", ".resources", true );
            if ( resFiles != NULL ) {
                for ( int i = 0; i < resFiles->GetNumFiles(); i++ ) {
                    mapBrowseList.Append( resFiles->GetFile( i ) );
                }
                fileSystem->FreeFileList( resFiles );
            }
            idLib::Printf( "Map scan: registered path '%s' resolves to '%s' -- %d file(s) found (searches ALL registered search paths at once, not just this one)\n",
                mapPathBuf, fileSystem->BuildOSPath( cvarSystem->GetCVarString( "fs_basepath" ), mapPathBuf, "maps" ), mapBrowseList.Num() );
            mapBrowseSel = -1;
        }
        ImGui::SameLine();
        ImGui::TextDisabled( "%d found", mapBrowseList.Num() );

        if ( mapBrowseList.Num() > 0 ) {
            ImGui::BeginChild( "##mapbrowsebox", ImVec2( 0, 120 ), true );
            for ( int i = 0; i < mapBrowseList.Num(); i++ ) {
                idStr label = mapBrowseList[i];
                label.StripPath();
                label.StripFileExtension();
                ImGui::PushID( i );
                if ( ImGui::Selectable( label.c_str(), mapBrowseSel == i ) ) {
                    mapBrowseSel = i;
                }
                ImGui::PopID();
            }
            ImGui::EndChild();
            if ( mapBrowseSel >= 0 && mapBrowseSel < mapBrowseList.Num() ) {
                if ( ImGui::Button( "Load selected map" ) ) {
                    idStr mapArg = mapBrowseList[mapBrowseSel];
                    mapArg.StripPath();
                    mapArg.StripFileExtension();
                    VisGuiRunCommand( va( "vis_mapLoad \"%s\"", mapArg.c_str() ) );
                }
            }
        }
        ImGui::SameLine();
        if ( ImGui::Button( "Unload map" ) ) { VisGuiRunCommand( "vis_mapUnload" ); }

        ImGui::Separator();
        // PRD follow-up (direct user report: "for the map can we choose the
        // wireframe hue and the background hue? those should also be
        // bindable parameters"). vis_mapRenderMode itself had no GUI control
        // at all before this -- console/config-only.
        int mapRenderMode = vis_mapRenderMode.GetInteger();
        static const char * mapRenderModeNames[] = { "Wireframe", "Textured (experimental)" };
        if ( ImGui::Combo( "Render mode (next load)", &mapRenderMode, mapRenderModeNames, 2 ) ) {
            vis_mapRenderMode.SetInteger( mapRenderMode );
        }
        float mapWireHue = vis_mapWireHue.GetFloat();
        if ( VisBindableSliderFloat( "Wireframe hue", VMT_MAPWIREHUE_MOD, &mapWireHue, 0.0f, 1.0f ) ) { vis_mapWireHue.SetFloat( mapWireHue ); }
        // PRD follow-up (direct user report: "we need blend modes for the
        // map flythrough layer maybe one mode for wireframe and one for
        // background. so they can do different modes. background could
        // darken and wireframe could lighten"). Same VISBLEND_* combo the
        // Layer Stack panel already uses.
        int mapWireBlend = vis_mapWireBlend.GetInteger();
        if ( ImGui::Combo( "Wireframe blend", &mapWireBlend, s_visBlendModeNames, VISBLEND_COUNT ) ) { vis_mapWireBlend.SetInteger( mapWireBlend ); }
        bool mapBgTint = vis_mapBgTint.GetBool();
        if ( ImGui::Checkbox( "Tint background with hue below", &mapBgTint ) ) { vis_mapBgTint.SetBool( mapBgTint ); }
        float mapBgHue = vis_mapBgHue.GetFloat();
        if ( VisBindableSliderFloat( "Background hue", VMT_MAPBGHUE_MOD, &mapBgHue, 0.0f, 1.0f ) ) { vis_mapBgHue.SetFloat( mapBgHue ); }
        int mapBgBlend = vis_mapBgBlend.GetInteger();
        if ( ImGui::Combo( "Background blend", &mapBgBlend, s_visBlendModeNames, VISBLEND_COUNT ) ) { vis_mapBgBlend.SetInteger( mapBgBlend ); }
        if ( !mapBgTint ) {
            ImGui::TextDisabled( "(check 'Tint background' above to use hue/blend)" );
        }
        // PRD follow-up (direct user report: "we also need to be able to
        // organize or sort the layers how we want. so that is a control
        // that doesn't exist yet"). The map view has exactly two layers
        // (background fill, wireframe/textured render) -- this picks which
        // one draws on top, same idea as the Layer Stack's Up/Down buttons
        // just expressed as a single toggle since there are only two.
        bool mapWireOnTop = vis_mapWireOnTop.GetBool();
        if ( ImGui::Checkbox( "Wireframe/map on top (uncheck to put background on top)", &mapWireOnTop ) ) { vis_mapWireOnTop.SetBool( mapWireOnTop ); }
    }

    if ( ImGui::CollapsingHeader( "MIDI I/O (FR-E5)" ) ) {
        ImGui::Text( "Input: %d device(s)%s", idMidiInput::NumDevices(), g_midiInput.IsOpen() ? " (open)" : "" );
        static int midiInIdx = 0;
        ImGui::SetNextItemWidth( 60 );
        ImGui::InputInt( "##midiinidx", &midiInIdx );
        ImGui::SameLine();
        if ( ImGui::Button( "Open##midiin" ) ) { VisGuiRunCommand( va( "vis_midiOpen %d", midiInIdx ) ); }
        ImGui::SameLine();
        if ( ImGui::Button( "Close##midiin" ) ) { VisGuiRunCommand( "vis_midiClose" ); }

        bool noteAdvance = vis_midiNoteAdvance.GetBool();
        if ( ImGui::Checkbox( "Note-on advances preset", &noteAdvance ) ) { vis_midiNoteAdvance.SetBool( noteAdvance ); }
        bool notePalette = vis_midiNotePalette.GetBool();
        if ( ImGui::Checkbox( "Note-on cycles palette", &notePalette ) ) { vis_midiNotePalette.SetBool( notePalette ); }

        ImGui::Separator();
        ImGui::Text( "Output: %d device(s)%s", idMidiOutput::NumDevices(), g_midiOutput.IsOpen() ? " (open)" : "" );
        static int midiOutIdx = 0;
        ImGui::SetNextItemWidth( 60 );
        ImGui::InputInt( "##midioutidx", &midiOutIdx );
        ImGui::SameLine();
        if ( ImGui::Button( "Open##midiout" ) ) { VisGuiRunCommand( va( "vis_midiOutOpen %d", midiOutIdx ) ); }
        ImGui::SameLine();
        if ( ImGui::Button( "Close##midiout" ) ) { VisGuiRunCommand( "vis_midiOutClose" ); }

        bool outBeatNote = vis_midiOutBeatNote.GetBool();
        if ( ImGui::Checkbox( "Beat -> note pulse", &outBeatNote ) ) { vis_midiOutBeatNote.SetBool( outBeatNote ); }
        bool outCC = vis_midiOutCC.GetBool();
        if ( ImGui::Checkbox( "Bands -> CC1/2/3", &outCC ) ) { vis_midiOutCC.SetBool( outCC ); }
        bool outClock = vis_midiOutClock.GetBool();
        if ( ImGui::Checkbox( "BPM-synced clock", &outClock ) ) { vis_midiOutClock.SetBool( outClock ); }
        int outChannel = vis_midiOutChannel.GetInteger();
        if ( ImGui::SliderInt( "Output channel", &outChannel, 0, 15 ) ) { vis_midiOutChannel.SetInteger( outChannel ); }
        int outNote = vis_midiOutNote.GetInteger();
        if ( ImGui::SliderInt( "Beat note number", &outNote, 0, 127 ) ) { vis_midiOutNote.SetInteger( outNote ); }
    }

    if ( ImGui::CollapsingHeader( "DMX / Art-Net (FR-C9)" ) ) {
        // Output-only transport. Mirrors the MIDI-output section above, but the
        // per-channel VALUES come straight from the modulation registry
        // (s_targetDefs / s_mod) -- every row is "DMX channel N <- target name".
        bool dmxOn = vis_dmxEnable.GetBool();
        if ( ImGui::Checkbox( "Enable Art-Net DMX output", &dmxOn ) ) { vis_dmxEnable.SetBool( dmxOn ); }
        ImGui::TextDisabled( "Art-Net over UDP -> port 6454, ~40Hz throttled" );

        static char dmxIpBuf[64] = "";
        static bool dmxIpInit = false;
        if ( !dmxIpInit ) { idStr::Copynz( dmxIpBuf, vis_dmxTargetIP.GetString(), sizeof( dmxIpBuf ) ); dmxIpInit = true; }
        ImGui::SetNextItemWidth( 160 );
        ImGui::InputTextWithHint( "##dmxip", "2.255.255.255 or a gateway IP", dmxIpBuf, sizeof( dmxIpBuf ) );
        ImGui::SameLine();
        if ( ImGui::Button( "Set IP" ) ) { VisGuiRunCommand( va( "vis_dmxTargetIP %s", dmxIpBuf ) ); }

        int uni = vis_dmxUniverse.GetInteger();
        ImGui::SetNextItemWidth( 120 );
        if ( ImGui::InputInt( "Universe", &uni ) ) { vis_dmxUniverse.SetInteger( idMath::ClampInt( 0, 32767, uni ) ); }

        ImGui::Separator();
        ImGui::TextUnformatted( "Assign a modulation target to a DMX channel:" );
        static int  dmxAssignCh = 1;
        static char dmxTgtBuf[64] = "";
        ImGui::SetNextItemWidth( 80 );
        if ( ImGui::InputInt( "Channel##dmx", &dmxAssignCh ) ) { dmxAssignCh = idMath::ClampInt( 1, 512, dmxAssignCh ); }
        dmxAssignCh = idMath::ClampInt( 1, 512, dmxAssignCh );
        ImGui::SameLine();
        ImGui::SetNextItemWidth( 120 );
        ImGui::InputTextWithHint( "##dmxtgt", "target name", dmxTgtBuf, sizeof( dmxTgtBuf ) );
        ImGui::SameLine();
        if ( ImGui::Button( "Assign##dmx" ) ) {
            VisGuiRunCommand( va( "vis_dmxChannel %d %s", dmxAssignCh, dmxTgtBuf[0] ? dmxTgtBuf : "none" ) );
        }

        // Registered target names -- click one to fill the box above.
        ImGui::TextDisabled( "registered targets (click to fill):" );
        ImGui::BeginChild( "##dmxtargets", ImVec2( 0, 70 ), true );
        for ( int t = 0; t < s_targetDefs.Num(); t++ ) {
            if ( ImGui::Selectable( s_targetDefs[t].name ) ) {
                idStr::Copynz( dmxTgtBuf, s_targetDefs[t].name, sizeof( dmxTgtBuf ) );
            }
        }
        ImGui::EndChild();

        ImGui::TextUnformatted( "Current channel assignments:" );
        ImGui::BeginChild( "##dmxassignments", ImVec2( 0, 90 ), true );
        int shown = 0;
        for ( int c = 0; c < 512; c++ ) {
            const int t = VisDmxChannelTarget( c );
            if ( t >= 0 && t < s_targetDefs.Num() ) {
                ImGui::Text( "ch %3d <- %s", c + 1, s_targetDefs[t].name );
                ImGui::SameLine();
                if ( ImGui::SmallButton( va( "x##dmxclr%d", c ) ) ) { VisGuiRunCommand( va( "vis_dmxChannel %d none", c + 1 ) ); }
                shown++;
            }
        }
        if ( shown == 0 ) { ImGui::TextDisabled( "(none assigned)" ); }
        ImGui::EndChild();
    }

    if ( ImGui::CollapsingHeader( "Display / Wallpaper / NDI" ) ) {
        // The actual OS window/display-mode toggle (windowed -> fullscreen
        // monitor 1..N -> windowed), matching the "F" hotkey exactly --
        // NOT the same thing as the Effect section's "Effect fills whole
        // window" checkbox (vis_fullscreen only changes the effect's own
        // draw area within whatever window mode is already active).
        if ( ImGui::Button( "Toggle window fullscreen (F)" ) ) { VisCycleDisplayMode(); }
        ImGui::Separator();
        // F1-menu parity: the DISPLAY tab shows a clickable list of detected
        // monitor native resolutions + windowed presets with a live "*
        // active" marker -- this panel previously only had blind numeric
        // fields. Mirrors BuildDisplayItems() (private to idVisualizerManager,
        // so rebuilt locally here) + MenuDraw2D's active-mode marker logic.
        static std::vector<visDisplayItem_t> displayBrowseList;
        static bool displayListScanned = false;
        if ( !displayListScanned || ImGui::Button( "Rescan monitors" ) ) {
            displayBrowseList.clear();
            idList<vidMode_t> modes;
            for ( int d = 0; R_GetModeListForDisplay( d, modes ); d++ ) {
                int nw = 0, nh = 0, bestArea = 0;
                for ( int i = 0; i < modes.Num(); i++ ) {
                    const int area = modes[i].width * modes[i].height;
                    if ( area > bestArea ) {
                        bestArea = area;
                        nw = modes[i].width;
                        nh = modes[i].height;
                    }
                }
                visDisplayItem_t it;
                it.kind = kVisGuiDispFsMonitor;
                it.mon = d + 1;
                it.w = nw;
                it.h = nh;
                it.label = va( "Fullscreen  -  Monitor %d  (%dx%d)", d + 1, nw, nh );
                displayBrowseList.push_back( it );
            }
            for ( int i = 0; i < 5; i++ ) {
                visDisplayItem_t it;
                it.kind = kVisGuiDispWindowed;
                it.mon = 0;
                it.w = s_visGuiWindowedPresets[i][0];
                it.h = s_visGuiWindowedPresets[i][1];
                it.label = va( "Windowed  -  %dx%d", it.w, it.h );
                displayBrowseList.push_back( it );
            }
            displayListScanned = true;
        }
        ImGui::BeginChild( "##displaybrowsebox", ImVec2( 0, 100 ), true );
        for ( size_t i = 0; i < displayBrowseList.size(); i++ ) {
            const visDisplayItem_t & di = displayBrowseList[i];
            const bool active = ( cvarSystem->GetCVarInteger( "r_customWidth" ) == di.w &&
                                  cvarSystem->GetCVarInteger( "r_customHeight" ) == di.h &&
                                  cvarSystem->GetCVarInteger( "r_fullscreen" ) == ( ( di.kind == kVisGuiDispFsMonitor ) ? di.mon : 0 ) );
            idStr label = va( "%s%s", di.label.c_str(), active ? "  *" : "" );
            ImGui::PushID( (int)i );
            if ( ImGui::Selectable( label.c_str(), active ) ) {
                cvarSystem->SetCVarInteger( "r_vidMode", -1 );
                cvarSystem->SetCVarInteger( "r_customWidth", di.w );
                cvarSystem->SetCVarInteger( "r_customHeight", di.h );
                cvarSystem->SetCVarInteger( "r_fullscreen", ( di.kind == kVisGuiDispFsMonitor ) ? di.mon : 0 );
                VisApplyVideoMode();
            }
            ImGui::PopID();
        }
        ImGui::EndChild();

        ImGui::Separator();
        if ( ImGui::Button( "List monitors (console)" ) ) { VisGuiRunCommand( "vis_displays" ); }
        static int displayIdx = 0;
        ImGui::SetNextItemWidth( 60 );
        ImGui::InputInt( "##displayidx", &displayIdx );
        ImGui::SameLine();
        if ( ImGui::Button( "Fullscreen on monitor" ) ) { VisGuiRunCommand( va( "vis_display %d", displayIdx ) ); }
        static int resW = 1920, resH = 1080;
        ImGui::SetNextItemWidth( 70 );
        ImGui::InputInt( "##resw", &resW );
        ImGui::SameLine();
        ImGui::SetNextItemWidth( 70 );
        ImGui::InputInt( "##resh", &resH );
        ImGui::SameLine();
        if ( ImGui::Button( "Windowed at size" ) ) { VisGuiRunCommand( va( "vis_resolution %d %d", resW, resH ) ); }

        ImGui::Separator();
        ImGui::TextDisabled( "Wallpaper mode: %s", vis_wallpaperMode.GetBool() ? "active" : "inactive" );
        ImGui::SameLine();
        if ( ImGui::Button( "Enable##wallpaper" ) ) { VisGuiRunCommand( "vis_wallpaperEnable" ); }
        ImGui::SameLine();
        if ( ImGui::Button( "Disable##wallpaper" ) ) { VisGuiRunCommand( "vis_wallpaperDisable" ); }

        ImGui::TextDisabled( "NDI output: %s", vis_ndiActive.GetBool() ? "active" : "inactive" );
        char ndiNameBuf[128];
        idStr::snPrintf( ndiNameBuf, sizeof( ndiNameBuf ), "%s", vis_ndiSourceName.GetString() );
        if ( ImGui::InputText( "NDI source name", ndiNameBuf, sizeof( ndiNameBuf ) ) ) {
            vis_ndiSourceName.SetString( ndiNameBuf );
        }
        if ( ImGui::Button( "Enable##ndi" ) ) { VisGuiRunCommand( "vis_ndiEnable" ); }
        ImGui::SameLine();
        if ( ImGui::Button( "Disable##ndi" ) ) { VisGuiRunCommand( "vis_ndiDisable" ); }
    }

    if ( ImGui::CollapsingHeader( "Recording / Video Export" ) ) {
        static char recordDirBuf[256] = "recordings/session";
        ImGui::InputText( "##recorddir", recordDirBuf, sizeof( recordDirBuf ) );
        ImGui::SameLine();
        if ( ImGui::Button( "Start capture" ) ) { VisGuiRunCommand( va( "vis_recordStart %s", recordDirBuf ) ); }
        ImGui::SameLine();
        if ( ImGui::Button( "Stop capture" ) ) { VisGuiRunCommand( "vis_recordStop" ); }
        ImGui::TextDisabled( "%s", s_milkRecording ? "recording..." : "not recording" );

        ImGui::Separator();
        static char videoOutBuf[256] = "recordings/out.mp4";
        ImGui::InputText( "##videoout", videoOutBuf, sizeof( videoOutBuf ) );
        static int videoFps = 30;
        ImGui::SetNextItemWidth( 60 );
        ImGui::InputInt( "FPS", &videoFps );
        static int videoPresetIdx = 0;
        static const char * videoPresetNames[] = { "none", "youtubehd", "youtube4k", "instagram", "tiktok" };
        ImGui::Combo( "Preset", &videoPresetIdx, videoPresetNames, 5 );
        if ( ImGui::Button( "Encode to video" ) ) {
            VisGuiRunCommand( va( "vis_videoEncode %s %s %d %s", recordDirBuf, videoOutBuf, videoFps, videoPresetNames[videoPresetIdx] ) );
        }
    }

    if ( ImGui::CollapsingHeader( "Audio bands", ImGuiTreeNodeFlags_DefaultOpen ) ) {
        // the 3-way bass/mid/treb macro split (what the VMS_BASS/MID/TREB
        // modulation-routing sources actually sample) stays as a quick glance...
        ImGui::Text( "bass %.2f  mid %.2f  treb %.2f", g_audioAnalyzer.GetBass(), g_audioAnalyzer.GetMid(), g_audioAnalyzer.GetTreb() );
        ImGui::ProgressBar( idMath::ClampFloat( 0.0f, 1.0f, g_audioAnalyzer.GetBassAtt() ), ImVec2( -1, 0 ), "bass_att" );
        ImGui::ProgressBar( idMath::ClampFloat( 0.0f, 1.0f, g_audioAnalyzer.GetMidAtt() ), ImVec2( -1, 0 ), "mid_att" );
        ImGui::ProgressBar( idMath::ClampFloat( 0.0f, 1.0f, g_audioAnalyzer.GetTrebAtt() ), ImVec2( -1, 0 ), "treb_att" );

        // ...but this fixed 3-way split isn't the same thing as vis_bands
        // (1-64, the actual per-band count DrawEffectBars/DrawEffectSpectrogram
        // use via GetBandLevel(i)) -- direct user report: "the audio bands
        // display should have the same amount of bands as what the user
        // defines in the effects section." GetBandCount() is kept in sync
        // with vis_bands once per frame in Draw2D(), so it's always current here.
        ImGui::Separator();
        const int bandCount = g_audioAnalyzer.GetBandCount();
        ImGui::TextDisabled( "%d live band(s) (vis_bands):", bandCount );
        for ( int i = 0; i < bandCount; i++ ) {
            ImGui::PushID( i );
            ImGui::ProgressBar( idMath::ClampFloat( 0.0f, 1.2f, g_audioAnalyzer.GetBandLevel( i ) ), ImVec2( -1, 0 ), va( "band %d", i ) );
            ImGui::PopID();
        }

        const float bpm = g_audioAnalyzer.GetBPM();
        if ( bpm > 0.0f ) {
            ImGui::Text( "BPM estimate: %.1f", bpm );
        } else {
            ImGui::Text( "BPM estimate: (not locked on yet)" );
        }
    }

    if ( ImGui::CollapsingHeader( "Modulation routing (right-click a source to MIDI-learn)", ImGuiTreeNodeFlags_DefaultOpen ) ) {
        // F1-menu parity: EFX_MOD is the master on/off switch for this whole
        // section (vis_mod's own description: "enable audio->visual
        // parameter routing") -- previously only reachable via the EFFECTS
        // tab/console, with no in-panel indication or control at all, so the
        // routes below could silently do nothing with no visible cause.
        bool modEnabled = vis_mod.GetBool();
        if ( ImGui::Checkbox( "Enable modulation routing", &modEnabled ) ) { vis_mod.SetBool( modEnabled ); }
        if ( !modEnabled ) {
            ImGui::TextDisabled( "Routing disabled -- every target below is held at its base value." );
        }
        for ( int t = 0; t < s_targetDefs.Num(); t++ ) {
            ImGui::PushID( t );
            ImGui::TextUnformatted( s_targetDefs[t].name );
            ImGui::SameLine( 100 );
            VisDrawModRouteCoreControls( t );
            ImGui::PopID();
        }
        if ( s_midiLearnTarget >= 0 ) {
            ImGui::TextColored( ImVec4( 1.0f, 0.8f, 0.2f, 1.0f ), "Listening for a MIDI CC to bind to '%s'...", s_targetDefs[s_midiLearnTarget].name );
        }
    }

    ImGui::End();
}

// PRD M3: RB_VisMilkShaderCompile runs on the backend/GL-context thread,
// where a plain idLib::Warning would silently vanish (redirected to
// OutputDebugString, never reaching the console -- the exact bug already
// documented in docs/bugfix-visualizer-black-screen.md). Poll its result
// here instead (Frame() is confirmed main thread) and print it where it'll
// actually be seen.
extern int RB_GetMilkWarpCompileStatus();
extern int RB_GetMilkMashCompileStatus();   // PRD M5: same mechanism, mash-up B side

static void VisMilkAdvancePreset( bool hardCut );   // forward decl: PRD FR-A5, defined below

void idVisualizerManager::Frame() {
    // PRD follow-up (direct user report: "every time i launch the app i
    // need to re-register paths for maps and milkdrop presets ... is there
    // a way we can save these to a config file"). Runs once, re-registering
    // every directory remembered in vis_mapSearchPathSaved/
    // vis_presetSearchPathSaved so the user never has to retype/reclick
    // Register every session.
    {
        static bool s_searchPathsReapplied = false;
        if ( !s_searchPathsReapplied ) {
            s_searchPathsReapplied = true;
            VisReapplySavedSearchPaths();
        }
    }
    {
        static int s_lastMilkWarpCompileStatus = 0;
        const int status = RB_GetMilkWarpCompileStatus();
        if ( status != s_lastMilkWarpCompileStatus ) {
            s_lastMilkWarpCompileStatus = status;
            if ( status == 1 ) {
                idLib::Printf( "visualizer: milk custom warp shader compiled+linked ok\n" );
            } else if ( status == -1 ) {
                idLib::Warning( "visualizer: milk custom warp shader failed to compile/link on the backend -- falling back to the CPU-mesh warp path" );
            }
        }
        static int s_lastMilkMashCompileStatus = 0;
        const int mashStatus = RB_GetMilkMashCompileStatus();
        if ( mashStatus != s_lastMilkMashCompileStatus ) {
            s_lastMilkMashCompileStatus = mashStatus;
            if ( mashStatus == 1 ) {
                idLib::Printf( "visualizer: milk mash-up (B side) custom warp shader compiled+linked ok\n" );
            } else if ( mashStatus == -1 ) {
                idLib::Warning( "visualizer: milk mash-up (B side) custom warp shader failed to compile/link on the backend" );
            }
        }
    }

    // resolve the overlay material on the main thread (Draw2D runs on the draw
    // worker thread and must not touch the decl system)
    if ( s_visSolid == NULL ) {
        s_visSolid = declManager->FindMaterial( "visualizer/solid" );
    }
    if ( s_visSolidAdd == NULL ) {
        s_visSolidAdd = declManager->FindMaterial( "visualizer/solidAdditive" );
    }
    if ( s_visFeedback == NULL ) {
        s_visFeedback = declManager->FindMaterial( "visualizer/feedback" );
    }
    if ( s_visFeedbackA == NULL ) {
        s_visFeedbackA = declManager->FindMaterial( "visualizer/feedbackA" );
    }
    if ( s_visFeedbackB == NULL ) {
        s_visFeedbackB = declManager->FindMaterial( "visualizer/feedbackB" );
    }
    if ( s_visWarp == NULL ) {
        // may legitimately be absent (shader spike); don't create a default
        s_visWarp = declManager->FindMaterial( "visualizer/warp", false );
    }
    if ( s_visBloom == NULL ) {
        s_visBloom = declManager->FindMaterial( "visualizer/bloom", false );
    }
    // PRD Pillar C real add/remove layer stack: one composite material per
    // (slot, blend mode) pair (see VisRenderLayerStack), resolved the same
    // lazy way as every other material here since Draw2D itself runs on
    // the draw-worker thread.
    for ( int i = 0; i < VIS_MAX_STACK_LAYERS; i++ ) {
        for ( int m = 0; m < VISBLEND_COUNT; m++ ) {
            if ( s_visLayerSlotMat[i][m] == NULL ) {
                s_visLayerSlotMat[i][m] = declManager->FindMaterial( va( "visualizer/layerSlot%d%s", i, s_visBlendModeMtrSuffix[m] ) );
            }
        }
    }
    // PRD follow-up: map-flythrough background blend-mode fill materials --
    // same lazy main-thread resolve pattern as s_visLayerSlotMat above.
    for ( int m = 0; m < VISBLEND_COUNT; m++ ) {
        if ( s_visSolidBlendMat[m] == NULL ) {
            s_visSolidBlendMat[m] = declManager->FindMaterial( s_visSolidBlendMatNames[m] );
        }
    }
    // keep circles round on non-4:3 displays: the 2D overlay is a 640x480 virtual
    // space stretched to the backbuffer, so shrink x by virtualAspect/actualAspect.
    const float actualAspect = ( renderSystem->GetHeight() > 0 )
        ? ( renderSystem->GetWidth() * renderSystem->GetPixelAspect() / (float)renderSystem->GetHeight() ) : ( VIS_W / VIS_H );
    s_aspectFix = ( actualAspect > 0.01f ) ? idMath::ClampFloat( 0.25f, 1.0f, ( VIS_W / VIS_H ) / actualAspect ) : 1.0f;

    // resolve the reactive image layers on the main thread when the list changes.
    // FindMaterial auto-generates an implicit blend+colored+clamp material for each
    // image file (idMaterial::SetDefaultText), so any TGA/JPG (and PNG once loaded)
    // just works.
    if ( idStr::Icmp( s_layerListCached.c_str(), vis_layerList.GetString() ) != 0 ) {
        s_layerListCached = vis_layerList.GetString();
        s_layerMats.clear();
        idStr list = s_layerListCached;
        idStr tok;
        int start = 0;
        for ( int i = 0; i <= list.Length(); i++ ) {
            if ( i == list.Length() || list[i] == ';' ) {
                tok = list.Mid( start, i - start );
                tok.StripLeading( ' ' );
                tok.StripTrailing( ' ' );
                if ( tok.Length() > 0 ) {
                    s_layerMats.push_back( declManager->FindMaterial( tok.c_str() ) );
                }
                start = i + 1;
            }
        }
    }

    // #1 auto-arm loopback: retry (throttled) until the audio device comes up
    // once, then leave the source mode alone so the user can switch freely.
    if ( !m_sourceArmed && vis_autoLoopback.GetBool() ) {
        const int nowMs = Sys_Milliseconds();
        if ( nowMs >= m_nextArmMs ) {
            m_nextArmMs = nowMs + 1500;
            if ( g_audioAnalyzer.GetSourceMode() != AudioSourceMode::WASAPI_LOOPBACK ) {
                g_audioAnalyzer.SetSourceMode( AudioSourceMode::WASAPI_LOOPBACK );
            }
            if ( g_audioAnalyzer.GetSourceMode() == AudioSourceMode::WASAPI_LOOPBACK ) {
                m_sourceArmed = true;
                idLib::Printf( "visualizer: loopback armed - any system audio now drives the visualizer\n" );
            }
        }
    }

    // Direct user report: "i connected my bluetooth headphones and then the
    // loopback signal died, i am still playing soundcloud music in browser
    // though" -- the boot-arm block above only ever runs until the FIRST
    // success (m_sourceArmed latches true), but the OS default output device
    // can change at any point in a session, not just at boot. Runs the whole
    // time (not gated on m_sourceArmed), same throttle interval; a no-op
    // unless RecheckDefaultDevice actually finds the bound endpoint no longer
    // matches the current default.
    {
        const int nowMs = Sys_Milliseconds();
        if ( nowMs >= m_nextDeviceCheckMs ) {
            m_nextDeviceCheckMs = nowMs + 1500;
            g_audioAnalyzer.RecheckDefaultDevice();
        }
    }

    // evaluate audio->visual routes on the main thread (analyzer just updated),
    // caching the results for the draw-worker thread. dt clamped so a hitch or
    // the first frame can't produce a huge rotation/LFO jump.
    const int now = Sys_Milliseconds();
    float dtSec = ( s_modLastMs > 0 ) ? ( now - s_modLastMs ) * 0.001f : 0.0f;
    dtSec = idMath::ClampFloat( 0.0f, 0.1f, dtSec );
    s_modLastMs = now;
    VisUpdateModulation( dtSec );
    VisUpdateMilkFrame( dtSec );   // PRD M1 step 4: MilkDrop per_frame_/per_pixel_ (main thread only, see note above VisUpdateMilkFrame)
    VisUpdateShaderToyFrame( dtSec );  // PRD FR-C10: ShaderToy layer's iTime/iFrame/fps uniforms (main thread only)
    VisUpdateMilkCamera( dtSec );  // PRD M4.5 FR-F3: advance the map flythrough camera (main thread only, see note above VisUpdateMilkCamera)
    VisUpdateMapRenderColors();    // PRD follow-up: bindable wireframe/background tint hues
    VisUpdateMidiOutput( dtSec );  // PRD FR-E5: beat->note, band envelopes->CC, BPM-synced clock (main thread only)
    VisUpdateDmxOutput( dtSec );   // PRD FR-C9: Art-Net DMX out of assigned modulation targets (main thread only, ~40Hz throttled)
    VisUpdateMidiLearn();          // PRD Pillar C: right-click-a-knob MIDI-learn, armed from the ImGui editor

    // PRD FR-E5: "trigger preset/effect/palette changes on notes" -- the
    // other consumer of MIDI note input besides VMS_MIDINOTE_VEL's envelope
    // routing (see VisSampleSource above). idMidiInput only exposes
    // current-state polling (GetNoteOn), not an event queue, so this
    // edge-detects the on-transition itself (a held note must not re-trigger
    // every single frame) against its own previous-frame snapshot -- the
    // same "only fires once at the transition" role g_audioAnalyzer.GetBeat()
    // already plays for the beat-triggered preset-cycle block below. Inlined
    // here (a member function) rather than a free function, since
    // AdvancePreset() below is private to idVisualizerManager.
    if ( vis_midiNoteAdvance.GetBool() || vis_midiNotePalette.GetBool() ) {
        for ( int ch = 0; ch < 16; ch++ ) {
            for ( int note = 0; note < 128; note++ ) {
                const bool on = g_midiInput.GetNoteOn( ch, note );
                if ( on && !s_midiNoteOnPrev[ch][note] ) {
                    if ( vis_midiNoteAdvance.GetBool() ) {
                        if ( s_milkActive ) {
                            VisMilkAdvancePreset( true );   // hard cut -- matches the existing beat-triggered instant-switch convention (FR-A5)
                        } else {
                            AdvancePreset();
                        }
                    }
                    if ( vis_midiNotePalette.GetBool() ) {
                        vis_palette.SetInteger( ( vis_palette.GetInteger() + 1 ) % 5 );
                    }
                }
                s_midiNoteOnPrev[ch][note] = on;
            }
        }
    }

    // PRD M4: layer-editor UI, now in its own separate top-level window
    // (see win_imgui.cpp's design note) rather than an overlay on the
    // game's render window. Vis_ImGuiSetToolWindowVisible runs
    // unconditionally (so the window actually hides the frame the cvar
    // flips off, not just freezes); the NewFrame/build/EndFrame sequence
    // stays gated on the cvar so a closed editor costs nothing (no
    // ImGui::Render()/render-command enqueue at all). Main thread only,
    // matching win_imgui.h's threading note.
    Vis_ImGuiSetToolWindowVisible( vis_imguiEditor.GetBool() );
    if ( vis_imguiEditor.GetBool() && Vis_ImGuiIsInitialized() ) {
        Vis_ImGuiNewFrame();
        VisDrawImGuiLayerEditor();
        Vis_ImGuiEndFrame();
    }

    s_warpPhase += dtSec * ( vis_warpSpeed.GetFloat() + VisModOr( VMT_WARPSPEED_MOD, 0.0f ) );   // independent of vis_mod otherwise
    if ( s_warpPhase > 1000.0f ) {
        s_warpPhase -= 1000.0f;                        // keep it bounded
    }

    // beat-synced preset auto-cycling
    if ( vis_presetCycle.GetBool() ) {
        if ( !m_cycleActive ) {          // enable edge: (re)scan and schedule
            BuildCycleList();
            m_cycleActive = true;
        }
        // don't yank presets around while the user is in the picker
        if ( !m_menuOpen ) {
            // PRD FR-A5: when a .milk preset is the active driver, this
            // same timer/beat gate advances IT instead of the .cfg cycle
            // list -- matches the fact that VisUpdateMilkFrame's own
            // "progress" variable already assumes vis_presetCycleSecs is
            // the MilkDrop preset's own switch timer (a real, pre-existing
            // inconsistency this closes: progress counted down to a
            // switch that never actually happened). The .cfg system's own
            // ">1 file" gate doesn't apply here -- .milk files are
            // (re)enumerated fresh each advance, not from the pre-scanned
            // m_cycleList.
            const bool canAdvance = s_milkActive || ( m_cycleList.Num() > 1 );
            if ( !m_cyclePending && now >= m_cycleNextMs ) {
                m_cyclePending = true;   // timer elapsed; may wait for a beat
            }
            if ( m_cyclePending && canAdvance &&
                 ( !vis_presetCycleOnBeat.GetBool() || g_audioAnalyzer.GetBeat() ) ) {
                if ( s_milkActive ) {
                    // hard cut (instant) when beat-gated -- matches FR-A5's
                    // "hard cut (beat-triggered instant switch)"; soft cut
                    // (crossfade) for a plain timer-only advance.
                    VisMilkAdvancePreset( vis_presetCycleOnBeat.GetBool() );
                } else {
                    AdvancePreset();
                }
                m_cyclePending = false;
                m_cycleNextMs = now + (int)( vis_presetCycleSecs.GetFloat() * 1000.0f );
            }
        }
    } else {
        m_cycleActive = false;
    }

    // auto-advance when the current track finished
    if ( m_playing && m_trackEndTime > 0 && Sys_Milliseconds() >= m_trackEndTime ) {
        if ( vis_autoAdvance.GetBool() && m_playlist.GetNumTracks() > 1 ) {
            Next();
        } else {
            m_playing = false;
            m_trackEndTime = 0;
        }
    }
}

void idVisualizerManager::PrintStatus() const {
    idLib::Printf( "visualizer status:\n" );
    idLib::Printf( "  source     : %s\n", g_audioAnalyzer.GetSourceMode() == AudioSourceMode::WASAPI_LOOPBACK ? "loopback" : "engine" );
    idLib::Printf( "  playing    : %s\n", m_playing ? m_nowPlaying.c_str() : "<stopped>" );
    idLib::Printf( "  playlist   : %d tracks, index %d\n", m_playlist.GetNumTracks(), m_playlist.GetCurrentIndex() );
    idLib::Printf( "  rms %.3f | bass %.2f (att %.2f) | mid %.2f (att %.2f) | treb %.2f (att %.2f)%s\n",
        g_audioAnalyzer.GetRMS(),
        g_audioAnalyzer.GetBass(), g_audioAnalyzer.GetBassAtt(),
        g_audioAnalyzer.GetMid(),  g_audioAnalyzer.GetMidAtt(),
        g_audioAnalyzer.GetTreb(), g_audioAnalyzer.GetTrebAtt(),
        g_audioAnalyzer.GetBeat() ? "  [BEAT]" : "" );
}

/*
================================================================================================
console commands
================================================================================================
*/

CONSOLE_COMMAND( vis_play, "play an audio file or .m3u playlist through the visualizer", idCmdSystem::ArgCompletion_SoundName ) {
    if ( args.Argc() != 2 ) {
        idLib::Printf( "usage: vis_play <music/file.wav | playlists/list.m3u>\n" );
        return;
    }
    g_visualizerManager.Play( args.Argv( 1 ) );
}

CONSOLE_COMMAND( vis_stop, "stop visualizer playback", NULL ) {
    g_visualizerManager.Stop();
}

CONSOLE_COMMAND( vis_next, "next playlist track", NULL ) {
    g_visualizerManager.Next();
}

CONSOLE_COMMAND( vis_prev, "previous playlist track", NULL ) {
    g_visualizerManager.Prev();
}

CONSOLE_COMMAND( vis_source, "set analyzer source: engine | loopback", NULL ) {
    if ( args.Argc() != 2 ) {
        idLib::Printf( "usage: vis_source <engine|loopback>\n" );
        return;
    }
    if ( idStr::Icmp( args.Argv( 1 ), "loopback" ) == 0 ) {
        g_visualizerManager.SetSourceMode( AudioSourceMode::WASAPI_LOOPBACK );
    } else {
        g_visualizerManager.SetSourceMode( AudioSourceMode::ENGINE );
    }
}

CONSOLE_COMMAND( vis_status, "print visualizer state and current band levels", NULL ) {
    g_visualizerManager.PrintStatus();
}

CONSOLE_COMMAND( vis_menu, "toggle the on-screen picker (songs / playlists / presets / effects). Also bound to F1.", NULL ) {
    g_visualizerManager.ToggleMenu();
}

CONSOLE_COMMAND( vis_presetSave, "save the current visual state as a named preset: vis_presetSave <name>", NULL ) {
    if ( args.Argc() != 2 ) {
        idLib::Printf( "usage: vis_presetSave <name>\n" );
        return;
    }
    g_visualizerManager.SavePreset( args.Argv( 1 ) );
}

CONSOLE_COMMAND( vis_presetLoad, "load a saved preset by name: vis_presetLoad <name>", NULL ) {
    if ( args.Argc() != 2 ) {
        idLib::Printf( "usage: vis_presetLoad <name>\n" );
        return;
    }
    g_visualizerManager.LoadPresetPath( va( "presets/%s.cfg", args.Argv( 1 ) ) );
}

// PRD follow-up (direct user report: "we need all of the options that
// exist in the ingame engine menu to exist in the imgui menu") -- console
// commands backing new ImGui controls that reach F1-menu-only capability:
// full-path .cfg loading (for a scanned-list picker, unlike vis_presetLoad's
// bare-name-only "presets/<name>.cfg" convention), one-click "save to the
// next free custom-NN slot" (the PRESETS tab's row 0), .cfg cycle-list
// stepping (SPACE/BACKSPACE hotkeys), and the F11 master effects-off toggle.
CONSOLE_COMMAND( vis_presetLoadPath, "load a saved .cfg preset by its full path: vis_presetLoadPath <path>", NULL ) {
    if ( args.Argc() != 2 ) {
        idLib::Printf( "usage: vis_presetLoadPath <path>\n" );
        return;
    }
    g_visualizerManager.LoadPresetPath( args.Argv( 1 ) );
}

CONSOLE_COMMAND( vis_presetSaveCustom, "save the current visual state as the next free custom-NN preset slot (same as the F1 PRESETS tab's row 0)", NULL ) {
    g_visualizerManager.SaveCurrentAsCustom();
}

CONSOLE_COMMAND( vis_presetNext, "step to the next preset in the .cfg cycle list (same as the SPACE hotkey)", NULL ) {
    g_visualizerManager.NextPreset();
}

CONSOLE_COMMAND( vis_presetPrev, "step to the previous preset in the .cfg cycle list (same as the BACKSPACE hotkey)", NULL ) {
    g_visualizerManager.PrevPreset();
}

CONSOLE_COMMAND( vis_effectsOff, "toggle all effects (feedback/warp/bloom) off, or restore if unchanged since (same as the F11 hotkey)", NULL ) {
    VisToggleEffectsMasterOff();
}

CONSOLE_COMMAND( vis_devices, "list capturable audio endpoints (render=loopback, capture=mic/line-in)", NULL ) {
    std::vector<AudioDeviceInfo> devs;
    AudioAnalyzer::EnumerateDevices( devs );
    idLib::Printf( "  0: system default output (loopback)\n" );
    for ( size_t i = 0; i < devs.size(); i++ ) {
        idLib::Printf( "  %d: %s [%s]\n", (int)i + 1, devs[i].name.c_str(), devs[i].isCapture ? "in" : "out" );
    }
    idLib::Printf( "%d endpoints (+ default). Select with: vis_device <n>\n", (int)devs.size() );
}

CONSOLE_COMMAND( vis_device, "select capture endpoint by index (see vis_devices; 0 = system default)", NULL ) {
    if ( args.Argc() != 2 ) {
        idLib::Printf( "usage: vis_device <index>   (0 = system default output)\n" );
        return;
    }
    const int idx = atoi( args.Argv( 1 ) );
    if ( idx <= 0 ) {
        g_audioAnalyzer.SetCaptureDevice( std::string(), false );
        idLib::Printf( "visualizer: using system default output (loopback)\n" );
        return;
    }
    std::vector<AudioDeviceInfo> devs;
    AudioAnalyzer::EnumerateDevices( devs );
    if ( idx - 1 >= (int)devs.size() ) {
        idLib::Printf( "no endpoint %d (see vis_devices)\n", idx );
        return;
    }
    const AudioDeviceInfo & d = devs[idx - 1];
    g_audioAnalyzer.SetCaptureDevice( d.id, d.isCapture );
    idLib::Printf( "visualizer: capturing '%s' [%s]\n", d.name.c_str(), d.isCapture ? "in" : "out" );
}

CONSOLE_COMMAND( vis_displays, "list monitors + their native resolutions", NULL ) {
    idList<vidMode_t> modes;
    int count = 0;
    for ( int d = 0; R_GetModeListForDisplay( d, modes ); d++ ) {
        int nw = 0, nh = 0, best = 0;
        for ( int i = 0; i < modes.Num(); i++ ) {
            const int area = modes[i].width * modes[i].height;
            if ( area > best ) { best = area; nw = modes[i].width; nh = modes[i].height; }
        }
        idLib::Printf( "  monitor %d: native %dx%d (%d modes)\n", d + 1, nw, nh, modes.Num() );
        count++;
    }
    idLib::Printf( "%d monitor(s). vis_display <n> = fullscreen on monitor n; vis_resolution <w> <h> = windowed\n", count );
}

CONSOLE_COMMAND( vis_display, "fullscreen on monitor <n> at its native resolution", NULL ) {
    if ( args.Argc() != 2 ) {
        idLib::Printf( "usage: vis_display <monitor#>  (see vis_displays; use vis_resolution for windowed)\n" );
        return;
    }
    const int n = atoi( args.Argv( 1 ) );
    idList<vidMode_t> modes;
    if ( n < 1 || !R_GetModeListForDisplay( n - 1, modes ) ) {
        idLib::Printf( "no monitor %d (see vis_displays)\n", n );
        return;
    }
    int nw = 0, nh = 0, best = 0;
    for ( int i = 0; i < modes.Num(); i++ ) {
        const int area = modes[i].width * modes[i].height;
        if ( area > best ) { best = area; nw = modes[i].width; nh = modes[i].height; }
    }
    cvarSystem->SetCVarInteger( "r_vidMode", -1 );
    cvarSystem->SetCVarInteger( "r_customWidth", nw );
    cvarSystem->SetCVarInteger( "r_customHeight", nh );
    cvarSystem->SetCVarInteger( "r_fullscreen", n );
    VisApplyVideoMode();
    idLib::Printf( "visualizer: fullscreen %dx%d on monitor %d\n", nw, nh, n );
}

// PRD M7: "wallpaper mode" -- reparents the game window behind the desktop
// icons via the Progman/WorkerW technique (see win_wallpaper.h for the full
// mechanism). vis_wallpaperMode mirrors the actual state for querying/UI
// display; the commands below do the real work, same "cvar for state,
// explicit command for the action" pattern vis_recordStart/Stop and
// vis_midiOpen/Close already follow in this file.
idCVar vis_wallpaperMode( "vis_wallpaperMode", "0", CVAR_BOOL, "read-only-ish mirror of whether wallpaper mode is active -- use vis_wallpaperEnable/Disable to change it" );

CONSOLE_COMMAND( vis_wallpaperEnable, "reparent the game window behind the desktop icons, borderless (PRD M7, Windows only)", NULL ) {
    if ( Vis_WallpaperEnable() ) {
        vis_wallpaperMode.SetBool( true );
    }
}

CONSOLE_COMMAND( vis_wallpaperDisable, "restore the window to normal desktop parenting (PRD M7)", NULL ) {
    Vis_WallpaperDisable();
    vis_wallpaperMode.SetBool( false );
}

// PRD M7: NDI (network video) output. Needs the free NDI Runtime
// redistributable installed (not the paid/registered SDK) -- see
// win_ndi.h for the full mechanism (dynamic-loading, zero build-time SDK
// dependency). Same "cvar for state, explicit command for the action"
// pattern as wallpaper mode just above.
idCVar vis_ndiActive( "vis_ndiActive", "0", CVAR_BOOL, "read-only-ish mirror of whether NDI output is active -- use vis_ndiEnable/Disable to change it" );
idCVar vis_ndiSourceName( "vis_ndiSourceName", "DOOM3BFG Visualizer", CVAR_ARCHIVE, "NDI source name shown to receivers (vMix, OBS, etc)" );

CONSOLE_COMMAND( vis_ndiEnable, "start broadcasting the visualizer as an NDI source (PRD M7, needs the free NDI Runtime installed)", NULL ) {
    if ( Vis_NDIEnable( vis_ndiSourceName.GetString() ) ) {
        vis_ndiActive.SetBool( true );
    } else if ( !Vis_NDIIsAvailable() ) {
        idLib::Printf( "vis_ndiEnable: NDI Runtime not found -- install the free redistributable from ndi.video\n" );
    }
}

CONSOLE_COMMAND( vis_ndiDisable, "stop NDI output (PRD M7)", NULL ) {
    Vis_NDIDisable();
    vis_ndiActive.SetBool( false );
}

// PRD M7: sends this frame's already-composited pixels to NDI, if active.
// Called from Draw2D right where the screen-recording capture already
// runs (after the effect is fully drawn, before the picker menu) so NDI
// viewers see exactly what a local player sees, menu/console excluded.
//
// Fixed real bug: this used to call renderSystem->CaptureRenderToMemory()
// (raw qglReadPixels) directly from here -- but Draw2D runs on the SMP
// draw-worker thread, not the thread that owns the GL context. Confirmed
// via an actual crash (non-deterministic fault offset each run). Fix:
// enqueue a render command instead (EnqueueCaptureToNDI, same pattern as
// EnqueueImGuiRender) and let the backend thread do the read-back and the
// Vis_NDISendFrame call itself (see RB_VisCapture in RenderSystem.cpp).
static void VisSendNDIFrame() {
    if ( !Vis_NDIIsEnabled() ) {
        return;
    }
    renderSystem->EnqueueCaptureToNDI();
}

// PRD FR-D6 (F hotkey): cycle windowed -> fullscreen monitor 1 -> ... ->
// fullscreen monitor N -> windowed. This engine has no "span multiple
// monitors as one canvas" or "render to every monitor simultaneously"
// concept (unlike MilkDrop's literal single/double/all-screen modes), so
// this is a deliberately honest facsimile -- cycling through each monitor
// one at a time plus windowed -- rather than a claim of exact parity.
static void VisCycleDisplayMode() {
    idList<vidMode_t> modes;
    int numMonitors = 0;
    for ( int d = 0; R_GetModeListForDisplay( d, modes ); d++ ) {
        numMonitors++;
    }
    if ( numMonitors == 0 ) {
        return;
    }
    const int current = cvarSystem->GetCVarInteger( "r_fullscreen" );   // <=0 windowed, N = fullscreen monitor N
    const int next = ( current <= 0 ) ? 1 : ( ( current < numMonitors ) ? current + 1 : 0 );
    if ( next == 0 ) {
        cvarSystem->SetCVarInteger( "r_fullscreen", 0 );
        VisApplyVideoMode();
        idLib::Printf( "visualizer: windowed\n" );
        return;
    }
    if ( !R_GetModeListForDisplay( next - 1, modes ) ) {
        return;
    }
    int nw = 0, nh = 0, best = 0;
    for ( int i = 0; i < modes.Num(); i++ ) {
        const int area = modes[i].width * modes[i].height;
        if ( area > best ) { best = area; nw = modes[i].width; nh = modes[i].height; }
    }
    cvarSystem->SetCVarInteger( "r_vidMode", -1 );
    cvarSystem->SetCVarInteger( "r_customWidth", nw );
    cvarSystem->SetCVarInteger( "r_customHeight", nh );
    cvarSystem->SetCVarInteger( "r_fullscreen", next );
    VisApplyVideoMode();
    idLib::Printf( "visualizer: fullscreen %dx%d on monitor %d\n", nw, nh, next );
}

CONSOLE_COMMAND( vis_resolution, "windowed mode at <w> <h>", NULL ) {
    if ( args.Argc() != 3 ) {
        idLib::Printf( "usage: vis_resolution <width> <height>  (windowed)\n" );
        return;
    }
    const int w = atoi( args.Argv( 1 ) );
    const int h = atoi( args.Argv( 2 ) );
    if ( w < 320 || h < 240 ) {
        idLib::Printf( "resolution too small\n" );
        return;
    }
    cvarSystem->SetCVarInteger( "r_vidMode", -1 );
    cvarSystem->SetCVarInteger( "r_customWidth", w );
    cvarSystem->SetCVarInteger( "r_customHeight", h );
    cvarSystem->SetCVarInteger( "r_fullscreen", 0 );
    VisApplyVideoMode();
    idLib::Printf( "visualizer: windowed %dx%d\n", w, h );
}

CONSOLE_COMMAND( vis_presets, "list saved presets under base/presets/", NULL ) {
    idFileList * files = fileSystem->ListFilesTree( "presets", ".cfg", true );
    int total = 0;
    if ( files != NULL ) {
        for ( int i = 0; i < files->GetNumFiles(); i++ ) {
            idLib::Printf( "  %s\n", files->GetFile( i ) );
            total++;
        }
        fileSystem->FreeFileList( files );
    }
    idLib::Printf( "%d presets\n", total );
}

CONSOLE_COMMAND( vis_debug, "dump raw analyzer values (bin width, low bins, raw+normalized bands)", NULL ) {
    const int sr = g_audioAnalyzer.GetSampleRate();
    idLib::Printf( "analyzer raw dump  (sampleRate %d, binWidth %.2f Hz)\n", sr, (float)sr / FFT_SIZE );
    idLib::Printf( "  low bins |mag|: " );
    for ( int i = 0; i < 12; i++ ) {
        idLib::Printf( "%.3f ", g_audioAnalyzer.GetFrequencyBin( i ) );
    }
    idLib::Printf( "\n" );
    const char * names[] = { "SubBass", "Bass", "MidLow", "MidHigh", "High", "Treble" };
    for ( int b = 0; b < static_cast<int>( AudioBand::COUNT ); b++ ) {
        AudioBand band = static_cast<AudioBand>( b );
        idLib::Printf( "  %-8s raw %.4f  avg %.4f  norm %.3f\n",
            names[b], g_audioAnalyzer.GetBandRaw( band ),
            g_audioAnalyzer.GetBandAverage( band ), g_audioAnalyzer.GetBandMagnitude( band ) );
    }
}

// free function called from idCommonLocal::Draw() (avoids coupling framework to the class)
/*
========================
#4 On-screen picker menu (immediate-mode overlay, keyboard driven)
========================
*/
static const int VIS_MENU_TABS = 7;
enum { TAB_MUSIC = 0, TAB_PLAYLISTS, TAB_PRESETS, TAB_DEVICES, TAB_DISPLAY, TAB_IMAGES, TAB_EFFECTS };
static const char * const s_menuTabNames[VIS_MENU_TABS] = { "MUSIC", "PLAYLISTS", "PRESETS", "DEVICES", "DISPLAY", "IMAGES", "EFFECTS" };
static const int s_bandChoices[] = { 3, 5, 7, 9 };

/*
========================
PRD M0: layer-type catalog (ZGE "source or processor" model)

Not a rendering change — a NAMED, CATEGORIZED index over what already exists:
the mutually-exclusive source effects (vis_effect 0-7) and the independently
toggleable processors (feedback/warp/kaleido/bloom), unified the way ZGameEditor
Visualizer's layer stack treats "source" and "processor" as one list. This is
the foundation M4 (the real multi-instance layer stack + compositing) builds
on; for now it's read-only/informational (`vis_layers`), and vis_effect/the
processor cvars remain the actual render-time control surface.
========================
*/
enum { VISLAYERCAT_SOURCE = 0, VISLAYERCAT_PROCESSOR };
struct visLayerType_t {
    const char * name;
    int          category;     // VISLAYERCAT_SOURCE or VISLAYERCAT_PROCESSOR
    int          effectIndex;  // SOURCE: value vis_effect takes to select it; PROCESSOR: -1
};
static const visLayerType_t s_layerTypes[] = {
    { "Bars",           VISLAYERCAT_SOURCE,    0 },
    { "Radial",         VISLAYERCAT_SOURCE,    1 },
    { "Scope",          VISLAYERCAT_SOURCE,    2 },
    { "Waveform Ring",  VISLAYERCAT_SOURCE,    3 },
    { "Particles",      VISLAYERCAT_SOURCE,    4 },
    { "Spectrogram",    VISLAYERCAT_SOURCE,    5 },
    { "Starfield",      VISLAYERCAT_SOURCE,    6 },
    { "Phase Scope",    VISLAYERCAT_SOURCE,    7 },
    { "Feedback Trail", VISLAYERCAT_PROCESSOR, -1 },
    { "Warp",           VISLAYERCAT_PROCESSOR, -1 },
    { "Kaleidoscope",   VISLAYERCAT_PROCESSOR, -1 },
    { "Bloom / Glow",   VISLAYERCAT_PROCESSOR, -1 },
};
static const int NUM_VIS_LAYER_TYPES = sizeof( s_layerTypes ) / sizeof( s_layerTypes[0] );

// Effect-tab rows. Music/playlist rows come from the scanned file lists.
enum { EFX_BARS = 0, EFX_RADIAL, EFX_SCOPE, EFX_RING, EFX_PARTICLES, EFX_SPECTRO, EFX_STARFIELD, EFX_PHASE, EFX_FEEDBACK, EFX_WARP, EFX_WARPMODE, EFX_KALEIDO, EFX_BLOOM, EFX_FULLSCREEN, EFX_BANDS, EFX_MOD, EFX_PALETTE, EFX_PRESETCYCLE, EFX_IMGUI, EFX_COUNT };
static const char * const s_warpModeNames[4] = { "ripple", "swirl", "tunnel", "fisheye" };
static const char * const paletteNames[5] = { "rainbow", "fire", "ocean", "synthwave", "mono" };
static const char * const s_warpQualNames[4] = { "off", "mesh", "hi-res", "shader" };

static void VisAppendFiles( idStrList & out, const char * dir, const char * ext ) {
    idFileList * files = fileSystem->ListFilesTree( dir, ext, true );
    if ( files != NULL ) {
        for ( int i = 0; i < files->GetNumFiles(); i++ ) {
            out.Append( files->GetFile( i ) );
        }
        fileSystem->FreeFileList( files );
    }
}

// PRD FR-D6/FR-A5: pick a random .milk file under presets/milk -- the exact
// LCG-seeded pick the K_A mash-up hotkey already used, factored out so
// FR-A5's auto-advance can share it instead of duplicating the same logic
// a third time (BuildCycleList's own Fisher-Yates seed is the same style,
// for the separate .cfg preset list). Returns false (out untouched) if no
// .milk files exist.
static bool VisPickRandomMilkPreset( idStr & out ) {
    idStrList files;
    VisAppendFiles( files, vis_milkPresetSubdir.GetString(), ".milk" );
    if ( files.Num() == 0 ) {
        return false;
    }
    unsigned int seed = (unsigned int)( Sys_Milliseconds() ^ ( files.Num() * 2654435761u ) );
    const int pick = (int)( ( seed >> 8 ) % (unsigned int)files.Num() );
    out = files[ pick ];
    return true;
}

// PRD FR-A5: advance to a random next .milk preset -- hard cut (instant,
// beat-triggered per the requirement's own wording) or soft cut (crossfade
// over vis_milkSoftCutSecs, reusing the M5 mash-up mechanism -- see
// VisUpdateMilkFrame's ramp/promotion logic above -- as the blend
// primitive). Silently does nothing (NFR-3) if a soft cut is already in
// progress (avoid corrupting the in-flight B side), no .milk preset is
// currently active (nothing to advance FROM), or no .milk files exist to
// advance TO.
static void VisMilkAdvancePreset( bool hardCut ) {
    if ( s_milkCutInProgress || !s_milkActive ) {
        return;
    }
    idStr nextPath;
    if ( !VisPickRandomMilkPreset( nextPath ) ) {
        return;
    }
    if ( hardCut ) {
        VisLoadMilkPreset( nextPath.c_str() );
        return;
    }
    // Fixed real bug (same class already found/fixed for the K_A mash-up
    // hotkey): VisLoadMilkPresetB() returns true as soon as the file loads
    // and the evaluator inits, even for a preset with no per_pixel_ code
    // and no working warp_ shader (a documented, expected "loaded but
    // inactive" case) -- checking its return value alone could start a
    // soft cut with no actual B side ever going to render. s_milkActiveB
    // is the real signal.
    if ( VisLoadMilkPresetB( nextPath.c_str() ) && s_milkActiveB ) {
        s_milkPresetBPath = nextPath;
        s_milkMashMix = 0.0f;
        s_milkCutInProgress = true;
    }
}

// Serialize the full visual state as a runnable .cfg (cvars + vis_route lines).
static void VisWritePresetTo( idFile * f ) {
    f->Printf( "// DOOM-3-BFG visualizer preset - generated by vis_presetSave\n" );
    f->Printf( "seta vis_effect %d\n", vis_effect.GetInteger() );
    f->Printf( "seta vis_bands %d\n", vis_bands.GetInteger() );
    f->Printf( "seta vis_fullscreen %d\n", vis_fullscreen.GetBool() ? 1 : 0 );
    f->Printf( "seta vis_feedback %d\n", vis_feedback.GetBool() ? 1 : 0 );
    f->Printf( "seta vis_feedbackDecay %f\n", vis_feedbackDecay.GetFloat() );
    f->Printf( "seta vis_feedbackZoom %f\n", vis_feedbackZoom.GetFloat() );
    f->Printf( "seta vis_warp %d\n", vis_warp.GetInteger() );
    f->Printf( "seta vis_warpMode %d\n", vis_warpMode.GetInteger() );
    f->Printf( "seta vis_warpAmount %f\n", vis_warpAmount.GetFloat() );
    f->Printf( "seta vis_warpFreq %f\n", vis_warpFreq.GetFloat() );
    f->Printf( "seta vis_warpSpeed %f\n", vis_warpSpeed.GetFloat() );
    f->Printf( "seta vis_kaleido %d\n", vis_kaleido.GetInteger() );
    f->Printf( "seta vis_bloom %f\n", vis_bloom.GetFloat() );
    f->Printf( "seta vis_bloomThreshold %f\n", vis_bloomThreshold.GetFloat() );
    f->Printf( "seta vis_bloomRadius %f\n", vis_bloomRadius.GetFloat() );
    f->Printf( "seta vis_bloomBeat %f\n", vis_bloomBeat.GetFloat() );
    f->Printf( "seta vis_mod %d\n", vis_mod.GetBool() ? 1 : 0 );
    f->Printf( "seta vis_palette %d\n", vis_palette.GetInteger() );
    f->Printf( "seta vis_hueShiftGlobal %f\n", vis_hueShiftGlobal.GetFloat() );
    f->Printf( "seta vis_lfoPeriod %f\n", vis_lfoPeriod.GetFloat() );
    f->Printf( "seta vis_effectX %f\n", vis_effectX.GetFloat() );
    f->Printf( "seta vis_effectY %f\n", vis_effectY.GetFloat() );
    f->Printf( "seta vis_layer %d\n", vis_layer.GetBool() ? 1 : 0 );
    f->Printf( "seta vis_layerList \"%s\"\n", vis_layerList.GetString() );
    f->Printf( "seta vis_layerScale %f\n", vis_layerScale.GetFloat() );
    f->Printf( "seta vis_layerAlpha %f\n", vis_layerAlpha.GetFloat() );
    f->Printf( "seta vis_layerColorize %d\n", vis_layerColorize.GetBool() ? 1 : 0 );
    f->Printf( "seta vis_layerHue %f\n", vis_layerHue.GetFloat() );
    f->Printf( "seta vis_layerX %f\n", vis_layerX.GetFloat() );
    f->Printf( "seta vis_layerY %f\n", vis_layerY.GetFloat() );
    f->Printf( "seta vis_mapWireHue %f\n", vis_mapWireHue.GetFloat() );
    f->Printf( "seta vis_mapBgHue %f\n", vis_mapBgHue.GetFloat() );
    f->Printf( "seta vis_mapBgTint %d\n", vis_mapBgTint.GetBool() ? 1 : 0 );
    f->Printf( "seta vis_mapWireBlend %d\n", vis_mapWireBlend.GetInteger() );
    f->Printf( "seta vis_mapBgBlend %d\n", vis_mapBgBlend.GetInteger() );
    f->Printf( "seta vis_mapWireOnTop %d\n", vis_mapWireOnTop.GetBool() ? 1 : 0 );
    // PRD Pillar C real add/remove layer stack: round-trip through the exact
    // same console commands the GUI/user would type, exactly like the
    // vis_route lines below already do for modulation routing.
    f->Printf( "vis_stackClear\n" );
    for ( int i = 0; i < s_layerStack.Num(); i++ ) {
        const visStackLayer_t & layer = s_layerStack[i];
        f->Printf( "vis_stackAdd %d %f %d %d %f %f\n", layer.effectType, layer.opacity,
            layer.blendMode, layer.enabled ? 1 : 0, layer.posX, layer.posY );
    }
    for ( int t = 0; t < s_targetDefs.Num(); t++ ) {
        const visRoute_t & r = s_routes[t];
        const int sh = ( r.shape >= 0 && r.shape < VSHAPE_COUNT ) ? r.shape : VSHAPE_LINEAR;
        // PRD FR-C8: only emit the trailing shape args when a shape/invert is
        // actually set, so a linear route serializes byte-for-byte identically
        // to before this feature (and old saves without these args reload fine,
        // defaulting to linear). vis_route's parser round-trips the extra args.
        if ( sh == VSHAPE_LINEAR && !r.invert ) {
            f->Printf( "vis_route %s %s %f %f\n", s_targetDefs[t].name,
                s_sourceNames[ r.source ], r.amount, r.base );
        } else {
            f->Printf( "vis_route %s %s %f %f %s %f %d %f\n", s_targetDefs[t].name,
                s_sourceNames[ r.source ], r.amount, r.base,
                s_shapeNames[sh], r.shapeParam, r.invert ? 1 : 0, r.shapeParam2 );
        }
    }
    // PRD FR-C9: DMX channel assignments round-trip through the same console
    // command the GUI/user would type (the vis_dmxEnable/IP/universe cvars are
    // CVAR_ARCHIVE and persist on their own via seta, above/elsewhere).
    for ( int c = 0; c < 512; c++ ) {
        const int t = VisDmxChannelTarget( c );
        if ( t >= 0 && t < s_targetDefs.Num() ) {
            f->Printf( "vis_dmxChannel %d %s\n", c + 1, s_targetDefs[t].name );
        }
    }
}

bool idVisualizerManager::SavePreset( const char * name ) {
    if ( name == NULL || name[0] == '\0' ) {
        idLib::Printf( "usage: vis_presetSave <name>\n" );
        return false;
    }
    idStr rel = va( "presets/%s.cfg", name );
    idFile * f = fileSystem->OpenFileWrite( rel.c_str() );
    if ( f == NULL ) {
        idLib::Warning( "visualizer: could not write preset '%s'", rel.c_str() );
        return false;
    }
    VisWritePresetTo( f );
    fileSystem->CloseFile( f );
    idLib::Printf( "visualizer: saved preset %s\n", rel.c_str() );
    return true;
}

void idVisualizerManager::SaveCurrentAsCustom() {
    // find the first free custom-NN name so we never silently overwrite
    idStr name;
    for ( int n = 1; n < 100; n++ ) {
        name = va( "custom-%02d", n );
        idFile * probe = fileSystem->OpenFileRead( va( "presets/%s.cfg", name.c_str() ) );
        if ( probe == NULL ) {
            break;   // this slot is free
        }
        fileSystem->CloseFile( probe );
    }
    SavePreset( name.c_str() );
}

void idVisualizerManager::LoadPresetPath( const char * cfgPath ) {
    if ( cfgPath == NULL || cfgPath[0] == '\0' ) {
        return;
    }
    // Fixed real bug (direct user report: "when i load presets via preset
    // cycling the center gets put to the right again"): LoadPresetPath just
    // `exec`s the target .cfg -- it only ever CHANGES the cvars/routes that
    // file actually mentions, so anything it doesn't mention keeps whatever
    // value was already live. vis_effectX/Y (and friends) didn't exist when
    // every preset shipped in base/presets/ was written, so none of them
    // contain a `seta vis_effectX ...` line -- loading one never puts
    // position back to 0, it just leaves it wherever it last was (e.g.
    // mid-drag, or from trying the new sliders). 0 is unambiguously the
    // right "preset switch" default for position specifically (unlike scale/
    // hue/etc, which have real preset-to-preset variety), so reset it here,
    // BEFORE the exec, rather than requiring every current and future preset
    // file to explicitly re-state "0" for something that should just be the
    // baseline. A preset saved from now on (VisWritePresetTo emits explicit
    // seta lines for all four) still overrides this immediately after, same
    // as any other cvar it sets.
    vis_effectX.SetFloat( 0.0f );
    vis_effectY.SetFloat( 0.0f );
    vis_layerX.SetFloat( 0.0f );
    vis_layerY.SetFloat( 0.0f );
    for ( int i = 0; i < s_layerStack.Num(); i++ ) {
        s_layerStack[i].posX = 0.0f;
        s_layerStack[i].posY = 0.0f;
    }
    // Also un-bind any modulation route left pointing at these -- same gap,
    // different symptom: an old preset (or the auto-cycle timer) has no way
    // to say "and stop routing mousex to effectxmod" if it was never told
    // that target exists in the first place.
    VisInitModTargets();
    const int posRouteTargets[] = { VMT_EFFECTX_MOD, VMT_EFFECTY_MOD, VMT_LAYERX_MOD, VMT_LAYERY_MOD };
    for ( int k = 0; k < 4; k++ ) {
        const int t = posRouteTargets[k];
        if ( t >= 0 && t < s_routes.Num() ) {
            s_routes[t] = { VMS_NONE, 0.0f, 0.0f };
        }
    }
    for ( int slot = 0; slot < VIS_MOD_STACK_SLOTS; slot++ ) {
        for ( int k = 0; k < 2; k++ ) {
            const int t = s_slotTarget[slot][ k == 0 ? (int)VSP_POSX : (int)VSP_POSY ];
            if ( t >= 0 && t < s_routes.Num() ) {
                s_routes[t] = { VMS_NONE, 0.0f, 0.0f };
            }
        }
    }
    cmdSystem->BufferCommandText( CMD_EXEC_APPEND, va( "exec %s\n", cfgPath ) );
    idLib::Printf( "visualizer: loading preset %s\n", cfgPath );
    idStr base = cfgPath;
    base.StripPath();
    base.StripFileExtension();
    VisSetBanner( va( "preset: %s", base.c_str() ) );
}

void idVisualizerManager::BuildCycleList() {
    m_cycleList.Clear();
    VisAppendFiles( m_cycleList, "presets", ".cfg" );

    // build the play order (identity, or a shuffle via a seeded Fisher-Yates)
    m_cycleOrder.Clear();
    const int n = m_cycleList.Num();
    for ( int i = 0; i < n; i++ ) {
        m_cycleOrder.Append( i );
    }
    if ( vis_presetCycleShuffle.GetBool() && n > 1 ) {
        unsigned int seed = (unsigned int)( Sys_Milliseconds() ^ ( n * 2654435761u ) );
        for ( int i = n - 1; i > 0; i-- ) {
            seed = seed * 1664525u + 1013904223u;      // LCG
            const int j = (int)( ( seed >> 8 ) % (unsigned int)( i + 1 ) );
            const int tmp = m_cycleOrder[i];
            m_cycleOrder[i] = m_cycleOrder[j];
            m_cycleOrder[j] = tmp;
        }
    }
    m_cycleIndex = 0;
    m_cyclePending = false;
    m_cycleNextMs = Sys_Milliseconds() + (int)( vis_presetCycleSecs.GetFloat() * 1000.0f );
}

void idVisualizerManager::AdvancePreset() {
    if ( m_cycleList.Num() == 0 ) {
        return;
    }
    m_cycleIndex = ( m_cycleIndex + 1 ) % m_cycleOrder.Num();
    const int fileIdx = m_cycleOrder[m_cycleIndex];
    if ( fileIdx >= 0 && fileIdx < m_cycleList.Num() ) {
        LoadPresetPath( m_cycleList[fileIdx].c_str() );
    }
}

// Manual preset navigation for the SPACE / BACKSPACE hotkeys. Reuses the
// auto-cycle list (building it on first use so the hotkeys work even when
// vis_presetCycle is off) and steps the play order by +/-1 with wraparound.
void idVisualizerManager::NavigatePreset( int dir ) {
    if ( m_cycleList.Num() == 0 ) {
        BuildCycleList();
    }
    if ( m_cycleOrder.Num() == 0 ) {
        return;
    }
    const int n = m_cycleOrder.Num();
    m_cycleIndex = ( ( m_cycleIndex + dir ) % n + n ) % n;   // wrap both directions
    const int fileIdx = m_cycleOrder[m_cycleIndex];
    if ( fileIdx >= 0 && fileIdx < m_cycleList.Num() ) {
        LoadPresetPath( m_cycleList[fileIdx].c_str() );
    }
}

// kinds for visDisplayItem_t
enum { DISP_FS_MONITOR = 0, DISP_WINDOWED = 1 };
static const int s_windowedPresets[][2] = { { 1280, 720 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 } };

// vis_layerList is a ';'-separated list of image paths. Helpers to query/toggle.
static bool VisLayerListHas( const char * list, const char * name ) {
    idStr s = list;
    int start = 0;
    for ( int i = 0; i <= s.Length(); i++ ) {
        if ( i == s.Length() || s[i] == ';' ) {
            idStr t = s.Mid( start, i - start );
            t.StripLeading( ' ' );
            t.StripTrailing( ' ' );
            if ( idStr::Icmp( t.c_str(), name ) == 0 ) {
                return true;
            }
            start = i + 1;
        }
    }
    return false;
}

// PRD follow-up (direct, repeated user request: "every time i launch the
// app i need to re-register paths for maps and milkdrop presets ... is
// there a way we can save these to a config file so i dont have to keep
// inputting them"). Appends `dir` to `cvar`'s semicolon list if it isn't
// already there (reusing VisLayerListHas' same list format/membership
// check), so every directory ever registered accumulates in a
// CVAR_ARCHIVE cvar and survives a restart.
static void VisRememberSearchPath( idCVar & cvar, const char * dir ) {
    if ( dir == NULL || dir[0] == '\0' || VisLayerListHas( cvar.GetString(), dir ) ) {
        return;
    }
    idStr updated = cvar.GetString();
    if ( updated.Length() > 0 ) {
        updated += ";";
    }
    updated += dir;
    cvar.SetString( updated.c_str() );
}

// Re-registers every directory remembered by vis_mapSearchPathSaved/
// vis_presetSearchPathSaved with the file system -- called once at startup
// (see Frame()) so a user never has to retype/reclick Register every
// session. idFileSystem::RegisterModSearchPath is purely additive/
// idempotent-safe to call for a path that's already registered.
static void VisReapplySavedSearchPaths() {
    idStr mapList = vis_mapSearchPathSaved.GetString();
    int start = 0;
    for ( int i = 0; i <= mapList.Length(); i++ ) {
        if ( i == mapList.Length() || mapList[i] == ';' ) {
            idStr t = mapList.Mid( start, i - start );
            t.StripLeading( ' ' );
            t.StripTrailing( ' ' );
            if ( t.Length() > 0 ) {
                fileSystem->RegisterModSearchPath( t.c_str() );
            }
            start = i + 1;
        }
    }
    idStr presetList = vis_presetSearchPathSaved.GetString();
    start = 0;
    for ( int i = 0; i <= presetList.Length(); i++ ) {
        if ( i == presetList.Length() || presetList[i] == ';' ) {
            idStr t = presetList.Mid( start, i - start );
            t.StripLeading( ' ' );
            t.StripTrailing( ' ' );
            if ( t.Length() > 0 ) {
                fileSystem->RegisterModSearchPath( t.c_str() );
                s_lastMilkSearchPath = t;   // last one wins, matches manual Register's own behavior
            }
            start = i + 1;
        }
    }
}

static idStr VisLayerListToggle( const char * list, const char * name ) {
    idStr s = list;
    idStr result;
    bool removed = false;
    int start = 0;
    for ( int i = 0; i <= s.Length(); i++ ) {
        if ( i == s.Length() || s[i] == ';' ) {
            idStr t = s.Mid( start, i - start );
            t.StripLeading( ' ' );
            t.StripTrailing( ' ' );
            if ( t.Length() > 0 ) {
                if ( idStr::Icmp( t.c_str(), name ) == 0 ) {
                    removed = true;                      // drop it (toggle off)
                } else {
                    if ( result.Length() ) { result += ";"; }
                    result += t;
                }
            }
            start = i + 1;
        }
    }
    if ( !removed ) {                                    // wasn't present -> add it
        if ( result.Length() ) { result += ";"; }
        result += name;
    }
    return result;
}

void idVisualizerManager::BuildDisplayItems() {
    m_displayItems.clear();

    // one "fullscreen on monitor N (native)" row per attached monitor
    idList<vidMode_t> modes;
    for ( int d = 0; R_GetModeListForDisplay( d, modes ); d++ ) {
        int nw = 0, nh = 0, bestArea = 0;
        for ( int i = 0; i < modes.Num(); i++ ) {
            const int area = modes[i].width * modes[i].height;
            if ( area > bestArea ) {
                bestArea = area;
                nw = modes[i].width;
                nh = modes[i].height;
            }
        }
        visDisplayItem_t it;
        it.kind = DISP_FS_MONITOR;
        it.mon  = d + 1;                 // r_fullscreen is 1-based
        it.w    = nw;
        it.h    = nh;
        it.label = va( "Fullscreen  -  Monitor %d  (%dx%d)", d + 1, nw, nh );
        m_displayItems.push_back( it );
    }

    // windowed presets (skip any larger than the largest detected monitor)
    for ( int i = 0; i < 5; i++ ) {
        visDisplayItem_t it;
        it.kind = DISP_WINDOWED;
        it.mon  = 0;
        it.w    = s_windowedPresets[i][0];
        it.h    = s_windowedPresets[i][1];
        it.label = va( "Windowed  -  %dx%d", it.w, it.h );
        m_displayItems.push_back( it );
    }
}

// Apply a video mode change: set the renderer cvars, restart the trail cleanly,
// force the cached materials to re-resolve, and trigger a (lightweight) vid_restart.
static void VisApplyVideoMode() {
    s_feedbackCaptured = false;          // start the trail clean at the new size
    s_visSolid = NULL;                   // re-resolve after the mode change (insurance;
    s_visSolidAdd = NULL;
    s_visFeedback = NULL;                // vid_restart here doesn't purge, but harmless)
    s_visFeedbackA = NULL;
    s_visFeedbackB = NULL;
    s_visFeedbackActive = NULL;          // BeginOffscreenRender resizes the images themselves
    s_milkPingPongParity = false;        // automatically; this just restarts the read/write pick cleanly
    s_visWarp = NULL;
    cmdSystem->BufferCommandText( CMD_EXEC_APPEND, "vid_restart\n" );
}

void idVisualizerManager::OpenMenu() {
    m_musicFiles.Clear();
    {
        static const char * const kMusicExts[] = { ".wav", ".ogg", ".mp3", ".flac", ".m4a", ".aac", ".opus", ".wma" };
        for ( int e = 0; e < 8; e++ ) {
            VisAppendFiles( m_musicFiles, "music", kMusicExts[e] );
        }
    }
    m_playlistFiles.Clear();
    VisAppendFiles( m_playlistFiles, "playlists", ".m3u" );
    VisAppendFiles( m_playlistFiles, "playlists", ".m3u8" );
    m_presetFiles.Clear();
    VisAppendFiles( m_presetFiles, "presets", ".cfg" );
    // Fixed real bug: real MilkDrop .milk presets under presets/milk were
    // never reachable from this menu at all -- only the vis_milkPreset
    // console command could load one. The PRESETS tab only ever scanned
    // for .cfg (this project's own simpler saved-settings format), so
    // selecting anything here could never set s_milkActive, leaving the
    // screen on the plain backdrop/menu with no warp mesh/waveform ever
    // drawn. MenuActivate below now branches on extension to load either
    // kind correctly.
    VisAppendFiles( m_presetFiles, vis_milkPresetSubdir.GetString(), ".milk" );
    // device list: a synthetic "system default" entry first, then enumerated ones
    // (EnumerateDevices clears its out vector, so enumerate into a temp).
    std::vector<AudioDeviceInfo> devs;
    AudioAnalyzer::EnumerateDevices( devs );
    m_deviceList.clear();
    AudioDeviceInfo def;
    def.name = "System default output (loopback)";
    def.id = "";
    def.isCapture = false;
    m_deviceList.push_back( def );
    for ( size_t i = 0; i < devs.size(); i++ ) {
        m_deviceList.push_back( devs[i] );
    }
    BuildDisplayItems();
    m_imageFiles.Clear();
    VisAppendFiles( m_imageFiles, "images", ".tga" );
    VisAppendFiles( m_imageFiles, "images", ".png" );
    VisAppendFiles( m_imageFiles, "images", ".jpg" );
    m_menuSel = 0;
    m_menuScroll = 0;
    m_menuOpen = true;
    if ( console != NULL ) {
        console->Close();   // so key events reach the menu, not the console
    }
}

void idVisualizerManager::CloseMenu() {
    m_menuOpen = false;
}

void idVisualizerManager::ToggleMenu() {
    if ( m_menuOpen ) {
        CloseMenu();
    } else {
        OpenMenu();
    }
}

int idVisualizerManager::MenuItemCount() const {
    switch ( m_menuTab ) {
        case TAB_MUSIC:     return m_musicFiles.Num();
        case TAB_PLAYLISTS: return m_playlistFiles.Num();
        case TAB_PRESETS:   return m_presetFiles.Num() + 1;   // +1 = "save current" row
        case TAB_DEVICES:   return (int)m_deviceList.size();
        case TAB_DISPLAY:   return (int)m_displayItems.size();
        case TAB_IMAGES:    return m_imageFiles.Num() + 1;   // +1 = "off" row
        default:            return EFX_COUNT;
    }
}

void idVisualizerManager::MenuActivate() {
    if ( m_menuTab == TAB_MUSIC ) {
        if ( m_menuSel >= 0 && m_menuSel < m_musicFiles.Num() ) {
            Play( m_musicFiles[m_menuSel].c_str() );
            CloseMenu();
        }
        return;
    }
    if ( m_menuTab == TAB_PLAYLISTS ) {
        if ( m_menuSel >= 0 && m_menuSel < m_playlistFiles.Num() ) {
            Play( m_playlistFiles[m_menuSel].c_str() );
            CloseMenu();
        }
        return;
    }
    if ( m_menuTab == TAB_PRESETS ) {
        if ( m_menuSel == 0 ) {
            // row 0 = capture the current visual state to a new custom-NN preset
            SaveCurrentAsCustom();
            m_presetFiles.Clear();
            VisAppendFiles( m_presetFiles, "presets", ".cfg" );          // rescan so it shows
            VisAppendFiles( m_presetFiles, vis_milkPresetSubdir.GetString(), ".milk" );
        } else {
            const int fi = m_menuSel - 1;
            if ( fi >= 0 && fi < m_presetFiles.Num() ) {
                const idStr & path = m_presetFiles[fi];
                idStr ext;
                path.ExtractFileExtension( ext );
                if ( ext.Icmp( "milk" ) == 0 ) {
                    VisLoadMilkPreset( path.c_str() );
                } else {
                    LoadPresetPath( path.c_str() );
                }
                CloseMenu();
            }
        }
        return;
    }
    if ( m_menuTab == TAB_DEVICES ) {
        if ( m_menuSel >= 0 && m_menuSel < (int)m_deviceList.size() ) {
            const AudioDeviceInfo & d = m_deviceList[m_menuSel];
            g_audioAnalyzer.SetCaptureDevice( d.id, d.isCapture );
            m_sourceArmed = true;   // user picked explicitly; stop boot auto-arm
        }
        return;   // keep the menu open so the '*' visibly moves
    }
    if ( m_menuTab == TAB_DISPLAY ) {
        if ( m_menuSel >= 0 && m_menuSel < (int)m_displayItems.size() ) {
            const visDisplayItem_t & it = m_displayItems[m_menuSel];
            cvarSystem->SetCVarInteger( "r_vidMode", -1 );   // use custom w/h
            cvarSystem->SetCVarInteger( "r_customWidth", it.w );
            cvarSystem->SetCVarInteger( "r_customHeight", it.h );
            cvarSystem->SetCVarInteger( "r_fullscreen", ( it.kind == DISP_FS_MONITOR ) ? it.mon : 0 );
            VisApplyVideoMode();
            CloseMenu();   // avoid drawing across the mode switch; reopen with F1
        }
        return;
    }
    if ( m_menuTab == TAB_IMAGES ) {
        if ( m_menuSel == 0 ) {
            vis_layerList.SetString( "" );       // row 0 = clear all layers
            vis_layer.SetBool( false );
        } else if ( m_menuSel - 1 < m_imageFiles.Num() ) {
            const idStr next = VisLayerListToggle( vis_layerList.GetString(), m_imageFiles[m_menuSel - 1].c_str() );
            vis_layerList.SetString( next.c_str() );
            vis_layer.SetBool( next.Length() > 0 );   // auto on/off with the list
        }
        return;   // keep the menu open so the '*' visibly toggles
    }
    // effects tab
    switch ( m_menuSel ) {
        case EFX_BARS:       vis_effect.SetInteger( 0 ); break;
        case EFX_RADIAL:     vis_effect.SetInteger( 1 ); break;
        case EFX_SCOPE:      vis_effect.SetInteger( 2 ); break;
        case EFX_RING:       vis_effect.SetInteger( 3 ); break;
        case EFX_PARTICLES:  vis_effect.SetInteger( 4 ); break;
        case EFX_SPECTRO:    vis_effect.SetInteger( 5 ); break;
        case EFX_STARFIELD:  vis_effect.SetInteger( 6 ); break;
        case EFX_PHASE:      vis_effect.SetInteger( 7 ); break;
        case EFX_FEEDBACK:   vis_feedback.SetBool( !vis_feedback.GetBool() ); break;
        case EFX_WARP:       vis_warp.SetInteger( ( vis_warp.GetInteger() + 1 ) % 4 ); break;
        case EFX_WARPMODE:   vis_warpMode.SetInteger( ( vis_warpMode.GetInteger() + 1 ) % 4 ); break;
        case EFX_BLOOM:      vis_bloom.SetFloat( vis_bloom.GetFloat() > 0.01f ? 0.0f : 0.5f ); break;
        case EFX_KALEIDO: {
            // cycle off -> 4 -> 6 -> 8 -> 12 -> off
            const int cur = vis_kaleido.GetInteger();
            int next = 0;
            if ( cur < 4 )       next = 4;
            else if ( cur < 6 )  next = 6;
            else if ( cur < 8 )  next = 8;
            else if ( cur < 12 ) next = 12;
            else                 next = 0;
            vis_kaleido.SetInteger( next );
            break;
        }
        case EFX_FULLSCREEN: vis_fullscreen.SetBool( !vis_fullscreen.GetBool() ); break;
        case EFX_BANDS: {
            const int cur = vis_bands.GetInteger();
            int idx = 0;
            for ( int i = 0; i < 4; i++ ) { if ( s_bandChoices[i] == cur ) { idx = i; break; } }
            vis_bands.SetInteger( s_bandChoices[( idx + 1 ) % 4] );
            break;
        }
        case EFX_MOD:        vis_mod.SetBool( !vis_mod.GetBool() ); break;
        case EFX_PALETTE:    vis_palette.SetInteger( ( vis_palette.GetInteger() + 1 ) % 5 ); break;
        case EFX_PRESETCYCLE: vis_presetCycle.SetBool( !vis_presetCycle.GetBool() ); break;
        // PRD (post-M4 follow-up): a menu-reachable way to reopen the
        // layer-editor tool window after closing it via its own X button
        // (which just hides it, see win_imgui.cpp) -- the K_G hotkey below
        // is the other way in.
        case EFX_IMGUI:      vis_imguiEditor.SetBool( !vis_imguiEditor.GetBool() ); break;
        default: break;
    }
}

bool idVisualizerManager::MenuProcessEvent( const sysEvent_t * ev ) {
    if ( ev == NULL ) {
        return false;
    }
    // PRD Pillar C follow-up: track raw cursor position for the "mousex"/
    // "mousey" modulation sources (see VMS_MOUSEX/VMS_MOUSEY, VisSampleSource).
    // Never consumes the event (always falls through to `return false` below,
    // same as every other non-SE_KEY event already did) -- this only
    // observes position, it doesn't compete with SWF/game input for it.
    if ( ev->evType == SE_MOUSE_ABSOLUTE ) {
        const int w = renderSystem->GetWidth();
        const int h = renderSystem->GetHeight();
        if ( w > 0 ) {
            s_mouseNormX = idMath::ClampFloat( 0.0f, 1.0f, (float)ev->GetXCoord() / (float)w );
        }
        if ( h > 0 ) {
            s_mouseNormY = idMath::ClampFloat( 0.0f, 1.0f, (float)ev->GetYCoord() / (float)h );
        }
        return false;
    }
    if ( ev->evType != SE_KEY ) {
        return false;
    }
    const int key = ev->evValue;
    const bool down = ( ev->evValue2 != 0 );

    if ( key == K_F1 ) {                 // global toggle
        if ( down ) {
            ToggleMenu();
        }
        return true;
    }
    // PRD (post-M4 follow-up): the layer-editor tool window (win_imgui.cpp)
    // is a separate top-level window from this one now -- closing it via
    // its own X button just hides it (vis_imguiEditor off), with no other
    // way back in until now. Unconditional like K_F1 above (not nested
    // inside the "!m_menuOpen" MilkDrop-hotkey block below), since this
    // toggles an entirely separate window/system, not anything about the
    // F1 overlay menu's own open/closed state. Also reachable from the F1
    // menu's EFFECTS tab (EFX_IMGUI).
    if ( key == K_G ) {
        if ( down ) {
            vis_imguiEditor.SetBool( !vis_imguiEditor.GetBool() );
        }
        return true;
    }
    if ( !m_menuOpen ) {
        // MilkDrop-style global hotkeys while the menu is closed. F1 (above)
        // opens the menu; these navigate presets/effects without it. Anything
        // else falls through so the console and game keep their input.
        if ( down ) {
            switch ( key ) {
                case K_SPACE:     NavigatePreset(  1 ); return true;   // next preset
                case K_BACKSPACE: NavigatePreset( -1 ); return true;   // prev preset
                case K_L: {   // PRD FR-D1: "load specific (opens browser)" -- the PRESETS
                    // tab (OpenMenu/MenuActivate) already lists AND loads both .cfg and
                    // .milk presets; this is just a direct shortcut to it instead of
                    // requiring F1 then TAB-cycling to the right tab.
                    OpenMenu();
                    m_menuTab = TAB_PRESETS;
                    return true;
                }
                case K_N:         vis_effect.SetInteger( ( vis_effect.GetInteger() + 1 ) % 8 ); return true;   // next effect
                // PRD FR-D1/D3/D4 (MilkDrop3 hotkey parity, roadmap "candidate additions"):
                case K_R:         vis_presetCycleShuffle.SetBool( !vis_presetCycleShuffle.GetBool() ); return true;   // shuffle order
                // Fixed real bug: K_GRAVE (backtick) never reached this switch at all
                // in any non-ID_RETAIL build -- idConsoleLocal::ProcessEvent
                // (Console.cpp) unconditionally intercepts K_GRAVE to toggle the
                // console whenever com_allowConsole is set, and that cvar defaults
                // to 1 outside ID_RETAIL builds (Common.cpp), running BEFORE
                // MenuProcessEvent is ever called (Common.cpp's ProcessEvent order).
                // F4 has no such interception anywhere in the engine.
                case K_F4:        vis_presetCycle.SetBool( !vis_presetCycle.GetBool() ); return true;                 // lock/unlock (pause auto-cycle)
                case K_C:         vis_palette.SetInteger( ( vis_palette.GetInteger() + 1 ) % 5 ); return true;        // next palette
                case K_F3: {                                                                                          // cycle time-between-presets
                    static const float s_cycleSecsChoices[] = { 15.0f, 30.0f, 60.0f, 120.0f, 300.0f };
                    const int nChoices = sizeof( s_cycleSecsChoices ) / sizeof( s_cycleSecsChoices[0] );
                    int idx = 0;
                    for ( int i = 0; i < nChoices; i++ ) {
                        if ( vis_presetCycleSecs.GetFloat() >= s_cycleSecsChoices[i] - 0.5f ) {
                            idx = i;
                        }
                    }
                    vis_presetCycleSecs.SetFloat( s_cycleSecsChoices[ ( idx + 1 ) % nChoices ] );
                    return true;
                }
                case K_F6: {   // PRD FR-D4: rate the current preset (cycles 0..5, persisted -- see M7's "ratings persistence" note)
                    if ( !s_milkPreset.GetName().IsEmpty() ) {
                        const int newRating = ( VisGetMilkRating( s_milkPreset.GetName().c_str() ) + 1 ) % 6;
                        VisSetMilkRating( s_milkPreset.GetName().c_str(), newRating );
                        VisSetBanner( va( "rating: %d/5", newRating ) );
                    }
                    return true;
                }
                case K_F8: vis_presetCycleOnBeat.SetBool( !vis_presetCycleOnBeat.GetBool() ); return true;   // auto-cut-on-beat
                case K_F11:   // toggle all effects (feedback/warp/bloom master), save-and-restore
                    VisToggleEffectsMasterOff();
                    return true;
                case K_F: VisCycleDisplayMode(); return true;   // FR-D6: windowed -> fullscreen monitor 1..N -> windowed
                case K_A: {   // FR-D6: mash-up -- start one (random B preset) or nudge mix toward B
                    if ( !s_milkActive ) {
                        return true;   // need a milk preset (A) active first
                    }
                    if ( !s_milkActiveB ) {
                        idStr nextPath;
                        if ( VisPickRandomMilkPreset( nextPath ) ) {
                            // Fixed real bug: VisLoadMilkPresetB() returns true as
                            // soon as the file loads and the evaluator inits, even
                            // for a preset with no per_pixel_ code (a documented,
                            // expected "loaded but inactive" case) -- checking its
                            // return value alone could set a mash-up mix with no
                            // actual B side active. s_milkActiveB is the real signal.
                            if ( VisLoadMilkPresetB( nextPath.c_str() ) && s_milkActiveB ) {
                                s_milkMashMix = 0.5f;
                            }
                        }
                    } else {
                        s_milkMashMix = idMath::ClampFloat( 0.0f, 1.0f, s_milkMashMix + 0.1f );
                    }
                    return true;
                }
                case K_Z:   // FR-D6: mash-up -- nudge mix back toward A, clearing it at 0
                    if ( s_milkActiveB ) {
                        s_milkMashMix -= 0.1f;
                        if ( s_milkMashMix <= 0.0f ) {
                            VisUnloadMilkPresetB();
                            s_milkMashMix = 0.0f;
                        }
                    }
                    return true;
                case K_LEFTARROW:
                    if ( idKeyInput::IsDown( K_LCTRL ) || idKeyInput::IsDown( K_RCTRL ) ) {
                        Prev(); return true;   // FR-D5 player transport: CTRL+left = prev track
                    }
                    break;
                case K_RIGHTARROW:
                    if ( idKeyInput::IsDown( K_LCTRL ) || idKeyInput::IsDown( K_RCTRL ) ) {
                        Next(); return true;   // FR-D5 player transport: CTRL+right = next track
                    }
                    break;
            }
        }
        return false;                    // menu closed: don't touch other input
    }
    if ( !down ) {
        return true;                     // swallow key-ups while we own input
    }

    switch ( key ) {
        case K_ESCAPE:
            CloseMenu();
            return true;
        case K_TAB:
        case K_RIGHTARROW:
            m_menuTab = ( m_menuTab + 1 ) % VIS_MENU_TABS;
            m_menuSel = 0; m_menuScroll = 0;
            return true;
        case K_LEFTARROW:
            m_menuTab = ( m_menuTab + VIS_MENU_TABS - 1 ) % VIS_MENU_TABS;
            m_menuSel = 0; m_menuScroll = 0;
            return true;
        case K_UPARROW: {
            const int c = MenuItemCount();
            if ( c > 0 ) { m_menuSel = ( m_menuSel + c - 1 ) % c; }
            return true;
        }
        case K_DOWNARROW: {
            const int c = MenuItemCount();
            if ( c > 0 ) { m_menuSel = ( m_menuSel + 1 ) % c; }
            return true;
        }
        case K_ENTER:
        case K_KP_ENTER:
            MenuActivate();
            return true;
        default:
            return true;                 // consume all other keys while open
    }
}

void idVisualizerManager::MenuDraw2D() {
    if ( !m_menuOpen || s_visSolid == NULL ) {
        return;
    }
    const idVec4 white( 1.0f, 1.0f, 1.0f, 1.0f );
    const idVec4 dim( 0.55f, 0.55f, 0.60f, 1.0f );
    const idVec4 accent( 0.30f, 0.85f, 1.0f, 1.0f );

    // dim the visualizer behind the menu
    VisFillRect( idVec4( 0.02f, 0.02f, 0.05f, 0.88f ), 0, 0, VIS_W, VIS_H );

    // title + tab bar
    renderSystem->DrawSmallStringExt( 24, 20, "DOOM-3-BFG VISUALIZER", accent, true );
    int tx = 24;
    for ( int t = 0; t < VIS_MENU_TABS; t++ ) {
        const bool on = ( t == m_menuTab );
        if ( on ) {
            const int wpx = ( idStr::Length( s_menuTabNames[t] ) + 2 ) * SMALLCHAR_WIDTH;
            VisFillRect( idVec4( 0.10f, 0.35f, 0.50f, 1.0f ), (float)tx - 4, 42.0f, (float)wpx, 20.0f );
        }
        renderSystem->DrawSmallStringExt( tx, 44, s_menuTabNames[t], on ? white : dim, true );
        tx += ( idStr::Length( s_menuTabNames[t] ) + 3 ) * SMALLCHAR_WIDTH;
    }
    VisFillRect( idVec4( 0.15f, 0.4f, 0.55f, 0.9f ), 24, 66, VIS_W - 48, 1.0f );

    // list geometry
    const int listTop = 78;
    const int rowH = 18;
    const int maxRows = 18;
    const int count = MenuItemCount();

    // keep the highlight on screen
    if ( m_menuSel < m_menuScroll ) {
        m_menuScroll = m_menuSel;
    } else if ( m_menuSel >= m_menuScroll + maxRows ) {
        m_menuScroll = m_menuSel - maxRows + 1;
    }
    if ( m_menuScroll < 0 ) {
        m_menuScroll = 0;
    }

    if ( count == 0 ) {
        renderSystem->DrawSmallStringExt( 32, listTop, "(nothing here - drop files under base/music or base/playlists)", dim, true );
    }

    for ( int row = 0; row < maxRows; row++ ) {
        const int i = m_menuScroll + row;
        if ( i >= count ) {
            break;
        }
        const int y = listTop + row * rowH;
        const bool sel = ( i == m_menuSel );
        if ( sel ) {
            VisFillRect( idVec4( 0.10f, 0.30f, 0.45f, 0.95f ), 24, (float)y - 2, VIS_W - 48, (float)rowH );
        }

        idStr label;
        if ( m_menuTab == TAB_MUSIC ) {
            label = m_musicFiles[i];
        } else if ( m_menuTab == TAB_PLAYLISTS ) {
            label = m_playlistFiles[i];
        } else if ( m_menuTab == TAB_PRESETS ) {
            if ( i == 0 ) {
                label = "[ + save current as new preset ]";
            } else {
                m_presetFiles[i - 1].ExtractFileName( label );   // basename
                label.StripFileExtension();
            }
        } else if ( m_menuTab == TAB_DEVICES ) {
            const AudioDeviceInfo & d = m_deviceList[i];
            const bool active = ( d.id == g_audioAnalyzer.GetCaptureDeviceId() &&
                                  d.isCapture == g_audioAnalyzer.GetCaptureIsCapture() &&
                                  g_audioAnalyzer.GetSourceMode() == AudioSourceMode::WASAPI_LOOPBACK );
            const char * tag = d.name.empty() ? "" : ( d.isCapture ? " [in]" : " [out]" );
            label = va( "%s%s%s", d.name.c_str(), tag, active ? "  *" : "" );
        } else if ( m_menuTab == TAB_DISPLAY ) {
            const visDisplayItem_t & di = m_displayItems[i];
            const bool active = ( cvarSystem->GetCVarInteger( "r_customWidth" ) == di.w &&
                                  cvarSystem->GetCVarInteger( "r_customHeight" ) == di.h &&
                                  cvarSystem->GetCVarInteger( "r_fullscreen" ) == ( ( di.kind == DISP_FS_MONITOR ) ? di.mon : 0 ) );
            label = va( "%s%s", di.label.c_str(), active ? "  *" : "" );
        } else if ( m_menuTab == TAB_IMAGES ) {
            if ( i == 0 ) {
                label = "[ clear all layers ]";
            } else {
                idStr nm = m_imageFiles[i - 1];
                nm.StripPath();
                const bool active = VisLayerListHas( vis_layerList.GetString(), m_imageFiles[i - 1].c_str() );
                label = va( "%s%s", nm.c_str(), active ? "  *" : "" );
            }
        } else {
            switch ( i ) {
                case EFX_BARS:       label = va( "Effect: Bars%s",      vis_effect.GetInteger() == 0 ? "  *" : "" ); break;
                case EFX_RADIAL:     label = va( "Effect: Radial%s",    vis_effect.GetInteger() == 1 ? "  *" : "" ); break;
                case EFX_SCOPE:      label = va( "Effect: Scope%s",     vis_effect.GetInteger() == 2 ? "  *" : "" ); break;
                case EFX_RING:       label = va( "Effect: Ring%s",      vis_effect.GetInteger() == 3 ? "  *" : "" ); break;
                case EFX_PARTICLES:  label = va( "Effect: Particles%s", vis_effect.GetInteger() == 4 ? "  *" : "" ); break;
                case EFX_SPECTRO:    label = va( "Effect: Spectrogram%s", vis_effect.GetInteger() == 5 ? "  *" : "" ); break;
                case EFX_STARFIELD:  label = va( "Effect: Starfield%s", vis_effect.GetInteger() == 6 ? "  *" : "" ); break;
                case EFX_PHASE:      label = va( "Effect: Phase Scope%s", vis_effect.GetInteger() == 7 ? "  *" : "" ); break;
                case EFX_FEEDBACK:   label = va( "Feedback trail : %s", vis_feedback.GetBool() ? "ON" : "off" ); break;
                case EFX_WARP:       label = va( "Warp mesh      : %s", s_warpQualNames[ idMath::ClampInt( 0, 3, vis_warp.GetInteger() ) ] ); break;
                case EFX_WARPMODE:   label = va( "Warp shape     : %s", s_warpModeNames[ idMath::ClampInt( 0, 3, vis_warpMode.GetInteger() ) ] ); break;
                case EFX_KALEIDO:    if ( vis_kaleido.GetInteger() >= 2 ) label = va( "Kaleidoscope   : %d-fold", vis_kaleido.GetInteger() ); else label = "Kaleidoscope   : off"; break;
                case EFX_BLOOM:      label = va( "Bloom / glow   : %s", vis_bloom.GetFloat() > 0.01f ? "ON" : "off" ); break;
                case EFX_FULLSCREEN: label = va( "Effect fills window (not display mode): %s", vis_fullscreen.GetBool() ? "ON" : "off" ); break;
                case EFX_BANDS:      label = va( "Spectrum bands : %d", vis_bands.GetInteger() ); break;
                case EFX_MOD:        label = va( "Audio routing  : %s", vis_mod.GetBool() ? "ON" : "off" ); break;
                case EFX_PALETTE:    label = va( "Palette        : %s", paletteNames[ idMath::ClampInt( 0, 4, vis_palette.GetInteger() ) ] ); break;
                case EFX_PRESETCYCLE: label = va( "Preset cycle   : %s", vis_presetCycle.GetBool() ? "ON" : "off" ); break;
                case EFX_IMGUI:      label = va( "Layer editor GUI (G): %s", vis_imguiEditor.GetBool() ? "ON" : "off" ); break;
                default: break;
            }
        }
        renderSystem->DrawSmallStringExt( 32, y, label.c_str(), sel ? white : dim, true );
    }

    // footer hint + now-playing
    renderSystem->DrawSmallStringExt( 24, VIS_H - 40, "UP/DOWN select   LEFT/RIGHT tab   ENTER activate   ESC/F1 close", dim, true );
    if ( m_playing && m_nowPlaying.Length() > 0 ) {
        renderSystem->DrawSmallStringExt( 24, VIS_H - 22, va( "now playing: %s", m_nowPlaying.c_str() ), accent, true );
    }
}

void Vis_Draw2D() {
    g_visualizerManager.Draw2D();
}

CONSOLE_COMMAND( vis_nextEffect, "cycle to the next visual effect (bars/radial/scope)", NULL ) {
    extern idCVar vis_effect;
    const int next = ( vis_effect.GetInteger() + 1 ) % 8;
    vis_effect.SetInteger( next );
    const char * names[] = { "bars", "radial spectrum", "oscilloscope", "waveform ring", "particles", "spectrogram", "starfield", "phase scope" };
    idLib::Printf( "vis_effect %d (%s)\n", next, names[next] );
}

CONSOLE_COMMAND( vis_beatSens, "set beat sensitivity (bass must exceed this * envelope; default 1.6)", NULL ) {
    if ( args.Argc() != 2 ) {
        idLib::Printf( "usage: vis_beatSens <value>\n" );
        return;
    }
    g_audioAnalyzer.SetBeatSensitivity( atof( args.Argv( 1 ) ) );
}

static int VisFindName( const char * const * names, int count, const char * s ) {
    for ( int i = 0; i < count; i++ ) {
        if ( idStr::Icmp( names[i], s ) == 0 ) {
            return i;
        }
    }
    return -1;
}

// PRD M0: informational view of the layer-type catalog (s_layerTypes) + each
// type's current on/off state read from the existing cvars. No new render
// path yet -- the real multi-instance layer stack + compositing is M4.
// PRD M1 step 2: parse-only test command for the .milk parser (MilkPreset.h),
// verified against real projectM test fixtures. No eval/rendering yet --
// this exists purely to confirm the parser against real preset files before
// the eval-binding (step 3) and warp-mesh integration (step 4) land.
// Usage: vis_milkLoad presets/milk/foo.milk (path relative to search paths).
// PRD M1 step 6: preset ingestion -- recursively list every .milk under
// base/presets/milk (real packs are organized in nested per-pack
// subdirectories, e.g. presets/milk/cream-of-the-crop/Author - Name.milk;
// ListFilesTree's recursive flag already handles that). Console-command
// first, matching this project's established pattern; a MUSIC/PLAYLISTS-
// style MENU tab is M2 (preset browser / 'L' key) scope, not this step.
// PRD M1 step 4: activate a .milk preset as the live warp-mesh driver, or
// deactivate with "none"/no args. See VisLoadMilkPreset/VisUpdateMilkFrame/
// DrawMilkWarpMesh above for the full pipeline this wires together.
CONSOLE_COMMAND( vis_milkPreset, "load a .milk preset to drive the warp mesh live, or 'none' to clear (PRD M1 step 4)", NULL ) {
    if ( args.Argc() < 2 || idStr::Icmp( args.Argv( 1 ), "none" ) == 0 ) {
        VisUnloadMilkPreset();
        idLib::Printf( "visualizer: milk preset cleared\n" );
        return;
    }
    VisLoadMilkPreset( args.Argv( 1 ) );
}

// PRD FR-C10: import a ShaderToy Image-pass shader. Accepts a local file path
// (the paste-and-save workflow: copy the shader source from shadertoy.com into
// a local .glsl/.txt file, relative to a registered search path like
// vis_milkPreset). "none"/no args clears it. See VisLoadShaderToyFile and the
// ShaderToy block above for the uniform-adapter wrapper + documented limits.
CONSOLE_COMMAND( vis_shadertoyLoad, "import a ShaderToy Image-pass shader from a local file to drive the full-screen warp layer, or 'none' to clear: vis_shadertoyLoad <path> (PRD FR-C10)", NULL ) {
    if ( args.Argc() < 2 || idStr::Icmp( args.Argv( 1 ), "none" ) == 0 ) {
        VisUnloadShaderToy();
        idLib::Printf( "visualizer: ShaderToy layer cleared\n" );
        return;
    }
    const char * arg = args.Argv( 1 );
    // A bare ShaderToy shader ID (e.g. "MsjBDR" from the /view/ URL path) has
    // no path separator or extension. Programmatic fetch-by-ID needs a
    // ShaderToy API key in the general case (https://www.shadertoy.com/api),
    // so this is a documented follow-up rather than a half-network call with a
    // faked key -- guide the user to the local-file workflow instead.
    const bool looksLikeBareId = ( idStr::FindChar( arg, '/' ) == -1 ) && ( idStr::FindChar( arg, '\\' ) == -1 ) && ( idStr::FindChar( arg, '.' ) == -1 );
    if ( looksLikeBareId ) {
        idLib::Printf( "vis_shadertoyLoad: '%s' looks like a bare ShaderToy ID. Fetch-by-ID is not implemented (it needs a ShaderToy API key). "
                       "Open https://www.shadertoy.com/view/%s , copy the Image-tab source into a local .glsl file, and pass that file path instead.\n", arg, arg );
        return;
    }
    VisLoadShaderToyFile( arg );
}

// PRD M5: mash-ups (blend two full presets). Preset A (vis_milkPreset) must
// already be active; B blends in on top of A's warp mesh at the given mix
// (default 0.5). See VisLoadMilkPresetB/DrawMilkWarpMesh for the mechanism.
CONSOLE_COMMAND( vis_milkMash, "load a second preset to mash-up with the active one: vis_milkMash <path> [mix 0-1] (PRD M5)", NULL ) {
    if ( args.Argc() < 2 ) {
        idLib::Printf( "usage: vis_milkMash <path-relative-to-fs> [mix 0-1, default 0.5]\n" );
        return;
    }
    if ( VisLoadMilkPresetB( args.Argv( 1 ) ) ) {
        s_milkMashMix = ( args.Argc() >= 3 ) ? idMath::ClampFloat( 0.0f, 1.0f, atof( args.Argv( 2 ) ) ) : 0.5f;
        idLib::Printf( "vis_milkMash: mix = %.2f\n", s_milkMashMix );
    }
}

CONSOLE_COMMAND( vis_milkMashClear, "clear the mash-up B side, back to a single preset (PRD M5)", NULL ) {
    VisUnloadMilkPresetB();
    s_milkMashMix = 0.0f;
    idLib::Printf( "vis_milkMashClear: cleared\n" );
}

// PRD FR-A5: manual trigger for a preset auto-advance -- the same
// soft/hard-cut mechanism vis_presetCycle's timer/beat gate drives
// automatically, exposed directly for on-demand use and testing (the
// timer path alone can take up to vis_presetCycleSecs to exercise).
CONSOLE_COMMAND( vis_milkCut, "advance to a random next .milk preset now: vis_milkCut [hard] (soft cut/crossfade by default) (PRD FR-A5)", NULL ) {
    const bool hardCut = ( args.Argc() >= 2 && idStr::Icmp( args.Argv( 1 ), "hard" ) == 0 );
    if ( !s_milkActive ) {
        idLib::Printf( "vis_milkCut: no .milk preset active (use vis_milkPreset first)\n" );
        return;
    }
    VisMilkAdvancePreset( hardCut );
    idLib::Printf( "vis_milkCut: %s cut %s\n", hardCut ? "hard" : "soft", hardCut ? "done" : "started" );
}

CONSOLE_COMMAND( vis_milkList, "list .milk presets under vis_milkPresetSubdir (PRD M1 step 6)", NULL ) {
    idStrList files;
    VisAppendFiles( files, vis_milkPresetSubdir.GetString(), ".milk" );
    if ( files.Num() == 0 ) {
        VisTryFixMilkDoubleNesting();
        files.Clear();
        VisAppendFiles( files, vis_milkPresetSubdir.GetString(), ".milk" );
    }
    idLib::Printf( "%d .milk preset(s) under %s:\n", files.Num(), vis_milkPresetSubdir.GetString() );
    for ( int i = 0; i < files.Num(); i++ ) {
        idLib::Printf( "  %s\n", files[i].c_str() );
    }
}

// PRD FR-D4/M7: console-callable equivalent of the F6 rating hotkey -- set
// (with an argument) or print (with none) the current preset's rating.
// Reuses the exact same VisGetMilkRating/VisSetMilkRating persistence the
// hotkey does, so either input method sees/affects the same saved value.
CONSOLE_COMMAND( vis_milkRate, "get/set the current .milk preset's rating 0-5 (PRD FR-D4/M7)", NULL ) {
    if ( s_milkPreset.GetName().IsEmpty() ) {
        idLib::Printf( "vis_milkRate: no preset loaded (use vis_milkPreset first)\n" );
        return;
    }
    if ( args.Argc() >= 2 ) {
        const int rating = idMath::ClampInt( 0, 5, atoi( args.Argv( 1 ) ) );
        VisSetMilkRating( s_milkPreset.GetName().c_str(), rating );
    }
    idLib::Printf( "vis_milkRate: '%s' rated %d/5\n", s_milkPreset.GetName().c_str(), VisGetMilkRating( s_milkPreset.GetName().c_str() ) );
}

/*
========================
PRD M4.5, Pillar F: geometry-only map load (first code slice)

Uses the render-only precedent already in this codebase, confirmed by
direct reading rather than assumed: neo/ui/RenderWindow.cpp calls
renderSystem->AllocRenderWorld() + world->InitFromMap(...) entirely outside
idGameLocal::LoadMap/idSessionLocal -- exactly what FR-F2 needs (no entity/
AI spawn, no gameplay bootstrap).

v1 scope, matching the corrected plan in plans/PRD-implementation-status.md
M4.5 (checked the actual idFileSystem implementation before assuming
otherwise -- twice, after two wrong first guesses): loose .map/.proc files
AND packed .resources containers under maps/ in whatever game directory is
ALREADY ACTIVE (base/, or a mod selected at launch via +set fs_game
<moddir>) are both reachable now -- the P2 follow-up this comment used to
describe (a new idFileSystem::AddResourceFile method, since it wasn't public
before) has landed: see FileSystem.h/.cpp and vis_mapLoad's mount call
below. Registering a brand-new mod directory at runtime still needs a
DIFFERENT new method (gamedir only filters search paths already known at
boot) and remains a real, separate P2 follow-up -- not attempted here.

Camera flythrough (FR-F3: portal graph -> spline path) is NOT implemented
yet -- this slice only proves discovery + geometry load work, the
prerequisite for that next step.
========================
*/
static idRenderWorld * s_milkMapWorld = NULL;
// PRD M4.5 follow-up (direct user report: map loads -- console confirms area
// count -- but renders solid black). Root cause confirmed by reading
// InitFromMap/the .proc grammar directly: lights are entities in this
// engine, parsed from the .map file by the normal game-spawn path, and the
// compiled .proc this feature loads from has no entity/light data at all
// (by design -- "geometry-only, no entities/AI"). So the loaded render
// world has zero light sources and every surface gets zero illumination.
//
// TWO fixes were attempted and reverted here before landing on the safe
// one below -- both reproduced a real, repeatable access violation
// (confirmed via Windows Event Log Application Error entries at an
// identical fault offset across independent runs, with and without a
// milkdrop preset also loaded, and with two different maps -- ruling out
// both the preset code and the specific map's data as the cause): calling
// idRenderWorld::AddLightDef/UpdateLightDef directly from vis_mapLoad, and
// (after deferring it) from VisRenderMilkMapScene's first real call.
// Symbolized the crash (Doom3BFG.map, RVA 0x00B1C110) to inside
// R_AddSingleLight (tr_frontend_addlights.cpp) -- deep in the per-light
// entity-interaction/shadow setup that every OTHER caller of AddLightDef
// (idLight entities, via idGameEdit::ParseSpawnArgsToRenderLight /
// idLight::PresentLightDefChange in neo/d3xp/Light.cpp) only ever reaches
// from within a fully-initialized game session with a real primary render
// world -- something this standalone map-preview feature (no game session,
// a render world that's never tr.primaryWorld, r_useParallelAddLights
// running R_AddSingleLight on a job-system worker thread by default)
// doesn't have, and further root-causing would need a real attached
// debugger this remote/scripted workflow doesn't have. Fixed instead with
// r_showTris (an existing renderer debug tool, RB_ShowTris in
// tr_backend_rendertools.cpp): draws surface wireframes directly from the
// already-culled drawSurf list, entirely bypassing the lighting/
// interaction/shadow system (and so AddLightDef) -- see
// VisRenderMilkMapScene/vis_mapLoad/vis_mapUnload for where it's toggled.
// Not full shading, but real, distinguishable geometry instead of a flat
// black screen, with no crash risk.
// PRD M4.5 follow-up (direct user request: "an option to display textures or
// render the level the way doom the game would"). armar1 (the map with the
// most complete texture coverage) rendered as pure white wireframe under the
// r_showTris fallback above. This adds a SECOND, still-safe render mode that
// shows each surface's REAL diffuse texture, full-bright, by forcing the
// renderer's existing non-light "ambient" pass (RB_DrawShaderPasses) to also
// draw SL_DIFFUSE stages -- see r_forceAmbientDiffuse in RenderSystem_init.cpp.
// Crucially this bypasses the interaction/shadow/AddLightDef path entirely (the
// twice-crashing path documented above), so no dynamic light is ever created;
// the geometry is just drawn unlit at full brightness with its own textures.
//   0 = white wireframe via r_showTris (the shipped, always-safe default)
//   1 = textured, unlit, full-bright via r_forceAmbientDiffuse
// Applied at vis_mapLoad time (like r_showTris); change it then re-run
// vis_mapLoad to switch an already-loaded map between modes.
idCVar vis_mapRenderMode( "vis_mapRenderMode", "0", CVAR_INTEGER | CVAR_ARCHIVE, "map-flythrough render style: 0 white wireframe (r_showTris, default, confirmed working), 1 textured unlit full-bright (r_forceAmbientDiffuse) -- crash-safe and confirmed working correctly, but will render invisible/black for any map surface whose real texture file is missing from disk (this map pack currently ships no shared texture library outside 'armar' character skins, so most maps show nothing in this mode -- not a bug in this feature; see PRD-implementation-status.md) -- takes effect on the next vis_mapLoad", 0, 1 );
// PRD follow-up (direct user report: "for the map can we choose the
// wireframe hue and the background hue? those should also be bindable
// parameters"). Driven continuously (not just set once at vis_mapLoad, the
// way vis_mapRenderMode itself is) so they can actually be audio/MIDI/
// mouse-reactive like every other bindable slider -- see
// VisUpdateMapRenderColors, called once per frame while a map is active.
idCVar vis_mapWireHue( "vis_mapWireHue", "0", CVAR_FLOAT | CVAR_ARCHIVE, "map-flythrough wireframe tint hue (0..1, mode 0 only) -- drag the slider or right-click it to bind to an audio band/MIDI/mouse", 0, 1 );
idCVar vis_mapBgHue( "vis_mapBgHue", "0", CVAR_FLOAT | CVAR_ARCHIVE, "map-flythrough background fill tint hue (0..1) behind the rendered map, used when vis_mapBgTint is on -- drag the slider or right-click it to bind to an audio band/MIDI/mouse", 0, 1 );
idCVar vis_mapBgTint( "vis_mapBgTint", "0", CVAR_BOOL | CVAR_ARCHIVE, "tint the map-flythrough background with vis_mapBgHue instead of leaving it black" );
// PRD follow-up (direct user report: "we need blend modes for the map
// flythrough layer maybe one mode for wireframe and one for background ...
// background could darken and wireframe could lighten"). VISBLEND_* index
// (0 Normal .. 7 Invert, same order/names as the Layer Stack's own blend
// combo) for each of the map view's two independent layers.
idCVar vis_mapWireBlend( "vis_mapWireBlend", "0", CVAR_INTEGER | CVAR_ARCHIVE, "map-flythrough wireframe blend mode: 0 Normal, 1 Additive, 2 Subtractive, 3 Multiply, 4 Screen, 5 Darken, 6 Lighten, 7 Invert", 0, VISBLEND_COUNT - 1 );
idCVar vis_mapBgBlend( "vis_mapBgBlend", "0", CVAR_INTEGER | CVAR_ARCHIVE, "map-flythrough background blend mode (used when vis_mapBgTint is on): 0 Normal, 1 Additive, 2 Subtractive, 3 Multiply, 4 Screen, 5 Darken, 6 Lighten, 7 Invert", 0, VISBLEND_COUNT - 1 );
// PRD follow-up (direct user report: "we also need to be able to organize
// or sort the layers how we want. so that is a control that doesn't exist
// yet"). The map view currently has exactly two layers -- the background
// fill and the wireframe/textured render -- so "sorting" them is a single
// draw-order toggle rather than a full reorderable list (see
// VisRenderMilkMapScene for how this is applied).
idCVar vis_mapWireOnTop( "vis_mapWireOnTop", "1", CVAR_BOOL | CVAR_ARCHIVE, "draw order for the map view's two layers: on (default) draws the background first then the wireframe/textured map on top; off reverses it -- pick whichever order looks right with your chosen blend modes" );

static bool s_milkShowTrisForced = false;   // true while THIS feature owns the render-mode cvars' values
static int  s_savedShowTris = 0;            // the user's own r_showTris value, restored on unload
static int  s_savedForceAmbientDiffuse = 0; // the user's own r_forceAmbientDiffuse value, restored on unload

// shared by vis_mapUnload and vis_mapLoad's failed-load cleanup path --
// restores whatever r_showTris / r_forceAmbientDiffuse values were active
// before vis_mapLoad forced them, and only once (repeated calls with nothing
// forced are a no-op).
static void VisRestoreMilkShowTris() {
    if ( s_milkShowTrisForced ) {
        cvarSystem->SetCVarInteger( "r_showTris", s_savedShowTris );
        cvarSystem->SetCVarInteger( "r_forceAmbientDiffuse", s_savedForceAmbientDiffuse );
        s_milkShowTrisForced = false;
    }
}
// PRD M4.5 P2 follow-up: tracks the basename of whatever packed .resources
// container vis_mapLoad mounted (empty if the currently-loaded map came from
// a loose file, or if none is loaded) -- so a later vis_mapLoad/vis_mapUnload
// can cleanly unmount it via the matching idFileSystem::UnloadMapResources,
// rather than leaking a mounted container every time the preview switches
// maps.
static idStr s_milkMountedResourceMap;

// unmount whatever packed .resources container this feature mounted, if any
// -- shared by vis_mapLoad (before mounting a different one) and
// vis_mapUnload, so there's exactly one place that needs to stay correct.
static void VisUnmountMilkMapResource() {
    if ( !s_milkMountedResourceMap.IsEmpty() ) {
        fileSystem->UnloadMapResources( s_milkMountedResourceMap.c_str() );
        s_milkMountedResourceMap.Clear();
    }
}

// PRD M4.5 follow-up: closes the previously-documented gap ("registering a
// brand-new mod folder without restart" -- idFileSystem had no public
// method for it, only boot-time fs_game/fs_game_base). Registers <dir> as
// an ADDITIONAL search path (idFileSystemLocal::RegisterModSearchPath, a
// purely additive sibling of AddGameDirectory that never touches the
// engine's "active mod" state) -- lets a modder point the map-flythrough
// preview at a brand-new map pack folder they just dropped in without
// relaunching the engine with +set fs_game. Run vis_mapList afterward to
// discover maps the newly-registered directory exposes.
CONSOLE_COMMAND( vis_mapSearchPath, "register a new mod/map-pack directory as an additional search path, without restarting: vis_mapSearchPath <dirName> (PRD M4.5)", NULL ) {
    if ( args.Argc() != 2 ) {
        idLib::Printf( "usage: vis_mapSearchPath <dirName>\n"
                       "  registers <fs_basepath>/<dirName> (and <fs_savepath>/<dirName>, if set)\n"
                       "  as an additional search path -- run vis_mapList afterward to see what it exposes\n" );
        return;
    }
    fileSystem->RegisterModSearchPath( args.Argv( 1 ) );
    VisRememberSearchPath( vis_mapSearchPathSaved, args.Argv( 1 ) );   // PRD follow-up: remember across launches
}

// Same underlying mechanism as vis_mapSearchPath (idFileSystem::
// RegisterModSearchPath is entirely content-agnostic -- it just adds a new
// search path), named separately for discoverability from the preset side.
// Pair with vis_milkPresetSubdir to point the PRESETS tab/vis_milkList/
// vis_milkPreset random-pick sources at a custom external .milk pack --
// e.g. a pack unpacked as "<install>/milkdrop_presets/presets-cream-of-the-
// crop-master/<artist>/..." needs `vis_presetSearchPath milkdrop_presets`
// then `vis_milkPresetSubdir presets-cream-of-the-crop-master`.
CONSOLE_COMMAND( vis_presetSearchPath, "register a new external .milk preset pack's root directory as an additional search path, without restarting: vis_presetSearchPath <dirName> (pair with vis_milkPresetSubdir)", NULL ) {
    if ( args.Argc() != 2 ) {
        idLib::Printf( "usage: vis_presetSearchPath <dirName>\n"
                       "  registers <fs_basepath>/<dirName> (and <fs_savepath>/<dirName>, if set)\n"
                       "  as an additional search path -- then set vis_milkPresetSubdir to the\n"
                       "  relative folder INSIDE it that actually holds the .milk files, and\n"
                       "  run vis_milkList to confirm what it finds\n" );
        return;
    }
    fileSystem->RegisterModSearchPath( args.Argv( 1 ) );
    s_lastMilkSearchPath = args.Argv( 1 );
    VisRememberSearchPath( vis_presetSearchPathSaved, args.Argv( 1 ) );   // PRD follow-up: remember across launches
}

CONSOLE_COMMAND( vis_mapList, "list .map/.resources files available to load under maps/ (PRD M4.5)", NULL ) {
    idFileList * looseFiles = fileSystem->ListFilesTree( "maps", ".map", true );
    // PRD M4.5 P2 follow-up: packed retail maps ship as maps/<name>.resources
    // (flattened, no subdirectory nesting, matching the engine's own
    // BeginLevelLoad convention -- see idFileSystemLocal::AddResourceFile).
    // .resources containers can't be looked INSIDE by ListFilesTree (same
    // reason loose .proc files inside them aren't independently listable),
    // but the container files themselves are plain OS files under maps/, so
    // listing THOSE by extension (the same approach the engine's own
    // GenerateResourceCRCs_f dev command already uses) is exactly what's
    // needed for discovery -- no new engine plumbing beyond the mount call
    // vis_mapLoad itself needs.
    idFileList * resourceFiles = fileSystem->ListFilesTree( "maps", ".resources", true );
    const int numLoose = ( looseFiles != NULL ) ? looseFiles->GetNumFiles() : 0;
    const int numResources = ( resourceFiles != NULL ) ? resourceFiles->GetNumFiles() : 0;
    if ( numLoose == 0 && numResources == 0 ) {
        idLib::Printf( "vis_mapList: no .map or .resources files found under maps/\n" );
    }
    if ( numLoose > 0 ) {
        idLib::Printf( "%d loose .map file(s) under maps/:\n", numLoose );
        for ( int i = 0; i < numLoose; i++ ) {
            idLib::Printf( "  %s\n", looseFiles->GetFile( i ) );
        }
    }
    if ( numResources > 0 ) {
        idLib::Printf( "%d packed .resources file(s) under maps/ (load the map name, not the .resources filename -- e.g. 'mars_city1', not 'mars_city1.resources'):\n", numResources );
        for ( int i = 0; i < numResources; i++ ) {
            idLib::Printf( "  %s\n", resourceFiles->GetFile( i ) );
        }
    }
    if ( looseFiles != NULL ) {
        fileSystem->FreeFileList( looseFiles );
    }
    if ( resourceFiles != NULL ) {
        fileSystem->FreeFileList( resourceFiles );
    }
}

CONSOLE_COMMAND( vis_mapLoad, "load a map's geometry only, no entities/AI (PRD M4.5)", NULL ) {
    if ( args.Argc() != 2 ) {
        idLib::Printf( "usage: vis_mapLoad <mapname>  (relative to maps/, e.g. 'game/mars_city1' -- no extension needed)\n" );
        return;
    }
    if ( s_milkMapWorld != NULL ) {
        renderSystem->FreeRenderWorld( s_milkMapWorld );
        s_milkMapWorld = NULL;
    }
    // PRD M4.5 P2 follow-up: opportunistically mount a matching packed
    // .resources container BEFORE trying InitFromMap -- retail installs
    // ship maps this way instead of (or as well as) loose .proc files.
    // AddResourceFile flattens subdirectories itself (matches the engine's
    // own BeginLevelLoad convention: the .resources file always sits
    // directly under maps/, even for a map whose .map/.proc lives in a
    // subdirectory), so the resource name is the map's BASENAME, not the
    // full argument. Harmless no-op (-1, silently) if no matching container
    // exists -- the loose-file path below still works unchanged either way.
    VisUnmountMilkMapResource();
    idStr mapBaseName = args.Argv( 1 );
    mapBaseName.StripPath();
    if ( fileSystem->AddResourceFile( va( "%s.resources", mapBaseName.c_str() ) ) >= 0 ) {
        s_milkMountedResourceMap = mapBaseName;
    }
    s_milkMapWorld = renderSystem->AllocRenderWorld();
    idStr mapName = "maps/";
    mapName += args.Argv( 1 );
    // Fixed real bug, found via a real (non-repacked) map's .proc file
    // (idRenderWorldLocal::InitFromMap's "bad area model lookup" path --
    // hit when a loose .proc references area geometry the engine expects
    // its matching packed .resources container to supply, which failed to
    // open just above): that failure path is common->Error(), which throws
    // idException rather than returning false -- confirmed by reading
    // Common_printf.cpp's idCommonLocal::Error directly (default ERP_DROP
    // code path -> throw idException(errorMessage)), and by common_frame.cpp
    // wrapping idCommonLocal::Frame()'s ENTIRE body in try/catch(idException&)
    // { return; } as the top-level recovery point. That means an exception
    // here unwinds straight past this whole function (this cleanup code
    // included) up to Frame()'s catch, leaving s_milkMapWorld a dangling
    // pointer to a half-initialized idRenderWorldLocal forever -- every
    // later frame's s_milkMapWorld!=NULL check (VisRenderMilkMapScene/
    // VisUpdateMilkCamera) would then try to use it. Catching here and
    // funneling into the exact same cleanup the plain "InitFromMap returned
    // false" branch already does closes that gap for both failure shapes.
    bool loaded = false;
    try {
        loaded = s_milkMapWorld->InitFromMap( mapName.c_str() );
    } catch ( idException & ) {
        loaded = false;
    }
    if ( !loaded ) {
        idLib::Warning( "vis_mapLoad: failed to load '%s'", mapName.c_str() );
        renderSystem->FreeRenderWorld( s_milkMapWorld );
        s_milkMapWorld = NULL;
        VisUnmountMilkMapResource();
        VisRestoreMilkShowTris();
        // Fixed real bug: a failed load on top of a previously-successful
        // one freed/nulled s_milkMapWorld here but never reset the tour --
        // VisRenderMilkMapScene's own s_milkMapWorld==NULL guard prevented
        // any actual bad render, but s_milkCamValid stayed stuck true and
        // VisUpdateMilkCamera kept pointlessly recomputing a camera
        // position from the old (now-freed) map's stale tour every frame
        // until the next successful vis_mapLoad/vis_mapUnload. Matches the
        // clean reset vis_mapUnload already performs.
        VisRebuildMilkTour();
        return;
    }
    idLib::Printf( "vis_mapLoad: loaded '%s' -- %d areas%s\n", mapName.c_str(), s_milkMapWorld->NumAreas(),
        s_milkMountedResourceMap.IsEmpty() ? "" : " (from packed .resources)" );
    // No real light sources exist in this "no entities" world (see this
    // file's earlier comment on s_milkMapWorld for why, and why the
    // straightforward AddLightDef fix was reverted after reproducing a real
    // crash) -- so force on a light-less render mode instead, per
    // vis_mapRenderMode. BOTH modes bypass the interaction/shadow/AddLightDef
    // path: mode 0 draws white wireframe (r_showTris), mode 1 draws the real
    // per-surface diffuse textures unlit/full-bright (r_forceAmbientDiffuse).
    // Save the user's current values once, so vis_mapUnload can restore them.
    if ( !s_milkShowTrisForced ) {
        s_savedShowTris = cvarSystem->GetCVarInteger( "r_showTris" );
        s_savedForceAmbientDiffuse = cvarSystem->GetCVarInteger( "r_forceAmbientDiffuse" );
        s_milkShowTrisForced = true;
    }
    if ( vis_mapRenderMode.GetInteger() == 1 ) {
        // textured, unlit, full-bright -- each surface shows its own diffusemap
        cvarSystem->SetCVarInteger( "r_forceAmbientDiffuse", 1 );
        cvarSystem->SetCVarInteger( "r_showTris", 0 );
    } else {
        // white wireframe (the always-safe shipped default)
        cvarSystem->SetCVarInteger( "r_showTris", 2 );   // 2 = draw all front-facing tris, always visible (GLS_DEPTHFUNC_ALWAYS)
        cvarSystem->SetCVarInteger( "r_forceAmbientDiffuse", 0 );
    }
    VisRebuildMilkTour();   // PRD M4.5 FR-F3: compute the area graph + a fresh camera tour immediately, so the flythrough starts right away
}

CONSOLE_COMMAND( vis_mapUnload, "free the currently loaded map's render world (PRD M4.5)", NULL ) {
    if ( s_milkMapWorld != NULL ) {
        renderSystem->FreeRenderWorld( s_milkMapWorld );
        s_milkMapWorld = NULL;
        VisUnmountMilkMapResource();
        VisRestoreMilkShowTris();
        VisRebuildMilkTour();   // s_milkMapWorld is NULL now -- this resets the tour/camera state to invalid
        idLib::Printf( "vis_mapUnload: cleared\n" );
    }
}

// PRD FR-E5 / M4 first slice: MIDI input device listing/open/close, and the
// midicc1-4 routing sources registered above (VMS_MIDICC1..4) so a CC can
// drive any modulation target via the existing `vis_route` command, e.g.
// `vis_route scale midicc1 1.0` maps CC1 (commonly the mod wheel) straight
// to the bar/spoke/scope scale target.
CONSOLE_COMMAND( vis_midiList, "list MIDI input devices (PRD FR-E5)", NULL ) {
    const int n = idMidiInput::NumDevices();
    idLib::Printf( "%d MIDI input device(s):\n", n );
    for ( int i = 0; i < n; i++ ) {
        idLib::Printf( "  %d: %s\n", i, idMidiInput::GetDeviceName( i ) );
    }
}

CONSOLE_COMMAND( vis_midiOpen, "open a MIDI input device by index, or none given for device 0 (PRD FR-E5)", NULL ) {
    const int dev = ( args.Argc() >= 2 ) ? atoi( args.Argv( 1 ) ) : 0;
    g_midiInput.Open( dev );
}

CONSOLE_COMMAND( vis_midiClose, "close the currently open MIDI input device (PRD FR-E5)", NULL ) {
    g_midiInput.Close();
    idLib::Printf( "vis_midiClose: closed\n" );
}

/*
========================
PRD FR-E5's other half: MIDI OUT -- beat -> note trigger, band envelopes ->
CC, and a BPM-synced MIDI clock. All driven from Frame() (main thread),
matching every other per-frame audio-reactive computation in this file.
========================
*/
idCVar vis_midiOutBeatNote( "vis_midiOutBeatNote", "0", CVAR_BOOL | CVAR_ARCHIVE, "send a MIDI note on/off pulse to the open output device on each detected beat" );
idCVar vis_midiOutCC( "vis_midiOutCC", "0", CVAR_BOOL | CVAR_ARCHIVE, "send bass/mid/treb as CC1/2/3 to the open MIDI output device" );
idCVar vis_midiOutClock( "vis_midiOutClock", "0", CVAR_BOOL | CVAR_ARCHIVE, "send a 24 ppqn MIDI clock synced to the estimated BPM (needs a few beats to lock on)" );
idCVar vis_midiOutChannel( "vis_midiOutChannel", "0", CVAR_INTEGER | CVAR_ARCHIVE, "MIDI channel (0-15) used for note/CC output", 0, 15 );
idCVar vis_midiOutNote( "vis_midiOutNote", "36", CVAR_INTEGER | CVAR_ARCHIVE, "MIDI note number sent on beat (36 = GM kick drum)", 0, 127 );

static int   s_midiNoteOffAtMs = 0;      // 0 = no note currently held
static int   s_midiLastCCSendMs = 0;
static float s_midiClockPhase = 0.0f;    // 0..1 fraction of the way to the next clock pulse

// called once per frame from Frame() (main thread), after the audio
// analyzer's Update() so GetBeat()/GetBPM()/band values are current.
static void VisUpdateMidiOutput( float dtSec ) {
    if ( !g_midiOutput.IsOpen() ) {
        return;
    }
    const int channel = vis_midiOutChannel.GetInteger();
    const int now = Sys_Milliseconds();

    if ( vis_midiOutBeatNote.GetBool() ) {
        if ( g_audioAnalyzer.GetBeat() ) {
            const float velocity = idMath::ClampFloat( 0.3f, 1.0f, g_audioAnalyzer.GetBassAtt() );
            g_midiOutput.SendNoteOn( channel, vis_midiOutNote.GetInteger(), velocity );
            s_midiNoteOffAtMs = now + 80;   // short trigger pulse, not a held note
        }
        if ( s_midiNoteOffAtMs != 0 && now >= s_midiNoteOffAtMs ) {
            g_midiOutput.SendNoteOff( channel, vis_midiOutNote.GetInteger() );
            s_midiNoteOffAtMs = 0;
        }
    }

    if ( vis_midiOutCC.GetBool() && now - s_midiLastCCSendMs >= 50 ) {
        // throttled to ~20Hz -- band envelopes don't need frame-rate
        // resolution, and a real MIDI cable/interface has finite bandwidth.
        s_midiLastCCSendMs = now;
        g_midiOutput.SendCC( channel, 1, idMath::ClampFloat( 0.0f, 1.0f, g_audioAnalyzer.GetBassAtt() * 0.5f ) );
        g_midiOutput.SendCC( channel, 2, idMath::ClampFloat( 0.0f, 1.0f, g_audioAnalyzer.GetMidAtt() * 0.5f ) );
        g_midiOutput.SendCC( channel, 3, idMath::ClampFloat( 0.0f, 1.0f, g_audioAnalyzer.GetTrebAtt() * 0.5f ) );
    }

    if ( vis_midiOutClock.GetBool() ) {
        const float bpm = g_audioAnalyzer.GetBPM();
        if ( bpm >= 40.0f && bpm <= 220.0f ) {
            // 24 pulses per quarter note, per the MIDI spec.
            const float pulsesPerSec = ( bpm / 60.0f ) * 24.0f;
            s_midiClockPhase += pulsesPerSec * dtSec;
            while ( s_midiClockPhase >= 1.0f ) {
                s_midiClockPhase -= 1.0f;
                g_midiOutput.SendClockPulse();
            }
        }
        // no valid BPM estimate yet -- deliberately sends nothing rather than
        // a fabricated/guessed tempo (see AudioAnalyzer::GetBPM's own comment).
    }
}

CONSOLE_COMMAND( vis_midiOutList, "list MIDI output devices (PRD FR-E5)", NULL ) {
    const int n = idMidiOutput::NumDevices();
    idLib::Printf( "%d MIDI output device(s):\n", n );
    for ( int i = 0; i < n; i++ ) {
        idLib::Printf( "  %d: %s\n", i, idMidiOutput::GetDeviceName( i ) );
    }
}

CONSOLE_COMMAND( vis_midiOutOpen, "open a MIDI output device by index, or none given for device 0 (PRD FR-E5)", NULL ) {
    const int dev = ( args.Argc() >= 2 ) ? atoi( args.Argv( 1 ) ) : 0;
    g_midiOutput.Open( dev );
}

CONSOLE_COMMAND( vis_midiOutClose, "close the currently open MIDI output device (PRD FR-E5)", NULL ) {
    g_midiOutput.Close();
    idLib::Printf( "vis_midiOutClose: closed\n" );
}

/*
========================
PRD FR-C9 (P2): Art-Net DMX output

Reuses FR-C1-EXPAND's modulation registry (s_targetDefs / s_mod) as the SOURCE
of DMX channel values: any registered modulation target (hue, bright, a band
level, a per-slot opacity, camspeed, ...) can be assigned to any of the 512
channels of one DMX universe, and this file emits one Art-Net ArtDMX UDP packet
per throttled tick carrying those 512 bytes. This is an OUTPUT transport that
sits alongside the MIDI *input* path (FR-E5): nothing here feeds back into the
visualizer, it only pushes live modulation values out to a network DMX gateway.

Transport is Art-Net over UDP (OpCode 0x5000) to port 6454, broadcast by
default (vis_dmxTargetIP "2.255.255.255" -- Art-Net's conventional 2.x directed
broadcast) or unicast to a specific gateway IP. sACN / E1.31 is intentionally
NOT implemented here (a documented follow-up, per the FR's "don't half-implement
both"). We reuse the engine's idUDP wrapper (sys_public.h / win_net.cpp), whose
socket is already SO_BROADCAST-enabled, rather than touching raw WinSock.

NFR-3 ("a bad preset/config logs and is skipped"): a failed socket open or an
unparseable target IP logs one warning and then leaves DMX output quietly
disabled/skipped for the session rather than crashing or spamming every frame.
========================
*/
idCVar vis_dmxEnable( "vis_dmxEnable", "0", CVAR_BOOL | CVAR_ARCHIVE, "enable Art-Net DMX output of assigned modulation targets (PRD FR-C9)" );
idCVar vis_dmxTargetIP( "vis_dmxTargetIP", "2.255.255.255", CVAR_ARCHIVE, "Art-Net destination IP: broadcast (2.255.255.255) or a gateway's unicast address (PRD FR-C9)" );
idCVar vis_dmxUniverse( "vis_dmxUniverse", "0", CVAR_INTEGER | CVAR_ARCHIVE, "Art-Net universe (0-32767) written into the ArtDMX packet (PRD FR-C9)", 0, 32767 );

static const int VIS_DMX_CHANNELS = 512;
static idUDP g_dmxSocket;
static int   s_dmxTarget[ VIS_DMX_CHANNELS ];   // per-channel modulation-target index into s_targetDefs, -1 = unassigned
static bool  s_dmxTargetInit = false;
static bool  s_dmxSocketOpenFailed = false;     // one-shot: after a failed open, don't retry/warn every frame
static bool  s_dmxAddrWarned = false;           // one-shot warning for an unparseable target IP
static int   s_dmxLastSendMs = 0;
static byte  s_dmxSeq = 1;                       // Art-Net sequence: 1..255 wrapping (0 would mean "sequencing disabled")

static void VisDmxEnsureTargetsInit() {
    if ( s_dmxTargetInit ) {
        return;
    }
    for ( int i = 0; i < VIS_DMX_CHANNELS; i++ ) {
        s_dmxTarget[i] = -1;
    }
    s_dmxTargetInit = true;
}

// Read accessor for the ImGui panel's assignment list (declared up top).
static int VisDmxChannelTarget( int channel0 ) {
    if ( !s_dmxTargetInit || channel0 < 0 || channel0 >= VIS_DMX_CHANNELS ) {
        return -1;
    }
    return s_dmxTarget[ channel0 ];
}

// Maps one modulation target's live s_mod value into a 0..255 DMX byte. Targets
// with a real clamp range [clampMin,clampMax] are normalized across that range;
// no-clamp targets (clampMin >= clampMax, e.g. hue/zoom/rotate) are assumed to
// already live in 0..1, as the FR specifies for "most targets".
static byte VisDmxTargetToByte( int target ) {
    if ( target < 0 || target >= s_targetDefs.Num() ) {
        return 0;
    }
    const visModTargetDef_t & def = s_targetDefs[ target ];
    float lo = 0.0f, hi = 1.0f;
    if ( def.clampMin < def.clampMax ) {
        lo = def.clampMin;
        hi = def.clampMax;
    }
    const float v = VisMod( target );   // bounds-checked accessor (never indexes out of range)
    float norm = ( hi > lo ) ? ( v - lo ) / ( hi - lo ) : 0.0f;
    norm = idMath::ClampFloat( 0.0f, 1.0f, norm );
    return (byte)( norm * 255.0f + 0.5f );
}

// Called every frame from Frame() (main thread), alongside VisUpdateMidiOutput.
static void VisUpdateDmxOutput( float dtSec ) {
    if ( !vis_dmxEnable.GetBool() ) {
        return;
    }
    VisDmxEnsureTargetsInit();

    // Lazy socket open. Prefer the conventional Art-Net port 6454 for the local
    // bind, but fall back to an ephemeral port if 6454 is already taken (e.g. an
    // Art-Net node app is already running on this same machine) -- the
    // DESTINATION port is 6454 regardless, so an ephemeral source port still
    // reaches the gateway.
    if ( !g_dmxSocket.IsOpen() ) {
        if ( s_dmxSocketOpenFailed ) {
            return;   // already tried and failed this session -- don't spam
        }
        if ( !g_dmxSocket.InitForPort( 6454 ) && !g_dmxSocket.InitForPort( PORT_ANY ) ) {
            idLib::Warning( "vis_dmx: could not open a UDP socket for Art-Net output -- DMX disabled" );
            s_dmxSocketOpenFailed = true;
            return;
        }
    }

    // Rate-limit to ~40Hz. Art-Net gateways typically expect <=44Hz and the
    // render loop can run much faster; this is the same "last sent ms" gate the
    // MIDI CC output above uses.
    const int now = Sys_Milliseconds();
    if ( now - s_dmxLastSendMs < 25 ) {
        return;
    }
    s_dmxLastSendMs = now;

    netadr_t to;
    if ( !Sys_StringToNetAdr( vis_dmxTargetIP.GetString(), &to, false ) || to.type == NA_BAD ) {
        if ( !s_dmxAddrWarned ) {
            idLib::Warning( "vis_dmx: could not parse vis_dmxTargetIP '%s' -- DMX output skipped", vis_dmxTargetIP.GetString() );
            s_dmxAddrWarned = true;
        }
        return;
    }
    s_dmxAddrWarned = false;
    to.port = 6454;   // Art-Net UDP port. netadr_t.port is host order; idUDP htons's it on send (win_net.cpp Net_NetadrToSockadr).

    // Build one ArtDMX packet: 18-byte header + 512 channel bytes.
    byte pkt[ 18 + VIS_DMX_CHANNELS ];
    memcpy( pkt, "Art-Net\0", 8 );                          // 8-byte ID string incl. trailing NUL
    pkt[8]  = 0x00;                                          // OpCode low  (ArtDMX = 0x5000, low byte first on the wire)
    pkt[9]  = 0x50;                                          // OpCode high
    pkt[10] = 0x00;                                          // ProtVerHi
    pkt[11] = 0x0e;                                          // ProtVerLo (protocol version 14)
    pkt[12] = s_dmxSeq;                                      // Sequence (1..255, wraps; 0 would disable sequencing)
    pkt[13] = 0x00;                                          // Physical (informational only)
    const int universe = vis_dmxUniverse.GetInteger();
    pkt[14] = (byte)( universe & 0xff );                    // SubUni: universe low byte
    pkt[15] = (byte)( ( universe >> 8 ) & 0xff );           // Net: universe high byte
    pkt[16] = (byte)( ( VIS_DMX_CHANNELS >> 8 ) & 0xff );   // Length high (BIG-endian) = 0x02
    pkt[17] = (byte)( VIS_DMX_CHANNELS & 0xff );            // Length low = 0x00

    byte * data = pkt + 18;
    for ( int c = 0; c < VIS_DMX_CHANNELS; c++ ) {
        data[c] = VisDmxTargetToByte( s_dmxTarget[c] );     // unassigned channel (-1) -> 0
    }

    g_dmxSocket.SendPacket( to, pkt, sizeof( pkt ) );

    s_dmxSeq++;
    if ( s_dmxSeq == 0 ) {
        s_dmxSeq = 1;   // keep sequencing enabled -- never emit sequence 0
    }
}

CONSOLE_COMMAND( vis_dmxChannel, "assign a modulation target to a DMX channel: vis_dmxChannel <1-512> <targetName|none> (PRD FR-C9)", NULL ) {
    if ( args.Argc() < 3 ) {
        idLib::Printf( "usage: vis_dmxChannel <channel 1-512> <targetName|none>\n" );
        return;
    }
    VisDmxEnsureTargetsInit();
    const int ch = atoi( args.Argv( 1 ) );
    if ( ch < 1 || ch > VIS_DMX_CHANNELS ) {
        idLib::Printf( "vis_dmxChannel: channel must be 1-%d\n", VIS_DMX_CHANNELS );
        return;
    }
    const char * name = args.Argv( 2 );
    if ( idStr::Icmp( name, "none" ) == 0 || idStr::Icmp( name, "-" ) == 0 ) {
        s_dmxTarget[ ch - 1 ] = -1;
        idLib::Printf( "vis_dmxChannel: channel %d cleared\n", ch );
        return;
    }
    const int t = VisFindModTarget( name );
    if ( t < 0 ) {
        idLib::Printf( "vis_dmxChannel: unknown modulation target '%s' (run vis_routes for the registered names)\n", name );
        return;
    }
    s_dmxTarget[ ch - 1 ] = t;
    idLib::Printf( "vis_dmxChannel: DMX channel %d <- %s\n", ch, s_targetDefs[t].name );
}

CONSOLE_COMMAND( vis_dmxChannels, "list current DMX channel -> modulation-target assignments (PRD FR-C9)", NULL ) {
    VisDmxEnsureTargetsInit();
    int n = 0;
    for ( int c = 0; c < VIS_DMX_CHANNELS; c++ ) {
        if ( s_dmxTarget[c] >= 0 && s_dmxTarget[c] < s_targetDefs.Num() ) {
            idLib::Printf( "  ch %3d <- %s\n", c + 1, s_targetDefs[ s_dmxTarget[c] ].name );
            n++;
        }
    }
    idLib::Printf( "%d DMX channel assignment(s); enable=%d ip=%s universe=%d\n",
        n, vis_dmxEnable.GetBool() ? 1 : 0, vis_dmxTargetIP.GetString(), vis_dmxUniverse.GetInteger() );
}

// PRD FR-E4 / M6: image-sequence screen recording. See the capture call at
// the end of Draw2D for the actual per-frame CaptureRenderToFile call.
CONSOLE_COMMAND( vis_recordStart, "start recording the visualizer to a TGA image sequence: vis_recordStart [dir] (PRD FR-E4/M6)", NULL ) {
    s_milkRecordDir = ( args.Argc() >= 2 ) ? args.Argv( 1 ) : "recordings/session";
    s_milkRecordFrame = 0;
    s_milkRecording = true;
    idLib::Printf( "vis_recordStart: recording to %s/frame_NNNNNN.tga (fs_savepath-relative)\n", s_milkRecordDir.c_str() );
}

CONSOLE_COMMAND( vis_recordStop, "stop recording (PRD FR-E4/M6)", NULL ) {
    if ( s_milkRecording ) {
        idLib::Printf( "vis_recordStop: captured %d frame(s) to %s/\n", s_milkRecordFrame, s_milkRecordDir.c_str() );
    }
    s_milkRecording = false;
}

/*
========================
PRD FR-E3/M6: real video-file export

Deliberately decoupled from capture: vis_recordStart/vis_recordStop (above)
keep writing a TGA per frame exactly as before, completely unchanged; this
reads that already-on-disk sequence back and feeds it through
idVisVideoEncoder (VisVideoEncoder.h/.cpp) to produce one real video file.
Runs synchronously inside the console command (main thread, not per-frame
Draw2D) -- a batch post-process, not a live encode, so there's no new
per-frame render-thread cost and none of the SMP/command-buffer timing rules
the rest of this file is careful about even come into play here. For a
session with hundreds/thousands of captured frames this command WILL block
for a few seconds while it encodes -- an accepted trade-off for a creator/
debug tool, not a real-time gameplay path.
========================
*/

// Reads back one frame written by CaptureRenderToFile above (an uncompressed
// 32-bit TGA -- exact format confirmed by reading R_WriteTGA directly: an
// 18-byte header, BGRA byte order, flip bit at header[17] bit 5, clear when
// CaptureRenderToFile's flipVertical=true as it always passes). Returns a
// newly Mem_Alloc'd top-down RGBA8 buffer (caller Mem_Free's it) or NULL if
// the file doesn't exist or isn't in the exact format we wrote -- either one
// just means "end of sequence" to the caller, not a hard error.
static byte * VisReadCapturedTGA( const char * relativePath, int & outW, int & outH ) {
    void * raw = NULL;
    const int len = fileSystem->ReadFile( relativePath, &raw );
    if ( len <= 18 || raw == NULL ) {
        if ( raw != NULL ) {
            fileSystem->FreeFile( raw );
        }
        return NULL;
    }
    const byte * header = (const byte *)raw;
    const int w = header[12] | ( header[13] << 8 );
    const int h = header[14] | ( header[15] << 8 );
    const bool alreadyTopDown = ( header[17] & ( 1 << 5 ) ) != 0;
    const int pixelBytes = header[16] / 8;   // R_WriteTGA always writes 32bpp
    if ( w <= 0 || h <= 0 || pixelBytes != 4 || len < 18 + w * h * 4 ) {
        fileSystem->FreeFile( raw );
        return NULL;   // not the exact format we wrote -- bail rather than misread garbage
    }

    byte * outRGBA = (byte *)Mem_Alloc( w * h * 4, TAG_AUDIO );
    const byte * src = header + 18;
    for ( int y = 0; y < h; y++ ) {
        const int srcY = alreadyTopDown ? y : ( h - 1 - y );
        const byte * srow = src + srcY * w * 4;
        byte * drow = outRGBA + y * w * 4;
        for ( int x = 0; x < w; x++ ) {
            drow[x * 4 + 0] = srow[x * 4 + 2];   // R <- B (TGA is BGRA)
            drow[x * 4 + 1] = srow[x * 4 + 1];   // G
            drow[x * 4 + 2] = srow[x * 4 + 0];   // B <- R
            drow[x * 4 + 3] = srow[x * 4 + 3];   // A
        }
    }
    fileSystem->FreeFile( raw );
    outW = w;
    outH = h;
    return outRGBA;
}

// PRD FR-E3 destination presets: target long-edge resolution + bitrate for
// a handful of common export targets, plus (fidelity fix, closes a
// previously-documented gap) a target ASPECT RATIO for the presets that
// genuinely need one -- real Instagram feed posts are 1:1 square and
// TikTok/Instagram Stories are 9:16 vertical, not just "whatever aspect the
// capture happened to be, rescaled." cropAspectW/cropAspectH of 0,0 means
// "no crop" (youtubehd/youtube4k keep the old long-edge-only rescale,
// preserving the capture's native aspect, since neither has a single
// canonical aspect ratio to force).
struct milkVideoPreset_t { const char * name; int longEdge; int64 bitRate; int cropAspectW; int cropAspectH; };
static const milkVideoPreset_t s_milkVideoPresets[] = {
    { "youtubehd",  1920,  8000000, 0, 0 },
    { "youtube4k",  3840, 35000000, 0, 0 },
    { "instagram",  1080,  3500000, 1, 1 },
    { "tiktok",     1080,  3500000, 9, 16 },
};

static const milkVideoPreset_t * VisFindVideoPreset( const char * name ) {
    if ( name == NULL || name[0] == '\0' || idStr::Icmp( name, "none" ) == 0 ) {
        return NULL;
    }
    for ( int i = 0; i < (int)( sizeof( s_milkVideoPresets ) / sizeof( s_milkVideoPresets[0] ) ); i++ ) {
        if ( idStr::Icmp( name, s_milkVideoPresets[i].name ) == 0 ) {
            return &s_milkVideoPresets[i];
        }
    }
    return NULL;
}

// PRD FR-E3/M6 fidelity fix: centers the largest aspectW:aspectH rectangle
// that fits inside a w x h source image, and returns a newly Mem_Alloc'd
// RGBA8 buffer at that cropped size (caller Mem_Free's it) -- or NULL if
// aspectW/aspectH is 0 (the "no crop" case both YouTube presets use).
// Rounds the crop to even pixels, matching the long-edge rescale's own
// evenness rounding (H264 requires even dimensions).
static byte * VisCropRGBACenter( const byte * rgba, int w, int h, int aspectW, int aspectH, int & outW, int & outH ) {
    if ( aspectW <= 0 || aspectH <= 0 ) {
        return NULL;
    }
    int cropW = w;
    int cropH = ( w * aspectH ) / aspectW;
    if ( cropH > h ) {
        cropH = h;
        cropW = ( h * aspectW ) / aspectH;
    }
    cropW = ( cropW / 2 ) * 2;
    cropH = ( cropH / 2 ) * 2;
    if ( cropW < 2 ) { cropW = 2; }
    if ( cropH < 2 ) { cropH = 2; }
    if ( cropW > w )  { cropW = w & ~1; }
    if ( cropH > h )  { cropH = h & ~1; }
    const int offX = ( w - cropW ) / 2;
    const int offY = ( h - cropH ) / 2;
    byte * out = (byte *)Mem_Alloc( cropW * cropH * 4, TAG_AUDIO );
    for ( int y = 0; y < cropH; y++ ) {
        const byte * srow = rgba + ( offY + y ) * w * 4 + offX * 4;
        byte * drow = out + y * cropW * 4;
        memcpy( drow, srow, cropW * 4 );
    }
    outW = cropW;
    outH = cropH;
    return out;
}

CONSOLE_COMMAND( vis_videoEncode, "encode a TGA sequence from vis_recordStart into a real video file: "
                 "vis_videoEncode <dir> <outFile.mp4> [fps] [preset] -- preset: none/youtubehd/youtube4k/instagram/tiktok (PRD FR-E3/M6)", NULL ) {
    if ( args.Argc() < 3 ) {
        idLib::Printf( "usage: vis_videoEncode <dir> <outFile.mp4> [fps] [preset]\n"
                        "  preset: none (default), youtubehd, youtube4k (long-edge rescale only),\n"
                        "          instagram (1:1 center crop), tiktok (9:16 center crop)\n" );
        return;
    }
    if ( !idVisVideoEncoder::IsAvailable() ) {
        idLib::Printf( "vis_videoEncode: no usable video encoder (H264/MPEG4) in this build's FFmpeg -- "
                        "the vis_recordStart image sequence is still there; encode it with an external tool instead\n" );
        return;
    }

    const idStr dir = args.Argv( 1 );
    const idStr outFile = args.Argv( 2 );
    const int fps = ( args.Argc() >= 4 ) ? atoi( args.Argv( 3 ) ) : 30;
    const milkVideoPreset_t * preset = ( args.Argc() >= 5 ) ? VisFindVideoPreset( args.Argv( 4 ) ) : NULL;
    if ( args.Argc() >= 5 && preset == NULL && idStr::Icmp( args.Argv( 4 ), "none" ) != 0 ) {
        idLib::Printf( "vis_videoEncode: unknown preset '%s' -- using native resolution/bitrate instead\n", args.Argv( 4 ) );
    }

    idVisVideoEncoder encoder;
    bool opened = false;
    int w = 0, h = 0;
    int frameNum = 0;
    int encoded = 0;
    for ( ;; ) {
        const idStr filename = va( "%s/frame_%06d.tga", dir.c_str(), frameNum );
        int fw = 0, fh = 0;
        byte * rgba = VisReadCapturedTGA( filename.c_str(), fw, fh );
        if ( rgba == NULL ) {
            break;   // end of sequence
        }
        // PRD M6: crop to the preset's target aspect ratio (if any) before
        // anything else looks at fw/fh -- everything below treats the
        // (possibly cropped) fw/fh as "the source," exactly as if the
        // capture itself had been that aspect ratio all along.
        if ( preset != NULL && preset->cropAspectW > 0 ) {
            int croppedW = 0, croppedH = 0;
            byte * cropped = VisCropRGBACenter( rgba, fw, fh, preset->cropAspectW, preset->cropAspectH, croppedW, croppedH );
            if ( cropped != NULL ) {
                Mem_Free( rgba );
                rgba = cropped;
                fw = croppedW;
                fh = croppedH;
            }
        }
        if ( !opened ) {
            w = fw;
            h = fh;
            int dstW = 0, dstH = 0;
            int64 bitRate = 0;
            if ( preset != NULL ) {
                // scale so the LONGER edge hits the preset's target, preserving
                // aspect ratio, rounded to even (H264 requires even dimensions).
                const float scale = (float)preset->longEdge / (float)( ( w > h ) ? w : h );
                dstW = ( (int)( w * scale ) / 2 ) * 2;
                dstH = ( (int)( h * scale ) / 2 ) * 2;
                bitRate = preset->bitRate;
            }
            // Fixed real bug: idVisVideoEncoder::Open passes outFile straight to
            // FFmpeg's avio_open, which is raw OS file I/O -- it knows nothing
            // about fs_savepath (the base every OTHER path in this command,
            // including the TGA sequence just read above, resolves against via
            // idFileSystem) and won't create missing parent directories either.
            // A plain relative path like "recordings/out.mp4" resolves against
            // whatever the process's OS working directory happens to be, not
            // the actual recordings/ tree the TGA sequence was written under --
            // confirmed on the Windows build: this failed with "couldn't open
            // ... for writing" every time despite vis_recordStart's TGAs having
            // written successfully to the same relative subpath moments before.
            const char * outFileOS = fileSystem->RelativePathToOSPath( outFile.c_str(), "fs_savepath" );
            fileSystem->CreateOSPath( outFileOS );
            if ( !encoder.Open( outFileOS, w, h, fps, dstW, dstH, bitRate ) ) {
                idLib::Printf( "vis_videoEncode: failed to open '%s' for encoding (see warning above)\n", outFileOS );
                Mem_Free( rgba );
                return;
            }
            opened = true;
        } else if ( fw != w || fh != h ) {
            idLib::Printf( "vis_videoEncode: frame %d is %dx%d, expected %dx%d -- stopping there\n", frameNum, fw, fh, w, h );
            Mem_Free( rgba );
            break;
        }
        encoder.EncodeFrame( rgba );
        Mem_Free( rgba );
        encoded++;
        frameNum++;
    }

    if ( opened ) {
        encoder.Close();
        idLib::Printf( "vis_videoEncode: encoded %d frame(s) from %s/ into %s%s\n", encoded, dir.c_str(), outFile.c_str(),
                        preset != NULL ? va( " (%s preset)", preset->name ) : "" );
    } else {
        idLib::Printf( "vis_videoEncode: no frames found at %s/frame_000000.tga (fs_savepath-relative)\n", dir.c_str() );
    }
}

/*
========================
PRD M4.5 FR-F3: portal-graph camera tour (print-only proof, not yet wired
to any camera/render path)

Reuses the BSP areas+portals idRenderWorld already builds as a navigable
graph, per the research confirmed in plans/PRD-implementation-status.md:
NumPortalsInArea/GetPortal (-> exitPortal_t{areas[2], w, ...}) are public,
no gameplay bootstrap needed. Each area's "center" is the average of all its
portal windings' centroids (idWinding has no direct area-bounds API in the
confirmed-public surface, but portal centroids are enough of a proxy to walk
between rooms). The tour is a simple random walk over the area-adjacency
graph (mirroring the LCG-seeded shuffle already used for
vis_presetCycleShuffle in BuildCycleList, rather than a new RNG dependency),
avoiding immediate backtrack when another option exists, stopping early at a
dead end (isolated area) rather than looping forever.

Deliberately NOT done here: collision-safe path nudging (needs the
collision-model-manager subsystem for this map, a separate integration not
yet investigated), spline generation between waypoints, or any actual
camera movement / Draw2D integration. This is purely a console-testable
proof that the graph-building + tour logic is sound before either of those
much larger pieces gets built on top of it.
========================
*/
// PRD M4.5: each neighbor edge now also carries the CONNECTING PORTAL's own
// centroid (not just which area is on the other side) -- used below to
// route the camera path THROUGH the actual doorway between two areas
// instead of a straight line between their room centers, which is the main
// real-world source of wall-clipping in non-rectangular rooms (an L-shaped
// room's center-to-center line can cut through the missing corner).
struct milkAreaEdge_t {
    int    area;
    idVec3 portalCenter;
};
struct milkAreaNode_t {
    idVec3               center;
    idList<milkAreaEdge_t> neighbors;   // may repeat if multiple portals connect the same pair (harmless for a random walk)
};

static bool VisBuildAreaGraph( idRenderWorld * world, idList<milkAreaNode_t> & areas ) {
    const int numAreas = world->NumAreas();
    if ( numAreas <= 0 ) {
        return false;
    }
    areas.SetNum( numAreas );
    for ( int a = 0; a < numAreas; a++ ) {
        idVec3 sum( 0.0f, 0.0f, 0.0f );
        int sumCount = 0;
        const int numPortals = world->NumPortalsInArea( a );
        for ( int p = 0; p < numPortals; p++ ) {
            const exitPortal_t portal = world->GetPortal( a, p );
            if ( portal.w == NULL || portal.w->GetNumPoints() == 0 ) {
                continue;
            }
            idVec3 centroid( 0.0f, 0.0f, 0.0f );
            const int n = portal.w->GetNumPoints();
            for ( int i = 0; i < n; i++ ) {
                centroid += ( *portal.w )[i].ToVec3();
            }
            centroid /= (float)n;
            sum += centroid;
            sumCount++;
            const int other = ( portal.areas[0] == a ) ? portal.areas[1] : portal.areas[0];
            if ( other != a && other >= 0 && other < numAreas ) {
                milkAreaEdge_t edge;
                edge.area = other;
                edge.portalCenter = centroid;
                areas[a].neighbors.Append( edge );
            }
        }
        areas[a].center = ( sumCount > 0 ) ? ( sum / (float)sumCount ) : vec3_origin;
    }
    return true;
}

// tour is now a list of EDGES (area index + the portal centroid used to
// reach it from the previous step), not just area indices -- the first
// entry's portalCenter is unused (there's no "previous step" yet).
static void VisBuildAreaTour( const idList<milkAreaNode_t> & areas, idList<milkAreaEdge_t> & tour, int maxSteps ) {
    tour.Clear();
    if ( areas.Num() == 0 ) {
        return;
    }
    unsigned int seed = (unsigned int)( Sys_Milliseconds() ^ ( areas.Num() * 2654435761u ) );
    // Fixed real bug: always starting the walk at area index 0 meant a map
    // where that specific area happens to be isolated (0 portals) silently
    // produced a zero-length tour and no flythrough motion at all, even if
    // the rest of the map is richly connected -- pick the first area that
    // actually has a neighbor instead, so an isolated area 0 doesn't sink
    // the whole feature for maps where it's just one dead room among many.
    int current = 0;
    for ( int i = 0; i < areas.Num(); i++ ) {
        if ( areas[i].neighbors.Num() > 0 ) {
            current = i;
            break;
        }
    }
    int prev = -1;
    milkAreaEdge_t first;
    first.area = current;
    first.portalCenter = areas[current].center;
    tour.Append( first );
    for ( int step = 0; step < maxSteps; step++ ) {
        const idList<milkAreaEdge_t> & neighbors = areas[current].neighbors;
        if ( neighbors.Num() == 0 ) {
            break;   // dead end (isolated area) -- stop rather than loop forever
        }
        seed = seed * 1664525u + 1013904223u;   // same LCG style as BuildCycleList's shuffle
        int pick = (int)( ( seed >> 8 ) % (unsigned int)neighbors.Num() );
        if ( neighbors[pick].area == prev && neighbors.Num() > 1 ) {
            pick = ( pick + 1 ) % neighbors.Num();   // avoid immediately backtracking when there's another option
        }
        prev = current;
        current = neighbors[pick].area;
        tour.Append( neighbors[pick] );
    }
}

// PRD M1 step 8 test command: prove the idVisFBO wrapper actually works on
// this GL setup before building the (much larger) render-path rewiring on
// top of it -- allocates a small FMT_RGBA16F test image, attaches it to a
// framebuffer, and reports completeness. Does not yet render anything into
// it or wire it into the visualizer's draw path; see the class comment
// above idVisFBO for what's still open.
CONSOLE_COMMAND( vis_fboTest, "test the M1 step 8 FBO wrapper (allocate FMT_RGBA16F + attach + check-complete)", NULL ) {
    // GetImage() only looks up an ALREADY-registered image -- it returns NULL
    // rather than creating one (confirmed by reading idImageManager::GetImage
    // directly), so on a fresh run this always fell through to the "couldn't
    // get/allocate" branch without ever exercising the FBO. Fixed to follow
    // the same GetImage-then-AllocImage(name) fallback CaptureRenderToImage
    // already uses.
    idImage * testImage = globalImages->GetImage( "_visFboTest" );
    if ( testImage == NULL ) {
        testImage = globalImages->AllocImage( "_visFboTest" );
    }
    if ( testImage == NULL ) {
        idLib::Printf( "vis_fboTest: couldn't get/allocate a test image\n" );
        return;
    }
    idImageOpts opts;
    opts.format = FMT_RGBA16F;
    opts.width = 256;
    opts.height = 256;
    opts.numLevels = 1;
    testImage->AllocImage( opts, TF_LINEAR, TR_CLAMP );

    idVisFBO fbo;
    if ( fbo.Create( testImage ) ) {
        idLib::Printf( "vis_fboTest: FBO created and complete (FMT_RGBA16F attach OK)\n" );
    } else {
        idLib::Printf( "vis_fboTest: FBO creation FAILED (see warning above, if any -- likely "
                        "GL_ARB_framebuffer_object unavailable or an incomplete attachment)\n" );
    }
    fbo.Destroy();
}

// PRD M1 step 8 test command: exercise the REAL render-path plumbing (unlike
// vis_fboTest above, which only proves the raw attach/complete check in
// isolation) -- draws a solid red rect via the normal 2D draw API while
// redirected into an offscreen FMT_RGBA16F target via
// renderSystem->BeginOffscreenRender/EndOffscreenRender, then lets End's
// full-screen quad blit present it. If the round trip works, the screen
// should flash solid red for one frame. See RC_VIS_FBO_BEGIN/RC_VIS_FBO_END
// (tr_local.h) and RB_VisFboBegin/RB_VisFboEnd (tr_backend_draw.cpp) for the
// backend side this drives. This is the piece that was still open after the
// FBO wrapper class landed -- see plans/PRD-implementation-status.md M1
// step 8 for the fuller architectural note on why the wrapper alone wasn't
// enough (raw qgl* calls from this file's own thread don't land on the
// backend's command stream).
CONSOLE_COMMAND( vis_fboRenderTest, "test the M1 step 8 offscreen render-path (flashes solid red for one frame via BeginOffscreenRender/EndOffscreenRender)", NULL ) {
    if ( s_visSolid == NULL ) {
        idLib::Printf( "vis_fboRenderTest: visualizer materials not resolved yet -- try again after a frame or two\n" );
        return;
    }
    renderSystem->BeginOffscreenRender( "_visFboRenderTest", renderSystem->GetWidth(), renderSystem->GetHeight() );
    renderSystem->SetColor( idVec4( 1.0f, 0.0f, 0.0f, 1.0f ) );
    renderSystem->DrawStretchPic( 0.0f, 0.0f, VIS_W, VIS_H, 0.0f, 0.0f, 1.0f, 1.0f, s_visSolid );
    renderSystem->EndOffscreenRender();
    idLib::Printf( "vis_fboRenderTest: issued -- screen should flash solid red for one frame\n" );
}

CONSOLE_COMMAND( vis_mapTour, "build+print a portal-graph camera tour for the loaded map (PRD M4.5 FR-F3, print-only)", NULL ) {
    if ( s_milkMapWorld == NULL ) {
        idLib::Printf( "vis_mapTour: no map loaded -- use vis_mapLoad first\n" );
        return;
    }
    idList<milkAreaNode_t> areas;
    if ( !VisBuildAreaGraph( s_milkMapWorld, areas ) ) {
        idLib::Printf( "vis_mapTour: map has no areas\n" );
        return;
    }
    int totalEdges = 0;
    for ( int i = 0; i < areas.Num(); i++ ) {
        totalEdges += areas[i].neighbors.Num();
    }
    idLib::Printf( "vis_mapTour: %d areas, %d area-adjacency edges\n", areas.Num(), totalEdges );

    idList<milkAreaEdge_t> tour;
    VisBuildAreaTour( areas, tour, 20 );
    idLib::Printf( "tour (%d steps):\n", tour.Num() );
    for ( int i = 0; i < tour.Num(); i++ ) {
        const int a = tour[i].area;
        idLib::Printf( "  area %d  center (%.1f %.1f %.1f)\n", a, areas[a].center.x, areas[a].center.y, areas[a].center.z );
    }
}

/*
========================
PRD M4.5 FR-F3: animated camera along the portal-graph tour + 3D scene render

Position/orientation are computed on the MAIN thread (VisUpdateMilkCamera,
called from Frame()) and cached as plain idVec3s, matching the same
"compute on main thread, draw-worker only reads" discipline as
s_mod[]/s_milkTex[]/s_milkWaveParams -- not because idRenderWorld::RenderScene
itself demands it (it's just another render-backend enqueue call, safe from
the draw-worker thread like DrawStretchPic), but for consistency with every
other piece of cached frame state in this file. VisRenderMilkMapScene (called
from Draw2D, draw-worker thread) only reads the cached position/look-at and
builds the renderView_t + calls RenderScene -- the exact field-population
pattern confirmed by directly reading neo/ui/RenderWindow.cpp's own
RenderScene call (memset-zero, vieworg, viewaxis, shaderParms[0..3]=1,
fov_x/fov_y, time[0]=time[1]=now).

Camera motion: a Catmull-Rom spline through the tour's waypoints (cyclic
wrap, matching the tour's own wrap-to-start behavior) instead of raw linear
interpolation, so the camera turns smoothly instead of snapping direction
at every waypoint. Collision-safe path nudging is implemented using
idRenderWorld::Trace() -- confirmed viable via direct investigation (no
idCollisionModelManager needed: InitFromMap's render-only load already
populates per-area world-model geometry that Trace() sweeps against, see
plans/PRD-implementation-status.md M4.5 for the full research). The tour's
raw area-to-area path is first expanded to route THROUGH each connecting
portal's centroid rather than straight from room-center to room-center (the
main real-world source of wall-clipping in non-rectangular rooms, fixed by
construction using data VisBuildAreaGraph already computes), then a single
best-effort nudge pass pushes any still-blocked segment's endpoint away
from the hit surface along its normal. This is a real, bounded improvement,
not an exhaustive/iterative solver -- documented as such, not oversold.
========================
*/
static idList<milkAreaNode_t> s_milkTourAreas;
static idList<idVec3>         s_milkTourPath;   // final, nudged waypoints (floor level, traced -- see VisRebuildMilkTour; MILK_CAM_HEIGHT added at use time)
static int                    s_milkTourIndex = 0;      // current segment's start waypoint
static float                  s_milkTourSegT = 0.0f;     // 0..1 progress along the current segment
static idVec3                 s_milkCamPos = vec3_origin;
static idVec3                 s_milkCamLookAt = vec3_origin;
static bool                   s_milkCamValid = false;

static const float MILK_CAM_SPEED = 90.0f;   // units/sec along the tour path (roughly walking pace in DOOM 3 units)
static const float MILK_CAM_HEIGHT = 48.0f;  // added above each waypoint's traced floor Z (see VisRebuildMilkTour's floor-snap pass) -- approximate eye height
static const float MILK_CAM_RADIUS = 16.0f;  // camera "size" for the collision-safety sweep below
static const float MILK_CAM_NUDGE = 24.0f;   // how far to push a blocked waypoint away from the wall it hit

// Catmull-Rom spline through p1..p2 (p0/p3 are the neighbors used for the
// tangent), t in 0..1. Standard 4-point form; only idVec3 +/-/scalar-* are
// used so this doesn't depend on any free-function operator overloads.
static idVec3 VisCatmullRom( const idVec3 & p0, const idVec3 & p1, const idVec3 & p2, const idVec3 & p3, float t ) {
    const float t2 = t * t;
    const float t3 = t2 * t;
    idVec3 result = p1 * 2.0f;
    result += ( p2 - p0 ) * t;
    result += ( p0 * 2.0f - p1 * 5.0f + p2 * 4.0f - p3 ) * t2;
    result += ( p1 * 3.0f - p0 - p2 * 3.0f + p3 ) * t3;
    result *= 0.5f;
    return result;
}

// (re)computes the area graph + a fresh, collision-nudged tour for the
// currently loaded map. Called once after a successful vis_mapLoad; safe to
// call again manually (e.g. to re-roll the tour) since it fully resets the
// camera state.
static void VisRebuildMilkTour() {
    s_milkTourAreas.Clear();
    s_milkTourPath.Clear();
    s_milkTourIndex = 0;
    s_milkTourSegT = 0.0f;
    s_milkCamValid = false;
    if ( s_milkMapWorld == NULL || !VisBuildAreaGraph( s_milkMapWorld, s_milkTourAreas ) ) {
        return;
    }
    idList<milkAreaEdge_t> rawTour;
    VisBuildAreaTour( s_milkTourAreas, rawTour, idMath::ClampInt( 8, 500, s_milkTourAreas.Num() * 3 ) );
    if ( rawTour.Num() < 2 ) {
        return;
    }

    // expand area-to-area steps into center -> portal -> center -> portal...
    idList<idVec3> path;
    path.Append( s_milkTourAreas[ rawTour[0].area ].center );
    for ( int i = 1; i < rawTour.Num(); i++ ) {
        path.Append( rawTour[i].portalCenter );
        path.Append( s_milkTourAreas[ rawTour[i].area ].center );
    }

    // PRD M4.5 fidelity fix: each raw waypoint above is an average of
    // doorway/portal-opening positions, not necessarily anywhere near the
    // actual floor -- MILK_CAM_HEIGHT used to get added directly on top of
    // that raw average as a blind fixed offset (documented as a known
    // approximation). Trace straight down from above each raw waypoint
    // (same idRenderWorld::Trace() the collision-nudge pass below already
    // uses) to find the real floor and snap the waypoint's Z to it instead,
    // so MILK_CAM_HEIGHT becomes a genuine "eye height above the floor"
    // rather than "eye height above an arbitrary doorway average." Falls
    // back to the raw centroid Z unchanged if no floor is found within range
    // (e.g. an outdoor/void area, or a portal centroid well clear of any
    // nearby floor).
    for ( int i = 0; i < path.Num(); i++ ) {
        modelTrace_t floorTrace;
        const idVec3 start( path[i].x, path[i].y, path[i].z + 64.0f );
        const idVec3 end( path[i].x, path[i].y, path[i].z - 256.0f );
        if ( s_milkMapWorld->Trace( floorTrace, start, end, 0.0f, true, true ) && floorTrace.fraction < 0.999f ) {
            path[i].z = floorTrace.point.z;
        }
    }

    // collision-safe nudge pass: sweep a MILK_CAM_RADIUS-sized trace along
    // each segment at camera height; if blocked, push the later waypoint
    // away from the hit surface along its normal.
    for ( int i = 0; i + 1 < path.Num(); i++ ) {
        idVec3 start = path[i];     start.z += MILK_CAM_HEIGHT;
        idVec3 end   = path[i + 1]; end.z   += MILK_CAM_HEIGHT;
        modelTrace_t trace;
        if ( s_milkMapWorld->Trace( trace, start, end, MILK_CAM_RADIUS, true, true ) && trace.fraction < 0.999f ) {
            path[i + 1] += trace.normal * MILK_CAM_NUDGE;
        }
    }

    s_milkTourPath = path;
    s_milkCamValid = ( s_milkTourPath.Num() >= 2 );
}

// main thread, called from Frame() alongside VisUpdateModulation/VisUpdateMilkFrame.
static void VisUpdateMilkCamera( float dtSec ) {
    if ( !s_milkCamValid || s_milkTourPath.Num() < 2 ) {
        return;
    }
    const int n = s_milkTourPath.Num();
    idVec3 a = s_milkTourPath[ s_milkTourIndex % n ];
    int nextIdx = ( s_milkTourIndex + 1 ) % n;
    idVec3 b = s_milkTourPath[ nextIdx ];
    const float segLen = ( b - a ).Length();
    // PRD FR-F4: fly-speed is the base constant times its routed multiplier
    // (VMT_CAM_SPEED, neutral 1.0 when unrouted / mod disabled -- so this is
    // exactly MILK_CAM_SPEED as before until someone routes it). VisModOr keeps
    // a stuck-empty s_mod view from freezing the camera (falls back to 1.0).
    const float camSpeed = MILK_CAM_SPEED * VisModOr( VMT_CAM_SPEED, 1.0f );
    const float advance = ( segLen > 1.0f ) ? ( camSpeed * dtSec / segLen ) : 1.0f;
    s_milkTourSegT += advance;
    if ( s_milkTourSegT >= 1.0f ) {
        // Fixed real bug: the tour is a one-way random walk through the
        // portal graph (VisBuildAreaTour), not an actual cycle back to its
        // own start -- the collision-nudge pass above only ever traces
        // consecutive forward-walk pairs (path[i] -> path[i+1]), never
        // path[last] -> path[0]. Treating the path as cyclic forever (the
        // old `% n` wraparound) meant that once per lap, forever, the
        // camera cut a straight, never-Trace()-tested, not-portal-routed
        // line from wherever the walk happened to end back to the start --
        // in most maps that segment runs straight through solid geometry,
        // directly undermining the "collision-safe" guarantee. Roll a
        // brand-new tour instead of wrapping, and skip position/lookAt
        // computation for this one frame (the next frame picks up the
        // fresh path from its own start cleanly) -- also a nice side
        // effect: real visual variety instead of an exact repeating loop.
        if ( nextIdx == 0 ) {
            VisRebuildMilkTour();
            return;
        }
        s_milkTourSegT -= idMath::Floor( s_milkTourSegT );   // keep the fractional remainder rather than losing it on a big hitch
        s_milkTourIndex = nextIdx;
    }

    const int i0 = ( s_milkTourIndex - 1 + n ) % n;
    const int i1 = s_milkTourIndex % n;
    const int i2 = ( s_milkTourIndex + 1 ) % n;
    const int i3 = ( s_milkTourIndex + 2 ) % n;
    idVec3 pos = VisCatmullRom( s_milkTourPath[i0], s_milkTourPath[i1], s_milkTourPath[i2], s_milkTourPath[i3], s_milkTourSegT );
    // look a little further ahead along the same spline, for a stable,
    // smoothly-turning look-at point instead of snapping onto the next raw waypoint.
    const float lookT = idMath::ClampFloat( 0.0f, 1.0f, s_milkTourSegT + 0.15f );
    idVec3 lookAt = VisCatmullRom( s_milkTourPath[i0], s_milkTourPath[i1], s_milkTourPath[i2], s_milkTourPath[i3], lookT );

    pos.z += MILK_CAM_HEIGHT;
    lookAt.z += MILK_CAM_HEIGHT;
    s_milkCamPos = pos;
    s_milkCamLookAt = lookAt;
}

// draw-worker thread, called from Draw2D. Builds the renderView_t from the
// cached camera state and queues the 3D scene render BEFORE any of the
// existing 2D overlay drawing this frame, so the visualizer's feedback/
// bloom/kaleidoscope processors (which operate on "whatever is on screen")
// composite on top of it, per FR-F5.
// same condition VisRenderMilkMapScene itself gates on -- see its forward
// declaration/comment near VisDrawEffectPanelBackdrop for why this exists.
static bool VisMapSceneActive() {
    return s_milkMapWorld != NULL && s_milkCamValid;
}

// PRD follow-up (direct user report: "for the map can we choose the
// wireframe hue and the background hue? those should also be bindable
// parameters"). Driven every frame (unlike vis_mapRenderMode's own
// r_showTris/r_forceAmbientDiffuse, which are only set once at
// vis_mapLoad) so the hue can actually be audio/MIDI/mouse-reactive.
// r_showTrisHue is reset to -1 (disabled -> plain white) whenever no map
// is loaded or mode 1 (textured) is active, so this never bleeds into
// ordinary r_showTris debug use elsewhere in the engine.
static void VisUpdateMapRenderColors() {
    if ( VisMapSceneActive() && vis_mapRenderMode.GetInteger() == 0 ) {
        float hue = vis_mapWireHue.GetFloat() + VisModOr( VMT_MAPWIREHUE_MOD, 0.0f );
        hue -= idMath::Floor( hue );   // wrap to [0,1)
        cvarSystem->SetCVarFloat( "r_showTrisHue", hue );
        // PRD follow-up: wireframe blend mode, same VISBLEND_* index the
        // Layer Stack's own blend combo uses.
        cvarSystem->SetCVarInteger( "r_showTrisBlendMode", vis_mapWireBlend.GetInteger() );
    } else {
        cvarSystem->SetCVarFloat( "r_showTrisHue", -1.0f );
        cvarSystem->SetCVarInteger( "r_showTrisBlendMode", -1 );
    }
}

// PRD follow-up: draws the map view's background-fill "layer" (hue + blend
// mode), if enabled. Extracted so VisRenderMilkMapScene can call it either
// before or after the 3D scene render, per vis_mapWireOnTop.
static void VisDrawMapBackgroundFill() {
    if ( !vis_mapBgTint.GetBool() ) {
        return;
    }
    const float bgHue = vis_mapBgHue.GetFloat() + VisModOr( VMT_MAPBGHUE_MOD, 0.0f );
    VisFillRectBlend( HueColor( bgHue - idMath::Floor( bgHue ), 1.0f ), 0, 0, VIS_W, VIS_H, vis_mapBgBlend.GetInteger() );
}

static void VisRenderMilkMapScene() {
    if ( s_milkMapWorld == NULL || !s_milkCamValid ) {
        return;
    }
    renderView_t view;
    memset( &view, 0, sizeof( view ) );
    view.vieworg = s_milkCamPos;
    const idVec3 lookDir = s_milkCamLookAt - s_milkCamPos;
    if ( lookDir.LengthSqr() > 1.0f ) {
        view.viewaxis = lookDir.ToAngles().ToMat3();
    } else {
        view.viewaxis.Identity();
    }
    view.shaderParms[0] = view.shaderParms[1] = view.shaderParms[2] = view.shaderParms[3] = 1.0f;
    // PRD FR-F4: FOV punches with the routed multiplier (VMT_CAM_FOV, neutral
    // 1.0 => identical to the old fixed 90deg). fov_x and fov_y scale by the
    // same factor so the aspect stays correct; when the multiplier is 1.0 this
    // is byte-for-byte the original derivation.
    const float fovMul = VisModOr( VMT_CAM_FOV, 1.0f );
    view.fov_x = 90.0f * fovMul;
    view.fov_y = 2.0f * atan( VIS_H / VIS_W ) * idMath::M_RAD2DEG * fovMul;   // matches RenderWindow.cpp's own fov_y derivation
    const int now = Sys_Milliseconds();
    view.time[0] = now;
    view.time[1] = now;
    // PRD follow-up (direct user report: "for the map can we choose ... the
    // background hue? those should also be bindable parameters" / "we also
    // need to be able to organize or sort the layers"). The map view has
    // exactly two layers -- this background fill and the 3D scene itself --
    // and vis_mapWireOnTop picks which one draws on top. Background-first
    // (the default) was confirmed via a real screenshot test to survive
    // rather than get wiped by the 3D view's own render, so background-
    // second is expected to work the same way (same 2D draw mechanism,
    // just called after RenderScene instead of before).
    if ( vis_mapWireOnTop.GetBool() ) {
        VisDrawMapBackgroundFill();
        s_milkMapWorld->RenderScene( &view );
    } else {
        s_milkMapWorld->RenderScene( &view );
        VisDrawMapBackgroundFill();
    }
}

CONSOLE_COMMAND( vis_milkLoad, "parse a .milk preset and print a summary (PRD M1, parser-only)", NULL ) {
    if ( args.Argc() != 2 ) {
        idLib::Printf( "usage: vis_milkLoad <path-relative-to-fs, e.g. presets/milk/foo.milk>\n" );
        return;
    }
    idMilkPreset preset;
    if ( preset.Load( args.Argv( 1 ) ) ) {
        preset.PrintSummary();
    }
}

// PRD M3: strips the "shader_body { ... }" wrapper MilkDrop's raw warp_/comp_
// text carries (idMilkPreset::GetWarpShader()/GetCompShader() return that
// concatenated text as-is; "shader_body" itself is a MilkDrop-only marker,
// not valid HLSL) down to just the inner statements idMilkShaderTranspiler's
// wrapping convention expects. Best-effort: if the text doesn't look like
// the expected wrapper (no braces found), returns it unchanged rather than
// silently mangling something unexpected.
static idStr VisStripShaderBody( const idStr & raw ) {
    idStr s = raw;
    s.StripLeading( ' ' );
    s.StripLeading( '\t' );
    if ( idStr::Icmpn( s.c_str(), "shader_body", 11 ) == 0 ) {
        s = s.Right( s.Length() - 11 );
    }
    int firstBrace = -1;
    for ( int i = 0; i < s.Length(); i++ ) {
        if ( s[i] == '{' ) { firstBrace = i; break; }
    }
    int lastBrace = -1;
    for ( int i = s.Length() - 1; i >= 0; i-- ) {
        if ( s[i] == '}' ) { lastBrace = i; break; }
    }
    if ( firstBrace < 0 || lastBrace < 0 || lastBrace <= firstBrace ) {
        return s;
    }
    return s.Mid( firstBrace + 1, lastBrace - firstBrace - 1 );
}

// PRD M3 test command: the EXACT warp_/comp_ text from a real preset
// (butterchurn-presets' presets/milkdrop/11.milk, warp_1..25/comp_1..30),
// used to empirically verify idMilkShaderTranspiler's preamble/wrapping
// convention -- see MilkShaderTranspiler.h for the full story (built +ran
// hlsl2glslfork against this exact text on the dev machine, diffed the
// output against Butterchurn's own independently-converted reference,
// confirmed semantically identical). This command re-proves the same thing
// on whatever build actually runs it, not just once during development.
static const char * s_shaderTranspileTestWarp =
    "shader_body {\n"
    "float corr = texsize.xy*texsize_noise_lq.zw;\n"
    "float2 uv1 = float2(uv.x-0.5,uv.y-0.5);//*aspect.xy;\n"
    "\n"
    "float3 noiseVal =.03*(tex2D(sampler_noise_lq, uv*.3+.01*rand_frame));\n"
    "noiseVal = .01;\n"
    "float3 Feedback = GetBlur1(1-uv);\n"
    "\n"
    "float2 zz = uv1 *texsize.xy *.015;\n"
    "//zz =mul(zz,float2x2(_qa));\n"
    "zz = zz.yx;\n"
    "float2 h1 = clamp(tan(zz.yx),-12,12) * cos(4*(q2+1)*zz);\n"
    "\n"
    "uv.xy += h1*texsize.zw * 4*(2+q1);\n"
    "\n"
    "\n"
    "float r = tex2D (sampler_noise_hq,uv/14).x*12;\n"
    "float2 uv6 = uv1;\n"
    "uv6 = mul(uv6, float2x2(cos(r),sin(r),-sin(r),cos(r)));\n"
    "float3 mus = .1/(sqrt(uv6.y));\n"
    "\n"
    "float3 crisp = tex2D(sampler_main,uv);\n"
    "//crisp = lerp(crisp.bgr,crisp.rgb,2*crisp);\n"
    "ret = .995*crisp+noiseVal-.02;\n"
    "}\n";

CONSOLE_COMMAND( vis_shaderTranspileTest, "PRD M3: re-verify the HLSL->GLSL transpiler against a known-good real preset shader", NULL ) {
    const idStr body = VisStripShaderBody( s_shaderTranspileTestWarp );
    idStr glsl, err;
    if ( idMilkShaderTranspiler::Transpile( body.c_str(), "milk_warp_test", glsl, err ) ) {
        idLib::Printf( "vis_shaderTranspileTest: PASSED -- transpiled %d bytes of HLSL into %d bytes of GLSL:\n%s\n",
            body.Length(), glsl.Length(), glsl.c_str() );
    } else {
        idLib::Printf( "vis_shaderTranspileTest: FAILED\n%s\n", err.c_str() );
    }
}

// PRD M3: transpile a real preset's warp_/comp_ shaders, if it has any.
CONSOLE_COMMAND( vis_shaderTranspile, "parse a .milk preset and transpile its warp_/comp_ shaders to GLSL, printing the result (PRD M3)", NULL ) {
    if ( args.Argc() != 2 ) {
        idLib::Printf( "usage: vis_shaderTranspile <path-relative-to-fs, e.g. presets/milk/foo.milk>\n" );
        return;
    }
    idMilkPreset preset;
    if ( !preset.Load( args.Argv( 1 ) ) ) {
        return;
    }
    bool any = false;
    if ( !preset.GetWarpShader().IsEmpty() ) {
        any = true;
        const idStr body = VisStripShaderBody( preset.GetWarpShader() );
        idStr glsl, err;
        if ( idMilkShaderTranspiler::Transpile( body.c_str(), "milk_warp", glsl, err ) ) {
            idLib::Printf( "warp_: PASSED\n%s\n", glsl.c_str() );
        } else {
            idLib::Printf( "warp_: FAILED\n%s\n", err.c_str() );
        }
    }
    if ( !preset.GetCompShader().IsEmpty() ) {
        any = true;
        const idStr body = VisStripShaderBody( preset.GetCompShader() );
        idStr glsl, err;
        if ( idMilkShaderTranspiler::Transpile( body.c_str(), "milk_comp", glsl, err ) ) {
            idLib::Printf( "comp_: PASSED\n%s\n", glsl.c_str() );
        } else {
            idLib::Printf( "comp_: FAILED\n%s\n", err.c_str() );
        }
    }
    if ( !any ) {
        idLib::Printf( "vis_shaderTranspile: '%s' has no warp_/comp_ shaders (an equation-only preset)\n", args.Argv( 1 ) );
    }
}

// PRD M1 step 3 test command: parse a .milk preset (MilkPreset.h), compile
// its per_frame_init_/per_frame_ code against a fresh idMilkEvaluator, run
// per_frame_init_ once then per_frame_ for a handful of simulated frames,
// and print the resulting q1..q8 + bound audio variables each time -- an
// end-to-end proof that parse -> compile -> execute -> variable read-back
// all work together, before any warp-mesh/render integration exists.
CONSOLE_COMMAND( vis_milkEval, "parse+evaluate a .milk preset's per_frame code a few times (PRD M1 test)", NULL ) {
    if ( args.Argc() != 2 ) {
        idLib::Printf( "usage: vis_milkEval <path-relative-to-fs, e.g. presets/milk/foo.milk>\n" );
        return;
    }
    idMilkPreset preset;
    if ( !preset.Load( args.Argv( 1 ) ) ) {
        return;
    }
    idMilkEvaluator eval;
    if ( !eval.Init() ) {
        idLib::Warning( "vis_milkEval: evaluator init failed" );
        return;
    }

    struct projectm_eval_code * initCode = eval.Compile( preset.GetPerFrameInit().c_str() );
    struct projectm_eval_code * frameCode = eval.Compile( preset.GetPerFrame().c_str() );
    struct projectm_eval_code * pixelCode = eval.Compile( preset.GetPerPixel().c_str() );

    eval.UpdateVariables( 0.0f, 32, 24, 1920, 1080 );
    if ( initCode != NULL ) {
        eval.Execute( initCode );
    }
    idLib::Printf( "vis_milkEval: '%s' -- per_frame_init %s, per_frame %s, per_pixel %s\n",
        preset.GetName().c_str(),
        initCode != NULL ? "compiled" : ( preset.GetPerFrameInit().IsEmpty() ? "(none)" : "FAILED" ),
        frameCode != NULL ? "compiled" : ( preset.GetPerFrame().IsEmpty() ? "(none)" : "FAILED" ),
        pixelCode != NULL ? "compiled" : ( preset.GetPerPixel().IsEmpty() ? "(none)" : "FAILED" ) );

    for ( int frame = 0; frame < 5; frame++ ) {
        eval.UpdateVariables( 1.0f / 30.0f, 32, 24, 1920, 1080 );
        if ( frameCode != NULL ) {
            eval.Execute( frameCode );
        }
        idLib::Printf( "  frame %d: bass=%.3f mid=%.3f treb=%.3f  q1=%.3f q2=%.3f q3=%.3f\n",
            frame, g_audioAnalyzer.GetBass(), g_audioAnalyzer.GetMid(), g_audioAnalyzer.GetTreb(),
            eval.GetQ( 0 ), eval.GetQ( 1 ), eval.GetQ( 2 ) );

        // exercise the exact per-vertex path M1 step 4's warp mesh will use:
        // set x/y/rad/ang for one sample vertex, run per_pixel_, then read
        // back whatever OUTPUT variable that preset's code assigns to
        // (zoom/rot/warp/dx/dy/... -- not pre-registered, found post-compile
        // via GetVariable since the compiler auto-creates them on first use).
        if ( pixelCode != NULL ) {
            eval.SetPixelVars( 0.5f, 0.5f, 0.1f, 0.0f );   // sample vertex near center
            eval.Execute( pixelCode );
            PRJM_EVAL_F * zoomVar = eval.GetVariable( "zoom" );
            if ( zoomVar != NULL ) {
                idLib::Printf( "    per_pixel sample: zoom=%.4f\n", *zoomVar );
            }
        }
    }

    eval.FreeCode( initCode );
    eval.FreeCode( frameCode );
    eval.FreeCode( pixelCode );
}

CONSOLE_COMMAND( vis_layers, "list layer types (ZGE source/processor model) and their state", NULL ) {
    idLib::Printf( "layer types (source = mutually exclusive; processor = independently toggled):\n" );
    for ( int i = 0; i < NUM_VIS_LAYER_TYPES; i++ ) {
        const visLayerType_t & lt = s_layerTypes[i];
        bool on = false;
        if ( lt.category == VISLAYERCAT_SOURCE ) {
            on = ( vis_effect.GetInteger() == lt.effectIndex );
        } else if ( idStr::Icmp( lt.name, "Feedback Trail" ) == 0 ) {
            on = vis_feedback.GetBool();
        } else if ( idStr::Icmp( lt.name, "Warp" ) == 0 ) {
            on = ( vis_warp.GetInteger() > 0 );
        } else if ( idStr::Icmp( lt.name, "Kaleidoscope" ) == 0 ) {
            on = ( vis_kaleido.GetInteger() >= 2 );
        } else if ( idStr::Icmp( lt.name, "Bloom / Glow" ) == 0 ) {
            on = ( vis_bloom.GetFloat() > 0.01f );
        }
        idLib::Printf( "  %-15s %-10s %s\n", lt.name,
            lt.category == VISLAYERCAT_SOURCE ? "[source]" : "[processor]",
            on ? "ON" : "off" );
    }
}

// PRD Pillar C real add/remove layer stack: console-command surface (also
// what the GUI's Add/Remove/blend/opacity controls call through
// VisGuiRunCommand, and what VisWritePresetTo/preset load round-trips
// through, exactly like every other vis_* command in this file).
CONSOLE_COMMAND( vis_stackAdd, "add a layer to the ZGE-style stack: vis_stackAdd <effectType 0-7> [opacity 0-1] [blendMode 0-7: Normal/Additive/Subtractive/Multiply/Screen/Darken/Lighten/Invert] [enabled 0/1] [posX -1..1] [posY -1..1]", NULL ) {
    if ( args.Argc() < 2 ) {
        idLib::Printf( "usage: vis_stackAdd <effectType 0-7> [opacity 0-1] [blendMode 0-7] [enabled 0/1] [posX -1..1] [posY -1..1]\n"
                       "  blendMode: 0 Normal, 1 Additive, 2 Subtractive, 3 Multiply, 4 Screen, 5 Darken, 6 Lighten, 7 Invert\n" );
        return;
    }
    const int effectType = atoi( args.Argv( 1 ) );
    const float opacity   = ( args.Argc() >= 3 ) ? (float)atof( args.Argv( 2 ) ) : 1.0f;
    // NOTE: 0/1 mean exactly the same thing they always did (Normal/Additive)
    // -- this argument was a bool before real blend-mode support existed, so
    // old saved presets/console lines with "0"/"1" here keep working unchanged.
    const int blendMode  = ( args.Argc() >= 4 ) ? atoi( args.Argv( 3 ) ) : VISBLEND_NORMAL;
    const bool enabled    = ( args.Argc() >= 5 ) ? ( atoi( args.Argv( 4 ) ) != 0 ) : true;
    const float posX      = ( args.Argc() >= 6 ) ? (float)atof( args.Argv( 5 ) ) : 0.0f;
    const float posY      = ( args.Argc() >= 7 ) ? (float)atof( args.Argv( 6 ) ) : 0.0f;
    if ( VisStackAddLayer( effectType, opacity, blendMode, enabled, posX, posY ) ) {
        idLib::Printf( "vis_stackAdd: added slot %d (%s)\n", s_layerStack.Num() - 1, s_layerTypes[effectType].name );
    }
}

CONSOLE_COMMAND( vis_stackRemove, "remove a layer from the stack: vis_stackRemove <index>", NULL ) {
    if ( args.Argc() != 2 ) {
        idLib::Printf( "usage: vis_stackRemove <index>\n" );
        return;
    }
    VisStackRemoveLayer( atoi( args.Argv( 1 ) ) );
}

CONSOLE_COMMAND( vis_stackMove, "reorder the stack: vis_stackMove <index> <newIndex>", NULL ) {
    if ( args.Argc() != 3 ) {
        idLib::Printf( "usage: vis_stackMove <index> <newIndex>\n" );
        return;
    }
    VisStackMoveLayer( atoi( args.Argv( 1 ) ), atoi( args.Argv( 2 ) ) );
}

CONSOLE_COMMAND( vis_stackClear, "remove every layer from the stack (falls back to plain vis_effect)", NULL ) {
    VisStackClear();
}

CONSOLE_COMMAND( vis_stackList, "list the current ZGE-style add/remove layer stack", NULL ) {
    if ( s_layerStack.Num() == 0 ) {
        idLib::Printf( "layer stack is empty -- vis_effect (%d) drives the single legacy effect slot\n", vis_effect.GetInteger() );
        return;
    }
    idLib::Printf( "layer stack (%d/%d slots, bottom to top):\n", s_layerStack.Num(), VIS_MAX_STACK_LAYERS );
    for ( int i = 0; i < s_layerStack.Num(); i++ ) {
        const visStackLayer_t & layer = s_layerStack[i];
        const int mode = ( layer.blendMode >= 0 && layer.blendMode < VISBLEND_COUNT ) ? layer.blendMode : VISBLEND_NORMAL;
        idLib::Printf( "  [%d] %-12s opacity %.2f  %-11s %s  pos (%.2f, %.2f)\n", i,
            ( layer.effectType >= 0 && layer.effectType <= 7 ) ? s_layerTypes[layer.effectType].name : "?",
            layer.opacity, s_visBlendModeNames[mode], layer.enabled ? "ON" : "off", layer.posX, layer.posY );
    }
}

CONSOLE_COMMAND( vis_routes, "list the current audio->visual parameter routes", NULL ) {
    VisInitModTargets();
    idLib::Printf( "audio->visual routes (value = base + shape(source) * amount):\n" );
    for ( int t = 0; t < s_targetDefs.Num(); t++ ) {
        const int sh = ( s_routes[t].shape >= 0 && s_routes[t].shape < VSHAPE_COUNT ) ? s_routes[t].shape : VSHAPE_LINEAR;
        idLib::Printf( "  %-7s <- %-8s amount %.3f  base %.3f  shape %-9s%s p=%.3f  (now %.3f)\n",
            s_targetDefs[t].name, VisFormatRouteSource( s_routes[t] ).c_str(),
            s_routes[t].amount, s_routes[t].base,
            s_shapeNames[sh], s_routes[t].invert ? " inv" : "    ", s_routes[t].shapeParam, VisMod(t) );
    }
    idLib::Printf( "sources: none bass mid treb bassatt midatt trebatt rms beat level lfo "
                   "midicc1 midicc2 midicc3 midicc4 midicc<channel>_<cc> (e.g. midicc3_74) "
                   "midinote<channel>_<note> (e.g. midinote0_60) "
                   "band<N> (e.g. band3, one of the current vis_bands spectrum bands)\n" );
}

// PRD M0: targets are looked up by NAME against the registry (VisFindModTarget),
// not a fixed array index — any target a layer registers is patchable here too.
CONSOLE_COMMAND( vis_route, "patch a route: vis_route <target> <source> <amount> [base] [shape] [param] [invert] [param2]", NULL ) {
    VisInitModTargets();
    if ( args.Argc() < 4 ) {
        idLib::Printf( "usage: vis_route <target> <source> <amount> [base] [shape] [param] [invert] [param2]\n"
                       "  targets: scale hue bright zoom rotate warpamountmod layerscalemod hueshiftmod\n"
                       "           effectxmod effectymod layerxmod layerymod (position, PRD Pillar C)\n"
                       "           camspeed camfov (map flythrough camera, PRD FR-F4)\n"
                       "           slot<N>.opacity/pspawn/pscale/starspeed/spectro/posx/posy (per stack slot 0-3, PRD FR-C1-EXPAND)\n"
                       "           run vis_routes for the full live target list\n"
                       "  sources: none bass mid treb bassatt midatt trebatt rms beat level lfo\n"
                       "           mousex mousey (normalized 0..1 cursor position in the game window)\n"
                       "           midicc1 midicc2 midicc3 midicc4 (channel 0 shortcuts)\n"
                       "           midicc<channel>_<cc> (PRD FR-E5: any CC on any channel, e.g. midicc3_74)\n"
                       "           midinote<channel>_<note> (PRD FR-E5: note velocity as an envelope, e.g. midinote0_60)\n"
                       "           band<N> (one of the current vis_bands spectrum bands, e.g. band3)\n"
                       "  shape (PRD FR-C8, optional, default linear): linear exp log scurve threshold quantize\n"
                       "           param  = exponent k (exp/log) | threshold (threshold) | level count N (quantize)\n"
                       "           invert = 0/1 (1-x, composable with any shape); param2 = hysteresis half-band (threshold)\n" );
        return;
    }
    const int t = VisFindModTarget( args.Argv( 1 ) );
    if ( t < 0 ) {
        idLib::Printf( "unknown target '%s'\n", args.Argv( 1 ) );
        return;
    }
    int midiCh = 0, midiCC = 0, midiNote = 0, bandIdx = 0;
    int src;
    if ( VisParseMidiCCSource( args.Argv( 2 ), midiCh, midiCC ) ) {
        src = VMS_MIDICC_PARAM;
    } else if ( VisParseMidiNoteSource( args.Argv( 2 ), midiCh, midiNote ) ) {
        src = VMS_MIDINOTE_VEL;
    } else if ( VisParseBandSource( args.Argv( 2 ), bandIdx ) ) {
        src = VMS_BAND;
    } else {
        src = VisFindName( s_sourceNames, VMS_COUNT, args.Argv( 2 ) );
        if ( src < 0 ) {
            idLib::Printf( "unknown source '%s'\n", args.Argv( 2 ) );
            return;
        }
    }
    s_routes[t].source = src;
    s_routes[t].midiChannel = midiCh;
    s_routes[t].midiCC = midiCC;
    s_routes[t].midiNote = midiNote;
    s_routes[t].bandIndex = bandIdx;
    s_routes[t].amount = atof( args.Argv( 3 ) );
    if ( args.Argc() >= 5 ) {
        s_routes[t].base = atof( args.Argv( 4 ) );
    }
    // PRD FR-C8: shape/param/invert/param2 are trailing optionals. Omitting
    // them leaves the route's existing shape untouched, so the classic 3-4 arg
    // form (and every old .cfg/preset routing line) behaves exactly as before.
    if ( args.Argc() >= 6 ) {
        const char * shapeArg = args.Argv( 5 );
        int sh = VisFindName( s_shapeNames, VSHAPE_COUNT, shapeArg );
        if ( sh < 0 && isdigit( (unsigned char)shapeArg[0] ) ) {
            sh = atoi( shapeArg );   // also accept a bare numeric index
        }
        if ( sh < 0 || sh >= VSHAPE_COUNT ) {
            idLib::Printf( "unknown shape '%s' (linear exp log scurve threshold quantize)\n", shapeArg );
            return;
        }
        s_routes[t].shape = sh;
    }
    if ( args.Argc() >= 7 ) {
        s_routes[t].shapeParam = atof( args.Argv( 6 ) );
    }
    if ( args.Argc() >= 8 ) {
        s_routes[t].invert = ( atoi( args.Argv( 7 ) ) != 0 );
    }
    if ( args.Argc() >= 9 ) {
        s_routes[t].shapeParam2 = atof( args.Argv( 8 ) );
    }
    const int echoShape = ( s_routes[t].shape >= 0 && s_routes[t].shape < VSHAPE_COUNT ) ? s_routes[t].shape : VSHAPE_LINEAR;
    idLib::Printf( "%s <- %s amount %.3f base %.3f shape %s%s param %.3f\n",
        s_targetDefs[t].name, VisFormatRouteSource( s_routes[t] ).c_str(), s_routes[t].amount, s_routes[t].base,
        s_shapeNames[echoShape], s_routes[t].invert ? " (inverted)" : "", s_routes[t].shapeParam );
}

CONSOLE_COMMAND( listMusic, "list audio files under base/music/", NULL ) {
    const char * extensions[] = { ".wav", ".ogg", ".mp3", ".flac", ".m4a", ".aac", ".opus", ".wma" };
    int total = 0;
    for ( int e = 0; e < 8; e++ ) {
        idFileList * files = fileSystem->ListFilesTree( "music", extensions[e], true );
        if ( files != NULL ) {
            for ( int i = 0; i < files->GetNumFiles(); i++ ) {
                idLib::Printf( "  %s\n", files->GetFile( i ) );
                total++;
            }
            fileSystem->FreeFileList( files );
        }
    }
    idLib::Printf( "%d music files\n", total );
}
