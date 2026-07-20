#include "../idlib/precompiled.h"
#pragma hdrstop
#include "VisualizerUI.h"
#include "Window.h"
#include "DeviceContext.h"
#include <cstring>
#include <vector>
#include "../sound/snd_local.h"
#include "../sound/visualizer_data.h"
#include "MilkDropParser.h"

extern idSoundSystemLocal soundSystemLocal;

VisualizerUI::VisualizerUI() {
    desktop = new (TAG_OLD_UI) idWindow(this);
    desktop->SetFlag(WIN_DESKTOP | WIN_MOVABLE | WIN_SIZABLE | WIN_CAPTURE);
    desktop->rect = idRectangle(10.0f, 10.0f, 400.0f, 300.0f);

    // Initialize state variables
    m_presetVar = desktop->AddStateVar("selected_preset", "default");
    m_deviceVar = desktop->AddStateVar("audio_device", "default_output");
    m_playlistVar = desktop->AddStateVar("playlist_path", "");
    m_active = false;

    // Initialize UI elements
    desktop->AddText( 10.0f, 10.0f, "Visualizer Tool", idVec4(1,1,1,1) );
    desktop->AddText( 10.0f, 40.0f, "Preset: ", idVec4(1,1,1,1) );
    desktop->AddText( 10.0f, 70.0f, "Audio Device: ", idVec4(1,1,1,1) );
    desktop->AddText( 10.0f, 100.0f, "Playlist: ", idVec4(1,1,1,1) );

    desktop->backColor = idVec4(0.1f, 0.1f, 0.1f, 1.0f);
    desktop->foreColor = idVec4(1.0f, 1.0f, 1.0f, 1.0f);
    desktop->borderColor = idVec4(0.5f, 0.5f, 0.5f, 1.0f);
    desktop->textScale = 0.5f;
}

VisualizerUI::~VisualizerUI() {
    delete desktop;
    desktop = NULL;
}

const char* VisualizerUI::Name() const {
    return "VisualizerTool";
}

const char* VisualizerUI::Comment() const {
    return "Sound Reactive Visualizer Controls";
}

bool VisualizerUI::IsInteractive() const {
    return true;
}

void VisualizerUI::Redraw(int time, bool hud) {
    if (!loading && desktop) {
        time = time;
        dc->PushClipRect(uiManagerLocal.screenRect);
        desktop->Redraw(0, 0, hud);

        // Draw visualizer bars
        float barWidth = desktop->rect.Width() / 64.0f;
        for (int i = 0; i < 64; i++) {
            float magnitude = g_audioAnalyzer.GetFrequencyBin(i * (FFT_SIZE / 128));
            float barHeight = magnitude * 300.0f; // scale factor
            dc->DrawRect(i * barWidth, desktop->rect.Height() - barHeight, barWidth - 2, barHeight, dc->foreColor);
        }

        dc->PopClipRect();
    }
}

const char *VisualizerUI::HandleEvent(const sysEvent_t *event, int time, bool *updateVisuals) {
    (void)time;
    if (desktop) {
        return desktop->HandleEvent(event, updateVisuals);
    }
    return "";
}

void VisualizerUI::StateChanged(int time, bool redraw) {
    time = time;
    if (desktop) {
        desktop->StateChanged(redraw);

        // Check for audio device change
        const char* device = desktop->GetStateString("audio_device", "default_output");
        if (strcmp(device, "WASAPI_LOOPBACK") == 0) {
            g_audioAnalyzer.SetSourceMode(AudioSourceMode::WASAPI_LOOPBACK);
        } else {
            g_audioAnalyzer.SetSourceMode(AudioSourceMode::ENGINE);
        }
    }
}

void VisualizerUI::UpdateUI() {
    const char* path = desktop->GetStateString("playlist_path", "");
    if (path[0] != '\0' && soundSystemLocal.playlistManager.GetCurrentTrack() != path) {
        soundSystemLocal.playlistManager.LoadPlaylist(path);
    }
}

void VisualizerUI::SetPreset(const char *presetName) {
    desktop->SetStateString("selected_preset", presetName);
    if (presetName && strstr(presetName, ".milk")) {
        LoadPreset(presetName);
    } else {
        m_active = false;
    }
}

void VisualizerUI::SetAudioDevice(const char *deviceName) {
    desktop->SetStateString("audio_device", deviceName);
}

void VisualizerUI::LoadPlaylist(const char *path) {
    desktop->SetStateString("playlist_path", path);
}

void VisualizerUI::LoadPreset(const char *path) {
    VisualizerPreset preset;
    if (MilkDropParser::Parse(path, preset)) {
        m_currentPreset = preset;
        m_active = true;
        common->Printf("Loaded MilkDrop preset: %s\n", m_currentPreset.name.c_str());
    } else {
        m_active = false;
        common->Warning("Failed to load MilkDrop preset: %s\n", path);
    }
}

void VisualizerUI::DrawButton(float x, float y, float w, float h, const char* text) {
    dc->DrawRect(x, y, w, h, desktop->borderColor);
    dc->DrawText(x + 5, y + 5, text, desktop->foreColor);
}
