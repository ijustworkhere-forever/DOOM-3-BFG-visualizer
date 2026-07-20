#ifndef __MILK_SHADER_TRANSPILER_H__
#define __MILK_SHADER_TRANSPILER_H__

#include "../idlib/precompiled.h"

/*
================================================
PRD M3: MilkDrop `warp_`/`comp_` HLSL -> GLSL transpiler.

Built on the vendored `hlsl2glslfork` (neo/external/hlsl2glslfork/, BSD-style
license) -- the real, tested HLSL compiler (shipped in Unity for years) that
Butterchurn's own MilkDrop shader converter (jberg/milkdrop-shader-converter)
wraps for exactly this job, not a hand-rolled parser or naive text
substitution.

The preamble/wrapping convention below (implicit MilkDrop globals --
`uv`/`ang`/`rad`/`time`/`aspect`/`texsize*`/`q1..q32`/`_qa.._qh`/samplers/
`GetPixel`/`GetBlur1-3`, the `ret` output variable) was reverse-engineered
against and EMPIRICALLY VERIFIED on a real preset's actual warp_/comp_
shader body (butterchurn-presets' `presets/milkdrop/11.milk`), by actually
building this exact library + preamble on this dev machine (hlsl2glslfork
already has Mac support) and confirming the output is semantically
identical to Butterchurn's own independently-produced reference conversion
(`presets/converted/11.json`) -- not just reasoned about from documentation.
See plans/PRD-implementation-status.md M3 for the full verification trail.

Known, deliberate scope limits, not silently glossed over:
  - `texsize` is `float4` (xy = size, zw = 1/size) -- confirmed by this
    verification catching and fixing an initial wrong guess (float2).
  - Only the FIXED, well-documented uniform/sampler set is declared
    (sampler_main/noise_lq/mq/hq/blur1-3, their texsize variants, q1..q32,
    _qa.._qh, the standard scalar uniforms). Per-preset custom-named
    textures (`sampler_<filename>`, with fw_/fc_/pw_/pc_ filter/wrap
    prefixes) are NOT supported -- a real, open scope gap for presets that
    reference their own custom images in shader code.
  - No glsl-optimizer pass (Butterchurn's own pipeline runs one after
    hlsl2glslfork) -- output is correct but unoptimized (more verbose
    intermediate variables, no dead-code elimination). A real, deliberate
    simplification: adding glsl-optimizer would roughly double the vendored
    code for a polish improvement, not a correctness one.
  - Transpile() itself only handles fragment shaders (warp_/comp_ are both
    pixel shaders in MilkDrop). PRD M3's live-render wiring (RB_VisMilkWarpDraw,
    neo/renderer/tr_backend_draw.cpp) still needs a matching VERTEX shader to
    pair with the transpiled fragment shader in one linked GL program --
    TranspileVertexPassthrough() below covers that: a single, fixed (no
    preset-specific content) full-screen-quad passthrough, transpiled once
    and cached, not per-preset.
================================================
*/
class idMilkShaderTranspiler {
public:
	// shaderBody: the preset's warp_/comp_ text with the "shader_body"
	// keyword and its outer { } already stripped (idMilkPreset::
	// GetWarpShader()/GetCompShader() return the raw concatenated text
	// including "shader_body { ... }" -- callers strip that wrapper before
	// calling this, see VisStripShaderBody in visualizer_manager.cpp).
	// entryName: an arbitrary valid identifier for the generated entry
	// function (e.g. "milk_warp"); only needs to be unique within one
	// Hlsl2Glsl_ConstructCompiler instance's lifetime.
	// Returns true with outGLSL filled in on success; false (with outError
	// filled in with the compiler's own parse/translate log) on failure --
	// callers should tolerate failure and skip the shader (NFR-3), same as
	// every other preset-parsing path in this project.
	static bool Transpile( const char * shaderBody, const char * entryName, idStr & outGLSL, idStr & outError );

	// PRD M3: a fixed (never preset-specific) full-screen-quad vertex
	// passthrough, transpiled through the SAME hlsl2glslfork pipeline as
	// Transpile() above -- deliberately NOT hand-written raw GLSL, because
	// the varying name hlsl2glslfork gives a TEXCOORD0-semantic fragment
	// input (confirmed empirically: "xlv_TEXCOORD0", not any documented/
	// stable public convention) has to match this vertex shader's own
	// TEXCOORD0-semantic OUTPUT name exactly for the two stages to link --
	// running both through the same translator is what guarantees that,
	// rather than hardcoding a name that could silently drift if this
	// vendored fork's internal naming ever changes. position is passed
	// through as pure NDC (matches backEnd.unitSquareSurface's own
	// clip-space vertices, see RB_VisFboEnd's identity-MVP comment in
	// tr_backend_draw.cpp -- this vertex shader has no MVP to apply at all).
	static bool TranspileVertexPassthrough( idStr & outGLSL, idStr & outError );
};

#endif // __MILK_SHADER_TRANSPILER_H__
