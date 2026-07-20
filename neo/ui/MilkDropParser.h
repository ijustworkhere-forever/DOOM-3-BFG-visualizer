#ifndef __MILKDROP_PARSER_H__
#define __MILKDROP_PARSER_H__

#include "VisualizerPreset.h"

/*
================================================
MilkDropParser

Parses this project's custom preset format (NOT the real MilkDrop .milk format —
see docs/research-milkdrop-projectm.md for the difference):

    [preset]
    name: My Preset
    [effect]
    vertex_shader:            (inline GLSL block terminated by a line reading END,
    ...                        or a path value on the same line)
    END
    fragment_shader: path.vfp
    [param]
    param 400: 0.5            (indices 400+ are audio-bound by convention)

Files are read through idFileSystem, so presets ship under base/ (loose or in
.resources containers).
================================================
*/
class MilkDropParser {
public:
    static bool Parse( const char * filePath, VisualizerPreset & outPreset );

private:
    static bool ParseText( const char * text, int length, VisualizerPreset & outPreset );
};

#endif // __MILKDROP_PARSER_H__
