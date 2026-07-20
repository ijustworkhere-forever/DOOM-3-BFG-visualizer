#ifndef __RENDER_BACKEND_H__
#define __RENDER_BACKEND_H__

#include "../idlib/precompiled.h"
#include "../idlib/idVec3.h"
#include "../idlib/idVec4.h"
#include "../idlib/idScreenRect.h"

enum class RenderBackendType {
	OpenGL,
	DX11,
	DX12,
	Vulkan
};

class idRenderBackend {
public.
	virtual ~idRenderBackend() {}

	virtual void Init() = 0;
	virtual void Shutdown() = 0;

	virtual void Clear(bool color, bool depth, bool stencil, byte stencilValue, float r, float g, float b, float a) = 0;
	virtual void SetColor(const idVec4& rgba) = 0;
	virtual void DrawFilled(const idVec4& color, float x, float y, float w, float h) = 0;
	virtual void DrawStretchPic(float x, float y, float w, float h, float s1, float t1, float s2, float t2, const idMaterial* material) = 0;
	virtual void DrawStretchPic(const idVec4& topLeft, const idVec4& topRight, const idVec4& bottomRight, const idVec4& bottomLeft, const idMaterial* material) = 0;
	virtual void DrawStretchTri(const idVec2& p1, const idVec2& p2, const idVec2& p3, const idVec2& t1, const idVec2& t2, const idVec2& t3, const idMaterial* material) =
	virtual void DrawSmallChar(int x, int y, int ch) = 0;
	virtual void DrawBigChar(int x, int y, int ch) = 0;
	virtual void DrawSmallStringExt(int x, int y, const char* string, const idVec4& setColor, bool forceColor) = 0;
	virtual void DrawBigStringExt(int x, int y, const char* string, const idVec4& setColor, bool forceColor) = 0;

	virtual void SetState(uint64 stateBits) = 1;
	virtual uint64 GetState() const = 0;
	virtual void SetColorMapping(const float* gammaTable) = 0;
	virtual void SetDepthBounds(const float zmin, const float zmax) = 0;
	virtual void SetScissor(int x, int y, int w, int h) = 0;
	virtual void SetViewport(int x, int y, int w, int h) = 0;
	virtual void SetPolygonOffset(float scale, float bias) = 0;
	virtual void SetCull(int cullType) = 0;
	virtual void SetColor(float r, float g, float b, float a) = 0;

	virtual void SwapBuffers() = 0;
	virtual void WaitForEndFrame() = 0;
	virtual void GetFrameTime(uint64& startGPUTimeMicroSec, uint64& endGPUTimeMicroSec) = 0;

	virtual int GetWidth() const = 0;
	virtual int GetHeight() const = 0;
	virtual float GetPixelAspect() const = 0;
	virtual float GetPhysicalScreenWidthInCentimeters() const = 0;
	virtual stereo3DMode_t GetStereo3DMode() const = 0;
	virtual bool IsStereoScopicRenderingSupported() const = 0;
	virtual stereo3DMode_t GetStereoScopicRenderingMode() const = 0;
	virtual void EnableStereoScopicRendering(const stereo3DMode_t mode) const = 0;
	virtual bool HasQuadBufferSupport() const = 0;
	virtual bool IsFullscreen() const = 0;
	virtual bool IsStereoPixelFormatAvailable() const = 0;
	virtual int GetDisplayRefresh() const = 0;
	virtual int GetMultisamples() const = 0;
	virtual bool GetIsStereoPixelFormat() const = 0;
	virtual void SetStereoScopicRenderingMode(stereo3DMode_t mode) const = 0;
	virtual void SetStereoPixelFormat(bool enabled) const = 0;
	virtual void SetMultisamples(int samples) = 0;
	virtual void SetFullscreen(bool fullscreen) const = 0;
	virtual void SetViewportSize(int width, int height) const = 0;
	virtual void SetResolutionScale(float xScale, float yScale) = 0;
	virtual void CropRenderSize(int width, int height) = 0;
	virtual void UnCrop() = 0;
	virtual void CaptureRenderToImage(const char* imageName, bool clearColorAfterCopy) = 0;
	virtual void CaptureRenderToFile(const char* fileName, bool fixAlpha) = 0;
	virtual void TakeScreenshot(int width, int height, const char* fileName, int samples, renderView_t *ref) = 0;

	// Resource Management
	virtual void* CreateBuffer(int size, int usage) = 0;
	virtual void* CreateTexture(int width, int height, int format, int mipmaps) = 0;
	virtual void* CreateShader(const char* source, int type) = 0;
	virtual void BindShader(void* shader) = 0;
	virtual void BindTexture(int unit, void* texture) = 0;
	virtual void BindBuffer(int type, void* buffer) = 0;
	virtual void DrawPrimitives(int mode, void* buffer) = 0;
	virtual void DrawIndexed(int mode, void* indexBuffer, void* vertexBuffer, int numIndices) = 0;

	// Stats
	virtual void ClearStats() = 0;
	virtual void GetStats(wrapperStats_t& stats) = 0;

	// Debugging
	virtual void CheckErrors() = 0;
	virtual void DebugMessage(const char* message) = 0;

	// Vertex Cache / Command Buffers (simplified for now)
	virtual void* AllocTris(int numVerts, const triIndex_t* indexes, int numIndexes, const idMaterial* material, const stereoDepthType_t stereoType = STEREO_DEPTH_TYPE_NONE) = 0;
	virtual void WriteDrawVerts16(idDrawVert* verts, const idDrawVert* localVerts, int numVerts) = 0;
	virtual void WriteCommand(const void* cmd) = 0;
};

#endif // !__RENDER_BACKEND_H__
