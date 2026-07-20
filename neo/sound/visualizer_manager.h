#ifndef __VISUALIZER_MANAGER_H__
#define __VISUALIZER_MANAGER_H__

#include "../idlib/precompiled.h"
#include "playlist_manager.h"
#include "audio_analyzer.h"

/*
================================================
idVisualizerManager

Single owner of visualizer state: the playlist, the current track, and the
analyzer source mode. Deliberately NOT part of idSoundSystemLocal.

Everything is drivable from the console before any UI exists:
    vis_play <file|playlist>   load + play a track or .m3u playlist
    vis_stop / vis_next / vis_prev
    vis_source <engine|loopback>
    vis_status                 print state + current band levels
    listMusic                  list audio files under base/music/

Design notes (see PLAN.md):
 - playback goes through the stock sound pipeline (PlayShaderDirectly on an
   implicitly generated sound shader), so tracks live under base/ (e.g.
   base/music/song.wav). OGG works once the decoder files are in the build.
 - analysis input comes from AudioAnalyzer::PushSamples; in loopback mode the
   WASAPI thread feeds it the final system mix (which includes engine audio).
================================================
*/
// A selectable video mode for the DISPLAY tab. kind 0 = fullscreen on monitor
// `mon` at w x h; kind 1 = windowed at w x h.
struct visDisplayItem_t {
    idStr label;
    int   kind;
    int   mon;
    int   w;
    int   h;
};

class idVisualizerManager {
public:
    idVisualizerManager();

    // called once per frame from idSoundSystemLocal::Render()
    void            Frame();

    // immediate-mode spectrum overlay, drawn from idCommonLocal::Draw() each
    // frame (works with no map loaded). Guarded by the vis_show cvar.
    void            Draw2D();

    void            Play( const char * path );      // file or playlist
    void            Stop();
    void            Next();
    void            Prev();

    void            SetSourceMode( AudioSourceMode mode );

    // #4 on-screen picker (immediate-mode overlay, keyboard driven).
    // MenuProcessEvent is called from idCommonLocal::ProcessEvent on the MAIN
    // thread (safe to touch the file system / decls); MenuDraw2D is called from
    // Draw2D on the draw-worker thread (read-only). Returns true if the event
    // was consumed. F1 toggles; arrows navigate; Enter activates; Esc closes.
    bool            MenuProcessEvent( const sysEvent_t * ev );
    void            MenuDraw2D();
    bool            IsMenuOpen() const { return m_menuOpen; }
    void            ToggleMenu();

    // Presets: snapshot the full visual state (effect, bands, feedback, routes)
    // to base/presets/<name>.cfg, and load one back by exec'ing it.
    bool            SavePreset( const char * name );
    void            SaveCurrentAsCustom();          // auto-named custom-NN
    void            LoadPresetPath( const char * cfgPath );
    // public wrappers for the private NavigatePreset -- so the ImGui panel and
    // a console command can reach the SPACE/BACKSPACE hotkeys' exact behavior
    // (the .cfg cycle list, distinct from the .milk-only vis_milkCut) without
    // exposing the whole private cycle-list machinery.
    void            NextPreset() { NavigatePreset( 1 ); }
    void            PrevPreset() { NavigatePreset( -1 ); }

    idPlaylistManager & GetPlaylist() { return m_playlist; }
    const char *    GetNowPlaying() const { return m_nowPlaying.c_str(); }
    bool            IsPlaying() const { return m_playing; }

    void            PrintStatus() const;

private:
    void            StartCurrentTrack();
    static void     TrackPathToShaderName( const char * path, idStr & out );

    idPlaylistManager   m_playlist;
    idStr               m_nowPlaying;       // shader name currently playing
    bool                m_playing;
    int                 m_trackEndTime;     // Sys_Milliseconds when track finishes (0 = unknown/looping)

    // #1 auto-arm: bring WASAPI loopback up once at boot (retrying until the
    // audio device is ready) so any system audio drives the visualizer with no
    // console commands. Stops touching the source mode after the first success.
    bool                m_sourceArmed;
    int                 m_nextArmMs;
    // Direct user report: "i connected my bluetooth headphones and then the
    // loopback signal died" -- unlike the one-shot boot arm above, this keeps
    // running for the whole session (OS default output can change any time,
    // not just at boot), throttled the same way.
    int                 m_nextDeviceCheckMs;

    // #4 picker menu state
    void                OpenMenu();
    void                CloseMenu();
    void                MenuActivate();     // act on the highlighted item
    int                 MenuItemCount() const;
    bool                m_menuOpen;
    int                 m_menuTab;          // 0 music, 1 playlists, 2 presets, 3 effects
    int                 m_menuSel;          // highlighted item within the tab
    int                 m_menuScroll;       // first visible row (list scrolling)
    idStrList           m_musicFiles;       // built on open (main thread)
    idStrList           m_playlistFiles;
    idStrList           m_presetFiles;
    std::vector<AudioDeviceInfo> m_deviceList;   // capture endpoints (built on open)
    void                BuildDisplayItems();      // enumerate monitors/resolutions
    std::vector<visDisplayItem_t> m_displayItems; // DISPLAY-tab modes (built on open)
    idStrList           m_imageFiles;             // IMAGES-tab layer images (built on open)

    // beat-synced preset auto-cycling
    void                BuildCycleList();
    void                AdvancePreset();
    void                NavigatePreset( int dir );   // manual next/prev preset (hotkeys)
    bool                m_cycleActive;      // tracks the vis_presetCycle enable edge
    int                 m_cycleIndex;       // position within m_cycleOrder
    int                 m_cycleNextMs;      // Sys_Milliseconds of the next switch
    bool                m_cyclePending;     // timer elapsed, waiting for a beat
    idStrList           m_cycleList;        // preset .cfg paths
    idList<int>         m_cycleOrder;       // play order (identity or shuffled)
};

extern idVisualizerManager g_visualizerManager;

#endif // __VISUALIZER_MANAGER_H__
