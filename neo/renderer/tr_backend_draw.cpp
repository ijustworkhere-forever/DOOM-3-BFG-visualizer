/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company. 

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").  

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 BFG Edition Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 BFG Edition Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 BFG Edition Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 BFG Edition Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

#pragma hdrstop
#include "../idlib/precompiled.h"

#include "tr_local.h"

idCVar r_drawEyeColor( "r_drawEyeColor", "0", CVAR_RENDERER | CVAR_BOOL, "Draw a colored box, red = left eye, blue = right eye, grey = non-stereo" );
idCVar r_motionBlur( "r_motionBlur", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_ARCHIVE, "1 - 5, log2 of the number of motion blur samples" );
idCVar r_forceZPassStencilShadows( "r_forceZPassStencilShadows", "0", CVAR_RENDERER | CVAR_BOOL, "force Z-pass rendering for performance testing" );
idCVar r_useStencilShadowPreload( "r_useStencilShadowPreload", "1", CVAR_RENDERER | CVAR_BOOL, "use stencil shadow preload algorithm instead of Z-fail" );
idCVar r_skipShaderPasses( "r_skipShaderPasses", "0", CVAR_RENDERER | CVAR_BOOL, "" );
idCVar r_skipInteractionFastPath( "r_skipInteractionFastPath", "1", CVAR_RENDERER | CVAR_BOOL, "" );
idCVar r_useLightStencilSelect( "r_useLightStencilSelect", "0", CVAR_RENDERER | CVAR_BOOL, "use stencil select pass" );

extern idCVar stereoRender_swapEyes;

backEndState_t	backEnd;

/*
================
SetVertexParm
================
*/
static ID_INLINE void SetVertexParm( renderParm_t rp, const float * value ) {
	renderProgManager.SetUniformValue( rp, value );
}

/*
================
SetVertexParms
================
*/
static ID_INLINE void SetVertexParms( renderParm_t rp, const float * value, int num ) {
	for ( int i = 0; i < num; i++ ) {
		renderProgManager.SetUniformValue( (renderParm_t)( rp + i ), value + ( i * 4 ) );
	}
}

/*
================
SetFragmentParm
================
*/
static ID_INLINE void SetFragmentParm( renderParm_t rp, const float * value ) {
	renderProgManager.SetUniformValue( rp, value );
}

/*
================
RB_SetMVP
================
*/
void RB_SetMVP( const idRenderMatrix & mvp ) { 
	SetVertexParms( RENDERPARM_MVPMATRIX_X, mvp[0], 4 );
}

/*
================
RB_SetMVPWithStereoOffset
================
*/
static void RB_SetMVPWithStereoOffset( const idRenderMatrix & mvp, const float stereoOffset ) { 
	idRenderMatrix offset = mvp;
	offset[0][3] += stereoOffset;

	SetVertexParms( RENDERPARM_MVPMATRIX_X, offset[0], 4 );
}

static const float zero[4] = { 0, 0, 0, 0 };
static const float one[4] = { 1, 1, 1, 1 };
static const float negOne[4] = { -1, -1, -1, -1 };

/*
================
RB_SetVertexColorParms
================
*/
static void RB_SetVertexColorParms( stageVertexColor_t svc ) {
	switch ( svc ) {
		case SVC_IGNORE:
			SetVertexParm( RENDERPARM_VERTEXCOLOR_MODULATE, zero );
			SetVertexParm( RENDERPARM_VERTEXCOLOR_ADD, one );
			break;
		case SVC_MODULATE:
			SetVertexParm( RENDERPARM_VERTEXCOLOR_MODULATE, one );
			SetVertexParm( RENDERPARM_VERTEXCOLOR_ADD, zero );
			break;
		case SVC_INVERSE_MODULATE:
			SetVertexParm( RENDERPARM_VERTEXCOLOR_MODULATE, negOne );
			SetVertexParm( RENDERPARM_VERTEXCOLOR_ADD, one );
			break;
	}
}

/*
================
RB_DrawElementsWithCounters
================
*/
void RB_DrawElementsWithCounters( const drawSurf_t *surf ) {
	// get vertex buffer
	const vertCacheHandle_t vbHandle = surf->ambientCache;
	idVertexBuffer * vertexBuffer;
	if ( vertexCache.CacheIsStatic( vbHandle ) ) {
		vertexBuffer = &vertexCache.staticData.vertexBuffer;
	} else {
		const uint64 frameNum = (int)( vbHandle >> VERTCACHE_FRAME_SHIFT ) & VERTCACHE_FRAME_MASK;
		if ( frameNum != ( ( vertexCache.currentFrame - 1 ) & VERTCACHE_FRAME_MASK ) ) {
			idLib::Warning( "RB_DrawElementsWithCounters, vertexBuffer == NULL" );
			return;
		}
		vertexBuffer = &vertexCache.frameData[vertexCache.drawListNum].vertexBuffer;
	}
	const int vertOffset = (int)( vbHandle >> VERTCACHE_OFFSET_SHIFT ) & VERTCACHE_OFFSET_MASK;

	// get index buffer
	const vertCacheHandle_t ibHandle = surf->indexCache;
	idIndexBuffer * indexBuffer;
	if ( vertexCache.CacheIsStatic( ibHandle ) ) {
		indexBuffer = &vertexCache.staticData.indexBuffer;
	} else {
		const uint64 frameNum = (int)( ibHandle >> VERTCACHE_FRAME_SHIFT ) & VERTCACHE_FRAME_MASK;
		if ( frameNum != ( ( vertexCache.currentFrame - 1 ) & VERTCACHE_FRAME_MASK ) ) {
			idLib::Warning( "RB_DrawElementsWithCounters, indexBuffer == NULL" );
			return;
		}
		indexBuffer = &vertexCache.frameData[vertexCache.drawListNum].indexBuffer;
	}
	const int indexOffset = (int)( ibHandle >> VERTCACHE_OFFSET_SHIFT ) & VERTCACHE_OFFSET_MASK;

	RENDERLOG_PRINTF( "Binding Buffers: %p:%i %p:%i\n", vertexBuffer, vertOffset, indexBuffer, indexOffset );

	if ( surf->jointCache ) {
		if ( !verify( renderProgManager.ShaderUsesJoints() ) ) {
			return;
		}
	} else {
		if ( !verify( !renderProgManager.ShaderUsesJoints() || renderProgManager.ShaderHasOptionalSkinning() ) ) {
			return;
		}
	}


	if ( surf->jointCache ) {
		idJointBuffer jointBuffer;
		if ( !vertexCache.GetJointBuffer( surf->jointCache, &jointBuffer ) ) {
			idLib::Warning( "RB_DrawElementsWithCounters, jointBuffer == NULL" );
			return;
		}
		assert( ( jointBuffer.GetOffset() & ( glConfig.uniformBufferOffsetAlignment - 1 ) ) == 0 );

		const GLuint ubo = reinterpret_cast< GLuint >( jointBuffer.GetAPIObject() );
		qglBindBufferRange( GL_UNIFORM_BUFFER, 0, ubo, jointBuffer.GetOffset(), jointBuffer.GetNumJoints() * sizeof( idJointMat ) );
	}

	renderProgManager.CommitUniforms();

	if ( backEnd.glState.currentIndexBuffer != (GLuint)indexBuffer->GetAPIObject() || !r_useStateCaching.GetBool() ) {
		qglBindBufferARB( GL_ELEMENT_ARRAY_BUFFER_ARB, (GLuint)indexBuffer->GetAPIObject() );
		backEnd.glState.currentIndexBuffer = (GLuint)indexBuffer->GetAPIObject();
	}

	if ( ( backEnd.glState.vertexLayout != LAYOUT_DRAW_VERT ) || ( backEnd.glState.currentVertexBuffer != (GLuint)vertexBuffer->GetAPIObject() ) || !r_useStateCaching.GetBool() ) {
		qglBindBufferARB( GL_ARRAY_BUFFER_ARB, (GLuint)vertexBuffer->GetAPIObject() );
		backEnd.glState.currentVertexBuffer = (GLuint)vertexBuffer->GetAPIObject();

		qglEnableVertexAttribArrayARB( PC_ATTRIB_INDEX_VERTEX );
		qglEnableVertexAttribArrayARB( PC_ATTRIB_INDEX_NORMAL );
		qglEnableVertexAttribArrayARB( PC_ATTRIB_INDEX_COLOR );
		qglEnableVertexAttribArrayARB( PC_ATTRIB_INDEX_COLOR2 );
		qglEnableVertexAttribArrayARB( PC_ATTRIB_INDEX_ST );
		qglEnableVertexAttribArrayARB( PC_ATTRIB_INDEX_TANGENT );

		qglVertexAttribPointerARB( PC_ATTRIB_INDEX_VERTEX, 3, GL_FLOAT, GL_FALSE, sizeof( idDrawVert ), (void *)( DRAWVERT_XYZ_OFFSET ) );
		qglVertexAttribPointerARB( PC_ATTRIB_INDEX_NORMAL, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof( idDrawVert ), (void *)( DRAWVERT_NORMAL_OFFSET ) );
		qglVertexAttribPointerARB( PC_ATTRIB_INDEX_COLOR, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof( idDrawVert ), (void *)( DRAWVERT_COLOR_OFFSET ) );
		qglVertexAttribPointerARB( PC_ATTRIB_INDEX_COLOR2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof( idDrawVert ), (void *)( DRAWVERT_COLOR2_OFFSET ) );
		qglVertexAttribPointerARB( PC_ATTRIB_INDEX_ST, 2, GL_HALF_FLOAT, GL_TRUE, sizeof( idDrawVert ), (void *)( DRAWVERT_ST_OFFSET ) );
		qglVertexAttribPointerARB( PC_ATTRIB_INDEX_TANGENT, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof( idDrawVert ), (void *)( DRAWVERT_TANGENT_OFFSET ) );

		backEnd.glState.vertexLayout = LAYOUT_DRAW_VERT;
	}
	
	qglDrawElementsBaseVertex( GL_TRIANGLES, 
							  r_singleTriangle.GetBool() ? 3 : surf->numIndexes,
							  GL_INDEX_TYPE,
							  (triIndex_t *)indexOffset,
							  vertOffset / sizeof ( idDrawVert ) );
							  

}

/*
======================
RB_GetShaderTextureMatrix
======================
*/
static void RB_GetShaderTextureMatrix( const float *shaderRegisters, const textureStage_t *texture, float matrix[16] ) {
	matrix[0*4+0] = shaderRegisters[ texture->matrix[0][0] ];
	matrix[1*4+0] = shaderRegisters[ texture->matrix[0][1] ];
	matrix[2*4+0] = 0.0f;
	matrix[3*4+0] = shaderRegisters[ texture->matrix[0][2] ];

	matrix[0*4+1] = shaderRegisters[ texture->matrix[1][0] ];
	matrix[1*4+1] = shaderRegisters[ texture->matrix[1][1] ];
	matrix[2*4+1] = 0.0f;
	matrix[3*4+1] = shaderRegisters[ texture->matrix[1][2] ];

	// we attempt to keep scrolls from generating incredibly large texture values, but
	// center rotations and center scales can still generate offsets that need to be > 1
	if ( matrix[3*4+0] < -40.0f || matrix[12] > 40.0f ) {
		matrix[3*4+0] -= (int)matrix[3*4+0];
	}
	if ( matrix[13] < -40.0f || matrix[13] > 40.0f ) {
		matrix[13] -= (int)matrix[13];
	}

	matrix[0*4+2] = 0.0f;
	matrix[1*4+2] = 0.0f;
	matrix[2*4+2] = 1.0f;
	matrix[3*4+2] = 0.0f;

	matrix[0*4+3] = 0.0f;
	matrix[1*4+3] = 0.0f;
	matrix[2*4+3] = 0.0f;
	matrix[3*4+3] = 1.0f;
}

/*
======================
RB_LoadShaderTextureMatrix
======================
*/
static void RB_LoadShaderTextureMatrix( const float *shaderRegisters, const textureStage_t *texture ) {	
	float texS[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
	float texT[4] = { 0.0f, 1.0f, 0.0f, 0.0f };

	if ( texture->hasMatrix ) {
		float matrix[16];
		RB_GetShaderTextureMatrix( shaderRegisters, texture, matrix );
		texS[0] = matrix[0*4+0];
		texS[1] = matrix[1*4+0];
		texS[2] = matrix[2*4+0];
		texS[3] = matrix[3*4+0];
	
		texT[0] = matrix[0*4+1];
		texT[1] = matrix[1*4+1];
		texT[2] = matrix[2*4+1];
		texT[3] = matrix[3*4+1];

		RENDERLOG_PRINTF( "Setting Texture Matrix\n");
		renderLog.Indent();
		RENDERLOG_PRINTF( "Texture Matrix S : %4.3f, %4.3f, %4.3f, %4.3f\n", texS[0], texS[1], texS[2], texS[3] );
		RENDERLOG_PRINTF( "Texture Matrix T : %4.3f, %4.3f, %4.3f, %4.3f\n", texT[0], texT[1], texT[2], texT[3] );
		renderLog.Outdent();
	} 

	SetVertexParm( RENDERPARM_TEXTUREMATRIX_S, texS );
	SetVertexParm( RENDERPARM_TEXTUREMATRIX_T, texT );
}

/*
=====================
RB_BakeTextureMatrixIntoTexgen
=====================
*/
static void RB_BakeTextureMatrixIntoTexgen( idPlane lightProject[3], const float *textureMatrix ) {
	float genMatrix[16];
	float final[16];

	genMatrix[0*4+0] = lightProject[0][0];
	genMatrix[1*4+0] = lightProject[0][1];
	genMatrix[2*4+0] = lightProject[0][2];
	genMatrix[3*4+0] = lightProject[0][3];

	genMatrix[0*4+1] = lightProject[1][0];
	genMatrix[1*4+1] = lightProject[1][1];
	genMatrix[2*4+1] = lightProject[1][2];
	genMatrix[3*4+1] = lightProject[1][3];

	genMatrix[0*4+2] = 0.0f;
	genMatrix[1*4+2] = 0.0f;
	genMatrix[2*4+2] = 0.0f;
	genMatrix[3*4+2] = 0.0f;

	genMatrix[0*4+3] = lightProject[2][0];
	genMatrix[1*4+3] = lightProject[2][1];
	genMatrix[2*4+3] = lightProject[2][2];
	genMatrix[3*4+3] = lightProject[2][3];

	R_MatrixMultiply( genMatrix, textureMatrix, final );

	lightProject[0][0] = final[0*4+0];
	lightProject[0][1] = final[1*4+0];
	lightProject[0][2] = final[2*4+0];
	lightProject[0][3] = final[3*4+0];

	lightProject[1][0] = final[0*4+1];
	lightProject[1][1] = final[1*4+1];
	lightProject[1][2] = final[2*4+1];
	lightProject[1][3] = final[3*4+1];
}

/*
======================
RB_BindVariableStageImage

Handles generating a cinematic frame if needed
======================
*/
static void RB_BindVariableStageImage( const textureStage_t *texture, const float *shaderRegisters ) {
	if ( texture->cinematic ) {
		cinData_t cin;

		if ( r_skipDynamicTextures.GetBool() ) {
			globalImages->defaultImage->Bind();
			return;
		}

		// offset time by shaderParm[7] (FIXME: make the time offset a parameter of the shader?)
		// We make no attempt to optimize for multiple identical cinematics being in view, or
		// for cinematics going at a lower framerate than the renderer.
		cin = texture->cinematic->ImageForTime( backEnd.viewDef->renderView.time[0] + idMath::Ftoi( 1000.0f * backEnd.viewDef->renderView.shaderParms[11] ) );
		if ( cin.imageY != NULL ) {
			GL_SelectTexture( 0 );
			cin.imageY->Bind();
			GL_SelectTexture( 1 );
			cin.imageCr->Bind();
			GL_SelectTexture( 2 );
			cin.imageCb->Bind();
		} else {
			globalImages->blackImage->Bind();
			// because the shaders may have already been set - we need to make sure we are not using a bink shader which would 
			// display incorrectly.  We may want to get rid of RB_BindVariableStageImage and inline the code so that the
			// SWF GUI case is handled better, too
			renderProgManager.BindShader_TextureVertexColor();
		}
	} else {
		// FIXME: see why image is invalid
		if ( texture->image != NULL ) {
			texture->image->Bind();
		}
	}
}

/*
================
RB_PrepareStageTexturing
================
*/
static void RB_PrepareStageTexturing( const shaderStage_t * pStage,  const drawSurf_t * surf ) {
	float useTexGenParm[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

	// set the texture matrix if needed
	RB_LoadShaderTextureMatrix( surf->shaderRegisters, &pStage->texture );

	// texgens
	if ( pStage->texture.texgen == TG_REFLECT_CUBE ) {

		// see if there is also a bump map specified
		const shaderStage_t *bumpStage = surf->material->GetBumpStage();
		if ( bumpStage != NULL ) {
			// per-pixel reflection mapping with bump mapping
			GL_SelectTexture( 1 );
			bumpStage->texture.image->Bind();
			GL_SelectTexture( 0 );

			RENDERLOG_PRINTF( "TexGen: TG_REFLECT_CUBE: Bumpy Environment\n" );
			if ( surf->jointCache ) {
				renderProgManager.BindShader_BumpyEnvironmentSkinned();
			} else {
				renderProgManager.BindShader_BumpyEnvironment();
			}
		} else {
			RENDERLOG_PRINTF( "TexGen: TG_REFLECT_CUBE: Environment\n" );
			if ( surf->jointCache ) {
				renderProgManager.BindShader_EnvironmentSkinned();
			} else {
				renderProgManager.BindShader_Environment();
			}
		}

	} else if ( pStage->texture.texgen == TG_SKYBOX_CUBE ) {

		renderProgManager.BindShader_SkyBox();

	} else if ( pStage->texture.texgen == TG_WOBBLESKY_CUBE ) {

		const int * parms = surf->material->GetTexGenRegisters();

		float wobbleDegrees = surf->shaderRegisters[ parms[0] ] * ( idMath::PI / 180.0f );
		float wobbleSpeed = surf->shaderRegisters[ parms[1] ] * ( 2.0f * idMath::PI / 60.0f );
		float rotateSpeed = surf->shaderRegisters[ parms[2] ] * ( 2.0f * idMath::PI / 60.0f );

		idVec3 axis[3];
		{
			// very ad-hoc "wobble" transform
			float s, c;
			idMath::SinCos( wobbleSpeed * backEnd.viewDef->renderView.time[0] * 0.001f, s, c );

			float ws, wc;
			idMath::SinCos( wobbleDegrees, ws, wc );

			axis[2][0] = ws * c;
			axis[2][1] = ws * s;
			axis[2][2] = wc;

			axis[1][0] = -s * s * ws;
			axis[1][2] = -s * ws * ws;
			axis[1][1] = idMath::Sqrt( idMath::Fabs( 1.0f - ( axis[1][0] * axis[1][0] + axis[1][2] * axis[1][2] ) ) );

			// make the second vector exactly perpendicular to the first
			axis[1] -= ( axis[2] * axis[1] ) * axis[2];
			axis[1].Normalize();

			// construct the third with a cross
			axis[0].Cross( axis[1], axis[2] );
		}

		// add the rotate
		float rs, rc;
		idMath::SinCos( rotateSpeed * backEnd.viewDef->renderView.time[0] * 0.001f, rs, rc );

		float transform[12];
		transform[0*4+0] = axis[0][0] * rc + axis[1][0] * rs;
		transform[0*4+1] = axis[0][1] * rc + axis[1][1] * rs;
		transform[0*4+2] = axis[0][2] * rc + axis[1][2] * rs;
		transform[0*4+3] = 0.0f;

		transform[1*4+0] = axis[1][0] * rc - axis[0][0] * rs;
		transform[1*4+1] = axis[1][1] * rc - axis[0][1] * rs;
		transform[1*4+2] = axis[1][2] * rc - axis[0][2] * rs;
		transform[1*4+3] = 0.0f;

		transform[2*4+0] = axis[2][0];
		transform[2*4+1] = axis[2][1];
		transform[2*4+2] = axis[2][2];
		transform[2*4+3] = 0.0f;

		SetVertexParms( RENDERPARM_WOBBLESKY_X, transform, 3 );
		renderProgManager.BindShader_WobbleSky();

	} else if ( ( pStage->texture.texgen == TG_SCREEN ) || ( pStage->texture.texgen == TG_SCREEN2 ) ) {

		useTexGenParm[0] = 1.0f;
		useTexGenParm[1] = 1.0f;
		useTexGenParm[2] = 1.0f;
		useTexGenParm[3] = 1.0f;

		float mat[16];
		R_MatrixMultiply( surf->space->modelViewMatrix, backEnd.viewDef->projectionMatrix, mat );

		RENDERLOG_PRINTF( "TexGen : %s\n", ( pStage->texture.texgen == TG_SCREEN ) ? "TG_SCREEN" : "TG_SCREEN2" );
		renderLog.Indent();

		float plane[4];
		plane[0] = mat[0*4+0];
		plane[1] = mat[1*4+0];
		plane[2] = mat[2*4+0];
		plane[3] = mat[3*4+0];
		SetVertexParm( RENDERPARM_TEXGEN_0_S, plane );
		RENDERLOG_PRINTF( "TEXGEN_S = %4.3f, %4.3f, %4.3f, %4.3f\n",  plane[0], plane[1], plane[2], plane[3] );

		plane[0] = mat[0*4+1];
		plane[1] = mat[1*4+1];
		plane[2] = mat[2*4+1];
		plane[3] = mat[3*4+1];
		SetVertexParm( RENDERPARM_TEXGEN_0_T, plane );
		RENDERLOG_PRINTF( "TEXGEN_T = %4.3f, %4.3f, %4.3f, %4.3f\n",  plane[0], plane[1], plane[2], plane[3] );

		plane[0] = mat[0*4+3];
		plane[1] = mat[1*4+3];
		plane[2] = mat[2*4+3];
		plane[3] = mat[3*4+3];
		SetVertexParm( RENDERPARM_TEXGEN_0_Q, plane );	
		RENDERLOG_PRINTF( "TEXGEN_Q = %4.3f, %4.3f, %4.3f, %4.3f\n",  plane[0], plane[1], plane[2], plane[3] );

		renderLog.Outdent();

	} else if ( pStage->texture.texgen == TG_DIFFUSE_CUBE ) {

		// As far as I can tell, this is never used
		idLib::Warning( "Using Diffuse Cube! Please contact Brian!" );

	} else if ( pStage->texture.texgen == TG_GLASSWARP ) {

		// As far as I can tell, this is never used
		idLib::Warning( "Using GlassWarp! Please contact Brian!" );
	}

	SetVertexParm( RENDERPARM_TEXGEN_0_ENABLED, useTexGenParm );
}

/*
================
RB_FinishStageTexturing
================
*/
static void RB_FinishStageTexturing( const shaderStage_t *pStage, const drawSurf_t *surf ) {

	if ( pStage->texture.cinematic ) {
		// unbind the extra bink textures
		GL_SelectTexture( 1 );
		globalImages->BindNull();
		GL_SelectTexture( 2 );
		globalImages->BindNull();
		GL_SelectTexture( 0 );
	}

	if ( pStage->texture.texgen == TG_REFLECT_CUBE ) {
		// see if there is also a bump map specified
		const shaderStage_t *bumpStage = surf->material->GetBumpStage();
		if ( bumpStage != NULL ) {
			// per-pixel reflection mapping with bump mapping
			GL_SelectTexture( 1 );
			globalImages->BindNull();
			GL_SelectTexture( 0 );
		} else {
			// per-pixel reflection mapping without bump mapping
		}
		renderProgManager.Unbind();
	}
}

/*
=========================================================================================

DEPTH BUFFER RENDERING

=========================================================================================
*/

/*
==================
RB_FillDepthBufferGeneric
==================
*/
static void RB_FillDepthBufferGeneric( const drawSurf_t * const * drawSurfs, int numDrawSurfs ) {
	for ( int i = 0; i < numDrawSurfs; i++ ) {
		const drawSurf_t * drawSurf = drawSurfs[i];
		const idMaterial * shader = drawSurf->material;

		// translucent surfaces don't put anything in the depth buffer and don't
		// test against it, which makes them fail the mirror clip plane operation
		if ( shader->Coverage() == MC_TRANSLUCENT ) {
			continue;
		}

		// get the expressions for conditionals / color / texcoords
		const float * regs = drawSurf->shaderRegisters;

		// if all stages of a material have been conditioned off, don't do anything
		int stage = 0;
		for ( ; stage < shader->GetNumStages(); stage++ ) {		
			const shaderStage_t * pStage = shader->GetStage( stage );
			// check the stage enable condition
			if ( regs[ pStage->conditionRegister ] != 0 ) {
				break;
			}
		}
		if ( stage == shader->GetNumStages() ) {
			continue;
		}

		// change the matrix if needed
		if ( drawSurf->space != backEnd.currentSpace ) {
			RB_SetMVP( drawSurf->space->mvp );

			backEnd.currentSpace = drawSurf->space;
		}

		uint64 surfGLState = 0;

		// set polygon offset if necessary
		if ( shader->TestMaterialFlag( MF_POLYGONOFFSET ) ) {
			surfGLState |= GLS_POLYGON_OFFSET;
			GL_PolygonOffset( r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * shader->GetPolygonOffset() );
		}

		// subviews will just down-modulate the color buffer
		float color[4];
		if ( shader->GetSort() == SS_SUBVIEW ) {
			surfGLState |= GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ZERO | GLS_DEPTHFUNC_LESS;
			color[0] = 1.0f;
			color[1] = 1.0f;
			color[2] = 1.0f;
			color[3] = 1.0f;
		} else {
			// others just draw black
			color[0] = 0.0f;
			color[1] = 0.0f;
			color[2] = 0.0f;
			color[3] = 1.0f;
		}

		renderLog.OpenBlock( shader->GetName() );

		bool drawSolid = false;
		if ( shader->Coverage() == MC_OPAQUE ) {
			drawSolid = true;
		} else if ( shader->Coverage() == MC_PERFORATED ) {
			// we may have multiple alpha tested stages
			// if the only alpha tested stages are condition register omitted,
			// draw a normal opaque surface
			bool didDraw = false;

			// perforated surfaces may have multiple alpha tested stages
			for ( stage = 0; stage < shader->GetNumStages(); stage++ ) {		
				const shaderStage_t *pStage = shader->GetStage(stage);

				if ( !pStage->hasAlphaTest ) {
					continue;
				}

				// check the stage enable condition
				if ( regs[ pStage->conditionRegister ] == 0 ) {
					continue;
				}

				// if we at least tried to draw an alpha tested stage,
				// we won't draw the opaque surface
				didDraw = true;

				// set the alpha modulate
				color[3] = regs[ pStage->color.registers[3] ];

				// skip the entire stage if alpha would be black
				if ( color[3] <= 0.0f ) {
					continue;
				}

				uint64 stageGLState = surfGLState;

				// set privatePolygonOffset if necessary
				if ( pStage->privatePolygonOffset ) {
					GL_PolygonOffset( r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * pStage->privatePolygonOffset );
					stageGLState |= GLS_POLYGON_OFFSET;
				}

				GL_Color( color );

#ifdef USE_CORE_PROFILE
				GL_State( stageGLState );
				idVec4 alphaTestValue( regs[ pStage->alphaTestRegister ] );
				SetFragmentParm( RENDERPARM_ALPHA_TEST, alphaTestValue.ToFloatPtr() );
#else
				GL_State( stageGLState | GLS_ALPHATEST_FUNC_GREATER | GLS_ALPHATEST_MAKE_REF( idMath::Ftob( 255.0f * regs[ pStage->alphaTestRegister ] ) ) );
#endif

				if ( drawSurf->jointCache ) {
					renderProgManager.BindShader_TextureVertexColorSkinned();
				} else {
					renderProgManager.BindShader_TextureVertexColor();
				}

				RB_SetVertexColorParms( SVC_IGNORE );

				// bind the texture
				GL_SelectTexture( 0 );
				pStage->texture.image->Bind();

				// set texture matrix and texGens
				RB_PrepareStageTexturing( pStage, drawSurf );

				// must render with less-equal for Z-Cull to work properly
				assert( ( GL_GetCurrentState() & GLS_DEPTHFUNC_BITS ) == GLS_DEPTHFUNC_LESS );

				// draw it
				RB_DrawElementsWithCounters( drawSurf );

				// clean up
				RB_FinishStageTexturing( pStage, drawSurf );

				// unset privatePolygonOffset if necessary
				if ( pStage->privatePolygonOffset ) {
					GL_PolygonOffset( r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * shader->GetPolygonOffset() );
				}
			}

			if ( !didDraw ) {
				drawSolid = true;
			}
		}

		// draw the entire surface solid
		if ( drawSolid ) {
			if ( shader->GetSort() == SS_SUBVIEW ) {
				renderProgManager.BindShader_Color();
				GL_Color( color );
				GL_State( surfGLState );
			} else {
				if ( drawSurf->jointCache ) {
					renderProgManager.BindShader_DepthSkinned();
				} else {
					renderProgManager.BindShader_Depth();
				}
				GL_State( surfGLState | GLS_ALPHAMASK );
			}

			// must render with less-equal for Z-Cull to work properly
			assert( ( GL_GetCurrentState() & GLS_DEPTHFUNC_BITS ) == GLS_DEPTHFUNC_LESS );

			// draw it
			RB_DrawElementsWithCounters( drawSurf );
		}

		renderLog.CloseBlock();
	}

#ifdef USE_CORE_PROFILE
	SetFragmentParm( RENDERPARM_ALPHA_TEST, vec4_zero.ToFloatPtr() );
#endif
}

/*
=====================
RB_FillDepthBufferFast

Optimized fast path code.

If there are subview surfaces, they must be guarded in the depth buffer to allow
the mirror / subview to show through underneath the current view rendering.

Surfaces with perforated shaders need the full shader setup done, but should be
drawn after the opaque surfaces.

The bulk of the surfaces should be simple opaque geometry that can be drawn very rapidly.

If there are no subview surfaces, we could clear to black and use fast-Z rendering
on the 360.
=====================
*/
static void RB_FillDepthBufferFast( drawSurf_t **drawSurfs, int numDrawSurfs ) {
	if ( numDrawSurfs == 0 ) {
		return;
	}

	// if we are just doing 2D rendering, no need to fill the depth buffer
	if ( backEnd.viewDef->viewEntitys == NULL ) {
		return;
	}

	renderLog.OpenMainBlock( MRB_FILL_DEPTH_BUFFER );
	renderLog.OpenBlock( "RB_FillDepthBufferFast" );

	GL_StartDepthPass( backEnd.viewDef->scissor );

	// force MVP change on first surface
	backEnd.currentSpace = NULL;

	// draw all the subview surfaces, which will already be at the start of the sorted list,
	// with the general purpose path
	GL_State( GLS_DEFAULT );

	int	surfNum;
	for ( surfNum = 0; surfNum < numDrawSurfs; surfNum++ ) {
		if ( drawSurfs[surfNum]->material->GetSort() != SS_SUBVIEW ) {
			break;
		}
		RB_FillDepthBufferGeneric( &drawSurfs[surfNum], 1 );
	}

	const drawSurf_t ** perforatedSurfaces = (const drawSurf_t ** )_alloca( numDrawSurfs * sizeof( drawSurf_t * ) );
	int numPerforatedSurfaces = 0;

	// draw all the opaque surfaces and build up a list of perforated surfaces that
	// we will defer drawing until all opaque surfaces are done
	GL_State( GLS_DEFAULT );

	// continue checking past the subview surfaces
	for ( ; surfNum < numDrawSurfs; surfNum++ ) {
		const drawSurf_t * surf = drawSurfs[ surfNum ];
		const idMaterial * shader = surf->material;

		// translucent surfaces don't put anything in the depth buffer
		if ( shader->Coverage() == MC_TRANSLUCENT ) {
			continue;
		}
		if ( shader->Coverage() == MC_PERFORATED ) {
			// save for later drawing
			perforatedSurfaces[ numPerforatedSurfaces ] = surf;
			numPerforatedSurfaces++;
			continue;
		}

		// set polygon offset?

		// set mvp matrix
		if ( surf->space != backEnd.currentSpace ) {
			RB_SetMVP( surf->space->mvp );
			backEnd.currentSpace = surf->space;
		}

		renderLog.OpenBlock( shader->GetName() );

		if ( surf->jointCache ) {
			renderProgManager.BindShader_DepthSkinned();
		} else {
			renderProgManager.BindShader_Depth();
		}

		// must render with less-equal for Z-Cull to work properly
		assert( ( GL_GetCurrentState() & GLS_DEPTHFUNC_BITS ) == GLS_DEPTHFUNC_LESS );

		// draw it solid
		RB_DrawElementsWithCounters( surf );

		renderLog.CloseBlock();
	}

	// draw all perforated surfaces with the general code path
	if ( numPerforatedSurfaces > 0 ) {
		RB_FillDepthBufferGeneric( perforatedSurfaces, numPerforatedSurfaces );
	}

	// Allow platform specific data to be collected after the depth pass.
	GL_FinishDepthPass();

	renderLog.CloseBlock();
	renderLog.CloseMainBlock();
}

/*
=========================================================================================

GENERAL INTERACTION RENDERING

=========================================================================================
*/

const int INTERACTION_TEXUNIT_BUMP			= 0;
const int INTERACTION_TEXUNIT_FALLOFF		= 1;
const int INTERACTION_TEXUNIT_PROJECTION	= 2;
const int INTERACTION_TEXUNIT_DIFFUSE		= 3;
const int INTERACTION_TEXUNIT_SPECULAR		= 4;

/*
==================
RB_SetupInteractionStage
==================
*/
static void RB_SetupInteractionStage( const shaderStage_t *surfaceStage, const float *surfaceRegs, const float lightColor[4],
									idVec4 matrix[2], float color[4] ) {

	if ( surfaceStage->texture.hasMatrix ) {
		matrix[0][0] = surfaceRegs[surfaceStage->texture.matrix[0][0]];
		matrix[0][1] = surfaceRegs[surfaceStage->texture.matrix[0][1]];
		matrix[0][2] = 0.0f;
		matrix[0][3] = surfaceRegs[surfaceStage->texture.matrix[0][2]];

		matrix[1][0] = surfaceRegs[surfaceStage->texture.matrix[1][0]];
		matrix[1][1] = surfaceRegs[surfaceStage->texture.matrix[1][1]];
		matrix[1][2] = 0.0f;
		matrix[1][3] = surfaceRegs[surfaceStage->texture.matrix[1][2]];

		// we attempt to keep scrolls from generating incredibly large texture values, but
		// center rotations and center scales can still generate offsets that need to be > 1
		if ( matrix[0][3] < -40.0f || matrix[0][3] > 40.0f ) {
			matrix[0][3] -= idMath::Ftoi( matrix[0][3] );
		}
		if ( matrix[1][3] < -40.0f || matrix[1][3] > 40.0f ) {
			matrix[1][3] -= idMath::Ftoi( matrix[1][3] );
		}
	} else {
		matrix[0][0] = 1.0f;
		matrix[0][1] = 0.0f;
		matrix[0][2] = 0.0f;
		matrix[0][3] = 0.0f;

		matrix[1][0] = 0.0f;
		matrix[1][1] = 1.0f;
		matrix[1][2] = 0.0f;
		matrix[1][3] = 0.0f;
	}

	if ( color != NULL ) {
		for ( int i = 0; i < 4; i++ ) {
			// clamp here, so cards with a greater range don't look different.
			// we could perform overbrighting like we do for lights, but
			// it doesn't currently look worth it.
			color[i] = idMath::ClampFloat( 0.0f, 1.0f, surfaceRegs[surfaceStage->color.registers[i]] ) * lightColor[i];
		}
	}
}

/*
=================
RB_DrawSingleInteraction
=================
*/
static void RB_DrawSingleInteraction( drawInteraction_t * din ) {
	if ( din->bumpImage == NULL ) {
		// stage wasn't actually an interaction
		return;
	}

	if ( din->diffuseImage == NULL || r_skipDiffuse.GetBool() ) {
		// this isn't a YCoCg black, but it doesn't matter, because
		// the diffuseColor will also be 0
		din->diffuseImage = globalImages->blackImage;
	}
	if ( din->specularImage == NULL || r_skipSpecular.GetBool() || din->ambientLight ) {
		din->specularImage = globalImages->blackImage;
	}
	if ( r_skipBump.GetBool() ) {
		din->bumpImage = globalImages->flatNormalMap;
	}

	// if we wouldn't draw anything, don't call the Draw function
	const bool diffuseIsBlack = ( din->diffuseImage == globalImages->blackImage )
									|| ( ( din->diffuseColor[0] <= 0 ) && ( din->diffuseColor[1] <= 0 ) && ( din->diffuseColor[2] <= 0 ) );
	const bool specularIsBlack = ( din->specularImage == globalImages->blackImage )
									|| ( ( din->specularColor[0] <= 0 ) && ( din->specularColor[1] <= 0 ) && ( din->specularColor[2] <= 0 ) );
	if ( diffuseIsBlack && specularIsBlack ) {
		return;
	}

	// bump matrix
	SetVertexParm( RENDERPARM_BUMPMATRIX_S, din->bumpMatrix[0].ToFloatPtr() );
	SetVertexParm( RENDERPARM_BUMPMATRIX_T, din->bumpMatrix[1].ToFloatPtr() );

	// diffuse matrix
	SetVertexParm( RENDERPARM_DIFFUSEMATRIX_S, din->diffuseMatrix[0].ToFloatPtr() );
	SetVertexParm( RENDERPARM_DIFFUSEMATRIX_T, din->diffuseMatrix[1].ToFloatPtr() );

	// specular matrix
	SetVertexParm( RENDERPARM_SPECULARMATRIX_S, din->specularMatrix[0].ToFloatPtr() );
	SetVertexParm( RENDERPARM_SPECULARMATRIX_T, din->specularMatrix[1].ToFloatPtr() );

	RB_SetVertexColorParms( din->vertexColor );

	SetFragmentParm( RENDERPARM_DIFFUSEMODIFIER, din->diffuseColor.ToFloatPtr() );
	SetFragmentParm( RENDERPARM_SPECULARMODIFIER, din->specularColor.ToFloatPtr() );

	// texture 0 will be the per-surface bump map
 	GL_SelectTexture( INTERACTION_TEXUNIT_BUMP );
	din->bumpImage->Bind();

	// texture 3 is the per-surface diffuse map
	GL_SelectTexture( INTERACTION_TEXUNIT_DIFFUSE );
	din->diffuseImage->Bind();

	// texture 4 is the per-surface specular map
	GL_SelectTexture( INTERACTION_TEXUNIT_SPECULAR );
	din->specularImage->Bind();

	RB_DrawElementsWithCounters( din->surf );
}

/*
=================
RB_SetupForFastPathInteractions

These are common for all fast path surfaces
=================
*/
static void RB_SetupForFastPathInteractions( const idVec4 & diffuseColor, const idVec4 & specularColor ) {
	const idVec4 sMatrix( 1, 0, 0, 0 );
	const idVec4 tMatrix( 0, 1, 0, 0 );

	// bump matrix
	SetVertexParm( RENDERPARM_BUMPMATRIX_S, sMatrix.ToFloatPtr() );
	SetVertexParm( RENDERPARM_BUMPMATRIX_T, tMatrix.ToFloatPtr() );

	// diffuse matrix
	SetVertexParm( RENDERPARM_DIFFUSEMATRIX_S, sMatrix.ToFloatPtr() );
	SetVertexParm( RENDERPARM_DIFFUSEMATRIX_T, tMatrix.ToFloatPtr() );

	// specular matrix
	SetVertexParm( RENDERPARM_SPECULARMATRIX_S, sMatrix.ToFloatPtr() );
	SetVertexParm( RENDERPARM_SPECULARMATRIX_T, tMatrix.ToFloatPtr() );

	RB_SetVertexColorParms( SVC_IGNORE );

	SetFragmentParm( RENDERPARM_DIFFUSEMODIFIER, diffuseColor.ToFloatPtr() );
	SetFragmentParm( RENDERPARM_SPECULARMODIFIER, specularColor.ToFloatPtr() );
}

/*
=============
RB_RenderInteractions

With added sorting and trivial path work.
=============
*/
static void RB_RenderInteractions( const drawSurf_t *surfList, const viewLight_t * vLight, int depthFunc, bool performStencilTest, bool useLightDepthBounds ) {
	if ( surfList == NULL ) {
		return;
	}

	// change the scissor if needed, it will be constant across all the surfaces lit by the light
	if ( !backEnd.currentScissor.Equals( vLight->scissorRect ) && r_useScissor.GetBool() ) {
		GL_Scissor( backEnd.viewDef->viewport.x1 + vLight->scissorRect.x1, 
					backEnd.viewDef->viewport.y1 + vLight->scissorRect.y1,
					vLight->scissorRect.x2 + 1 - vLight->scissorRect.x1,
					vLight->scissorRect.y2 + 1 - vLight->scissorRect.y1 );
		backEnd.currentScissor = vLight->scissorRect;
	}

	// perform setup here that will be constant for all interactions
	if ( performStencilTest ) {
		GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHMASK | depthFunc | GLS_STENCIL_FUNC_EQUAL | GLS_STENCIL_MAKE_REF( STENCIL_SHADOW_TEST_VALUE ) | GLS_STENCIL_MAKE_MASK( STENCIL_SHADOW_MASK_VALUE ) );

	} else {
		GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHMASK | depthFunc | GLS_STENCIL_FUNC_ALWAYS );
	}

	// some rare lights have multiple animating stages, loop over them outside the surface list
	const idMaterial * lightShader = vLight->lightShader;
	const float * lightRegs = vLight->shaderRegisters;

	drawInteraction_t inter = {};
	inter.ambientLight = lightShader->IsAmbientLight();

	//---------------------------------
	// Split out the complex surfaces from the fast-path surfaces
	// so we can do the fast path ones all in a row.
	// The surfaces should already be sorted by space because they
	// are added single-threaded, and there is only a negligable amount
	// of benefit to trying to sort by materials.
	//---------------------------------
	static const int MAX_INTERACTIONS_PER_LIGHT = 1024;
	static const int MAX_COMPLEX_INTERACTIONS_PER_LIGHT = 128;
	idStaticList< const drawSurf_t *, MAX_INTERACTIONS_PER_LIGHT > allSurfaces;
	idStaticList< const drawSurf_t *, MAX_COMPLEX_INTERACTIONS_PER_LIGHT > complexSurfaces;
	for ( const drawSurf_t * walk = surfList; walk != NULL; walk = walk->nextOnLight ) {

		// make sure the triangle culling is done
		if ( walk->shadowVolumeState != SHADOWVOLUME_DONE ) {
			assert( walk->shadowVolumeState == SHADOWVOLUME_UNFINISHED || walk->shadowVolumeState == SHADOWVOLUME_DONE );

			uint64 start = Sys_Microseconds();
			while ( walk->shadowVolumeState == SHADOWVOLUME_UNFINISHED ) {
				Sys_Yield();
			}
			uint64 end = Sys_Microseconds();

			backEnd.pc.shadowMicroSec += end - start;
		}

		const idMaterial * surfaceShader = walk->material;
		if ( surfaceShader->GetFastPathBumpImage() ) {
			allSurfaces.Append( walk );
		} else {
			complexSurfaces.Append( walk );
		}
	}
	for ( int i = 0; i < complexSurfaces.Num(); i++ ) {
		allSurfaces.Append( complexSurfaces[i] );
	}

	bool lightDepthBoundsDisabled = false;

	for ( int lightStageNum = 0; lightStageNum < lightShader->GetNumStages(); lightStageNum++ ) {
		const shaderStage_t	*lightStage = lightShader->GetStage( lightStageNum );

		// ignore stages that fail the condition
		if ( !lightRegs[ lightStage->conditionRegister ] ) {
			continue;
		}

		const float lightScale = r_lightScale.GetFloat();
		const idVec4 lightColor(
			lightScale * lightRegs[ lightStage->color.registers[0] ],
			lightScale * lightRegs[ lightStage->color.registers[1] ],
			lightScale * lightRegs[ lightStage->color.registers[2] ],
			lightRegs[ lightStage->color.registers[3] ] );
		// apply the world-global overbright and the 2x factor for specular
		const idVec4 diffuseColor = lightColor;
		const idVec4 specularColor = lightColor * 2.0f;

		float lightTextureMatrix[16];
		if ( lightStage->texture.hasMatrix ) {
			RB_GetShaderTextureMatrix( lightRegs, &lightStage->texture, lightTextureMatrix );
		}

		// texture 1 will be the light falloff texture
		GL_SelectTexture( INTERACTION_TEXUNIT_FALLOFF );
		vLight->falloffImage->Bind();

		// texture 2 will be the light projection texture
		GL_SelectTexture( INTERACTION_TEXUNIT_PROJECTION );
		lightStage->texture.image->Bind();

		// force the light textures to not use anisotropic filtering, which is wasted on them
		// all of the texture sampler parms should be constant for all interactions, only
		// the actual texture image bindings will change

		//----------------------------------
		// For all surfaces on this light list, generate an interaction for this light stage
		//----------------------------------

		// setup renderparms assuming we will be drawing trivial surfaces first
		RB_SetupForFastPathInteractions( diffuseColor, specularColor );

		// even if the space does not change between light stages, each light stage may need a different lightTextureMatrix baked in
		backEnd.currentSpace = NULL;

		for ( int sortedSurfNum = 0; sortedSurfNum < allSurfaces.Num(); sortedSurfNum++ ) {
			const drawSurf_t * const surf = allSurfaces[ sortedSurfNum ];

			// select the render prog
			if ( lightShader->IsAmbientLight() ) {
				if ( surf->jointCache ) {
					renderProgManager.BindShader_InteractionAmbientSkinned();
				} else {
					renderProgManager.BindShader_InteractionAmbient();
				}
			} else {
				if ( surf->jointCache ) {
					renderProgManager.BindShader_InteractionSkinned();
				} else {
					renderProgManager.BindShader_Interaction();
				}
			}

			const idMaterial * surfaceShader = surf->material;
			const float * surfaceRegs = surf->shaderRegisters;

			inter.surf = surf;

			// change the MVP matrix, view/light origin and light projection vectors if needed
			if ( surf->space != backEnd.currentSpace ) {
				backEnd.currentSpace = surf->space;

				// turn off the light depth bounds test if this model is rendered with a depth hack
				if ( useLightDepthBounds ) {
					if ( !surf->space->weaponDepthHack && surf->space->modelDepthHack == 0.0f ) {
						if ( lightDepthBoundsDisabled ) {
							GL_DepthBoundsTest( vLight->scissorRect.zmin, vLight->scissorRect.zmax );
							lightDepthBoundsDisabled = false;
						}
					} else {
						if ( !lightDepthBoundsDisabled ) {
							GL_DepthBoundsTest( 0.0f, 0.0f );
							lightDepthBoundsDisabled = true;
						}
					}
				}

				// model-view-projection
				RB_SetMVP( surf->space->mvp );

				// tranform the light/view origin into model local space
				idVec4 localLightOrigin( 0.0f );
				idVec4 localViewOrigin( 1.0f );
				R_GlobalPointToLocal( surf->space->modelMatrix, vLight->globalLightOrigin, localLightOrigin.ToVec3() );
				R_GlobalPointToLocal( surf->space->modelMatrix, backEnd.viewDef->renderView.vieworg, localViewOrigin.ToVec3() );

				// set the local light/view origin
				SetVertexParm( RENDERPARM_LOCALLIGHTORIGIN, localLightOrigin.ToFloatPtr() );
				SetVertexParm( RENDERPARM_LOCALVIEWORIGIN, localViewOrigin.ToFloatPtr() );

				// transform the light project into model local space
				idPlane lightProjection[4];
				for ( int i = 0; i < 4; i++ ) {
					R_GlobalPlaneToLocal( surf->space->modelMatrix, vLight->lightProject[i], lightProjection[i] );
				}

				// optionally multiply the local light projection by the light texture matrix
				if ( lightStage->texture.hasMatrix ) {
					RB_BakeTextureMatrixIntoTexgen( lightProjection, lightTextureMatrix );
				}

				// set the light projection
				SetVertexParm( RENDERPARM_LIGHTPROJECTION_S, lightProjection[0].ToFloatPtr() );
				SetVertexParm( RENDERPARM_LIGHTPROJECTION_T, lightProjection[1].ToFloatPtr() );
				SetVertexParm( RENDERPARM_LIGHTPROJECTION_Q, lightProjection[2].ToFloatPtr() );
				SetVertexParm( RENDERPARM_LIGHTFALLOFF_S, lightProjection[3].ToFloatPtr() );
			}

			// check for the fast path
			if ( surfaceShader->GetFastPathBumpImage() && !r_skipInteractionFastPath.GetBool() ) {
				renderLog.OpenBlock( surf->material->GetName() );

				// texture 0 will be the per-surface bump map
				GL_SelectTexture( INTERACTION_TEXUNIT_BUMP );
				surfaceShader->GetFastPathBumpImage()->Bind();

				// texture 3 is the per-surface diffuse map
				GL_SelectTexture( INTERACTION_TEXUNIT_DIFFUSE );
				surfaceShader->GetFastPathDiffuseImage()->Bind();

				// texture 4 is the per-surface specular map
				GL_SelectTexture( INTERACTION_TEXUNIT_SPECULAR );
				surfaceShader->GetFastPathSpecularImage()->Bind();

				RB_DrawElementsWithCounters( surf );

				renderLog.CloseBlock();
				continue;
			}
			
			renderLog.OpenBlock( surf->material->GetName() );

			inter.bumpImage = NULL;
			inter.specularImage = NULL;
			inter.diffuseImage = NULL;
			inter.diffuseColor[0] = inter.diffuseColor[1] = inter.diffuseColor[2] = inter.diffuseColor[3] = 0;
			inter.specularColor[0] = inter.specularColor[1] = inter.specularColor[2] = inter.specularColor[3] = 0;

			// go through the individual surface stages
			//
			// This is somewhat arcane because of the old support for video cards that had to render
			// interactions in multiple passes.
			//
			// We also have the very rare case of some materials that have conditional interactions
			// for the "hell writing" that can be shined on them.
			for ( int surfaceStageNum = 0; surfaceStageNum < surfaceShader->GetNumStages(); surfaceStageNum++ ) {
				const shaderStage_t	*surfaceStage = surfaceShader->GetStage( surfaceStageNum );

				switch( surfaceStage->lighting ) {
					case SL_COVERAGE: {
						// ignore any coverage stages since they should only be used for the depth fill pass
						// for diffuse stages that use alpha test.
						break;
					}
					case SL_AMBIENT: {
						// ignore ambient stages while drawing interactions
						break;
					}
					case SL_BUMP: {
						// ignore stage that fails the condition
						if ( !surfaceRegs[ surfaceStage->conditionRegister ] ) {
							break;
						}
						// draw any previous interaction
						if ( inter.bumpImage != NULL ) {
							RB_DrawSingleInteraction( &inter );
						}
						inter.bumpImage = surfaceStage->texture.image;
						inter.diffuseImage = NULL;
						inter.specularImage = NULL;
						RB_SetupInteractionStage( surfaceStage, surfaceRegs, NULL,
												inter.bumpMatrix, NULL );
						break;
					}
					case SL_DIFFUSE: {
						// ignore stage that fails the condition
						if ( !surfaceRegs[ surfaceStage->conditionRegister ] ) {
							break;
						}
						// draw any previous interaction
						if ( inter.diffuseImage != NULL ) {
							RB_DrawSingleInteraction( &inter );
						}
						inter.diffuseImage = surfaceStage->texture.image;
						inter.vertexColor = surfaceStage->vertexColor;
						RB_SetupInteractionStage( surfaceStage, surfaceRegs, diffuseColor.ToFloatPtr(),
												inter.diffuseMatrix, inter.diffuseColor.ToFloatPtr() );
						break;
					}
					case SL_SPECULAR: {
						// ignore stage that fails the condition
						if ( !surfaceRegs[ surfaceStage->conditionRegister ] ) {
							break;
						}
						// draw any previous interaction
						if ( inter.specularImage != NULL ) {
							RB_DrawSingleInteraction( &inter );
						}
						inter.specularImage = surfaceStage->texture.image;
						inter.vertexColor = surfaceStage->vertexColor;
						RB_SetupInteractionStage( surfaceStage, surfaceRegs, specularColor.ToFloatPtr(),
												inter.specularMatrix, inter.specularColor.ToFloatPtr() );
						break;
					}
				}
			}

			// draw the final interaction
			RB_DrawSingleInteraction( &inter );

			renderLog.CloseBlock();
		}
	}

	if ( useLightDepthBounds && lightDepthBoundsDisabled ) {
		GL_DepthBoundsTest( vLight->scissorRect.zmin, vLight->scissorRect.zmax );
	}

	renderProgManager.Unbind();
}

/*
==============================================================================================

STENCIL SHADOW RENDERING

==============================================================================================
*/

/*
=====================
RB_StencilShadowPass

The stencil buffer should have been set to 128 on any surfaces that might receive shadows.
=====================
*/
static void RB_StencilShadowPass( const drawSurf_t *drawSurfs, const viewLight_t * vLight ) {
	if ( r_skipShadows.GetBool() ) {
		return;
	}

	if ( drawSurfs == NULL ) {
		return;
	}

	RENDERLOG_PRINTF( "---------- RB_StencilShadowPass ----------\n" );

	renderProgManager.BindShader_Shadow();

	GL_SelectTexture( 0 );
	globalImages->BindNull();

	uint64 glState = 0;

	// for visualizing the shadows
	if ( r_showShadows.GetInteger() ) {
		// set the debug shadow color
		SetFragmentParm( RENDERPARM_COLOR, colorMagenta.ToFloatPtr() );
		if ( r_showShadows.GetInteger() == 2 ) {
			// draw filled in
			glState = GLS_DEPTHMASK | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHFUNC_LESS;
		} else {
			// draw as lines, filling the depth buffer
			glState = GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO | GLS_POLYMODE_LINE | GLS_DEPTHFUNC_ALWAYS;
		}
	} else {
		// don't write to the color or depth buffer, just the stencil buffer
		glState = GLS_DEPTHMASK | GLS_COLORMASK | GLS_ALPHAMASK | GLS_DEPTHFUNC_LESS;
	}

	GL_PolygonOffset( r_shadowPolygonFactor.GetFloat(), -r_shadowPolygonOffset.GetFloat() );

	// the actual stencil func will be set in the draw code, but we need to make sure it isn't
	// disabled here, and that the value will get reset for the interactions without looking
	// like a no-change-required
	GL_State( glState | GLS_STENCIL_OP_FAIL_KEEP | GLS_STENCIL_OP_ZFAIL_KEEP | GLS_STENCIL_OP_PASS_INCR | 
		GLS_STENCIL_MAKE_REF( STENCIL_SHADOW_TEST_VALUE ) | GLS_STENCIL_MAKE_MASK( STENCIL_SHADOW_MASK_VALUE ) | GLS_POLYGON_OFFSET );

	// Two Sided Stencil reduces two draw calls to one for slightly faster shadows
	GL_Cull( CT_TWO_SIDED );


	// process the chain of shadows with the current rendering state
	backEnd.currentSpace = NULL;

	for ( const drawSurf_t * drawSurf = drawSurfs; drawSurf != NULL; drawSurf = drawSurf->nextOnLight ) {
		if ( drawSurf->scissorRect.IsEmpty() ) {
			continue;	// !@# FIXME: find out why this is sometimes being hit!
						// temporarily jump over the scissor and draw so the gl error callback doesn't get hit
		}

		// make sure the shadow volume is done
		if ( drawSurf->shadowVolumeState != SHADOWVOLUME_DONE ) {
			assert( drawSurf->shadowVolumeState == SHADOWVOLUME_UNFINISHED || drawSurf->shadowVolumeState == SHADOWVOLUME_DONE );

			uint64 start = Sys_Microseconds();
			while ( drawSurf->shadowVolumeState == SHADOWVOLUME_UNFINISHED ) {
				Sys_Yield();
			}
			uint64 end = Sys_Microseconds();

			backEnd.pc.shadowMicroSec += end - start;
		}

		if ( drawSurf->numIndexes == 0 ) {
			continue;	// a job may have created an empty shadow volume
		}

		if ( !backEnd.currentScissor.Equals( drawSurf->scissorRect ) && r_useScissor.GetBool() ) {
			// change the scissor
			GL_Scissor( backEnd.viewDef->viewport.x1 + drawSurf->scissorRect.x1,
						backEnd.viewDef->viewport.y1 + drawSurf->scissorRect.y1,
						drawSurf->scissorRect.x2 + 1 - drawSurf->scissorRect.x1,
						drawSurf->scissorRect.y2 + 1 - drawSurf->scissorRect.y1 );
			backEnd.currentScissor = drawSurf->scissorRect;
		}

		if ( drawSurf->space != backEnd.currentSpace ) {
			// change the matrix
			RB_SetMVP( drawSurf->space->mvp );

			// set the local light position to allow the vertex program to project the shadow volume end cap to infinity
			idVec4 localLight( 0.0f );
			R_GlobalPointToLocal( drawSurf->space->modelMatrix, vLight->globalLightOrigin, localLight.ToVec3() );
			SetVertexParm( RENDERPARM_LOCALLIGHTORIGIN, localLight.ToFloatPtr() );

			backEnd.currentSpace = drawSurf->space;
		}

		if ( r_showShadows.GetInteger() == 0 ) {
			if ( drawSurf->jointCache ) {
				renderProgManager.BindShader_ShadowSkinned();
			} else {
				renderProgManager.BindShader_Shadow();
			}
		} else {
			if ( drawSurf->jointCache ) {
				renderProgManager.BindShader_ShadowDebugSkinned();
			} else {
				renderProgManager.BindShader_ShadowDebug();
			}
		}

		// set depth bounds per shadow
		if ( r_useShadowDepthBounds.GetBool() ) {
			GL_DepthBoundsTest( drawSurf->scissorRect.zmin, drawSurf->scissorRect.zmax );
		}

		// Determine whether or not the shadow volume needs to be rendered with Z-pass or
		// Z-fail. It is worthwhile to spend significant resources to reduce the number of
		// cases where shadow volumes need to be rendered with Z-fail because Z-fail
		// rendering can be significantly slower even on today's hardware. For instance,
		// on NVIDIA hardware Z-fail rendering causes the Z-Cull to be used in reverse:
		// Z-near becomes Z-far (trivial accept becomes trivial reject). Using the Z-Cull
		// in reverse is far less efficient because the Z-Cull only stores Z-near per 16x16
		// pixels while the Z-far is stored per 4x2 pixels. (The Z-near coallesce buffer
		// which has 4x4 granularity is only used when updating the depth which is not the
		// case for shadow volumes.) Note that it is also important to NOT use a Z-Cull
		// reconstruct because that would clear the Z-near of the Z-Cull which results in
		// no trivial rejection for Z-fail stencil shadow rendering.

		const bool renderZPass = ( drawSurf->renderZFail == 0 ) || r_forceZPassStencilShadows.GetBool();


		if ( renderZPass ) {
			// Z-pass
			qglStencilOpSeparate( GL_FRONT, GL_KEEP, GL_KEEP, GL_INCR );
			qglStencilOpSeparate( GL_BACK, GL_KEEP, GL_KEEP, GL_DECR );
		} else if ( r_useStencilShadowPreload.GetBool() ) {
			// preload + Z-pass
			qglStencilOpSeparate( GL_FRONT, GL_KEEP, GL_DECR, GL_DECR );
			qglStencilOpSeparate( GL_BACK, GL_KEEP, GL_INCR, GL_INCR );
		} else {
			// Z-fail
		}


		// get vertex buffer
		const vertCacheHandle_t vbHandle = drawSurf->shadowCache;
		idVertexBuffer * vertexBuffer;
		if ( vertexCache.CacheIsStatic( vbHandle ) ) {
			vertexBuffer = &vertexCache.staticData.vertexBuffer;
		} else {
			const uint64 frameNum = (int)( vbHandle >> VERTCACHE_FRAME_SHIFT ) & VERTCACHE_FRAME_MASK;
			if ( frameNum != ( ( vertexCache.currentFrame - 1 ) & VERTCACHE_FRAME_MASK ) ) {
				idLib::Warning( "RB_DrawElementsWithCounters, vertexBuffer == NULL" );
				continue;
			}
			vertexBuffer = &vertexCache.frameData[vertexCache.drawListNum].vertexBuffer;
		}
		const int vertOffset = (int)( vbHandle >> VERTCACHE_OFFSET_SHIFT ) & VERTCACHE_OFFSET_MASK;

		// get index buffer
		const vertCacheHandle_t ibHandle = drawSurf->indexCache;
		idIndexBuffer * indexBuffer;
		if ( vertexCache.CacheIsStatic( ibHandle ) ) {
			indexBuffer = &vertexCache.staticData.indexBuffer;
		} else {
			const uint64 frameNum = (int)( ibHandle >> VERTCACHE_FRAME_SHIFT ) & VERTCACHE_FRAME_MASK;
			if ( frameNum != ( ( vertexCache.currentFrame - 1 ) & VERTCACHE_FRAME_MASK ) ) {
				idLib::Warning( "RB_DrawElementsWithCounters, indexBuffer == NULL" );
				continue;
			}
			indexBuffer = &vertexCache.frameData[vertexCache.drawListNum].indexBuffer;
		}
		const uint64 indexOffset = (int)( ibHandle >> VERTCACHE_OFFSET_SHIFT ) & VERTCACHE_OFFSET_MASK;

		RENDERLOG_PRINTF( "Binding Buffers: %p %p\n", vertexBuffer, indexBuffer );


		if ( backEnd.glState.currentIndexBuffer != (GLuint)indexBuffer->GetAPIObject() || !r_useStateCaching.GetBool() ) {
			qglBindBufferARB( GL_ELEMENT_ARRAY_BUFFER_ARB, (GLuint)indexBuffer->GetAPIObject() );
			backEnd.glState.currentIndexBuffer = (GLuint)indexBuffer->GetAPIObject();
		}

		if ( drawSurf->jointCache ) {
			assert( renderProgManager.ShaderUsesJoints() );

			idJointBuffer jointBuffer;
			if ( !vertexCache.GetJointBuffer( drawSurf->jointCache, &jointBuffer ) ) {
				idLib::Warning( "RB_DrawElementsWithCounters, jointBuffer == NULL" );
				continue;
			}
			assert( ( jointBuffer.GetOffset() & ( glConfig.uniformBufferOffsetAlignment - 1 ) ) == 0 );

			const GLuint ubo = reinterpret_cast< GLuint >( jointBuffer.GetAPIObject() );
			qglBindBufferRange( GL_UNIFORM_BUFFER, 0, ubo, jointBuffer.GetOffset(), jointBuffer.GetNumJoints() * sizeof( idJointMat ) );

			if ( ( backEnd.glState.vertexLayout != LAYOUT_DRAW_SHADOW_VERT_SKINNED) || ( backEnd.glState.currentVertexBuffer != (GLuint)vertexBuffer->GetAPIObject() ) || !r_useStateCaching.GetBool() ) {
				qglBindBufferARB( GL_ARRAY_BUFFER_ARB, (GLuint)vertexBuffer->GetAPIObject() );
				backEnd.glState.currentVertexBuffer = (GLuint)vertexBuffer->GetAPIObject();

				qglEnableVertexAttribArrayARB( PC_ATTRIB_INDEX_VERTEX );
				qglDisableVertexAttribArrayARB( PC_ATTRIB_INDEX_NORMAL );
				qglEnableVertexAttribArrayARB( PC_ATTRIB_INDEX_COLOR );
				qglEnableVertexAttribArrayARB( PC_ATTRIB_INDEX_COLOR2 );
				qglDisableVertexAttribArrayARB( PC_ATTRIB_INDEX_ST );
				qglDisableVertexAttribArrayARB( PC_ATTRIB_INDEX_TANGENT );

				qglVertexAttribPointerARB( PC_ATTRIB_INDEX_VERTEX, 4, GL_FLOAT, GL_FALSE, sizeof( idShadowVertSkinned ), (void *)( SHADOWVERTSKINNED_XYZW_OFFSET ) );
				qglVertexAttribPointerARB( PC_ATTRIB_INDEX_COLOR, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof( idShadowVertSkinned ), (void *)( SHADOWVERTSKINNED_COLOR_OFFSET ) );
				qglVertexAttribPointerARB( PC_ATTRIB_INDEX_COLOR2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof( idShadowVertSkinned ), (void *)( SHADOWVERTSKINNED_COLOR2_OFFSET ) );

				backEnd.glState.vertexLayout = LAYOUT_DRAW_SHADOW_VERT_SKINNED;
			}

		} else {

			if ( ( backEnd.glState.vertexLayout != LAYOUT_DRAW_SHADOW_VERT ) || ( backEnd.glState.currentVertexBuffer != (GLuint)vertexBuffer->GetAPIObject() ) || !r_useStateCaching.GetBool() ) {
				qglBindBufferARB( GL_ARRAY_BUFFER_ARB, (GLuint)vertexBuffer->GetAPIObject() );
				backEnd.glState.currentVertexBuffer = (GLuint)vertexBuffer->GetAPIObject();

				qglEnableVertexAttribArrayARB( PC_ATTRIB_INDEX_VERTEX );
				qglDisableVertexAttribArrayARB( PC_ATTRIB_INDEX_NORMAL );
				qglDisableVertexAttribArrayARB( PC_ATTRIB_INDEX_COLOR );
				qglDisableVertexAttribArrayARB( PC_ATTRIB_INDEX_COLOR2 );
				qglDisableVertexAttribArrayARB( PC_ATTRIB_INDEX_ST );
				qglDisableVertexAttribArrayARB( PC_ATTRIB_INDEX_TANGENT );

				qglVertexAttribPointerARB( PC_ATTRIB_INDEX_VERTEX, 4, GL_FLOAT, GL_FALSE, sizeof( idShadowVert ), (void *)( SHADOWVERT_XYZW_OFFSET ) );

				backEnd.glState.vertexLayout = LAYOUT_DRAW_SHADOW_VERT;
			}
		}

		renderProgManager.CommitUniforms();

		if ( drawSurf->jointCache ) {
			qglDrawElementsBaseVertex( GL_TRIANGLES, r_singleTriangle.GetBool() ? 3 : drawSurf->numIndexes, GL_INDEX_TYPE, (triIndex_t *)indexOffset, vertOffset / sizeof( idShadowVertSkinned ) );
		} else {
			qglDrawElementsBaseVertex( GL_TRIANGLES, r_singleTriangle.GetBool() ? 3 : drawSurf->numIndexes, GL_INDEX_TYPE, (triIndex_t *)indexOffset, vertOffset / sizeof( idShadowVert ) );
		}

		if ( !renderZPass && r_useStencilShadowPreload.GetBool() ) {
			// render again with Z-pass
			qglStencilOpSeparate( GL_FRONT, GL_KEEP, GL_KEEP, GL_INCR );
			qglStencilOpSeparate( GL_BACK, GL_KEEP, GL_KEEP, GL_DECR );

			if ( drawSurf->jointCache ) {
				qglDrawElementsBaseVertex( GL_TRIANGLES, r_singleTriangle.GetBool() ? 3 : drawSurf->numIndexes, GL_INDEX_TYPE, (triIndex_t *)indexOffset, vertOffset / sizeof ( idShadowVertSkinned ) );
			} else {
				qglDrawElementsBaseVertex( GL_TRIANGLES, r_singleTriangle.GetBool() ? 3 : drawSurf->numIndexes, GL_INDEX_TYPE, (triIndex_t *)indexOffset, vertOffset / sizeof ( idShadowVert ) );
			}
		}
	}

	// cleanup the shadow specific rendering state

	GL_Cull( CT_FRONT_SIDED );

	// reset depth bounds
	if ( r_useShadowDepthBounds.GetBool() ) {
		if ( r_useLightDepthBounds.GetBool() ) {
			GL_DepthBoundsTest( vLight->scissorRect.zmin, vLight->scissorRect.zmax );
		} else {
			GL_DepthBoundsTest( 0.0f, 0.0f );
		}
	}
}

/*
==================
RB_StencilSelectLight

Deform the zeroOneCubeModel to exactly cover the light volume. Render the deformed cube model to the stencil buffer in
such a way that only fragments that are directly visible and contained within the volume will be written creating a 
mask to be used by the following stencil shadow and draw interaction passes.
==================
*/
static void RB_StencilSelectLight( const viewLight_t * vLight ) {
	renderLog.OpenBlock( "Stencil Select" );

	// enable the light scissor
	if ( !backEnd.currentScissor.Equals( vLight->scissorRect ) && r_useScissor.GetBool() ) {
		GL_Scissor( backEnd.viewDef->viewport.x1 + vLight->scissorRect.x1, 
					backEnd.viewDef->viewport.y1 + vLight->scissorRect.y1,
					vLight->scissorRect.x2 + 1 - vLight->scissorRect.x1,
					vLight->scissorRect.y2 + 1 - vLight->scissorRect.y1 );
		backEnd.currentScissor = vLight->scissorRect;
	}

	// clear stencil buffer to 0 (not drawable)
	uint64 glStateMinusStencil = GL_GetCurrentStateMinusStencil();
	GL_State( glStateMinusStencil | GLS_STENCIL_FUNC_ALWAYS | GLS_STENCIL_MAKE_REF( STENCIL_SHADOW_TEST_VALUE ) | GLS_STENCIL_MAKE_MASK( STENCIL_SHADOW_MASK_VALUE ) );	// make sure stencil mask passes for the clear
	GL_Clear( false, false, true, 0, 0.0f, 0.0f, 0.0f, 0.0f );	// clear to 0 for stencil select

	// set the depthbounds
	GL_DepthBoundsTest( vLight->scissorRect.zmin, vLight->scissorRect.zmax );


	GL_State( GLS_COLORMASK | GLS_ALPHAMASK | GLS_DEPTHMASK | GLS_DEPTHFUNC_LESS | GLS_STENCIL_FUNC_ALWAYS | GLS_STENCIL_MAKE_REF( STENCIL_SHADOW_TEST_VALUE ) | GLS_STENCIL_MAKE_MASK( STENCIL_SHADOW_MASK_VALUE ) );
	GL_Cull( CT_TWO_SIDED );

	renderProgManager.BindShader_Depth();

	// set the matrix for deforming the 'zeroOneCubeModel' into the frustum to exactly cover the light volume
	idRenderMatrix invProjectMVPMatrix;
	idRenderMatrix::Multiply( backEnd.viewDef->worldSpace.mvp, vLight->inverseBaseLightProject, invProjectMVPMatrix );
	RB_SetMVP( invProjectMVPMatrix );

	// two-sided stencil test
	qglStencilOpSeparate( GL_FRONT, GL_KEEP, GL_REPLACE, GL_ZERO );
	qglStencilOpSeparate( GL_BACK, GL_KEEP, GL_ZERO, GL_REPLACE );

	RB_DrawElementsWithCounters( &backEnd.zeroOneCubeSurface );

	// reset stencil state

	GL_Cull( CT_FRONT_SIDED );

	renderProgManager.Unbind();


	// unset the depthbounds
	GL_DepthBoundsTest( 0.0f, 0.0f );

	renderLog.CloseBlock();
}

/*
==============================================================================================

DRAW INTERACTIONS

==============================================================================================
*/
/*
==================
RB_DrawInteractions
==================
*/
static void RB_DrawInteractions() {
	if ( r_skipInteractions.GetBool() ) {
		return;
	}

	renderLog.OpenMainBlock( MRB_DRAW_INTERACTIONS );
	renderLog.OpenBlock( "RB_DrawInteractions" );

	GL_SelectTexture( 0 );


	const bool useLightDepthBounds = r_useLightDepthBounds.GetBool();

	//
	// for each light, perform shadowing and adding
	//
	for ( const viewLight_t * vLight = backEnd.viewDef->viewLights; vLight != NULL; vLight = vLight->next ) {
		// do fogging later
		if ( vLight->lightShader->IsFogLight() ) {
			continue;
		}
		if ( vLight->lightShader->IsBlendLight() ) {
			continue;
		}

		if ( vLight->localInteractions == NULL && vLight->globalInteractions == NULL && vLight->translucentInteractions == NULL ) {
			continue;
		}

		const idMaterial * lightShader = vLight->lightShader;
		renderLog.OpenBlock( lightShader->GetName() );

		// set the depth bounds for the whole light
		if ( useLightDepthBounds ) {
			GL_DepthBoundsTest( vLight->scissorRect.zmin, vLight->scissorRect.zmax );
		}

		// only need to clear the stencil buffer and perform stencil testing if there are shadows
		const bool performStencilTest = ( vLight->globalShadows != NULL || vLight->localShadows != NULL );

		// mirror flips the sense of the stencil select, and I don't want to risk accidentally breaking it
		// in the normal case, so simply disable the stencil select in the mirror case
		const bool useLightStencilSelect = ( r_useLightStencilSelect.GetBool() && backEnd.viewDef->isMirror == false );

		if ( performStencilTest ) {
			if ( useLightStencilSelect ) {
				// write a stencil mask for the visible light bounds to hi-stencil
				RB_StencilSelectLight( vLight );
			} else {
				// always clear whole S-Cull tiles
				idScreenRect rect;
				rect.x1 = ( vLight->scissorRect.x1 +  0 ) & ~15;
				rect.y1 = ( vLight->scissorRect.y1 +  0 ) & ~15;
				rect.x2 = ( vLight->scissorRect.x2 + 15 ) & ~15;
				rect.y2 = ( vLight->scissorRect.y2 + 15 ) & ~15;

				if ( !backEnd.currentScissor.Equals( rect ) && r_useScissor.GetBool() ) {
					GL_Scissor( backEnd.viewDef->viewport.x1 + rect.x1,
								backEnd.viewDef->viewport.y1 + rect.y1,
								rect.x2 + 1 - rect.x1,
								rect.y2 + 1 - rect.y1 );
					backEnd.currentScissor = rect;
				}
				GL_State( GLS_DEFAULT );	// make sure stencil mask passes for the clear
				GL_Clear( false, false, true, STENCIL_SHADOW_TEST_VALUE, 0.0f, 0.0f, 0.0f, 0.0f );
			}
		}

		if ( vLight->globalShadows != NULL ) {
			renderLog.OpenBlock( "Global Light Shadows" );
			RB_StencilShadowPass( vLight->globalShadows, vLight );
			renderLog.CloseBlock();
		}

		if ( vLight->localInteractions != NULL ) {
			renderLog.OpenBlock( "Local Light Interactions" );
			RB_RenderInteractions( vLight->localInteractions, vLight, GLS_DEPTHFUNC_EQUAL, performStencilTest, useLightDepthBounds );
			renderLog.CloseBlock();
		}

		if ( vLight->localShadows != NULL ) {
			renderLog.OpenBlock( "Local Light Shadows" );
			RB_StencilShadowPass( vLight->localShadows, vLight );
			renderLog.CloseBlock();
		}

		if ( vLight->globalInteractions != NULL ) {
			renderLog.OpenBlock( "Global Light Interactions" );
			RB_RenderInteractions( vLight->globalInteractions, vLight, GLS_DEPTHFUNC_EQUAL, performStencilTest, useLightDepthBounds );
			renderLog.CloseBlock();
		}


		if ( vLight->translucentInteractions != NULL && !r_skipTranslucent.GetBool() ) {
			renderLog.OpenBlock( "Translucent Interactions" );

			// Disable the depth bounds test because translucent surfaces don't work with
			// the depth bounds tests since they did not write depth during the depth pass.
			if ( useLightDepthBounds ) {
				GL_DepthBoundsTest( 0.0f, 0.0f );
			}

			// The depth buffer wasn't filled in for translucent surfaces, so they
			// can never be constrained to perforated surfaces with the depthfunc equal.

			// Translucent surfaces do not receive shadows. This is a case where a
			// shadow buffer solution would work but stencil shadows do not because
			// stencil shadows only affect surfaces that contribute to the view depth
			// buffer and translucent surfaces do not contribute to the view depth buffer.

			RB_RenderInteractions( vLight->translucentInteractions, vLight, GLS_DEPTHFUNC_LESS, false, false );

			renderLog.CloseBlock();
		}

		renderLog.CloseBlock();
	}

	// disable stencil shadow test
	GL_State( GLS_DEFAULT );

	// unbind texture units
	for ( int i = 0; i < 5; i++ ) {
		GL_SelectTexture( i );
		globalImages->BindNull();
	}
	GL_SelectTexture( 0 );

	// reset depth bounds
	if ( useLightDepthBounds ) {
		GL_DepthBoundsTest( 0.0f, 0.0f );
	}

	renderLog.CloseBlock();
	renderLog.CloseMainBlock();
}

/*
=============================================================================================

NON-INTERACTION SHADER PASSES

=============================================================================================
*/

/*
=====================
RB_DrawShaderPasses

Draw non-light dependent passes

If we are rendering Guis, the drawSurf_t::sort value is a depth offset that can
be multiplied by guiEye for polarity and screenSeparation for scale.
=====================
*/
static int RB_DrawShaderPasses( const drawSurf_t * const * const drawSurfs, const int numDrawSurfs, 
									const float guiStereoScreenOffset, const int stereoEye ) {
	// only obey skipAmbient if we are rendering a view
	if ( backEnd.viewDef->viewEntitys && r_skipAmbient.GetBool() ) {
		return numDrawSurfs;
	}

	renderLog.OpenBlock( "RB_DrawShaderPasses" );

	GL_SelectTexture( 1 );
	globalImages->BindNull();

	GL_SelectTexture( 0 );

	backEnd.currentSpace = (const viewEntity_t *)1;	// using NULL makes /analyze think surf->space needs to be checked...
	float currentGuiStereoOffset = 0.0f;

	int i = 0;
	for ( ; i < numDrawSurfs; i++ ) {
		const drawSurf_t * surf = drawSurfs[i];
		const idMaterial * shader = surf->material;

		// r_forceAmbientDiffuse: a material with only interaction stages
		// (diffusemap/bumpmap/specularmap, no explicit ambient stage) normally
		// draws nothing here and is only visible under a light. For the
		// light-less map-flythrough preview we still want its diffuse map, so
		// don't skip it for lack of an ambient stage -- the per-stage filter
		// below promotes its SL_DIFFUSE stage into this ambient pass instead.
		if ( !shader->HasAmbient() && !r_forceAmbientDiffuse.GetBool() ) {
			continue;
		}

		if ( shader->IsPortalSky() ) {
			continue;
		}

		// some deforms may disable themselves by setting numIndexes = 0
		if ( surf->numIndexes == 0 ) {
			continue;
		}

		if ( shader->SuppressInSubview() ) {
			continue;
		}

		if ( backEnd.viewDef->isXraySubview && surf->space->entityDef ) {
			if ( surf->space->entityDef->parms.xrayIndex != 2 ) {
				continue;
			}
		}

		// we need to draw the post process shaders after we have drawn the fog lights
		if ( shader->GetSort() >= SS_POST_PROCESS && !backEnd.currentRenderCopied ) {
			break;
		}

		// if we are rendering a 3D view and the surface's eye index doesn't match 
		// the current view's eye index then we skip the surface
		// if the stereoEye value of a surface is 0 then we need to draw it for both eyes.
		const int shaderStereoEye = shader->GetStereoEye();
		const bool isEyeValid = stereoRender_swapEyes.GetBool() ? ( shaderStereoEye == stereoEye ) : ( shaderStereoEye != stereoEye );
		if ( ( stereoEye != 0 ) && ( shaderStereoEye != 0 ) && ( isEyeValid ) ) {
			continue;
		}

		renderLog.OpenBlock( shader->GetName() );

		// determine the stereoDepth offset 
		// guiStereoScreenOffset will always be zero for 3D views, so the !=
		// check will never force an update due to the current sort value.
		const float thisGuiStereoOffset = guiStereoScreenOffset * surf->sort;

		// change the matrix and other space related vars if needed
		if ( surf->space != backEnd.currentSpace || thisGuiStereoOffset != currentGuiStereoOffset ) {
			backEnd.currentSpace = surf->space;
			currentGuiStereoOffset = thisGuiStereoOffset;

			const viewEntity_t *space = backEnd.currentSpace;

			if ( guiStereoScreenOffset != 0.0f ) {
				RB_SetMVPWithStereoOffset( space->mvp, currentGuiStereoOffset );
			} else {
				RB_SetMVP( space->mvp );
			}

			// set eye position in local space
			idVec4 localViewOrigin( 1.0f );
			R_GlobalPointToLocal( space->modelMatrix, backEnd.viewDef->renderView.vieworg, localViewOrigin.ToVec3() );
			SetVertexParm( RENDERPARM_LOCALVIEWORIGIN, localViewOrigin.ToFloatPtr() );

			// set model Matrix
			float modelMatrixTranspose[16];
			R_MatrixTranspose( space->modelMatrix, modelMatrixTranspose );
			SetVertexParms( RENDERPARM_MODELMATRIX_X, modelMatrixTranspose, 4 );

			// Set ModelView Matrix
			float modelViewMatrixTranspose[16];
			R_MatrixTranspose( space->modelViewMatrix, modelViewMatrixTranspose );
			SetVertexParms( RENDERPARM_MODELVIEWMATRIX_X, modelViewMatrixTranspose, 4 );
		}

		// change the scissor if needed
		if ( !backEnd.currentScissor.Equals( surf->scissorRect ) && r_useScissor.GetBool() ) {
			GL_Scissor( backEnd.viewDef->viewport.x1 + surf->scissorRect.x1, 
						backEnd.viewDef->viewport.y1 + surf->scissorRect.y1,
						surf->scissorRect.x2 + 1 - surf->scissorRect.x1,
						surf->scissorRect.y2 + 1 - surf->scissorRect.y1 );
			backEnd.currentScissor = surf->scissorRect;
		}

		// get the expressions for conditionals / color / texcoords
		const float	*regs = surf->shaderRegisters;

		// set face culling appropriately
		if ( surf->space->isGuiSurface ) {
			GL_Cull( CT_TWO_SIDED );
		} else {
			GL_Cull( shader->GetCullType() );
		}

		uint64 surfGLState = surf->extraGLState;

		// set polygon offset if necessary
		if ( shader->TestMaterialFlag(MF_POLYGONOFFSET) ) {
			GL_PolygonOffset( r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * shader->GetPolygonOffset() );
			surfGLState = GLS_POLYGON_OFFSET;
		}

		for ( int stage = 0; stage < shader->GetNumStages(); stage++ ) {		
			const shaderStage_t *pStage = shader->GetStage(stage);

			// check the enable condition
			if ( regs[ pStage->conditionRegister ] == 0 ) {
				continue;
			}

			// skip the stages involved in lighting.
			// r_forceAmbientDiffuse additionally promotes the SL_DIFFUSE stage
			// into this non-light ambient pass so it draws as a plain textured,
			// full-bright (color = white, drawStateBits = 0 = opaque) surface via
			// the old-style TextureVertexColor path below -- SL_BUMP/SL_SPECULAR
			// stay skipped (they'd sample wrong as flat color). This never routes
			// through the interaction/light path, so it can't reach R_AddSingleLight.
			if ( pStage->lighting != SL_AMBIENT &&
				 !( r_forceAmbientDiffuse.GetBool() && pStage->lighting == SL_DIFFUSE ) ) {
				continue;
			}

			uint64 stageGLState = surfGLState;
			if ( ( surfGLState & GLS_OVERRIDE ) == 0 ) {
				stageGLState |= pStage->drawStateBits;
			}

			// skip if the stage is ( GL_ZERO, GL_ONE ), which is used for some alpha masks
			if ( ( stageGLState & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS ) ) == ( GLS_SRCBLEND_ZERO | GLS_DSTBLEND_ONE ) ) {
				continue;
			}

			// see if we are a new-style stage
			newShaderStage_t *newStage = pStage->newStage;
			if ( newStage != NULL ) {
				//--------------------------
				//
				// new style stages
				//
				//--------------------------
				if ( r_skipNewAmbient.GetBool() ) {
					continue;
				}
				renderLog.OpenBlock( "New Shader Stage" );

				GL_State( stageGLState );
			
				renderProgManager.BindShader( newStage->glslProgram, newStage->glslProgram );

				for ( int j = 0; j < newStage->numVertexParms; j++ ) {
					float parm[4];
					parm[0] = regs[ newStage->vertexParms[j][0] ];
					parm[1] = regs[ newStage->vertexParms[j][1] ];
					parm[2] = regs[ newStage->vertexParms[j][2] ];
					parm[3] = regs[ newStage->vertexParms[j][3] ];
					SetVertexParm( (renderParm_t)( RENDERPARM_USER + j ), parm );
				}

				// set rpEnableSkinning if the shader has optional support for skinning
				if ( surf->jointCache && renderProgManager.ShaderHasOptionalSkinning() ) {
					const idVec4 skinningParm( 1.0f );
					SetVertexParm( RENDERPARM_ENABLE_SKINNING, skinningParm.ToFloatPtr() );
				}

				// bind texture units
				for ( int j = 0; j < newStage->numFragmentProgramImages; j++ ) {
					idImage * image = newStage->fragmentProgramImages[j];
					if ( image != NULL ) {
						GL_SelectTexture( j );
						image->Bind();
					}
				}

				// draw it
				RB_DrawElementsWithCounters( surf );

				// unbind texture units
				for ( int j = 0; j < newStage->numFragmentProgramImages; j++ ) {
					idImage * image = newStage->fragmentProgramImages[j];
					if ( image != NULL ) {
						GL_SelectTexture( j );
						globalImages->BindNull();
					}
				}

				// clear rpEnableSkinning if it was set
				if ( surf->jointCache && renderProgManager.ShaderHasOptionalSkinning() ) {
					const idVec4 skinningParm( 0.0f );
					SetVertexParm( RENDERPARM_ENABLE_SKINNING, skinningParm.ToFloatPtr() );
				}

				GL_SelectTexture( 0 );
				renderProgManager.Unbind();

				renderLog.CloseBlock();
				continue;
			}

			//--------------------------
			//
			// old style stages
			//
			//--------------------------

			// set the color
			float color[4];
			color[0] = regs[ pStage->color.registers[0] ];
			color[1] = regs[ pStage->color.registers[1] ];
			color[2] = regs[ pStage->color.registers[2] ];
			color[3] = regs[ pStage->color.registers[3] ];

			// skip the entire stage if an add would be black
			if ( ( stageGLState & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS ) ) == ( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE ) 
				&& color[0] <= 0 && color[1] <= 0 && color[2] <= 0 ) {
				continue;
			}

			// skip the entire stage if a blend would be completely transparent
			if ( ( stageGLState & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS ) ) == ( GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA )
				&& color[3] <= 0 ) {
				continue;
			}

			stageVertexColor_t svc = pStage->vertexColor;

			renderLog.OpenBlock( "Old Shader Stage" );
			GL_Color( color );

			if ( surf->space->isGuiSurface ) {
				// Force gui surfaces to always be SVC_MODULATE
				svc = SVC_MODULATE;

				// use special shaders for bink cinematics
				if ( pStage->texture.cinematic ) {
					if ( ( stageGLState & GLS_OVERRIDE ) != 0 ) {
						// This is a hack... Only SWF Guis set GLS_OVERRIDE
						// Old style guis do not, and we don't want them to use the new GUI renederProg
						renderProgManager.BindShader_BinkGUI();
					} else {
						renderProgManager.BindShader_Bink();
					}
				} else {
					if ( ( stageGLState & GLS_OVERRIDE ) != 0 ) {
						// This is a hack... Only SWF Guis set GLS_OVERRIDE
						// Old style guis do not, and we don't want them to use the new GUI renderProg
						renderProgManager.BindShader_GUI();
					} else {
						if ( surf->jointCache ) {
							renderProgManager.BindShader_TextureVertexColorSkinned();
						} else {
							renderProgManager.BindShader_TextureVertexColor();
						}
					}
				}
			} else if ( ( pStage->texture.texgen == TG_SCREEN ) || ( pStage->texture.texgen == TG_SCREEN2 ) ) {
				renderProgManager.BindShader_TextureTexGenVertexColor();
			} else if ( pStage->texture.cinematic ) {
				renderProgManager.BindShader_Bink();
			} else {
				if ( surf->jointCache ) {
					renderProgManager.BindShader_TextureVertexColorSkinned();
				} else {
					renderProgManager.BindShader_TextureVertexColor();
				}
			}
		
			RB_SetVertexColorParms( svc );

			// bind the texture
			RB_BindVariableStageImage( &pStage->texture, regs );

			// set privatePolygonOffset if necessary
			if ( pStage->privatePolygonOffset ) {
				GL_PolygonOffset( r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * pStage->privatePolygonOffset );
				stageGLState |= GLS_POLYGON_OFFSET;
			}

			// set the state
			GL_State( stageGLState );
		
			RB_PrepareStageTexturing( pStage, surf );

			// draw it
			RB_DrawElementsWithCounters( surf );

			RB_FinishStageTexturing( pStage, surf );

			// unset privatePolygonOffset if necessary
			if ( pStage->privatePolygonOffset ) {
				GL_PolygonOffset( r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * shader->GetPolygonOffset() );
			}
			renderLog.CloseBlock();
		}

		renderLog.CloseBlock();
	}

	GL_Cull( CT_FRONT_SIDED );
	GL_Color( 1.0f, 1.0f, 1.0f );

	renderLog.CloseBlock();
	return i;
}

/*
=============================================================================================

BLEND LIGHT PROJECTION

=============================================================================================
*/

/*
=====================
RB_T_BlendLight
=====================
*/
static void RB_T_BlendLight( const drawSurf_t *drawSurfs, const viewLight_t * vLight ) {
	backEnd.currentSpace = NULL;

	for ( const drawSurf_t * drawSurf = drawSurfs; drawSurf != NULL; drawSurf = drawSurf->nextOnLight ) {
		if ( drawSurf->scissorRect.IsEmpty() ) {
			continue;	// !@# FIXME: find out why this is sometimes being hit!
						// temporarily jump over the scissor and draw so the gl error callback doesn't get hit
		}

		if ( !backEnd.currentScissor.Equals( drawSurf->scissorRect ) && r_useScissor.GetBool() ) {
			// change the scissor
			GL_Scissor( backEnd.viewDef->viewport.x1 + drawSurf->scissorRect.x1,
						backEnd.viewDef->viewport.y1 + drawSurf->scissorRect.y1,
						drawSurf->scissorRect.x2 + 1 - drawSurf->scissorRect.x1,
						drawSurf->scissorRect.y2 + 1 - drawSurf->scissorRect.y1 );
			backEnd.currentScissor = drawSurf->scissorRect;
		}

		if ( drawSurf->space != backEnd.currentSpace ) {
			// change the matrix
			RB_SetMVP( drawSurf->space->mvp );

			// change the light projection matrix
			idPlane	lightProjectInCurrentSpace[4];
			for ( int i = 0; i < 4; i++ ) {
				R_GlobalPlaneToLocal( drawSurf->space->modelMatrix, vLight->lightProject[i], lightProjectInCurrentSpace[i] );
			}

			SetVertexParm( RENDERPARM_TEXGEN_0_S, lightProjectInCurrentSpace[0].ToFloatPtr() );
			SetVertexParm( RENDERPARM_TEXGEN_0_T, lightProjectInCurrentSpace[1].ToFloatPtr() );
			SetVertexParm( RENDERPARM_TEXGEN_0_Q, lightProjectInCurrentSpace[2].ToFloatPtr() );
			SetVertexParm( RENDERPARM_TEXGEN_1_S, lightProjectInCurrentSpace[3].ToFloatPtr() );	// falloff

			backEnd.currentSpace = drawSurf->space;
		}

		RB_DrawElementsWithCounters( drawSurf );
	}
}

/*
=====================
RB_BlendLight

Dual texture together the falloff and projection texture with a blend
mode to the framebuffer, instead of interacting with the surface texture
=====================
*/
static void RB_BlendLight( const drawSurf_t *drawSurfs, const drawSurf_t *drawSurfs2, const viewLight_t * vLight ) {
	if ( drawSurfs == NULL ) {
		return;
	}
	if ( r_skipBlendLights.GetBool() ) {
		return;
	}
	renderLog.OpenBlock( vLight->lightShader->GetName() );

	const idMaterial * lightShader = vLight->lightShader;
	const float	* regs = vLight->shaderRegisters;

	// texture 1 will get the falloff texture
	GL_SelectTexture( 1 );
	vLight->falloffImage->Bind();

	// texture 0 will get the projected texture
	GL_SelectTexture( 0 );

	renderProgManager.BindShader_BlendLight();

	for ( int i = 0; i < lightShader->GetNumStages(); i++ ) {
		const shaderStage_t	*stage = lightShader->GetStage(i);

		if ( !regs[ stage->conditionRegister ] ) {
			continue;
		}

		GL_State( GLS_DEPTHMASK | stage->drawStateBits | GLS_DEPTHFUNC_EQUAL );

		GL_SelectTexture( 0 );
		stage->texture.image->Bind();

		if ( stage->texture.hasMatrix ) {
			RB_LoadShaderTextureMatrix( regs, &stage->texture );
		}

		// get the modulate values from the light, including alpha, unlike normal lights
		float lightColor[4];
		lightColor[0] = regs[ stage->color.registers[0] ];
		lightColor[1] = regs[ stage->color.registers[1] ];
		lightColor[2] = regs[ stage->color.registers[2] ];
		lightColor[3] = regs[ stage->color.registers[3] ];
		GL_Color( lightColor );

		RB_T_BlendLight( drawSurfs, vLight );
		RB_T_BlendLight( drawSurfs2, vLight );
	}

	GL_SelectTexture( 1 );
	globalImages->BindNull();

	GL_SelectTexture( 0 );

	renderProgManager.Unbind();
	renderLog.CloseBlock();
}

/*
=========================================================================================================

FOG LIGHTS

=========================================================================================================
*/

/*
=====================
RB_T_BasicFog
=====================
*/
static void RB_T_BasicFog( const drawSurf_t *drawSurfs, const idPlane fogPlanes[4], const idRenderMatrix * inverseBaseLightProject ) {
	backEnd.currentSpace = NULL;

	for ( const drawSurf_t * drawSurf = drawSurfs; drawSurf != NULL; drawSurf = drawSurf->nextOnLight ) {
		if ( drawSurf->scissorRect.IsEmpty() ) {
			continue;	// !@# FIXME: find out why this is sometimes being hit!
						// temporarily jump over the scissor and draw so the gl error callback doesn't get hit
		}

		if ( !backEnd.currentScissor.Equals( drawSurf->scissorRect ) && r_useScissor.GetBool() ) {
			// change the scissor
			GL_Scissor( backEnd.viewDef->viewport.x1 + drawSurf->scissorRect.x1,
						backEnd.viewDef->viewport.y1 + drawSurf->scissorRect.y1,
						drawSurf->scissorRect.x2 + 1 - drawSurf->scissorRect.x1,
						drawSurf->scissorRect.y2 + 1 - drawSurf->scissorRect.y1 );
			backEnd.currentScissor = drawSurf->scissorRect;
		}

		if ( drawSurf->space != backEnd.currentSpace ) {
			idPlane localFogPlanes[4];
			if ( inverseBaseLightProject == NULL ) {
				RB_SetMVP( drawSurf->space->mvp );
				for ( int i = 0; i < 4; i++ ) {
					R_GlobalPlaneToLocal( drawSurf->space->modelMatrix, fogPlanes[i], localFogPlanes[i] );
				}
			} else {
				idRenderMatrix invProjectMVPMatrix;
				idRenderMatrix::Multiply( backEnd.viewDef->worldSpace.mvp, *inverseBaseLightProject, invProjectMVPMatrix );
				RB_SetMVP( invProjectMVPMatrix );
				for ( int i = 0; i < 4; i++ ) {
					inverseBaseLightProject->InverseTransformPlane( fogPlanes[i], localFogPlanes[i], false );
				}
			}

			SetVertexParm( RENDERPARM_TEXGEN_0_S, localFogPlanes[0].ToFloatPtr() );
			SetVertexParm( RENDERPARM_TEXGEN_0_T, localFogPlanes[1].ToFloatPtr() );
			SetVertexParm( RENDERPARM_TEXGEN_1_T, localFogPlanes[2].ToFloatPtr() );
			SetVertexParm( RENDERPARM_TEXGEN_1_S, localFogPlanes[3].ToFloatPtr() );

			backEnd.currentSpace = ( inverseBaseLightProject == NULL ) ? drawSurf->space : NULL;
		}

		if ( drawSurf->jointCache ) {
			renderProgManager.BindShader_FogSkinned();
		} else {
			renderProgManager.BindShader_Fog();
		}

		RB_DrawElementsWithCounters( drawSurf );
	}
}

/*
==================
RB_FogPass
==================
*/
static void RB_FogPass( const drawSurf_t * drawSurfs,  const drawSurf_t * drawSurfs2, const viewLight_t * vLight ) {
	renderLog.OpenBlock( vLight->lightShader->GetName() );

	// find the current color and density of the fog
	const idMaterial * lightShader = vLight->lightShader;
	const float * regs = vLight->shaderRegisters;
	// assume fog shaders have only a single stage
	const shaderStage_t * stage = lightShader->GetStage( 0 );

	float lightColor[4];
	lightColor[0] = regs[ stage->color.registers[0] ];
	lightColor[1] = regs[ stage->color.registers[1] ];
	lightColor[2] = regs[ stage->color.registers[2] ];
	lightColor[3] = regs[ stage->color.registers[3] ];

	GL_Color( lightColor );

	// calculate the falloff planes
	float a;

	// if they left the default value on, set a fog distance of 500
	if ( lightColor[3] <= 1.0f ) {
		a = -0.5f / DEFAULT_FOG_DISTANCE;
	} else {
		// otherwise, distance = alpha color
		a = -0.5f / lightColor[3];
	}

	// texture 0 is the falloff image
	GL_SelectTexture( 0 );
	globalImages->fogImage->Bind();

	// texture 1 is the entering plane fade correction
	GL_SelectTexture( 1 );
	globalImages->fogEnterImage->Bind();

	// S is based on the view origin
	const float s = vLight->fogPlane.Distance( backEnd.viewDef->renderView.vieworg );

	const float FOG_SCALE = 0.001f;

	idPlane fogPlanes[4];

	// S-0
	fogPlanes[0][0] = a * backEnd.viewDef->worldSpace.modelViewMatrix[0*4+2];
	fogPlanes[0][1] = a * backEnd.viewDef->worldSpace.modelViewMatrix[1*4+2];
	fogPlanes[0][2] = a * backEnd.viewDef->worldSpace.modelViewMatrix[2*4+2];
	fogPlanes[0][3] = a * backEnd.viewDef->worldSpace.modelViewMatrix[3*4+2] + 0.5f;

	// T-0
	fogPlanes[1][0] = 0.0f;//a * backEnd.viewDef->worldSpace.modelViewMatrix[0*4+0];
	fogPlanes[1][1] = 0.0f;//a * backEnd.viewDef->worldSpace.modelViewMatrix[1*4+0];
	fogPlanes[1][2] = 0.0f;//a * backEnd.viewDef->worldSpace.modelViewMatrix[2*4+0];
	fogPlanes[1][3] = 0.5f;//a * backEnd.viewDef->worldSpace.modelViewMatrix[3*4+0] + 0.5f;

	// T-1 will get a texgen for the fade plane, which is always the "top" plane on unrotated lights
	fogPlanes[2][0] = FOG_SCALE * vLight->fogPlane[0];
	fogPlanes[2][1] = FOG_SCALE * vLight->fogPlane[1];
	fogPlanes[2][2] = FOG_SCALE * vLight->fogPlane[2];
	fogPlanes[2][3] = FOG_SCALE * vLight->fogPlane[3] + FOG_ENTER;

	// S-1
	fogPlanes[3][0] = 0.0f;
	fogPlanes[3][1] = 0.0f;
	fogPlanes[3][2] = 0.0f;
	fogPlanes[3][3] = FOG_SCALE * s + FOG_ENTER;

	// draw it
	GL_State( GLS_DEPTHMASK | GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_DEPTHFUNC_EQUAL );
	RB_T_BasicFog( drawSurfs, fogPlanes, NULL );
	RB_T_BasicFog( drawSurfs2, fogPlanes, NULL );

	// the light frustum bounding planes aren't in the depth buffer, so use depthfunc_less instead
	// of depthfunc_equal
	GL_State( GLS_DEPTHMASK | GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_DEPTHFUNC_LESS );
	GL_Cull( CT_BACK_SIDED );

	backEnd.zeroOneCubeSurface.space = &backEnd.viewDef->worldSpace;
	backEnd.zeroOneCubeSurface.scissorRect = backEnd.viewDef->scissor;
	RB_T_BasicFog( &backEnd.zeroOneCubeSurface, fogPlanes, &vLight->inverseBaseLightProject );

	GL_Cull( CT_FRONT_SIDED );

	GL_SelectTexture( 1 );
	globalImages->BindNull();

	GL_SelectTexture( 0 );

	renderProgManager.Unbind();

	renderLog.CloseBlock();
}

/*
==================
RB_FogAllLights
==================
*/
static void RB_FogAllLights() {
	if ( r_skipFogLights.GetBool() || r_showOverDraw.GetInteger() != 0 
		 || backEnd.viewDef->isXraySubview /* don't fog in xray mode*/ ) {
		return;
	}
	renderLog.OpenMainBlock( MRB_FOG_ALL_LIGHTS );
	renderLog.OpenBlock( "RB_FogAllLights" );

	// force fog plane to recalculate
	backEnd.currentSpace = NULL;

	for ( viewLight_t * vLight = backEnd.viewDef->viewLights; vLight != NULL; vLight = vLight->next ) {
		if ( vLight->lightShader->IsFogLight() ) {
			RB_FogPass( vLight->globalInteractions, vLight->localInteractions, vLight );
		} else if ( vLight->lightShader->IsBlendLight() ) {
			RB_BlendLight( vLight->globalInteractions, vLight->localInteractions, vLight );
		}
	}

	renderLog.CloseBlock();
	renderLog.CloseMainBlock();
}

/*
=========================================================================================================

BACKEND COMMANDS

=========================================================================================================
*/

/*
==================
RB_DrawViewInternal
==================
*/
void RB_DrawViewInternal( const viewDef_t * viewDef, const int stereoEye ) {
	renderLog.OpenBlock( "RB_DrawViewInternal" );

	//-------------------------------------------------
	// guis can wind up referencing purged images that need to be loaded.
	// this used to be in the gui emit code, but now that it can be running
	// in a separate thread, it must not try to load images, so do it here.
	//-------------------------------------------------
	drawSurf_t **drawSurfs = (drawSurf_t **)&viewDef->drawSurfs[0];
	const int numDrawSurfs = viewDef->numDrawSurfs;

	for ( int i = 0; i < numDrawSurfs; i++ ) {
		const drawSurf_t * ds = viewDef->drawSurfs[ i ];
		if ( ds->material != NULL ) {
			const_cast<idMaterial *>( ds->material )->EnsureNotPurged();
		}
	}

	//-------------------------------------------------
	// RB_BeginDrawingView
	//
	// Any mirrored or portaled views have already been drawn, so prepare
	// to actually render the visible surfaces for this view
	//
	// clear the z buffer, set the projection matrix, etc
	//-------------------------------------------------

	// set the window clipping
	GL_Viewport( viewDef->viewport.x1,
		viewDef->viewport.y1,
		viewDef->viewport.x2 + 1 - viewDef->viewport.x1,
		viewDef->viewport.y2 + 1 - viewDef->viewport.y1 );

	// the scissor may be smaller than the viewport for subviews
	GL_Scissor( backEnd.viewDef->viewport.x1 + viewDef->scissor.x1,
				backEnd.viewDef->viewport.y1 + viewDef->scissor.y1,
				viewDef->scissor.x2 + 1 - viewDef->scissor.x1,
				viewDef->scissor.y2 + 1 - viewDef->scissor.y1 );
	backEnd.currentScissor = viewDef->scissor;

	backEnd.glState.faceCulling = -1;		// force face culling to set next time

	// ensures that depth writes are enabled for the depth clear
	GL_State( GLS_DEFAULT );


	// Clear the depth buffer and clear the stencil to 128 for stencil shadows as well as gui masking
	GL_Clear( false, true, true, STENCIL_SHADOW_TEST_VALUE, 0.0f, 0.0f, 0.0f, 0.0f );

	// normal face culling
	GL_Cull( CT_FRONT_SIDED );

#ifdef USE_CORE_PROFILE
	// bind one global Vertex Array Object (VAO)
	qglBindVertexArray( glConfig.global_vao );
#endif

	//------------------------------------
	// sets variables that can be used by all programs
	//------------------------------------
	{
		//
		// set eye position in global space
		//
		float parm[4];
		parm[0] = backEnd.viewDef->renderView.vieworg[0];
		parm[1] = backEnd.viewDef->renderView.vieworg[1];
		parm[2] = backEnd.viewDef->renderView.vieworg[2];
		parm[3] = 1.0f;

		SetVertexParm( RENDERPARM_GLOBALEYEPOS, parm ); // rpGlobalEyePos

		// sets overbright to make world brighter
		// This value is baked into the specularScale and diffuseScale values so
		// the interaction programs don't need to perform the extra multiply,
		// but any other renderprogs that want to obey the brightness value
		// can reference this.
		float overbright = r_lightScale.GetFloat() * 0.5f;
		parm[0] = overbright;
		parm[1] = overbright;
		parm[2] = overbright;
		parm[3] = overbright;
		SetFragmentParm( RENDERPARM_OVERBRIGHT, parm );

		// Set Projection Matrix
		float projMatrixTranspose[16];
		R_MatrixTranspose( backEnd.viewDef->projectionMatrix, projMatrixTranspose );
		SetVertexParms( RENDERPARM_PROJMATRIX_X, projMatrixTranspose, 4 );
	}

	//-------------------------------------------------
	// fill the depth buffer and clear color buffer to black except on subviews
	//-------------------------------------------------
	RB_FillDepthBufferFast( drawSurfs, numDrawSurfs );

	//-------------------------------------------------
	// main light renderer
	//-------------------------------------------------
	RB_DrawInteractions();

	//-------------------------------------------------
	// now draw any non-light dependent shading passes
	//-------------------------------------------------
	int processed = 0;
	if ( !r_skipShaderPasses.GetBool() ) {
		renderLog.OpenMainBlock( MRB_DRAW_SHADER_PASSES );
		float guiScreenOffset;
		if ( viewDef->viewEntitys != NULL ) {
			// guiScreenOffset will be 0 in non-gui views
			guiScreenOffset = 0.0f;
		} else {
			guiScreenOffset = stereoEye * viewDef->renderView.stereoScreenSeparation;
		}
		processed = RB_DrawShaderPasses( drawSurfs, numDrawSurfs, guiScreenOffset, stereoEye );
		renderLog.CloseMainBlock();
	}

	//-------------------------------------------------
	// fog and blend lights, drawn after emissive surfaces
	// so they are properly dimmed down
	//-------------------------------------------------
	RB_FogAllLights();

	//-------------------------------------------------
	// capture the depth for the motion blur before rendering any post process surfaces that may contribute to the depth
	//-------------------------------------------------
	if ( r_motionBlur.GetInteger() > 0 ) {
		const idScreenRect & viewport = backEnd.viewDef->viewport;
		globalImages->currentDepthImage->CopyDepthbuffer( viewport.x1, viewport.y1, viewport.GetWidth(), viewport.GetHeight() );
	}

	//-------------------------------------------------
	// now draw any screen warping post-process effects using _currentRender
	//-------------------------------------------------
	if ( processed < numDrawSurfs && !r_skipPostProcess.GetBool() ) {
		int x = backEnd.viewDef->viewport.x1;
		int y = backEnd.viewDef->viewport.y1;
		int	w = backEnd.viewDef->viewport.x2 - backEnd.viewDef->viewport.x1 + 1;
		int	h = backEnd.viewDef->viewport.y2 - backEnd.viewDef->viewport.y1 + 1;

		RENDERLOG_PRINTF( "Resolve to %i x %i buffer\n", w, h );

		GL_SelectTexture( 0 );

		// resolve the screen
		globalImages->currentRenderImage->CopyFramebuffer( x, y, w, h );
		backEnd.currentRenderCopied = true;

		// RENDERPARM_SCREENCORRECTIONFACTOR amd RENDERPARM_WINDOWCOORD overlap
		// diffuseScale and specularScale

		// screen power of two correction factor (no longer relevant now)
		float screenCorrectionParm[4];
		screenCorrectionParm[0] = 1.0f;
		screenCorrectionParm[1] = 1.0f;
		screenCorrectionParm[2] = 0.0f;
		screenCorrectionParm[3] = 1.0f;
		SetFragmentParm( RENDERPARM_SCREENCORRECTIONFACTOR, screenCorrectionParm ); // rpScreenCorrectionFactor

		// window coord to 0.0 to 1.0 conversion
		float windowCoordParm[4];
		windowCoordParm[0] = 1.0f / w;
		windowCoordParm[1] = 1.0f / h;
		windowCoordParm[2] = 0.0f;
		windowCoordParm[3] = 1.0f;
		SetFragmentParm( RENDERPARM_WINDOWCOORD, windowCoordParm ); // rpWindowCoord

		// render the remaining surfaces
		renderLog.OpenMainBlock( MRB_DRAW_SHADER_PASSES_POST );
		RB_DrawShaderPasses( drawSurfs + processed, numDrawSurfs - processed, 0.0f /* definitely not a gui */, stereoEye );
		renderLog.CloseMainBlock();
	}

	//-------------------------------------------------
	// render debug tools
	//-------------------------------------------------
	RB_RenderDebugTools( drawSurfs, numDrawSurfs );

	renderLog.CloseBlock();
}

/*
==================
RB_MotionBlur

Experimental feature
==================
*/
void RB_MotionBlur() {
	if ( !backEnd.viewDef->viewEntitys ) {
		// 3D views only
		return;
	}
	if ( r_motionBlur.GetInteger() <= 0 ) {
		return;
	}
	if ( backEnd.viewDef->isSubview ) {
		return;
	}

	GL_CheckErrors();

	// clear the alpha buffer and draw only the hands + weapon into it so
	// we can avoid blurring them
	qglClearColor( 0, 0, 0, 1 );
	GL_State( GLS_COLORMASK | GLS_DEPTHMASK );
	qglClear( GL_COLOR_BUFFER_BIT );
	GL_Color( 0, 0, 0, 0 );
	GL_SelectTexture( 0 );
	globalImages->blackImage->Bind();
	backEnd.currentSpace = NULL;

	drawSurf_t **drawSurfs = (drawSurf_t **)&backEnd.viewDef->drawSurfs[0];
	for ( int surfNum = 0; surfNum < backEnd.viewDef->numDrawSurfs; surfNum++ ) {
		const drawSurf_t * surf = drawSurfs[ surfNum ];

		if ( !surf->space->weaponDepthHack && !surf->space->skipMotionBlur && !surf->material->HasSubview() ) {
			// Apply motion blur to this object
			continue;
		}

		const idMaterial * shader = surf->material;
		if ( shader->Coverage() == MC_TRANSLUCENT ) {
			// muzzle flash, etc
			continue;
		}

		// set mvp matrix
		if ( surf->space != backEnd.currentSpace ) {
			RB_SetMVP( surf->space->mvp );
			backEnd.currentSpace = surf->space;
		}

		// this could just be a color, but we don't have a skinned color-only prog
		if ( surf->jointCache ) {
			renderProgManager.BindShader_TextureVertexColorSkinned();
		} else {
			renderProgManager.BindShader_TextureVertexColor();
		}

		// draw it solid
		RB_DrawElementsWithCounters( surf );
	}
	GL_State( GLS_DEPTHFUNC_ALWAYS );

	// copy off the color buffer and the depth buffer for the motion blur prog
	// we use the viewport dimensions for copying the buffers in case resolution scaling is enabled.
	const idScreenRect & viewport = backEnd.viewDef->viewport;
	globalImages->currentRenderImage->CopyFramebuffer( viewport.x1, viewport.y1, viewport.GetWidth(), viewport.GetHeight() );

	// in stereo rendering, each eye needs to get a separate previous frame mvp
	int mvpIndex = ( backEnd.viewDef->renderView.viewEyeBuffer == 1 ) ? 1 : 0;

	// derive the matrix to go from current pixels to previous frame pixels
	idRenderMatrix	inverseMVP;
	idRenderMatrix::Inverse( backEnd.viewDef->worldSpace.mvp, inverseMVP );

	idRenderMatrix	motionMatrix;
	idRenderMatrix::Multiply( backEnd.prevMVP[mvpIndex], inverseMVP, motionMatrix );

	backEnd.prevMVP[mvpIndex] = backEnd.viewDef->worldSpace.mvp;

	RB_SetMVP( motionMatrix );

	GL_State( GLS_DEPTHFUNC_ALWAYS );
	GL_Cull( CT_TWO_SIDED );

	renderProgManager.BindShader_MotionBlur();

	// let the fragment program know how many samples we are going to use
	idVec4 samples( (float)( 1 << r_motionBlur.GetInteger() ) );
	SetFragmentParm( RENDERPARM_OVERBRIGHT, samples.ToFloatPtr() );

	GL_SelectTexture( 0 );
	globalImages->currentRenderImage->Bind();
	GL_SelectTexture( 1 );
	globalImages->currentDepthImage->Bind();

	RB_DrawElementsWithCounters( &backEnd.unitSquareSurface );
	GL_CheckErrors();
}

/*
==================
RB_DrawView

StereoEye will always be 0 in mono modes, or -1 / 1 in stereo modes.
If the view is a GUI view that is repeated for both eyes, the viewDef.stereoEye value
is 0, so the stereoEye parameter is not always the same as that.
==================
*/
void RB_DrawView( const void *data, const int stereoEye ) {
	const drawSurfsCommand_t * cmd = (const drawSurfsCommand_t *)data;

	backEnd.viewDef = cmd->viewDef;

	// we will need to do a new copyTexSubImage of the screen
	// when a SS_POST_PROCESS material is used
	backEnd.currentRenderCopied = false;

	// if there aren't any drawsurfs, do nothing
	if ( !backEnd.viewDef->numDrawSurfs ) {
		return;
	}

	// skip render bypasses everything that has models, assuming
	// them to be 3D views, but leaves 2D rendering visible
	if ( r_skipRender.GetBool() && backEnd.viewDef->viewEntitys ) {
		return;
	}

	// skip render context sets the wgl context to NULL,
	// which should factor out the API cost, under the assumption
	// that all gl calls just return if the context isn't valid
	if ( r_skipRenderContext.GetBool() && backEnd.viewDef->viewEntitys ) {
		GLimp_DeactivateContext();
	}

	backEnd.pc.c_surfaces += backEnd.viewDef->numDrawSurfs;

	RB_ShowOverdraw();

	// render the scene
	RB_DrawViewInternal( cmd->viewDef, stereoEye );

	RB_MotionBlur();

	// restore the context for 2D drawing if we were stubbing it out
	if ( r_skipRenderContext.GetBool() && backEnd.viewDef->viewEntitys ) {
		GLimp_ActivateContext();
		GL_SetDefaultState();
	}

	// optionally draw a box colored based on the eye number
	if ( r_drawEyeColor.GetBool() ) {
		const idScreenRect & r = backEnd.viewDef->viewport;
		GL_Scissor( ( r.x1 + r.x2 ) / 2, ( r.y1 + r.y2 ) / 2, 32, 32 );
		switch ( stereoEye ) {
			case -1:
				GL_Clear( true, false, false, 0, 1.0f, 0.0f, 0.0f, 1.0f );
				break;
			case 1:
				GL_Clear( true, false, false, 0, 0.0f, 1.0f, 0.0f, 1.0f );
				break;
			default:
				GL_Clear( true, false, false, 0, 0.5f, 0.5f, 0.5f, 1.0f );
				break;
		}
	}
}

/*
==================
RB_CopyRender

Copy part of the current framebuffer to an image
==================
*/
void RB_CopyRender( const void *data ) {
	const copyRenderCommand_t * cmd = (const copyRenderCommand_t *)data;

	if ( r_skipCopyTexture.GetBool() ) {
		return;
	}

	RENDERLOG_PRINTF( "***************** RB_CopyRender *****************\n" );

	if ( cmd->image ) {
		cmd->image->CopyFramebuffer( cmd->x, cmd->y, cmd->imageWidth, cmd->imageHeight );
	}

	if ( cmd->clearColorAfterCopy ) {
		GL_Clear( true, false, false, STENCIL_SHADOW_TEST_VALUE, 0, 0, 0, 0 );
	}
}

/*
==================
RB_PostProcess

==================
*/
extern idCVar rs_enable;
void RB_PostProcess( const void * data ) {

	// only do the post process step if resolution scaling is enabled. Prevents the unnecessary copying of the framebuffer and
	// corresponding full screen quad pass.
	if ( rs_enable.GetInteger() == 0 ) { 
		return;
	}

	// resolve the scaled rendering to a temporary texture
	postProcessCommand_t * cmd = (postProcessCommand_t *)data;
	const idScreenRect & viewport = cmd->viewDef->viewport;
	globalImages->currentRenderImage->CopyFramebuffer( viewport.x1, viewport.y1, viewport.GetWidth(), viewport.GetHeight() );

	GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO | GLS_DEPTHMASK | GLS_DEPTHFUNC_ALWAYS );
	GL_Cull( CT_TWO_SIDED );

	int screenWidth = renderSystem->GetWidth();
	int screenHeight = renderSystem->GetHeight();

	// set the window clipping
	GL_Viewport( 0, 0, screenWidth, screenHeight );
	GL_Scissor( 0, 0, screenWidth, screenHeight );

	GL_SelectTexture( 0 );
	globalImages->currentRenderImage->Bind();
	renderProgManager.BindShader_PostProcess();

	// Draw
	RB_DrawElementsWithCounters( &backEnd.unitSquareSurface );

	renderLog.CloseBlock();
}

/*
==================
RB_VisFboBegin / RB_VisFboEnd

PRD M1 step 8: redirect subsequent 2D draws into an offscreen FBO instead of
the default backbuffer (Begin), then blit that FBO's color texture back onto
the backbuffer via a full-screen textured quad (End) -- qglBlitFramebuffer
isn't loaded in qgl.h, so the quad-draw technique RB_PostProcess already uses
above is the substitute. Lazily creates/recreates one static FBO attached to
whichever image the command names; only one redirect can be active at a time
(enforced up in idRenderSystemLocal::BeginOffscreenRender).
==================
*/
static GLuint	visFbo = 0;
static GLuint	visFboTexNum = 0;

void RB_VisFboBegin( const void * data ) {
	const visFboBeginCommand_t * cmd = (const visFboBeginCommand_t *)data;
	if ( cmd->colorImage == NULL || qglGenFramebuffers == NULL || qglBindFramebuffer == NULL ||
		 qglFramebufferTexture2D == NULL || qglCheckFramebufferStatus == NULL ) {
		return;		// GL_ARB_framebuffer_object unavailable -- draws just fall through to the backbuffer
	}

	// (Re)allocate on THIS thread (the one that owns the GL context) --
	// see the fix comment on visFboBeginCommand_t in tr_local.h.
	// (idImageOpts::operator== isn't const-qualified, hence the local copies.)
	idImageOpts targetOpts = cmd->targetOpts;
	idImageOpts currentOpts = cmd->colorImage->GetOpts();
	if ( !( targetOpts == currentOpts ) ) {
		cmd->colorImage->AllocImage( targetOpts, TF_LINEAR, TR_CLAMP );
	}

	const GLuint texNum = cmd->colorImage->GetTexNum();
	if ( visFbo == 0 || visFboTexNum != texNum ) {
		if ( visFbo != 0 && qglDeleteFramebuffers != NULL ) {
			qglDeleteFramebuffers( 1, &visFbo );
			visFbo = 0;
		}
		qglGenFramebuffers( 1, &visFbo );
		qglBindFramebuffer( GL_FRAMEBUFFER, visFbo );
		qglFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texNum, 0 );
		const GLenum status = qglCheckFramebufferStatus( GL_FRAMEBUFFER );
		if ( status != GL_FRAMEBUFFER_COMPLETE ) {
			idLib::Warning( "RB_VisFboBegin: incomplete framebuffer (status 0x%x), falling back to the backbuffer", (unsigned int)status );
			qglBindFramebuffer( GL_FRAMEBUFFER, 0 );
			qglDeleteFramebuffers( 1, &visFbo );
			visFbo = 0;
			return;
		}
		visFboTexNum = texNum;
	} else {
		qglBindFramebuffer( GL_FRAMEBUFFER, visFbo );
	}

	GL_Viewport( 0, 0, cmd->colorImage->GetUploadWidth(), cmd->colorImage->GetUploadHeight() );
	GL_Scissor( 0, 0, cmd->colorImage->GetUploadWidth(), cmd->colorImage->GetUploadHeight() );
	GL_Clear( true, false, false, 0, 0.0f, 0.0f, 0.0f, 0.0f );
}

void RB_VisFboEnd( const void * data ) {
	const visFboEndCommand_t * cmd = (const visFboEndCommand_t *)data;
	if ( visFbo == 0 || qglBindFramebuffer == NULL ) {
		return;		// Begin already fell back to the backbuffer -- nothing to unwind or blit
	}

	qglBindFramebuffer( GL_FRAMEBUFFER, 0 );	// back to the default/window framebuffer

	const int screenWidth = renderSystem->GetWidth();
	const int screenHeight = renderSystem->GetHeight();
	GL_Viewport( 0, 0, screenWidth, screenHeight );
	GL_Scissor( 0, 0, screenWidth, screenHeight );

	GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO | GLS_DEPTHMASK | GLS_DEPTHFUNC_ALWAYS );
	GL_Cull( CT_TWO_SIDED );

	// Fixed real bug: backEnd.unitSquareSurface's vertices are already in
	// clip-space NDC (-1..1, see R_MakeFullScreenTris), so BindShader_Texture's
	// vertex shader needs an identity MVP to pass them through untouched.
	// Without this, it silently reused whatever MVP the last-processed view
	// left behind (the 3D scene's perspective projection), warping the quad
	// entirely outside the clip volume and rasterizing zero fragments -- the
	// offscreen content itself was always correct, only this blit onto the
	// backbuffer was failing. Compare RB_DrawTestImage
	// (tr_backend_rendertools.cpp), which sets this the same way before its
	// own BindShader_Texture() draw; RB_PostProcess/RB_MotionBlur get this
	// for free by running right after a real 2D/3D view already set a valid
	// MVP, which this offscreen redirect path can't rely on.
	idRenderMatrix identityMVP;
	identityMVP.Identity();
	RB_SetMVP( identityMVP );

	GL_SelectTexture( 0 );
	if ( cmd->colorImage != NULL ) {
		cmd->colorImage->Bind();
	}
	renderProgManager.BindShader_Texture();

	RB_DrawElementsWithCounters( &backEnd.unitSquareSurface );
}

/*
==================
RB_VisMilkShaderCompile / RB_VisMilkWarpDraw

PRD M3: the live-render half of the HLSL->GLSL shader transpiler
(idMilkShaderTranspiler, neo/sound/MilkShaderTranspiler.cpp) -- compiles a
preset's transpiled warp/comp GLSL into a raw GL program (once, at preset
load) and draws a full-screen quad with it every frame the preset is active.

This deliberately bypasses idRenderProgManager's normal material-driven
pipeline (BindShader/CommitUniforms/the RENDERPARM_/rpUser<N> uniform
scheme) instead of trying to force MilkDrop's own uniform preamble
(sampler_main, bass, q1, _qa...) through it -- that scheme is built entirely
around this engine's own shader authoring convention and has no path for
arbitrary externally-named uniforms. idRenderProgManager::LoadShaderFromSource
(previously dead code, zero callers -- see docs/PRD-implementation-status.md
M3 notes) is the one piece of it actually reused here: a bare compile+link
returning a raw GL program id, with none of the surrounding bookkeeping.

Known, deliberate scope gaps (also documented on visMilkWarpUniforms_t in
RenderSystem.h): sampler_blur1-3/noise_lq-hq have no real blur-pass/noise-
texture pipeline yet, so they're bound to sampler_main/whiteImage as inert
placeholders rather than left as undefined texture units; _qa.._qh (real
MilkDrop 2x2 matrices) aren't modeled by idMilkEvaluator at all yet, so
identity is passed. A preset's custom shader will therefore not be pixel-
perfect against real MilkDrop for presets that lean on those features, but
will still run and produce real, audio-reactive output for the (majority)
of the transpiler's own uniform preamble that's fully wired -- see
MilkShaderTranspiler.h's own "known scope limits" comment for the same
category of documented, non-silent simplification.
==================
*/
static GLuint	s_milkWarpProgId = 0;
static GLuint	s_milkCompProgId = 0;	// PRD M3: MilkDrop's separate post-composite shader -- 0 if the preset has none
// PRD M5: mash-up "B" side's own warp_/comp_ program pair, compiled and kept
// ALONGSIDE (not replacing) the "A" side's above, so both can be drawn --
// and alpha-blended together -- the same frame.
static GLuint	s_milkWarpProgIdB = 0;
static GLuint	s_milkCompProgIdB = 0;

// Fixed real bug: a preset with warp_ but no comp_ used to draw DIRECTLY
// from cmd->mainImage (this frame's captured feedback, sampler_main) into
// whatever framebuffer the outer RC_VIS_FBO_BEGIN redirect already bound --
// confirmed via direct testing (a warp_-only hardcoded-solid-color test
// preset rendered solid BLACK, not its real output) that this is the SAME
// texture being read (sampler_main) and written (the bound draw target)
// in one draw call -- a classic feedback loop, undefined behavior in GL,
// which several drivers resolve by returning black. Every prior test
// happened to use a comp_ shader too, whose SECOND pass samples the SAFE
// intermediate (never cmd->mainImage) while drawing into the real
// destination, so this never surfaced before. Fix: whenever there's no
// preset-supplied comp_, compile this fixed, hand-written GLSL passthrough
// (skip the HLSL transpiler entirely -- it's just "copy one pixel", no
// preset-specific logic) and use IT for the second pass instead, so the
// no-comp_ case goes through the exact same safe two-hop structure (warp_
// into the intermediate, then a copy from the intermediate into the real
// destination) as the has-comp_ case, never reading and writing the same
// texture in one draw. Uses the SAME preamble uniform/varying names
// (sampler_main, milkOutputAlpha, xlv_TEXCOORD0) the transpiled shaders use,
// so it plugs into the exact same vertex passthrough with no changes.
static const char * kMilkPassthroughFragmentGLSL =
	"uniform sampler2D sampler_main;\n"
	"uniform float milkOutputAlpha;\n"
	"varying vec2 xlv_TEXCOORD0;\n"
	"void main() {\n"
	// Fixed real bug, confirmed via direct testing: passing xlv_TEXCOORD0
	// (a varying) DIRECTLY as an argument to texture2D() produced solid
	// black (not a compile/link failure -- LoadShaderFromSource still
	// returned a valid, usable program ID), while copying it to a plain
	// local variable first and passing THAT works correctly. This matches
	// a known class of GLSL 1.10/1.20-era compiler bugs on some drivers
	// where a varying used directly as a call argument (rather than read
	// into a local first) is mishandled. All the transpiler's OWN
	// generated shaders already avoid this by construction (hlsl2glslfork
	// always copies varyings into locals as part of its wrapper), so only
	// this one hand-written shader needed the explicit workaround.
	"    vec2 uv = xlv_TEXCOORD0;\n"
	"    gl_FragData[0] = vec4( texture2D( sampler_main, uv ).rgb, milkOutputAlpha );\n"
	"}\n";
// shared by both A and B (stateless, no preset-specific content) -- compiled
// once, lazily, the first time either side compiles a working vertex
// passthrough to compile it against.
static GLuint	s_milkPassthroughProgId = 0;

// PRD M3: idLib::Warning/Printf silently redirect to OutputDebugString (never
// reaching the visible console/log) when called off the main thread -- a
// real bug already hit once before in this project (see docs/bugfix-
// visualizer-black-screen.md's "lessons for next time"). RB_VisMilkShaderCompile
// runs on the backend/GL-context thread, so a plain idLib::Warning on
// compile/link failure here would be invisible. This status flag is polled
// and actually printed from idVisualizerManager::Frame() (confirmed main
// thread) each frame instead. 0 = no compile attempted yet, 1 = last compile
// succeeded, -1 = last compile failed.
static idSysInterlockedInteger s_milkWarpCompileStatus;
// PRD M5: same main-thread-safe status mechanism, for the mash-up B side.
static idSysInterlockedInteger s_milkMashCompileStatus;

int RB_GetMilkWarpCompileStatus() {
	return s_milkWarpCompileStatus.GetValue();
}

int RB_GetMilkMashCompileStatus() {
	return s_milkMashCompileStatus.GetValue();
}

void RB_VisMilkShaderCompile( const void * data ) {
	const visMilkShaderCompileCommand_t * cmd = (const visMilkShaderCompileCommand_t *)data;

	// PRD M5: isMashB routes this compile into the B-side program slots
	// instead of A's -- both slots are independent, so compiling one never
	// touches the other (A keeps rendering with whatever it already had
	// while a new B preset's shader compiles, and vice versa).
	GLuint & warpProgId = cmd->isMashB ? s_milkWarpProgIdB : s_milkWarpProgId;
	GLuint & compProgId_ = cmd->isMashB ? s_milkCompProgIdB : s_milkCompProgId;

	if ( warpProgId != 0 && qglDeleteProgram != NULL ) {
		qglDeleteProgram( warpProgId );
		warpProgId = 0;
	}
	if ( compProgId_ != 0 && qglDeleteProgram != NULL ) {
		qglDeleteProgram( compProgId_ );
		compProgId_ = 0;
	}

	if ( cmd->vertexSource != NULL && cmd->fragmentSource != NULL ) {
		idList<int> unusedUniforms;	// LoadShaderFromSource ignores this param; kept only to match its signature
		const int progId = renderProgManager.LoadShaderFromSource( cmd->vertexSource->c_str(), cmd->fragmentSource->c_str(), unusedUniforms );
		if ( progId >= 0 ) {
			warpProgId = (GLuint)progId;
			if ( cmd->isMashB ) {
				s_milkMashCompileStatus.SetValue( 1 );
			} else {
				s_milkWarpCompileStatus.SetValue( 1 );
			}
		} else if ( cmd->isMashB ) {
			s_milkMashCompileStatus.SetValue( -1 );
		} else {
			s_milkWarpCompileStatus.SetValue( -1 );
		}
	}

	// PRD M3: comp_ -- MilkDrop's separate post-composite shader, a second
	// full-screen pass over the already-warped image (see RB_VisMilkWarpDraw's
	// two-pass comment below). Reuses the exact same vertex passthrough as
	// the warp pass -- both are just "sample one full-screen texture, write
	// the result" programs, only the fragment shader differs.
	if ( warpProgId != 0 && cmd->vertexSource != NULL && cmd->compFragmentSource != NULL ) {
		idList<int> unusedUniforms;
		const int compId = renderProgManager.LoadShaderFromSource( cmd->vertexSource->c_str(), cmd->compFragmentSource->c_str(), unusedUniforms );
		if ( compId >= 0 ) {
			compProgId_ = (GLuint)compId;
		}
		// a comp_ compile failure isn't reflected in s_milkWarpCompileStatus --
		// the warp pass alone still works fine without it, matching this
		// project's NFR-3 tolerate-and-skip convention (lose the extra
		// polish pass, not the whole custom-shader feature).
	}

	// lazily compile the shared passthrough (see its own comment above) the
	// first time ANY valid vertex source is available -- stateless, so one
	// copy serves both the A and B sides forever after.
	if ( s_milkPassthroughProgId == 0 && cmd->vertexSource != NULL ) {
		idList<int> unusedUniforms;
		const int passthroughId = renderProgManager.LoadShaderFromSource( cmd->vertexSource->c_str(), kMilkPassthroughFragmentGLSL, unusedUniforms );
		if ( passthroughId >= 0 ) {
			s_milkPassthroughProgId = (GLuint)passthroughId;
		}
	}

	delete cmd->vertexSource;
	delete cmd->fragmentSource;
	delete cmd->compFragmentSource;
}

// small helper: only set a uniform if the compiled program actually
// declares/uses it (unreferenced uniforms are legitimately optimized out by
// the driver and return -1 from glGetUniformLocation -- not an error).
static void RB_SetMilkUniform1f( GLuint prog, const char * name, float v ) {
	if ( qglGetUniformLocation == NULL || qglUniform1f == NULL ) {
		return;
	}
	const GLint loc = qglGetUniformLocation( prog, name );
	if ( loc >= 0 ) {
		qglUniform1f( loc, v );
	}
}

// shared by both the warp and comp passes: everything except which texture
// gets bound as sampler_main (that differs -- warp samples last frame's
// feedback, comp samples the just-warped intermediate).
static void RB_SetMilkCommonUniforms( GLuint prog, const visMilkWarpUniforms_t & u ) {
	RB_SetMilkUniform1f( prog, "time", u.time );
	RB_SetMilkUniform1f( prog, "frame", u.frame );
	RB_SetMilkUniform1f( prog, "fps", u.fps );
	RB_SetMilkUniform1f( prog, "progress", u.progress );
	RB_SetMilkUniform1f( prog, "bass", u.bass );
	RB_SetMilkUniform1f( prog, "mid", u.mid );
	RB_SetMilkUniform1f( prog, "treb", u.treb );
	RB_SetMilkUniform1f( prog, "bass_att", u.bassAtt );
	RB_SetMilkUniform1f( prog, "mid_att", u.midAtt );
	RB_SetMilkUniform1f( prog, "treb_att", u.trebAtt );
	RB_SetMilkUniform1f( prog, "rand_frame", u.randFrame );
	RB_SetMilkUniform1f( prog, "rand_preset", u.randPreset );
	RB_SetMilkUniform1f( prog, "vol", u.vol );
	RB_SetMilkUniform1f( prog, "vol_att", u.volAtt );

	if ( qglGetUniformLocation != NULL && qglUniform2fv != NULL ) {
		const GLint loc = qglGetUniformLocation( prog, "aspect" );
		if ( loc >= 0 ) {
			const float aspect[2] = { u.aspectX, u.aspectY };
			qglUniform2fv( loc, 1, aspect );
		}
	}

	// roam_cos/roam_sin: MilkDrop's four slow per-frame drifters (float4 each).
	if ( qglGetUniformLocation != NULL && qglUniform4fv != NULL ) {
		const GLint cosLoc = qglGetUniformLocation( prog, "roam_cos" );
		if ( cosLoc >= 0 ) {
			qglUniform4fv( cosLoc, 1, u.roamCos );
		}
		const GLint sinLoc = qglGetUniformLocation( prog, "roam_sin" );
		if ( sinLoc >= 0 ) {
			qglUniform4fv( sinLoc, 1, u.roamSin );
		}
	}

	char qname[8];
	for ( int i = 0; i < 32; i++ ) {
		idStr::snPrintf( qname, sizeof( qname ), "q%d", i + 1 );
		RB_SetMilkUniform1f( prog, qname, u.q[i] );
	}

	// _qa.._qh: PRD M3 fidelity fix -- real MilkDrop2 semantics are NOT
	// independent matrices, they're just the SAME q1..q32 scalars set just
	// above, repacked 4-at-a-time into 8 2x2 matrices (the documented
	// MilkDrop2/community convention: _qa = q1..q4, _qb = q5..q8, ...,
	// _qh = q29..q32 -- "packed _qa.._qh" per this repo's own
	// docs/research-milkdrop-projectm.md). Previously hardcoded identity
	// since idMilkEvaluator's q1..q32 weren't being reused here at all; now
	// a preset's per_frame_/per_pixel_ EEL2 code that sets q1..q4 (etc) to
	// drive a custom warp_/comp_ shader's _qa read actually reaches the
	// shader. A q-var a preset never touches defaults to 0 (idMilkEvaluator::
	// GetQ's own documented default), which matches real MilkDrop's own
	// per-preset q-var initialization -- not a regression from identity for
	// presets that don't use _qa.._qh at all (the compiled shader wouldn't
	// even query the uniform location in that case).
	// transpose=GL_TRUE matches HLSL's row-major float2x2(a,b,c,d)
	// constructor order (GL's own uniformMatrix* transpose flag exists
	// exactly for uploading row-major source data, rather than hand-
	// transposing the array here). The row-major direction itself is
	// inferred from documented MilkDrop2 shader convention, not verified
	// against a live reference-MilkDrop visual comparison (no such tooling
	// available in this environment) -- if it turns out backwards, the
	// visible effect is a transposed (not scrambled/incorrect-magnitude)
	// matrix, a strictly smaller error than the all-identity behavior this replaces.
	if ( qglGetUniformLocation != NULL && qglUniformMatrix2fv != NULL ) {
		static const char * const qMatNames[8] = { "_qa", "_qb", "_qc", "_qd", "_qe", "_qf", "_qg", "_qh" };
		for ( int i = 0; i < 8; i++ ) {
			const GLint loc = qglGetUniformLocation( prog, qMatNames[i] );
			if ( loc >= 0 ) {
				const float m[4] = { u.q[i * 4 + 0], u.q[i * 4 + 1], u.q[i * 4 + 2], u.q[i * 4 + 3] };
				qglUniformMatrix2fv( loc, 1, GL_TRUE, m );
			}
		}
	}
}

// sampler_blur1-3/noise_lq-hq: no blur-pass or noise-texture pipeline exists
// in this codebase yet (see the RB_VisMilkShaderCompile/RB_VisMilkWarpDraw
// block comment above) -- bind inert placeholders (mainImage for "blur",
// the stock white image for "noise") so a shader sampling these reads
// something well-defined rather than an unbound texture unit. Shared by
// both passes.
static void RB_BindMilkPlaceholderSamplers( GLuint prog, idImage * mainImage, float texW, float texH ) {
	if ( qglGetUniformLocation == NULL || qglUniform1i == NULL ) {
		return;
	}
	static const char * const blurNames[3] = { "sampler_blur1", "sampler_blur2", "sampler_blur3" };
	static const char * const blurSizeNames[3] = { "texsize_blur1", "texsize_blur2", "texsize_blur3" };
	GL_SelectTexture( 1 );
	if ( mainImage != NULL ) {
		mainImage->Bind();
	}
	for ( int i = 0; i < 3; i++ ) {
		const GLint loc = qglGetUniformLocation( prog, blurNames[i] );
		if ( loc >= 0 ) {
			qglUniform1i( loc, 1 );
		}
		if ( qglUniform4fv != NULL ) {
			const GLint sizeLoc = qglGetUniformLocation( prog, blurSizeNames[i] );
			if ( sizeLoc >= 0 ) {
				const float texsize[4] = { texW, texH, texW > 0.0f ? 1.0f / texW : 0.0f, texH > 0.0f ? 1.0f / texH : 0.0f };
				qglUniform4fv( sizeLoc, 1, texsize );
			}
		}
	}

	static const char * const noiseNames[3] = { "sampler_noise_lq", "sampler_noise_mq", "sampler_noise_hq" };
	static const char * const noiseSizeNames[3] = { "texsize_noise_lq", "texsize_noise_mq", "texsize_noise_hq" };
	GL_SelectTexture( 2 );
	if ( globalImages->whiteImage != NULL ) {
		globalImages->whiteImage->Bind();
	}
	const float whiteW = ( globalImages->whiteImage != NULL ) ? (float)globalImages->whiteImage->GetUploadWidth() : 1.0f;
	const float whiteH = ( globalImages->whiteImage != NULL ) ? (float)globalImages->whiteImage->GetUploadHeight() : 1.0f;
	for ( int i = 0; i < 3; i++ ) {
		const GLint loc = qglGetUniformLocation( prog, noiseNames[i] );
		if ( loc >= 0 ) {
			qglUniform1i( loc, 2 );
		}
		if ( qglUniform4fv != NULL ) {
			const GLint sizeLoc = qglGetUniformLocation( prog, noiseSizeNames[i] );
			if ( sizeLoc >= 0 ) {
				const float texsize[4] = { whiteW, whiteH, whiteW > 0.0f ? 1.0f / whiteW : 0.0f, whiteH > 0.0f ? 1.0f / whiteH : 0.0f };
				qglUniform4fv( sizeLoc, 1, texsize );
			}
		}
	}

	// sampler_noisevol_lq/hq: MilkDrop's 3D noise volumes (sampler3D). KNOWN
	// SIMPLIFICATION -- this engine has no 3D-texture pipeline, so no real
	// volume is bound. Point them at a dedicated texture unit (3) that carries
	// no 2D binding, so the sampler3D reads GL's default (incomplete) 3D
	// texture and returns a constant rather than conflicting with a 2D texture
	// bound on a shared unit. The preset still transpiles and renders in its
	// real palette; only the noise-volume term is approximated (constant).
	// texsize_noisevol_* are still supplied with a plausible 32^3 size so any
	// preset arithmetic on them stays finite. See kMilkPreamble.
	static const char * const noiseVolNames[2] = { "sampler_noisevol_lq", "sampler_noisevol_hq" };
	static const char * const noiseVolSizeNames[2] = { "texsize_noisevol_lq", "texsize_noisevol_hq" };
	for ( int i = 0; i < 2; i++ ) {
		const GLint loc = qglGetUniformLocation( prog, noiseVolNames[i] );
		if ( loc >= 0 ) {
			qglUniform1i( loc, 3 );
		}
		if ( qglUniform4fv != NULL ) {
			const GLint sizeLoc = qglGetUniformLocation( prog, noiseVolSizeNames[i] );
			if ( sizeLoc >= 0 ) {
				const float dim = 32.0f;
				const float texsize[4] = { dim, dim, 1.0f / dim, 1.0f / dim };
				qglUniform4fv( sizeLoc, 1, texsize );
			}
		}
	}
}

// one full-screen-quad draw with `prog`, sampling `sampleImage` as
// sampler_main, writing to whatever framebuffer is currently bound. Shared
// by both the single-pass (warp only) and two-pass (warp then comp) cases.
static void RB_DrawMilkFullscreenPass( GLuint prog, idImage * sampleImage, const visMilkWarpUniforms_t & u, float outputAlpha ) {
	qglUseProgram( prog );
	RB_SetMilkCommonUniforms( prog, u );
	RB_SetMilkUniform1f( prog, "milkOutputAlpha", outputAlpha );

	const float texW = (float)sampleImage->GetUploadWidth();
	const float texH = (float)sampleImage->GetUploadHeight();
	if ( qglGetUniformLocation != NULL && qglUniform4fv != NULL ) {
		const GLint loc = qglGetUniformLocation( prog, "texsize" );
		if ( loc >= 0 ) {
			const float texsize[4] = { texW, texH, texW > 0.0f ? 1.0f / texW : 0.0f, texH > 0.0f ? 1.0f / texH : 0.0f };
			qglUniform4fv( loc, 1, texsize );
		}
	}

	GL_SelectTexture( 0 );
	sampleImage->Bind();
	if ( qglGetUniformLocation != NULL && qglUniform1i != NULL ) {
		// sampler_main and its filter/wrap-qualified aliases (fw/pw/fc/pc)
		// all refer to the SAME main texture -- GLSL 1.10 can't carry
		// per-sampler filter/wrap state, so they all bind to unit 0 here
		// (previously fw/pw were declared but never bound, reading garbage).
		static const char * const mainNames[5] = {
			"sampler_main", "sampler_fw_main", "sampler_pw_main", "sampler_fc_main", "sampler_pc_main"
		};
		for ( int i = 0; i < 5; i++ ) {
			const GLint mloc = qglGetUniformLocation( prog, mainNames[i] );
			if ( mloc >= 0 ) {
				qglUniform1i( mloc, 0 );
			}
		}
	}

	RB_BindMilkPlaceholderSamplers( prog, sampleImage, texW, texH );

	RB_DrawElementsWithCounters( &backEnd.unitSquareSurface );
}

// PRD M3: lazily-created FBO for the warp_->comp_ intermediate target --
// mirrors visFbo/visFboTexNum's exact pattern above (RB_VisFboBegin), one
// static FBO reused/reattached whenever the target image's texture handle
// changes, not recreated every frame.
static GLuint	s_milkCompFbo = 0;
static GLuint	s_milkCompFboTexNum = 0;

void RB_VisMilkWarpDraw( const void * data ) {
	const visMilkWarpDrawCommand_t * cmd = (const visMilkWarpDrawCommand_t *)data;

	if ( s_milkWarpProgId == 0 || qglUseProgram == NULL || cmd->mainImage == NULL ) {
		return;	// nothing compiled (or the last compile failed) -- caller's CPU-mesh path is the fallback
	}

	GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO | GLS_DEPTHMASK | GLS_DEPTHFUNC_ALWAYS );
	GL_Cull( CT_TWO_SIDED );

	// same reasoning as RB_VisFboEnd just above: backEnd.unitSquareSurface's
	// vertices are already in clip-space NDC, so the pass-through vertex
	// shader needs an identity MVP to leave them untouched.
	idRenderMatrix identityMVP;
	identityMVP.Identity();
	RB_SetMVP( identityMVP );

	// PRD M3: two-pass warp_->(comp_ or a fixed passthrough) pipeline --
	// ALWAYS two passes now, never draw cmd->mainImage directly into
	// whatever the outer RC_VIS_FBO_BEGIN redirect already bound. Fixed
	// real bug: the original single-pass (no comp_) code did exactly that
	// -- and cmd->mainImage (sampler_main, this frame's captured feedback)
	// can legitimately BE the same texture the outer redirect's destination
	// is attached to (confirmed via direct testing: a warp_-only hardcoded-
	// solid-color test preset rendered solid BLACK instead of its real
	// output). Reading and writing the same texture in one draw call is
	// undefined behavior in GL; several drivers resolve it to black. This
	// never surfaced before because every prior test preset happened to
	// have a comp_ shader too, whose second pass samples the SAFE
	// intermediate (never cmd->mainImage) -- so now EVERY preset routes
	// through that same safe two-hop structure: pass 1 (warp_, always safe:
	// mainImage -> intermediate) is unconditional, and pass 2 uses the
	// preset's own comp_ if it compiled, or the fixed hand-written
	// passthrough (see kMilkPassthroughFragmentGLSL above) otherwise --
	// either way, pass 2 samples the intermediate, never cmd->mainImage.
	const GLuint secondPassProg = ( s_milkCompProgId != 0 ) ? s_milkCompProgId : s_milkPassthroughProgId;
	if ( secondPassProg != 0 && cmd->intermediateImage != NULL &&
		 qglGenFramebuffers != NULL && qglBindFramebuffer != NULL &&
		 qglFramebufferTexture2D != NULL && qglCheckFramebufferStatus != NULL && qglGetIntegerv != NULL ) {

		// remember whatever framebuffer the outer RC_VIS_FBO_BEGIN redirect
		// already bound -- pass 2 needs to draw back into THAT, not assume
		// framebuffer 0 (the default backbuffer), since this whole draw
		// only ever runs already-nested inside that outer redirect.
		GLint prevFbo = 0;
		qglGetIntegerv( GL_FRAMEBUFFER_BINDING, &prevFbo );

		const int w = cmd->mainImage->GetUploadWidth();
		const int h = cmd->mainImage->GetUploadHeight();
		idImageOpts targetOpts = cmd->intermediateImage->GetOpts();
		if ( targetOpts.width != w || targetOpts.height != h ) {
			targetOpts.width = w;
			targetOpts.height = h;
			targetOpts.format = FMT_RGBA16F;
			targetOpts.colorFormat = CFM_DEFAULT;
			targetOpts.numLevels = 1;
			cmd->intermediateImage->AllocImage( targetOpts, TF_LINEAR, TR_CLAMP );
		}

		const GLuint texNum = cmd->intermediateImage->GetTexNum();
		if ( s_milkCompFbo == 0 || s_milkCompFboTexNum != texNum ) {
			if ( s_milkCompFbo != 0 && qglDeleteFramebuffers != NULL ) {
				qglDeleteFramebuffers( 1, &s_milkCompFbo );
				s_milkCompFbo = 0;
			}
			qglGenFramebuffers( 1, &s_milkCompFbo );
			qglBindFramebuffer( GL_FRAMEBUFFER, s_milkCompFbo );
			qglFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texNum, 0 );
			const GLenum status = qglCheckFramebufferStatus( GL_FRAMEBUFFER );
			if ( status != GL_FRAMEBUFFER_COMPLETE ) {
				// no safe intermediate available this frame -- skip the draw
				// entirely rather than fall back to the known-broken direct
				// single-pass path (NFR-3: lose a frame of custom-shader
				// output, not silently render garbage/black).
				idLib::Warning( "RB_VisMilkWarpDraw: incomplete intermediate framebuffer (status 0x%x) -- custom warp shader skipped this frame", (unsigned int)status );
				qglBindFramebuffer( GL_FRAMEBUFFER, (GLuint)prevFbo );
				qglDeleteFramebuffers( 1, &s_milkCompFbo );
				s_milkCompFbo = 0;
				qglUseProgram( 0 );
				renderProgManager.Unbind();
				return;
			}
			s_milkCompFboTexNum = texNum;
		} else {
			qglBindFramebuffer( GL_FRAMEBUFFER, s_milkCompFbo );
		}

		GL_Viewport( 0, 0, w, h );
		GL_Scissor( 0, 0, w, h );

		// pass 1: warp_ samples last frame's feedback (cmd->mainImage),
		// writes into the intermediate target.
		RB_DrawMilkFullscreenPass( s_milkWarpProgId, cmd->mainImage, cmd->uniforms, 1.0f );

		// pass 2: comp_ (or the fixed passthrough) samples the just-warped
		// intermediate, writes back into whatever the outer redirect
		// actually wants this frame.
		qglBindFramebuffer( GL_FRAMEBUFFER, (GLuint)prevFbo );
		GL_Viewport( 0, 0, w, h );
		GL_Scissor( 0, 0, w, h );
		RB_DrawMilkFullscreenPass( secondPassProg, cmd->intermediateImage, cmd->uniforms, 1.0f );
	}

	qglUseProgram( 0 );
	// invalidate idRenderProgManager's cached current-program state -- it
	// only knows about its own tables' indices, never this raw progId, so
	// the next real BindShader() call must not skip re-binding because it
	// thinks the same (vIndex,fIndex) is already active.
	renderProgManager.Unbind();
}

// PRD M5: draws the mash-up "B" side's own compiled warp_/comp_ program (see
// RB_VisMilkShaderCompile's isMashB) alpha-blended on top of whatever
// RB_VisMilkWarpDraw already drew this frame for the "A" side. Mirrors
// RB_VisMilkWarpDraw's own two-pass (warp_->comp_) structure exactly --
// only the program IDs, the blend state of the FINAL pass (alpha-blended
// here instead of opaque-overwrite), and that final pass's output alpha
// (mashMix here instead of always-1.0) differ. Reuses A's own intermediate
// image/FBO: by the time this command runs, A's warp_->comp_ sequence has
// already fully consumed it this frame, so it's safe scratch space for B's
// own warp_->comp_ sequence too.
void RB_VisMilkMashDraw( const void * data ) {
	const visMilkMashDrawCommand_t * cmd = (const visMilkMashDrawCommand_t *)data;

	if ( s_milkWarpProgIdB == 0 || qglUseProgram == NULL || cmd->mainImage == NULL ) {
		return;	// B side didn't compile (or hasn't yet) -- mash-up silently contributes nothing extra this frame
	}

	GL_Cull( CT_TWO_SIDED );
	idRenderMatrix identityMVP;
	identityMVP.Identity();
	RB_SetMVP( identityMVP );

	const float mix = idMath::ClampFloat( 0.0f, 1.0f, cmd->mashMix );

	// Fixed real bug (same class/root cause as RB_VisMilkWarpDraw's own
	// fix above -- see its comment): never draw cmd->mainImage directly
	// into the destination framebuffer, since it can legitimately BE that
	// framebuffer's attached texture (a feedback loop, undefined behavior
	// in GL). Always two-hop through the intermediate: pass 1 (warp_B,
	// always safe) into the intermediate, pass 2 (comp_B if present,
	// otherwise the shared fixed passthrough) samples the intermediate --
	// never cmd->mainImage -- and alpha-blends onto the real destination.
	const GLuint secondPassProgB = ( s_milkCompProgIdB != 0 ) ? s_milkCompProgIdB : s_milkPassthroughProgId;
	if ( secondPassProgB != 0 && cmd->intermediateImage != NULL &&
		 qglGenFramebuffers != NULL && qglBindFramebuffer != NULL &&
		 qglFramebufferTexture2D != NULL && qglCheckFramebufferStatus != NULL && qglGetIntegerv != NULL ) {

		GLint prevFbo = 0;
		qglGetIntegerv( GL_FRAMEBUFFER_BINDING, &prevFbo );

		const int w = cmd->mainImage->GetUploadWidth();
		const int h = cmd->mainImage->GetUploadHeight();
		idImageOpts targetOpts = cmd->intermediateImage->GetOpts();
		if ( targetOpts.width != w || targetOpts.height != h ) {
			targetOpts.width = w;
			targetOpts.height = h;
			targetOpts.format = FMT_RGBA16F;
			targetOpts.colorFormat = CFM_DEFAULT;
			targetOpts.numLevels = 1;
			cmd->intermediateImage->AllocImage( targetOpts, TF_LINEAR, TR_CLAMP );
		}

		const GLuint texNum = cmd->intermediateImage->GetTexNum();
		if ( s_milkCompFbo == 0 || s_milkCompFboTexNum != texNum ) {
			if ( s_milkCompFbo != 0 && qglDeleteFramebuffers != NULL ) {
				qglDeleteFramebuffers( 1, &s_milkCompFbo );
				s_milkCompFbo = 0;
			}
			qglGenFramebuffers( 1, &s_milkCompFbo );
			qglBindFramebuffer( GL_FRAMEBUFFER, s_milkCompFbo );
			qglFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texNum, 0 );
			const GLenum status = qglCheckFramebufferStatus( GL_FRAMEBUFFER );
			if ( status != GL_FRAMEBUFFER_COMPLETE ) {
				idLib::Warning( "RB_VisMilkMashDraw: incomplete intermediate framebuffer (status 0x%x) -- mash-up B side skipped this frame", (unsigned int)status );
				qglBindFramebuffer( GL_FRAMEBUFFER, (GLuint)prevFbo );
				qglDeleteFramebuffers( 1, &s_milkCompFbo );
				s_milkCompFbo = 0;
				qglUseProgram( 0 );
				renderProgManager.Unbind();
				return;
			}
			s_milkCompFboTexNum = texNum;
		} else {
			qglBindFramebuffer( GL_FRAMEBUFFER, s_milkCompFbo );
		}

		GL_Viewport( 0, 0, w, h );
		GL_Scissor( 0, 0, w, h );

		// pass 1: warp_B samples the same captured feedback frame A did,
		// writes into the (reused) intermediate target -- opaque, since it's
		// only scratch space, not the final composited output.
		GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO | GLS_DEPTHMASK | GLS_DEPTHFUNC_ALWAYS );
		RB_DrawMilkFullscreenPass( s_milkWarpProgIdB, cmd->mainImage, cmd->uniforms, 1.0f );

		// pass 2: comp_B (or the fixed passthrough) samples the just-warped
		// intermediate, alpha-blended on top of whatever's already in the
		// destination (A's own output) at cmd->mashMix -- the same "draw A
		// opaque, draw B translucent on top" compositing the CPU-mesh
		// mash-up path already uses.
		qglBindFramebuffer( GL_FRAMEBUFFER, (GLuint)prevFbo );
		GL_Viewport( 0, 0, w, h );
		GL_Scissor( 0, 0, w, h );
		GL_State( GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_DEPTHMASK | GLS_DEPTHFUNC_ALWAYS );
		RB_DrawMilkFullscreenPass( secondPassProgB, cmd->intermediateImage, cmd->uniforms, mix );
	}

	qglUseProgram( 0 );
	renderProgManager.Unbind();
}
