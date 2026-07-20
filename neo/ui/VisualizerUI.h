#ifndef __VISUALIZER_UI_H__
#define __VISUALIZER_UI_H__

#include "UserInterfaceLocal.h"
#include "Window.h"
#include "../sound/visualizer_data.h"
#include "VisualizerPreset.h"

class VisualizerUI : public idUserInterfaceLocal {
public:
    VisualizerUI();
    virtual ~VisualizerUI();

    // Standard overrides
    virtual const char *Name() const override;
    virtual const char *Comment() const override;
    virtual bool IsInteractive() const override;
    virtual bool IsVisualizer() const override { return true; }
    virtual void Redraw(int time, bool hud) override;
    virtual const char *HandleEvent(const sysEvent_t *event, int time, bool *updateVisuals) override;
    virtual void StateChanged(int time, bool redraw) override;

    // Visualizer specific UI
    void UpdateUI();
    void SetPreset(const char *presetName);
    void SetAudioDevice(const char *deviceName);
    void LoadPlaylist(const char *path);
    void LoadPreset(const char *path);

    const VisualizerPreset& GetPreset() const { return m_currentPreset; }
    bool IsActive() const { return m_active; }

private:
    idWinVar* m_presetVar;
    idWinVar* m_deviceVar;
    idWinVar* m_playlistVar;
    VisualizerPreset m_currentPreset;
    bool m_active;
};

#endif // __VISUALIZER_UI_H__
