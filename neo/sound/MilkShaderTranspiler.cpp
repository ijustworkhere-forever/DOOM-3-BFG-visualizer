#include "../idlib/precompiled.h"
#pragma hdrstop
#include "MilkShaderTranspiler.h"

#include "../external/hlsl2glslfork/include/hlsl2glsl.h"

// PRD M3: the implicit MilkDrop global declarations real warp_/comp_ shader
// bodies reference but never declare themselves -- see MilkShaderTranspiler.h
// for how this was verified (built + ran hlsl2glslfork on this exact
// preamble against a real preset, on this dev machine, and diffed the
// output against Butterchurn's own independently-converted reference).
static const char * kMilkPreamble =
	// --- source-level compatibility shims (expanded by hlsl2glslfork's
	// mojoshader/hlmojo preprocessor before the HLSL grammar sees a token;
	// verified in-pipeline: yylex pulls tokens via hlmojo_preprocessor_nexttoken
	// in Gen_hlslang.cpp, so object-like #defines expand here exactly as they
	// would in real MilkDrop's own D3DX shader prefix) ---
	//
	// double{,2,3,4}: this era's HLSL/Cg (and this fork's flex lexer) has no
	// "double" vector type -- it only knows float{2,3,4}/half{2,3,4}. A real
	// preset line like `double3 dist = ...;` therefore lexed `double3` as a
	// bare (undeclared) identifier and then `dist` as a second identifier with
	// no operator between them -> the exact "'double3' : undeclared identifier"
	// + "'<name>' : syntax error" pair seen on one line in the wild. Real
	// MilkDrop tolerates such presets because D3DX treats double as float
	// precision on DX9 pixel shaders; alias them to float so those presets
	// compile instead of dropping to the grayscale CPU-mesh fallback.
	"#define double float\n"
	"#define double2 float2\n"
	"#define double3 float3\n"
	"#define double4 float4\n"
	// lowercase texture intrinsics: real MilkDrop's shader prefix defines these
	// (HLSL/this fork are case-sensitive, so a preset's `tex2d`/`tex3d` would
	// otherwise be undeclared). Map them to the real declared intrinsics.
	"#define tex2d tex2D\n"
	"#define tex3d tex3D\n"
	"#define tex2dlod tex2Dlod\n"
	"#define tex3dlod tex3Dlod\n"
	"#define tex2dbias tex2Dbias\n"
	"#define tex2dproj tex2Dproj\n"
	"uniform sampler2D sampler_main;\n"
	"uniform sampler2D sampler_fw_main;\n"
	"uniform sampler2D sampler_pw_main;\n"
	// sampler_fc_main/sampler_pc_main: the filtered-clamp / point-clamp
	// variants of the main (previous-frame feedback) texture -- MilkDrop's
	// f/p (filtered/point) x w/c (wrap/clamp) main-sampler matrix. GLSL 1.10
	// can't express per-sampler filter/wrap state, so like sampler_fw/pw_main
	// above these are the SAME main texture; the render side binds all of them
	// to the one main image (RB_DrawMilkFullscreenPass).
	"uniform sampler2D sampler_fc_main;\n"
	"uniform sampler2D sampler_pc_main;\n"
	// sampler_noisevol_lq/hq: MilkDrop's 3D noise volume textures. Declared as
	// real sampler3D so presets that `tex3D(sampler_noisevol_hq, uvw)` compile
	// to valid GLSL (texture3D). KNOWN SIMPLIFICATION: this engine has no 3D
	// texture pipeline (only TT_2D/TT_CUBIC), so no real noise volume is bound
	// -- the sampler reads GL's default (incomplete) 3D texture and returns a
	// constant. The preset renders in its real palette (driven by sampler_main
	// etc.) instead of the grayscale fallback; only the noise-volume term is
	// approximated. See RB_BindMilkPlaceholderSamplers.
	"uniform sampler3D sampler_noisevol_lq;\n"
	"uniform sampler3D sampler_noisevol_hq;\n"
	"uniform sampler2D sampler_blur1;\n"
	"uniform sampler2D sampler_blur2;\n"
	"uniform sampler2D sampler_blur3;\n"
	"uniform sampler2D sampler_noise_lq;\n"
	"uniform sampler2D sampler_noise_mq;\n"
	"uniform sampler2D sampler_noise_hq;\n"
	"uniform float4 texsize;\n"
	"uniform float4 texsize_noise_lq;\n"
	"uniform float4 texsize_noise_mq;\n"
	"uniform float4 texsize_noise_hq;\n"
	// texel size (xy = size, zw = 1/size, matching every other texsize_* here)
	// of the 3D noise volumes above -- float4 to match this preamble's uniform
	// convention and cover the .xy/.xyz swizzles presets use on it.
	"uniform float4 texsize_noisevol_lq;\n"
	"uniform float4 texsize_noisevol_hq;\n"
	"uniform float4 texsize_blur1;\n"
	"uniform float4 texsize_blur2;\n"
	"uniform float4 texsize_blur3;\n"
	"uniform float2 aspect;\n"
	"uniform float time;\n"
	"uniform float frame;\n"
	"uniform float fps;\n"
	"uniform float progress;\n"
	"uniform float bass, mid, treb, bass_att, mid_att, treb_att;\n"
	// vol/vol_att: MilkDrop's instantaneous / attenuated overall-volume audio
	// scalars. Wired on the CPU side (visualizer_manager.cpp) to the mean of
	// bass/mid/treb (and their attenuated variants) from g_audioAnalyzer --
	// see visMilkWarpUniforms_t.vol/volAtt.
	"uniform float vol, vol_att;\n"
	// roam_cos/roam_sin: MilkDrop's four slowly-drifting per-frame "roaming"
	// pseudo-random values, packed as float4 (presets swizzle .x/.y/.zww/.zxy,
	// which need up to 4 components -- declaring these too narrow is what
	// produced the "vector field selection out of range" errors). Computed once
	// per frame on the CPU from slow cos/sin of time.
	"uniform float4 roam_cos;\n"
	"uniform float4 roam_sin;\n"
	"uniform float rand_frame;\n"
	"uniform float rand_preset;\n"
	"uniform float q1,q2,q3,q4,q5,q6,q7,q8,q9,q10,q11,q12,q13,q14,q15,q16;\n"
	"uniform float q17,q18,q19,q20,q21,q22,q23,q24,q25,q26,q27,q28,q29,q30,q31,q32;\n"
	"uniform float2x2 _qa,_qb,_qc,_qd,_qe,_qf,_qg,_qh;\n"
	// PRD M5: not a real MilkDrop uniform -- the engine's own compositing
	// knob for mash-up ("B" side) blending. warp_/comp_ output is opaque by
	// default (1.0); the mash-up B draw sets this to the mix amount instead
	// so standard alpha blending can composite two arbitrary custom shaders
	// the same way the CPU-mesh path already blends two identical meshes
	// (draw A opaque, draw B translucent on top).
	"uniform float milkOutputAlpha;\n"
	// lum(): MilkDrop's luminance helper (many presets call it as a builtin but
	// never define it -> "'lum' : no matching overloaded function found").
	// Real value: dot with MilkDrop's own luma weights (0.32,0.49,0.29 -- the
	// same weights MilkDrop2's blur/luma code uses). Both a float3 and a float4
	// overload so `lum(rgb)` and `lum(tex2D(...))` (a float4) both resolve.
	"float lum(float3 c) { return dot(c, float3(0.32, 0.49, 0.29)); }\n"
	"float lum(float4 c) { return dot(c.xyz, float3(0.32, 0.49, 0.29)); }\n"
	"float3 GetPixel(float2 uv) { return tex2D(sampler_main, uv).xyz; }\n"
	"float3 GetBlur1(float2 uv) { return tex2D(sampler_blur1, uv).xyz; }\n"
	"float3 GetBlur2(float2 uv) { return tex2D(sampler_blur2, uv).xyz; }\n"
	"float3 GetBlur3(float2 uv) { return tex2D(sampler_blur3, uv).xyz; }\n";

// PRD M3, confirmed real bug: certain real preset shader constructs (verified
// minimal repro: `clamp(tan(v.yx), -12, 12)` -- a vector clamped against
// bare scalar bounds, extremely common in real MilkDrop presets) make the
// vendored hlsl2glslfork's parser (Hlsl2Glsl_Parse, ultimately its
// bison-generated yyparse driver in Gen_hlslang_tab.cpp) block forever.
// Confirmed on the Windows build via direct process inspection: CPU time
// stayed flat (not a busy spin loop -- a real wait/block), the process had
// no window yet and Get-Process still reported it as "responding" (so it's
// not the whole process wedged, just whatever thread is running this call).
// Adding the missing scalar-broadcast overloads for min/max/clamp to
// Initialize.cpp (a very plausible-looking cause, since only same-vector-type
// overloads were declared) did NOT fix it, and tracing findFunction ->
// promoteFunctionArguments -> constructBuiltIn/ir_add_unary_math ->
// TIntermUnary::promote by hand found no loop in any of that -- every path
// there degrades to a clean parse error. The actual hang is somewhere deeper
// in the 6000+ line generated bison parser or its custom lexer hooks
// (hlmojo preprocessor / PaParseComment) that wasn't isolated despite
// substantial bisection. Given hlsl2glslfork is unmaintained upstream and
// this is a real, triggerable-by-real-content bug, the correct fix here is
// containment, not a hunt for the exact loop: run the parse/translate on an
// isolated worker thread with a hard timeout so a hang in this vendored
// library can never freeze the whole game (matches this function's own
// documented contract in MilkShaderTranspiler.h -- callers already tolerate
// failure and skip the shader, NFR-3). Once a timeout is observed, all
// hlsl2glslfork global state (GlobalPoolAllocator/SymbolTables, its TLS
// init flag) is left in an indeterminate, possibly partially-mutated state
// from the abandoned call -- reusing it for a later call risks a crash or a
// second hang rather than a clean failure, so the transpiler is permanently
// disabled for the rest of the process's lifetime after the first timeout.
namespace {
	const int MILK_TRANSPILE_TIMEOUT_MS = 4000;

	struct milkTranspileJob_t {
		idStr		shaderBody;
		idStr		entryName;
		idStr		outGLSL;
		idStr		outError;
		bool		isVertex;	// false = fragment (Transpile), true = TranspileVertexPassthrough
		bool		success;
	};

	// PRD M3: the fixed full-screen-quad vertex passthrough HLSL -- see
	// TranspileVertexPassthrough's doc comment in MilkShaderTranspiler.h for
	// why this goes through the same translator instead of being hand-
	// written GLSL. Position passes through untouched (already NDC, see
	// backEnd.unitSquareSurface); uv is the same TEXCOORD0 the fragment
	// shader's wrapper expects.
	static const char * kMilkVertexPassthrough =
		"struct VS_OUTPUT { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
		"VS_OUTPUT milkVertexMain( float4 inPos : POSITION, float2 inUv : TEXCOORD0 ) {\n"
		"VS_OUTPUT o;\n"
		"o.pos = inPos;\n"
		"o.uv = inUv;\n"
		"return o;\n"
		"}\n";

	bool RunTranspileJob( milkTranspileJob_t & job ) {
		static bool s_initialized = false;
		if ( !s_initialized ) {
			if ( !Hlsl2Glsl_Initialize() ) {
				job.outError = "Hlsl2Glsl_Initialize failed";
				return false;
			}
			s_initialized = true;
		}

		ShHandle compiler = Hlsl2Glsl_ConstructCompiler( job.isVertex ? EShLangVertex : EShLangFragment );
		if ( compiler == NULL ) {
			job.outError = "Hlsl2Glsl_ConstructCompiler failed";
			return false;
		}

		idStr full;
		if ( job.isVertex ) {
			full = kMilkVertexPassthrough;
		} else {
			// Fixed real bug: the entry function's uv/ang/rad/uv_orig params
			// originally had NO HLSL semantics at all -- confirmed by
			// actually inspecting the transpiled GLSL output (not just
			// checking it parsed without error, which this bug slipped
			// past): hlsl2glslfork's linker, unable to match any of them to
			// a real input binding, silently fell back to declaring ONE
			// dummy varying and casting it to all four parameter slots
			// (`milk_warp(vec2(xlv_), float(xlv_), float(xlv_), vec2(xlv_))`)
			// -- syntactically valid GLSL, but semantically nonsense (ang/
			// rad were never a real angle/radius, and uv/uv_orig were
			// identical low-entropy garbage). Only `uv` needs a genuine
			// per-pixel input -- give it the one semantic this linker
			// actually recognizes for a texcoord-like varying (`TEXCOORD0`);
			// `ang`/`rad`/`uv_orig` are then computed as locals from that
			// single real input, matching real MilkDrop semantics (they're
			// always derived from position, never independently supplied)
			// -- and matching this shader's own full-screen-quad draw path
			// (RB_VisMilkWarpDraw), which has no separate per-vertex CPU
			// pre-warp step, so uv_orig (the pre-warp coordinate) is simply
			// this pixel's own uv.
			full = kMilkPreamble;
			full += "float4 ";
			full += job.entryName;
			full += "( float2 uv : TEXCOORD0 ) : COLOR0 {\n";
			full += "float2 uv_orig = uv;\n";
			full += "float2 _milkRel = uv - float2(0.5, 0.5);\n";
			full += "float rad = length(_milkRel);\n";
			full += "float ang = atan2(_milkRel.y, _milkRel.x);\n";
			full += "float3 ret = float3(0.0,0.0,0.0);\n";
			full += job.shaderBody;
			full += "\nreturn float4(ret, milkOutputAlpha);\n}\n";
		}

		const int parseOk = Hlsl2Glsl_Parse( compiler, full.c_str(), ETargetGLSL_110, NULL, ETranslateOpNone );
		if ( !parseOk ) {
			const char * log = Hlsl2Glsl_GetInfoLog( compiler );
			job.outError = ( log != NULL ) ? log : "(no parse error log)";
			Hlsl2Glsl_DestructCompiler( compiler );
			return false;
		}

		const char * entryName = job.isVertex ? "milkVertexMain" : job.entryName.c_str();
		const int translateOk = Hlsl2Glsl_Translate( compiler, entryName, ETargetGLSL_110, ETranslateOpNone );
		if ( !translateOk ) {
			const char * log = Hlsl2Glsl_GetInfoLog( compiler );
			job.outError = ( log != NULL ) ? log : "(no translate error log)";
			Hlsl2Glsl_DestructCompiler( compiler );
			return false;
		}

		const char * glsl = Hlsl2Glsl_GetShader( compiler );
		job.outGLSL = ( glsl != NULL ) ? glsl : "";
		Hlsl2Glsl_DestructCompiler( compiler );
		return true;
	}

	class idMilkTranspileThread : public idSysThread {
	public:
		milkTranspileJob_t *	job;
		idSysSignal				finished;

		idMilkTranspileThread() : job( NULL ), finished( true ) {}

		virtual int Run() {
			job->success = RunTranspileJob( *job );
			finished.Raise();
			return 0;
		}
	};

	// shared timeout-guarded execution for both public entry points below --
	// see the block comment above milkTranspileJob_t for why this exists.
	bool RunGuarded( milkTranspileJob_t * job, idStr & outGLSL, idStr & outError ) {
		static bool s_permanentlyDisabled = false;
		if ( s_permanentlyDisabled ) {
			outError = "MilkDrop shader transpiler disabled for this session (a previous shader hung hlsl2glslfork's parser)";
			delete job;
			return false;
		}

		idMilkTranspileThread * thread = new idMilkTranspileThread();
		thread->job = job;
		thread->StartThread( "milkShaderTranspile", CORE_ANY );

		const bool finishedInTime = thread->finished.Wait( MILK_TRANSPILE_TIMEOUT_MS );

		if ( !finishedInTime ) {
			s_permanentlyDisabled = true;
			outError = "MilkDrop shader transpile timed out (hlsl2glslfork parser hang) -- transpiler disabled for the rest of this session";
			idLib::Warning( "idMilkShaderTranspiler: %s", outError.c_str() );
			// deliberately leaked: 'thread' and 'job' -- the worker may
			// still be blocked inside hlsl2glslfork indefinitely; deleting
			// either out from under it (or force-terminating the OS thread
			// mid-allocation) risks corrupting the CRT heap or
			// hlsl2glslfork's own static pool allocator far worse than
			// leaking one idle thread and ~1KB.
			return false;
		}

		outGLSL = job->outGLSL;
		outError = job->outError;
		const bool success = job->success;

		thread->StopThread( true );
		delete thread;
		delete job;

		return success;
	}
}

bool idMilkShaderTranspiler::Transpile( const char * shaderBody, const char * entryName, idStr & outGLSL, idStr & outError ) {
	if ( shaderBody == NULL || shaderBody[0] == '\0' ) {
		outError = "empty shader body";
		return false;
	}

	// heap-allocated, intentionally leaked on timeout -- see RunGuarded.
	milkTranspileJob_t * job = new milkTranspileJob_t();
	job->shaderBody = shaderBody;
	job->entryName = entryName;
	job->isVertex = false;
	job->success = false;

	return RunGuarded( job, outGLSL, outError );
}

bool idMilkShaderTranspiler::TranspileVertexPassthrough( idStr & outGLSL, idStr & outError ) {
	milkTranspileJob_t * job = new milkTranspileJob_t();
	job->isVertex = true;
	job->success = false;

	return RunGuarded( job, outGLSL, outError );
}
