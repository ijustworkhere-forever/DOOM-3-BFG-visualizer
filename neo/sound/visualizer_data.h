#ifndef __VISUALIZER_DATA_H__
#define __VISUALIZER_DATA_H__

#include "audio_analyzer.h"

// The analyzer is shared by the sound system and renderer translation units.

// Global analyzer instance for the visualizer
extern AudioAnalyzer g_audioAnalyzer;

#endif // __VISUALIZER_DATA_H__
