#ifndef __VISUALIZER_PRESET_H__
#define __VISUALIZER_PRESET_H__

#include "../idlib/Str.h"
#include <vector>
#include <string>
#include <map>

struct VisualizerPreset {
    idStr name;
    std::string vertexShader;
    std::string fragmentShader;
    std::map<int, float> parameters;
};

#endif // __VISUALIZER_PRESET_H__
