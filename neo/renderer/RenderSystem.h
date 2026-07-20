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

#ifndef __RENDERER_H__
#define __RENDERER_H__

/*
===============================================================================

	idRenderSystem is responsible for managing the screen, which can have
	multiple idRenderWorld and 2D drawing done on it.

===============================================================================
*/
enum stereo3DMode_t {
	STEREO3D_OFF,

	// half-resolution, non-square pixel views
	STEREO3D_SIDE_BY_SIDE_COMPRESSED,
	STEREO3D_TOP_AND_BOTTOM_COMPRESSED,

	// two full resolution views side by side, as for a dual cable display
	STEREO3D_SIDE_BY_SIDE,

	STEREO3D_INTERLACED,

	// OpenGL quad buffer
	STEREO3D_QUAD_BUFFER,

	// two full resolution views stacked with a 30 pixel guard band
	// On the PC this can be configured as a custom video timing, but
	// it definitely isn't a consumer level task.  The quad_buffer
	// support can handle 720P-3D with apropriate driver support.
	STEREO3D_HDMI_720
};

typedef enum {
	AUTORENDER_DEFAULTICON = 0,
	AUTORENDER_HELLICON,
	AUTORENDER_DIALOGICON,
	AUTORENDER_MAX
} autoRenderIconType_t ;

enum stereoDepthType_t {
	STEREO_DEPTH_TYPE_NONE,
	STEREO_DEPTH_TYPE_NEAR,
	STEREO_DEPTH_TYPE_MID,
	STEREO_DEPTH_TYPE_FAR
};


enum graphicsVendor_t {
	VENDOR_NVIDIA,
	VENDOR_AMD,
	VENDOR_INTEL
};

// Contains variables specific to the OpenGL configuration being run right now.
// These are constant once the OpenGL subsystem is initialized.
struct glconfig_t {
	const char *		renderer_string;
	const char *		vendor_string;
	const char *		version_string;
	const char *		extensions_string;
	const char *		wgl_extensions_string;
	const char *		shading_language_string;

	float				glVersion;				// atof( version_string )
	graphicsVendor_t	vendor;

	int					maxTextureSize;			// queried from GL
	int					maxTextureCoords;
	int					maxTextureImageUnits;
	int					uniformBufferOffsetAlignment;
	float				maxTextureAnisotropy;

	int					colorBits;
	int					depthBits;
	int					stencilBits;

	bool				multitextureAvailable;
	bool				directStateAccess;
	bool				textureCompressionAvailable;
	bool				anisotropicFilterAvailable;
	bool				textureLODBiasAvailable;
	bool				seamlessCubeMapAvailable;
	bool				sRGBFramebufferAvailable;
	bool				vertexBufferObjectAvailable;
	bool				mapBufferRangeAvailable;
	bool				vertexArrayObjectAvailable;
	bool				framebufferObjectAvailable;	// VIS: PRD M1 step 8
	bool				drawElementsBaseVertexAvailable;
	bool				fragmentProgramAvailable;
	bool				glslAvailable;
	bool				uniformBufferAvailable;
	bool				twoSidedStencilAvailable;
	bool				depthBoundsTestAvailable;
	bool				syncAvailable;
	bool				timerQueryAvailable;
	bool				occlusionQueryAvailable;
	bool				debugOutputAvailable;
	bool				swapControlTearAvailable;

	stereo3DMode_t		stereo3Dmode;
	int					nativeScreenWidth; // this is the native screen width resolution of the renderer
	int					nativeScreenHeight; // this is the native screen height resolution of the renderer

	int					displayFrequency;

	int					isFullscreen;					// monitor number
	bool				isStereoPixelFormat;
	bool				stereoPixelFormatAvailable;
	int					multisamples;

	// Screen separation for stereoscopic rendering is set based on this.
	// PC vid code sets this, converting from diagonals / inches / whatever as needed.
	// If the value can't be determined, set something reasonable, like 50cm.
	float				physicalScreenWidthInCentimeters;	

	float				pixelAspect;

	GLuint				global_vao;
};



struct emptyCommand_t;

bool R_IsInitialized();

const int SMALLCHAR_WIDTH		= 8;
const int SMALLCHAR_HEIGHT		= 16;
const int BIGCHAR_WIDTH			= 16;
const int BIGCHAR_HEIGHT		= 16;

// all drawing is done to a 640 x 480 virtual screen size
// and will be automatically scaled to the real resolution
const int SCREEN_WIDTH			= 640;
const int SCREEN_HEIGHT			= 480;

const int TITLESAFE_LEFT		= 32;
const int TITLESAFE_RIGHT		= 608;
const int TITLESAFE_TOP			= 24;
const int TITLESAFE_BOTTOM		= 456;
const int TITLESAFE_WIDTH		= TITLESAFE_RIGHT - TITLESAFE_LEFT;
const int TITLESAFE_HEIGHT		= TITLESAFE_BOTTOM - TITLESAFE_TOP;

class idRenderWorld;

// PRD M3: per-frame values a transpiled MilkDrop custom warp/comp shader's
// uniform preamble expects (see neo/sound/MilkShaderTranspiler.cpp's
// kMilkPreamble) that this engine already computes every frame (audio
// analysis, EEL2 evaluator time/q-vars) -- passed to EnqueueVisMilkWarpDraw
// as plain data since RB_VisMilkWarpDraw sets these as raw GLSL uniforms by
// MilkDrop's own naming convention, not through the RENDERPARM_/rpUser<N>
// system every other shader in this engine uses. Known, deliberate scope gap
// remaining (documented in docs/ M3 notes, not modeled here at all): sampler_
// blur1-3/noise_lq-hq (no blur-pass or noise-texture pipeline exists yet).
// _qa.._qh (real MilkDrop2 2x2 matrices) ARE now real values -- they're just
// q[0..31] repacked 4-at-a-time (RB_SetMilkCommonUniforms in
// tr_backend_draw.cpp), the documented MilkDrop2 convention, not independent
// state idMilkEvaluator needs to track separately. rand_frame/rand_preset
// ARE real seeded RNG (a plain LCG, seeded per-preset-load / advanced
// per-frame -- see VisNextMilkRandFrame/VisSeedMilkRandPreset in
// visualizer_manager.cpp), just still scalar rather than MilkDrop's native
// float4.
struct visMilkWarpUniforms_t {
	float	time, frame, fps, progress;
	float	bass, mid, treb, bassAtt, midAtt, trebAtt;
	float	vol, volAtt;		// MilkDrop `vol`/`vol_att`: mean of bass/mid/treb (and _att)
	float	aspectX, aspectY;
	float	q[32];			// q1..q32, index 0 = q1
	float	randFrame, randPreset;
	float	roamCos[4], roamSin[4];	// MilkDrop `roam_cos`/`roam_sin`: 4 slow per-frame drifters
};

class idRenderSystem {
public:

	virtual					~idRenderSystem() {}

	// set up cvars and basic data structures, but don't
	// init OpenGL, so it can also be used for dedicated servers
	virtual void			Init() = 0;

	// only called before quitting
	virtual void			Shutdown() = 0;

	virtual void			ResetGuiModels() = 0;

	virtual void			InitOpenGL() = 0;

	virtual void			ShutdownOpenGL() = 0;

	virtual bool			IsOpenGLRunning() const = 0;

	virtual bool			IsFullScreen() const = 0;
	virtual int				GetWidth() const = 0;
	virtual int				GetHeight() const = 0;

	// return w/h of a single pixel. This will be 1.0 for normal cases.
	// A side-by-side stereo 3D frame will have a pixel aspect of 0.5.
	// A top-and-bottom stereo 3D frame will have a pixel aspect of 2.0
	virtual float			GetPixelAspect() const = 0;

	// This is used to calculate stereoscopic screen offset for a given interocular distance.
	virtual float			GetPhysicalScreenWidthInCentimeters() const = 0;

	// GetWidth() / GetHeight() return the size of a single eye
	// view, which may be replicated twice in a stereo display
	virtual stereo3DMode_t	GetStereo3DMode() const = 0;
	virtual bool			IsStereoScopicRenderingSupported() const = 0;
	virtual stereo3DMode_t	GetStereoScopicRenderingMode() const = 0;
	virtual void			EnableStereoScopicRendering( const stereo3DMode_t mode ) const = 0;
	virtual bool			HasQuadBufferSupport() const = 0;

	// allocate a renderWorld to be used for drawing
	virtual idRenderWorld *	AllocRenderWorld() = 0;
	virtual	void			FreeRenderWorld( idRenderWorld * rw ) = 0;

	// All data that will be used in a level should be
	// registered before rendering any frames to prevent disk hits,
	// but they can still be registered at a later time
	// if necessary.
	virtual void			BeginLevelLoad() = 0;
	virtual void			EndLevelLoad() = 0;
	virtual void			Preload( const idPreloadManifest &manifest, const char *mapName ) = 0;
	virtual void			LoadLevelImages() = 0;

	virtual void			BeginAutomaticBackgroundSwaps( autoRenderIconType_t icon = AUTORENDER_DEFAULTICON ) = 0;
	virtual void			EndAutomaticBackgroundSwaps() = 0;
	virtual bool			AreAutomaticBackgroundSwapsRunning( autoRenderIconType_t * icon = NULL ) const = 0;

	// font support
	virtual class idFont *	RegisterFont( const char * fontName ) = 0;
	virtual void			ResetFonts() = 0;

	virtual void			SetColor( const idVec4 & rgba ) = 0;
	virtual void			SetColor4( float r, float g, float b, float a ) { SetColor( idVec4( r, g, b, a ) ); }

	virtual uint32			GetColor() = 0;

	virtual void			SetGLState( const uint64 glState ) = 0;

	virtual void			DrawFilled( const idVec4 & color, float x, float y, float w, float h ) = 0;
	virtual void			DrawStretchPic( float x, float y, float w, float h, float s1, float t1, float s2, float t2, const idMaterial *material ) = 0;
			void			DrawStretchPic( const idVec4 & rect, const idVec4 & st, const idMaterial * material ) { DrawStretchPic( rect.x, rect.y, rect.z, rect.w, st.x, st.y, st.z, st.w, material ); }
	virtual void			DrawStretchPic( const idVec4 & topLeft, const idVec4 & topRight, const idVec4 & bottomRight, const idVec4 & bottomLeft, const idMaterial * material ) = 0;
	virtual void			DrawStretchTri ( const idVec2 & p1, const idVec2 & p2, const idVec2 & p3, const idVec2 & t1, const idVec2 & t2, const idVec2 & t3, const idMaterial *material ) = 0;
	virtual idDrawVert *	AllocTris( int numVerts, const triIndex_t * indexes, int numIndexes, const idMaterial * material, const stereoDepthType_t stereoType = STEREO_DEPTH_TYPE_NONE ) = 0;

	virtual void			PrintMemInfo( MemInfo_t *mi ) = 0;

	virtual void			DrawSmallChar( int x, int y, int ch ) = 0;
	virtual void			DrawSmallStringExt( int x, int y, const char *string, const idVec4 &setColor, bool forceColor ) = 0;
	virtual void			DrawBigChar( int x, int y, int ch ) = 0;
	virtual void			DrawBigStringExt( int x, int y, const char *string, const idVec4 &setColor, bool forceColor ) = 0;

	// dump all 2D drawing so far this frame to the demo file
	virtual void			WriteDemoPics() = 0;

	// draw the 2D pics that were saved out with the current demo frame
	virtual void			DrawDemoPics() = 0;

	// Performs final closeout of any gui models being defined.
	//
	// Waits for the previous GPU rendering to complete and vsync.
	//
	// Returns the head of the linked command list that was just closed off.
	//
	// Returns timing information from the previous frame.
	//
	// After this is called, new command buffers can be built up in parallel
	// with the rendering of the closed off command buffers by RenderCommandBuffers()
	virtual const emptyCommand_t *	SwapCommandBuffers( uint64 *frontEndMicroSec, uint64 *backEndMicroSec, uint64 *shadowMicroSec, uint64 *gpuMicroSec ) = 0;

	// SwapCommandBuffers operation can be split in two parts for non-smp rendering
	// where the GPU is idled intentionally for minimal latency.
	virtual void			SwapCommandBuffers_FinishRendering( uint64 *frontEndMicroSec, uint64 *backEndMicroSec, uint64 *shadowMicroSec, uint64 *gpuMicroSec ) = 0;
	virtual const emptyCommand_t *	SwapCommandBuffers_FinishCommandBuffers() = 0;

	// issues GPU commands to render a built up list of command buffers returned
	// by SwapCommandBuffers().  No references should be made to the current frameData,
	// so new scenes and GUIs can be built up in parallel with the rendering.
	virtual void			RenderCommandBuffers( const emptyCommand_t * commandBuffers ) = 0;

	// aviDemo uses this.
	// Will automatically tile render large screen shots if necessary
	// Samples is the number of jittered frames for anti-aliasing
	// If ref == NULL, common->UpdateScreen will be used
	// This will perform swapbuffers, so it is NOT an approppriate way to
	// generate image files that happen during gameplay, as for savegame
	// markers.  Use WriteRender() instead.
	virtual void			TakeScreenshot( int width, int height, const char *fileName, int samples, struct renderView_s *ref ) = 0;

	// the render output can be cropped down to a subset of the real screen, as
	// for save-game reviews and split-screen multiplayer.  Users of the renderer
	// will not know the actual pixel size of the area they are rendering to

	// the x,y,width,height values are in virtual SCREEN_WIDTH / SCREEN_HEIGHT coordinates

	// to render to a texture, first set the crop size with makePowerOfTwo = true,
	// then perform all desired rendering, then capture to an image
	// if the specified physical dimensions are larger than the current cropped region, they will be cut down to fit
	virtual void			CropRenderSize( int width, int height ) = 0;
	virtual void			CaptureRenderToImage( const char *imageName, bool clearColorAfterCopy = false ) = 0;
	// fixAlpha will set all the alpha channel values to 0xff, which allows screen captures
	// to use the default tga loading code without having dimmed down areas in many places
	virtual void			CaptureRenderToFile( const char *fileName, bool fixAlpha = false ) = 0;

	// PRD M7: same synchronous "flush + qglReadPixels" pattern
	// CaptureRenderToFile already uses, but reads BGRA8 directly into a
	// caller-provided buffer (outBufferSize must be >= GetWidth()*GetHeight()*4)
	// instead of writing a TGA to disk -- for a live consumer (NDI output)
	// that needs this frame's pixels in memory, not a round trip through a
	// file. Returns false (no-op) if outBufferSize is too small or the
	// renderer isn't initialized yet; fills in outWidth/outHeight on success.
	virtual bool			CaptureRenderToMemory( byte * outBGRA, int outBufferSize, int & outWidth, int & outHeight ) = 0;
	virtual void			UnCrop() = 0;

	// PRD M1 step 8: redirect subsequent 2D draws (DrawStretchPic/etc, up
	// until EndOffscreenRender) into an offscreen render target instead of
	// the backbuffer. Unlike CaptureRenderToImage (which copies an ALREADY-
	// drawn backbuffer, capped at the window's fixed-point pixel format --
	// see idImage::CopyFramebuffer, always GL_BACK), this renders directly
	// into imageName's own texture format -- e.g. FMT_RGBA16F -- so callers
	// needing more precision than the 8-bit backbuffer (MilkDrop-style
	// feedback accumulation) can get it, PROVIDED they also read back from
	// imageName directly next frame rather than going through
	// CaptureRenderToImage again. imageName is allocated/resized to width x
	// height (format FMT_RGBA16F) here if it doesn't already match.
	// EndOffscreenRender blits imageName back onto the backbuffer via a
	// full-screen textured quad (no glBlitFramebuffer available on this GL
	// binding set) so the frame is still visibly presented. No-op (with a
	// warning) if GL_ARB_framebuffer_object isn't available on this GL, or
	// if Begin/End calls are mismatched/nested.
	virtual void			BeginOffscreenRender( const char * imageName, int width, int height ) = 0;
	virtual void			EndOffscreenRender() = 0;

	// PRD M4: submits a deep-copied ImGui draw-data snapshot (built and
	// ImGui::Render()'d on the main thread -- ImGui's own context isn't
	// safe to touch from two threads at once) to the thread that actually
	// owns the GL context, via the same render-command mechanism
	// BeginOffscreenRender uses. drawDataClone is an opaque `void *` (a
	// heap-allocated deep copy of an ImDrawData) so this interface doesn't
	// need to depend on ImGui's types; ownership transfers to the render
	// command, which frees it after submitting.
	virtual void			EnqueueImGuiRender( void * drawDataClone ) = 0;

	// PRD M6/M7: defers the qglReadPixels work these two need to the
	// backend command thread instead of doing it synchronously wherever
	// the caller happens to run (see visCaptureCommand_t in tr_local.h --
	// CaptureRenderToFile/CaptureRenderToMemory above are still correct
	// when called from a properly-synchronized thread, e.g. the regular
	// screenshot command; these two exist for callers like Draw2D that
	// run on the SMP draw-worker thread and can't call qgl* directly).
	virtual void			EnqueueCaptureToFile( const char * fileName, bool fixAlpha ) = 0;
	virtual void			EnqueueCaptureToNDI() = 0;

	// PRD M3: compile a MilkDrop preset's transpiled warp/comp GLSL (see
	// idMilkShaderTranspiler) into a raw GL program on the thread that owns
	// the GL context. Called once per preset load, not per frame; replaces
	// whatever was compiled before. No-op (logs a warning) on compile/link
	// failure -- callers keep using the existing CPU-mesh warp path in that
	// case, same NFR-3 tolerance as every other preset-parsing failure mode.
	// compFragmentSource is NULL if the preset has no comp_ shader (or it
	// failed to transpile) -- only the warp pass runs in that case.
	// isMashB (PRD M5) routes this compile to a SEPARATE program slot for the
	// mash-up "B" side, alongside (not replacing) whatever's compiled for
	// the primary "A" side, so both can be drawn -- and alpha-blended
	// together -- the same frame.
	virtual void			EnqueueVisMilkShaderCompile( const char * vertexSource, const char * fragmentSource, const char * compFragmentSource, bool isMashB ) = 0;

	// PRD M3: draw one full-screen quad with the most recently compiled
	// MilkDrop custom shader (silently does nothing if none compiled/the
	// last compile failed -- caller's CPU-mesh path is the fallback).
	// imageName is bound as the shader's sampler_main.
	virtual void			EnqueueVisMilkWarpDraw( const char * imageName, const visMilkWarpUniforms_t & uniforms ) = 0;

	// PRD M5: draw the mash-up "B" side's own warp_/comp_ program (see
	// EnqueueVisMilkShaderCompile's isMashB), alpha-blended on top of
	// whatever this frame's EnqueueVisMilkWarpDraw call already drew --
	// mashMix (0..1) is B's compositing alpha; A always stays opaque.
	// Silently does nothing if B has no working warp_ shader compiled.
	virtual void			EnqueueVisMilkMashDraw( const char * imageName, const visMilkWarpUniforms_t & uniforms, float mashMix ) = 0;

	// the image has to be already loaded ( most straightforward way would be through a FindMaterial )
	// texture filter / mipmapping / repeat won't be modified by the upload
	// returns false if the image wasn't found
	virtual bool			UploadImage( const char *imageName, const byte *data, int width, int height ) = 0;

	// consoles switch stereo 3D eye views each 60 hz frame
	virtual int				GetFrameCount() const = 0;
};

extern idRenderSystem *			renderSystem;

//
// functions mainly intended for editor and dmap integration
//

// for use by dmap to do the carving-on-light-boundaries and for the editor for display
void R_LightProjectionMatrix( const idVec3 &origin, const idPlane &rearPlane, idVec4 mat[4] );

// used by the view shot taker
void R_ScreenshotFilename( int &lastNumber, const char *base, idStr &fileName );

#endif /* !__RENDERER_H__ */
