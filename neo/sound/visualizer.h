#ifndef __VISUALIZER_H__
#define __VISUALIZER_H__

#include "audio_analyzer.h"
#include <vector>

class Visualizer {
public:
    Visualizer();
    ~Visualizer();

    // Update the visual state based on audio data
    void Update();

    // Get the height of a specific bar for rendering
    float GetBarHeight(int barIndex) const;

private:
    // Array of bar heights (e.g., 64 bars)
    float m_barHeights[64];

    // Smoothing factor for the bars
    const float m_smoothing = 0.15f;
};

#endif // __VISUALIZER_H__
