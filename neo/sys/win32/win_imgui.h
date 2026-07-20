#ifndef __WIN_IMGUI_H__
#define __WIN_IMGUI_H__

/*
================================================
PRD M4: ImGui (Win32 + OpenGL3 backends, vendored under neo/external/imgui/)
integration glue. Same windows.h-free-header discipline as win_midi.h/
win_wallpaper.h -- this header exposes only plain functions so
cross-platform-intended code (win_glimp.cpp still needs it since IT owns
the window/GL context, but visualizer_manager.cpp only needs this narrow
surface) doesn't have to see <windows.h> or the ImGui backend headers.

Note: visualizer_manager.cpp DOES include imgui.h directly (to build actual
UI content with ImGui:: widget calls) -- that's safe, since imgui.h itself
has zero Windows/GL dependency by design; only the impl_win32 and
impl_opengl3 backend headers (which stay confined to win_imgui.cpp) touch
platform APIs.

Threading (read before using): ImGui's own context is NOT safe to touch
from two threads concurrently. Vis_ImGuiNewFrame()/the UI-building ImGui::
calls/Vis_ImGuiEndFrame() must all happen on the SAME thread each frame --
this codebase calls them from the main thread (idVisualizerManager::Frame),
matching the "resolve on main thread" rule already used for every other
per-frame visualizer computation. Vis_ImGuiEndFrame() deep-copies the
finished draw data and hands the clone to the render command stream
(RC_IMGUI_RENDER, see tr_local.h/RenderSystem.h) so the ACTUAL GL submission
(RB_ImGuiRender, implemented in win_imgui.cpp) happens on whichever thread
owns the GL context -- the same split every other GL-touching visualizer
feature (the M1 step 8 FBO work) already established.
================================================
*/

// Called once, after GLimp_Init has a valid window AND a current GL context
// (both are required by ImGui_ImplWin32_Init/ImGui_ImplOpenGL3_Init).
// Returns false (logging why) on failure -- callers should treat ImGui as
// simply unavailable rather than fatal.
bool Vis_ImGuiInit();

// Called once, from GLimp_Shutdown, BEFORE the GL context/window are torn
// down (mirrors Init's ordering requirement in reverse). Safe to call even
// if Init was never called or failed.
void Vis_ImGuiShutdown();

// True once Vis_ImGuiInit() has succeeded; callers should skip all other
// Vis_ImGui* calls if this is false (ImGui unavailable on this build/GL).
bool Vis_ImGuiIsInitialized();

// Shows/hides the separate tool window the layer editor now lives in (see
// win_imgui.cpp's design note on s_imguiHwnd for why it's a standalone
// top-level window rather than an overlay on the game's own render
// window). Call every frame with the current vis_imguiEditor value,
// regardless of whether it's true or false -- unlike Vis_ImGuiNewFrame/
// EndFrame (only called while true), this needs to run even when it just
// flipped false so the window actually hides instead of freezing its last
// contents. Idempotent (diffs internally), safe to call before Init.
void Vis_ImGuiSetToolWindowVisible( bool visible );

// Starts a new ImGui frame (ImGui_ImplOpenGL3_NewFrame + ImGui_ImplWin32_NewFrame
// + ImGui::NewFrame()). Call once per frame, on the main thread, before any
// ImGui:: widget-building calls.
void Vis_ImGuiNewFrame();

// Finishes the frame (ImGui::Render()), deep-copies the resulting draw data
// (so the backend-thread submission below can't race the NEXT frame's
// NewFrame overwriting ImGui's internal buffers), and enqueues it via
// renderSystem->EnqueueImGuiRender() for the backend thread to actually
// submit to the GPU. Call once per frame, on the main thread, after all
// ImGui:: widget-building calls for this frame are done.
void Vis_ImGuiEndFrame();

// Routes a single Win32 message to ImGui (mouse/keyboard/etc capture).
// Called from MainWndProc; msg/wParam/lParam/hwnd are passed through as
// plain integers/pointers so this header stays windows.h-free (the .cpp
// casts them back to the real Win32 types). Returns true if ImGui consumed
// the message (caller may still want to fall through to its own handling;
// see MainWndProc's own comment for how the engine already shares message
// handling with DirectInput's separate polling path).
bool Vis_ImGuiWndProcHandler( void * hwnd, unsigned int msg, unsigned long long wParam, long long lParam );

// True if ImGui wants to consume mouse/keyboard input this frame (e.g. a
// text field has focus) -- exposed so the engine's own input handling can
// choose to skip if it wants to avoid double-handling input while an ImGui
// widget has focus. Not currently wired to anything; exposed for future use.
bool Vis_ImGuiWantCaptureMouse();
bool Vis_ImGuiWantCaptureKeyboard();

#endif // __WIN_IMGUI_H__
