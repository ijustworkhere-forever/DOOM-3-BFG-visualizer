#ifndef __MILK_PRESET_H__
#define __MILK_PRESET_H__

#include "../idlib/precompiled.h"

/*
================================================
idMilkPreset

Parses a MilkDrop / projectM .milk preset into its component parts, per
PRD Pillar A / plans/PRD-implementation-status.md M1. A .milk file is INI-ish
text under a single "[presetNN]" header: scalar key=value params, plus
several families of NUMBERED keys whose fragments must be concatenated in
NUMERIC order (not necessarily file order) to reconstruct one EEL2
expression or shader source per family -- confirmed against real fixtures
from projectM's presets/tests corpus (github.com/projectM-visualizer/projectm),
NOT just the paraphrased research notes, because the wavecode/shapecode key
format they describe as bracketed "[wavecode_NN]" sections turned out to be
wrong: real files use FLAT keys with the index embedded in the key name
(wavecode_0_r=, wave_0_per_point1=, ...).

Concatenation uses "\n" between fragments (verified against the vendored
projectm-eval lexer: Scanner.l treats \n as insignificant whitespace outside
a comment, and "//" comments run until the next \n -- so joining with "\n"
is required to keep a comment-only fragment from swallowing the next one,
while still being harmless for a mid-expression continuation).

This parser only extracts text; it does not compile or evaluate anything
(that's the eval-binding step, projectm-eval) and does not touch rendering.
warp_/comp_ shader bodies are captured (backtick-prefix stripped, per real
files) for the future HLSL transpiler (PRD FR-A4 / M3) but unused until then.
================================================
*/

struct milkWaveCode_t {
	int				index;
	idDict			params;		// wavecode_<index>_<param> (prefix stripped)
	idStr			perFrame;	// wave_<index>_per_frame* fragments, joined
	idStr			perPoint;	// wave_<index>_per_point* fragments, joined
};

struct milkShapeCode_t {
	int				index;
	idDict			params;		// shapecode_<index>_<param> (prefix stripped)
	idStr			perFrame;	// shape_<index>_per_frame* fragments, joined
};

class idMilkPreset {
public:
	idMilkPreset();

	void			Clear();

	// vfsPath is relative to the search paths, e.g. "presets/milk/foo.milk".
	// Malformed files are tolerated (NFR-3): unrecognized lines are skipped,
	// never fatal. Returns false only if the file couldn't be read at all.
	bool			Load( const char * vfsPath );

	const idStr &	GetName() const { return m_name; }

	// top-level scalar params: fDecay, zoom, rot, warp, fWarpScale,
	// fZoomExponent, ob_*/ib_* borders, wave_r/g/b/a/x/y, sx/sy/cx/cy/dx/dy,
	// nWaveMode, bMaximizeWaveColor, and the rest of the ~60 MilkDrop builtins.
	const idDict &	GetParams() const { return m_params; }

	const idStr &	GetPerFrameInit() const { return m_perFrameInit; }
	const idStr &	GetPerFrame() const { return m_perFrame; }
	const idStr &	GetPerPixel() const { return m_perPixel; }

	// raw HLSL text (ps_2_0/ps_3_0), unused until the M3 transpiler exists.
	const idStr &	GetWarpShader() const { return m_warpShader; }
	const idStr &	GetCompShader() const { return m_compShader; }

	int				NumWaves() const { return m_waves.Num(); }
	const milkWaveCode_t & GetWave( int i ) const { return m_waves[i]; }

	int				NumShapes() const { return m_shapes.Num(); }
	const milkShapeCode_t & GetShape( int i ) const { return m_shapes[i]; }

	// debug/console summary (vis_milkLoad): counts + a short preview of the
	// concatenated per_frame/per_pixel text, so parsing can be verified
	// against real preset files before any eval/rendering integration exists.
	void			PrintSummary() const;

private:
	idStr					m_name;
	idDict					m_params;
	idStr					m_perFrameInit;
	idStr					m_perFrame;
	idStr					m_perPixel;
	idStr					m_warpShader;
	idStr					m_compShader;
	idList<milkWaveCode_t>	m_waves;
	idList<milkShapeCode_t> m_shapes;
};

#endif // __MILK_PRESET_H__
