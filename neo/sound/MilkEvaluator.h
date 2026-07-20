#ifndef __MILK_EVALUATOR_H__
#define __MILK_EVALUATOR_H__

#include "../idlib/precompiled.h"
#include "../framework/projectm-eval/api/projectm-eval.h"

/*
================================================
idMilkEvaluator

Owns ONE vendored projectm-eval context (PRD M1 step 3) and the standard set
of MilkDrop host variables (bass/mid/treb/*_att/time/frame/fps/q1..q32/...)
bound as shared double*'s, per docs/research-milkdrop-projectm.md sections 1
and 5. A context's q1..q32 are the bridge between per_frame_/per_pixel_ code:
since projectm_eval_context_register_variable returns a pointer the SAME
memory the compiled code reads/writes, per_frame_ and per_pixel_ code
compiled against the same context automatically see each other's q-values
with no extra copying -- this class only has to update the AUDIO/time
variables once a frame; the q's take care of themselves.

One idMilkEvaluator = one active preset's persistent variable state. Created
fresh on preset load (fresh q-values), destroyed on preset unload. Runs on
the main thread only (matches the existing modulation-matrix threading model
in visualizer_manager.cpp), so the lock callbacks in HostLocks.c stay no-ops.

Not yet wired to the warp-mesh/render pipeline (M1 steps 4-5) or to preset
loading (that's the next integration step) -- this class alone is
responsible for the eval context and variable plumbing, testable in
isolation via vis_milkEval (see visualizer_manager.cpp).
================================================
*/

#define MILK_NUM_Q_VARS 64   // PRD M5: MilkDrop3-level q1..q64 (was q1..q32/MilkDrop2-level).
                              // Purely a registration-count bump -- q1..q32 stay byte-identical
                              // for MilkDrop2/projectM presets (100% backward compatible, matching
                              // MD3's own documented compatibility guarantee); q33..q64 are simply
                              // now also registered for presets that use them.

class idMilkEvaluator {
public:
	idMilkEvaluator();
	~idMilkEvaluator();

	// creates the eval context and registers every host variable. Returns
	// false (and leaves the evaluator unusable) only on allocation failure.
	bool			Init();
	void			Shutdown();
	bool			IsInitialized() const { return m_ctx != NULL; }

	// compiles one EEL2 source string (a concatenated per_frame_/per_pixel_/
	// per_frame_init_ fragment from idMilkPreset). Returns NULL and logs the
	// parser's own error location on failure -- callers should tolerate NULL
	// (skip that fragment) rather than treat it as fatal (NFR-3).
	struct projectm_eval_code * Compile( const char * code );
	void			FreeCode( struct projectm_eval_code * code );
	PRJM_EVAL_F		Execute( struct projectm_eval_code * code );

	// pushes this frame's audio/time values into the bound variables. Call
	// once per frame on the main thread BEFORE executing per_frame_/
	// per_pixel_ code for this preset.
	// progress: caller-computed 0..1 (real MilkDrop's "progress" is fraction
	// of the way to the next scheduled preset switch, NOT audio playback
	// position -- this engine has no track-seek/position API at all, so
	// computing it here would either be a no-op or fabricate data; the
	// caller already tracks preset-cycle timing and can pass a real value.
	// Defaults to 0 for callers that don't care (e.g. the isolated test
	// command, vis_milkEval).
	void			UpdateVariables( float dtSec, int meshW, int meshH, int pixelsW, int pixelsH, float progress = 0.0f );

	// per_pixel_ evaluation additionally needs x/y/rad/ang set per vertex;
	// exposed separately since they change every warp-mesh vertex, not once
	// a frame like the rest of UpdateVariables().
	void			SetPixelVars( float x, float y, float rad, float ang );

	// q1..q32 raw access, e.g. for debug/preset-cycle reset.
	PRJM_EVAL_F		GetQ( int index ) const;   // index 0..MILK_NUM_Q_VARS-1
	void			SetQ( int index, PRJM_EVAL_F value );

	// Generic named-variable lookup, for MilkDrop's per_pixel_/per_frame_
	// OUTPUT variables (zoom, rot, warp, dx, dy, sx, sy, cx, cy, ...) that
	// the PRESET's compiled code assigns to rather than the host. Unlike the
	// fixed set above, these don't need pre-registration: the vendored
	// compiler auto-creates a context variable the first time compiled code
	// references it (confirmed in TreeVariables.c's find-or-create pattern),
	// so calling this AFTER a successful Compile() returns a pointer to
	// whatever the preset just wrote -- or NULL if that preset never
	// touched the variable (caller should fall back to its own default in
	// that case, e.g. leave the built-in warp math for an unset "warp").
	PRJM_EVAL_F *	GetVariable( const char * name );

private:
	struct projectm_eval_context *	m_ctx;
	projectm_eval_mem_buffer		m_globalMem;

	PRJM_EVAL_F *	m_varTime;
	PRJM_EVAL_F *	m_varFrame;
	PRJM_EVAL_F *	m_varFps;
	PRJM_EVAL_F *	m_varProgress;
	PRJM_EVAL_F *	m_varBass;
	PRJM_EVAL_F *	m_varMid;
	PRJM_EVAL_F *	m_varTreb;
	PRJM_EVAL_F *	m_varBassAtt;
	PRJM_EVAL_F *	m_varMidAtt;
	PRJM_EVAL_F *	m_varTrebAtt;
	PRJM_EVAL_F *	m_varMeshX;
	PRJM_EVAL_F *	m_varMeshY;
	PRJM_EVAL_F *	m_varPixelsX;
	PRJM_EVAL_F *	m_varPixelsY;
	PRJM_EVAL_F *	m_varAspectX;
	PRJM_EVAL_F *	m_varAspectY;
	PRJM_EVAL_F *	m_varX;
	PRJM_EVAL_F *	m_varY;
	PRJM_EVAL_F *	m_varRad;
	PRJM_EVAL_F *	m_varAng;
	PRJM_EVAL_F *	m_varQ[MILK_NUM_Q_VARS];

	double			m_frameCount;
};

#endif // __MILK_EVALUATOR_H__
