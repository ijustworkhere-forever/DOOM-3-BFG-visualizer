#pragma hdrstop
#include "../idlib/precompiled.h"
#include "MilkEvaluator.h"
#include "visualizer_data.h"   // extern g_audioAnalyzer (pulls in audio_analyzer.h itself)

// gmegabuf and reg00..reg99 are documented as GLOBAL PERSISTENT state that
// survives preset switches (docs/research-milkdrop-projectm.md section 1),
// unlike q1..q32 which reset per preset load. So these two are shared by
// EVERY idMilkEvaluator instance (one static buffer for the process), while
// q1..q32 are registered fresh per instance/context below.
static projectm_eval_mem_buffer	s_milkGlobalMem = NULL;
static PRJM_EVAL_F					s_milkGlobalRegisters[100];
static bool							s_milkGlobalInit = false;

static void MilkEnsureGlobalState() {
	if ( s_milkGlobalInit ) {
		return;
	}
	s_milkGlobalInit = true;
	s_milkGlobalMem = projectm_eval_memory_buffer_create();
	memset( s_milkGlobalRegisters, 0, sizeof( s_milkGlobalRegisters ) );
}

idMilkEvaluator::idMilkEvaluator() :
	m_ctx( NULL ),
	m_globalMem( NULL ),
	m_varTime( NULL ), m_varFrame( NULL ), m_varFps( NULL ), m_varProgress( NULL ),
	m_varBass( NULL ), m_varMid( NULL ), m_varTreb( NULL ),
	m_varBassAtt( NULL ), m_varMidAtt( NULL ), m_varTrebAtt( NULL ),
	m_varMeshX( NULL ), m_varMeshY( NULL ), m_varPixelsX( NULL ), m_varPixelsY( NULL ),
	m_varAspectX( NULL ), m_varAspectY( NULL ),
	m_varX( NULL ), m_varY( NULL ), m_varRad( NULL ), m_varAng( NULL ),
	m_frameCount( 0.0 ) {
	memset( m_varQ, 0, sizeof( m_varQ ) );
}

idMilkEvaluator::~idMilkEvaluator() {
	Shutdown();
}

bool idMilkEvaluator::Init() {
	Shutdown();

	MilkEnsureGlobalState();
	m_globalMem = s_milkGlobalMem;

	m_ctx = projectm_eval_context_create( m_globalMem, &s_milkGlobalRegisters );
	if ( m_ctx == NULL ) {
		idLib::Warning( "idMilkEvaluator::Init: projectm_eval_context_create failed" );
		return false;
	}

	m_varTime     = projectm_eval_context_register_variable( m_ctx, "time" );
	m_varFrame    = projectm_eval_context_register_variable( m_ctx, "frame" );
	m_varFps      = projectm_eval_context_register_variable( m_ctx, "fps" );
	m_varProgress = projectm_eval_context_register_variable( m_ctx, "progress" );
	m_varBass     = projectm_eval_context_register_variable( m_ctx, "bass" );
	m_varMid      = projectm_eval_context_register_variable( m_ctx, "mid" );
	m_varTreb     = projectm_eval_context_register_variable( m_ctx, "treb" );
	m_varBassAtt  = projectm_eval_context_register_variable( m_ctx, "bass_att" );
	m_varMidAtt   = projectm_eval_context_register_variable( m_ctx, "mid_att" );
	m_varTrebAtt  = projectm_eval_context_register_variable( m_ctx, "treb_att" );
	m_varMeshX    = projectm_eval_context_register_variable( m_ctx, "meshx" );
	m_varMeshY    = projectm_eval_context_register_variable( m_ctx, "meshy" );
	m_varPixelsX  = projectm_eval_context_register_variable( m_ctx, "pixelsx" );
	m_varPixelsY  = projectm_eval_context_register_variable( m_ctx, "pixelsy" );
	m_varAspectX  = projectm_eval_context_register_variable( m_ctx, "aspectx" );
	m_varAspectY  = projectm_eval_context_register_variable( m_ctx, "aspecty" );
	m_varX        = projectm_eval_context_register_variable( m_ctx, "x" );
	m_varY        = projectm_eval_context_register_variable( m_ctx, "y" );
	m_varRad      = projectm_eval_context_register_variable( m_ctx, "rad" );
	m_varAng      = projectm_eval_context_register_variable( m_ctx, "ang" );

	for ( int i = 0; i < MILK_NUM_Q_VARS; i++ ) {
		m_varQ[i] = projectm_eval_context_register_variable( m_ctx, va( "q%d", i + 1 ) );
	}

	m_frameCount = 0.0;
	if ( m_varTime )     *m_varTime = 0;
	if ( m_varFrame )    *m_varFrame = 0;
	if ( m_varProgress ) *m_varProgress = 0;

	return true;
}

void idMilkEvaluator::Shutdown() {
	if ( m_ctx != NULL ) {
		projectm_eval_context_destroy( m_ctx );
		m_ctx = NULL;
	}
	// m_globalMem is the shared static buffer -- never destroyed here.
	m_globalMem = NULL;
}

struct projectm_eval_code * idMilkEvaluator::Compile( const char * code ) {
	if ( m_ctx == NULL || code == NULL || code[0] == '\0' ) {
		return NULL;
	}
	struct projectm_eval_code * handle = projectm_eval_code_compile( m_ctx, code );
	if ( handle == NULL ) {
		int line = 0, column = 0;
		const char * err = projectm_eval_get_error( m_ctx, &line, &column );
		idLib::Warning( "idMilkEvaluator::Compile: %s (line %d, col %d) in: %s",
			err != NULL ? err : "unknown error", line, column, code );
	}
	return handle;
}

void idMilkEvaluator::FreeCode( struct projectm_eval_code * code ) {
	if ( code != NULL ) {
		projectm_eval_code_destroy( code );
	}
}

PRJM_EVAL_F idMilkEvaluator::Execute( struct projectm_eval_code * code ) {
	if ( code == NULL ) {
		return 0;
	}
	return projectm_eval_code_execute( code );
}

void idMilkEvaluator::UpdateVariables( float dtSec, int meshW, int meshH, int pixelsW, int pixelsH, float progress ) {
	if ( m_ctx == NULL ) {
		return;
	}
	m_frameCount += 1.0;
	if ( m_varTime )     *m_varTime += dtSec;
	if ( m_varFrame )    *m_varFrame = m_frameCount;
	if ( m_varFps )      *m_varFps = ( dtSec > 0.0f ) ? ( 1.0f / dtSec ) : 0.0f;
	if ( m_varProgress ) *m_varProgress = idMath::ClampFloat( 0.0f, 1.0f, progress );
	if ( m_varBass )    *m_varBass = g_audioAnalyzer.GetBass();
	if ( m_varMid )     *m_varMid = g_audioAnalyzer.GetMid();
	if ( m_varTreb )    *m_varTreb = g_audioAnalyzer.GetTreb();
	if ( m_varBassAtt ) *m_varBassAtt = g_audioAnalyzer.GetBassAtt();
	if ( m_varMidAtt )  *m_varMidAtt = g_audioAnalyzer.GetMidAtt();
	if ( m_varTrebAtt ) *m_varTrebAtt = g_audioAnalyzer.GetTrebAtt();
	if ( m_varMeshX )   *m_varMeshX = meshW;
	if ( m_varMeshY )   *m_varMeshY = meshH;
	if ( m_varPixelsX ) *m_varPixelsX = pixelsW;
	if ( m_varPixelsY ) *m_varPixelsY = pixelsH;
	if ( m_varAspectX && pixelsH > 0 ) *m_varAspectX = ( pixelsH > pixelsW ) ? ( (float)pixelsH / pixelsW ) : 1.0f;
	if ( m_varAspectY && pixelsW > 0 ) *m_varAspectY = ( pixelsW > pixelsH ) ? ( (float)pixelsW / pixelsH ) : 1.0f;
}

void idMilkEvaluator::SetPixelVars( float x, float y, float rad, float ang ) {
	if ( m_varX )   *m_varX = x;
	if ( m_varY )   *m_varY = y;
	if ( m_varRad ) *m_varRad = rad;
	if ( m_varAng ) *m_varAng = ang;
}

PRJM_EVAL_F idMilkEvaluator::GetQ( int index ) const {
	if ( index < 0 || index >= MILK_NUM_Q_VARS || m_varQ[index] == NULL ) {
		return 0;
	}
	return *m_varQ[index];
}

void idMilkEvaluator::SetQ( int index, PRJM_EVAL_F value ) {
	if ( index < 0 || index >= MILK_NUM_Q_VARS || m_varQ[index] == NULL ) {
		return;
	}
	*m_varQ[index] = value;
}

PRJM_EVAL_F * idMilkEvaluator::GetVariable( const char * name ) {
	if ( m_ctx == NULL || name == NULL || name[0] == '\0' ) {
		return NULL;
	}
	// same find-or-create path the compiler itself uses (TreeVariables.c),
	// so this returns the compiled code's own variable, not a fresh one.
	return projectm_eval_context_register_variable( m_ctx, name );
}
