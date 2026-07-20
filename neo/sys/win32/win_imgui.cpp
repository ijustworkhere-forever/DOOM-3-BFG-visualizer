// PRD M4: ImGui Win32+OpenGL3 integration glue. See win_imgui.h for the
// design notes (threading, why the header stays windows.h-free).
#pragma hdrstop
#include "../../idlib/precompiled.h"
#include "win_local.h"
#include "win_imgui.h"

#include "../../external/imgui/imgui.h"
#include "../../external/imgui/backends/imgui_impl_win32.h"
#include "../../external/imgui/backends/imgui_impl_opengl3.h"

#include "../../renderer/RenderSystem.h"
#include "../../renderer/tr_local.h"		// imguiRenderCommand_t, RC_IMGUI_RENDER

// imgui_impl_win32.h intentionally wraps this declaration in '#if 0' (to
// keep that header windows.h-free) and documents copying it into the
// caller's own .cpp instead -- this file already has windows.h via
// win_local.h, so this is exactly that copy.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam );

static bool s_imguiInitialized = false;

// PRD (post-M4 follow-up, direct user request): the layer-editor panel
// lives in its OWN top-level Win32 window, separate from the game's render
// window -- like the classic system console window (win_syscon.cpp's
// Sys_CreateConsole) that pops up at launch, not an overlay drawn on top of
// the 3D view. Deliberately SHARES the game's single GL context/resources
// (font atlas, vertex/index buffers ImGui's OpenGL3 backend owns) rather
// than creating and managing a second, independent context via
// wglShareLists -- simpler, and safe because RB_ImGuiRender already runs
// exclusively on the one thread that owns GL context binding (see
// win_imgui.h's threading note); temporarily rebinding that SAME context to
// a different HDC for the duration of one render call, then rebinding it
// back, introduces no new cross-thread concern. The tool window gets its
// own pixel format via DescribePixelFormat/SetPixelFormat using the exact
// format INDEX already applied to the main window (win32.pixelformat) --
// not a fresh ChoosePixelFormat call -- guaranteeing GL-context compatibility
// by construction rather than by chance.
static const char * const IMGUI_TOOL_WNDCLASS_NAME = "VisImGuiToolWindow";
static HWND  s_imguiHwnd = NULL;
static HDC   s_imguiHdc = NULL;
static bool  s_imguiToolWindowVisible = false;

static LRESULT CALLBACK ImGuiToolWndProc( HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam ) {
	Vis_ImGuiWndProcHandler( hWnd, uMsg, wParam, lParam );
	if ( uMsg == WM_CLOSE ) {
		// Hide, don't destroy -- toggling vis_imguiEditor off is the "close
		// this tool window" gesture, matching how the classic system
		// console window is hidden/shown (Sys_ShowConsole) rather than
		// destroyed and recreated each time. vis_imguiEditor itself is
		// defined in visualizer_manager.cpp; forward-declared here rather
		// than pulling in that whole header for one cvar.
		extern idCVar vis_imguiEditor;
		vis_imguiEditor.SetBool( false );
		ShowWindow( hWnd, SW_HIDE );
		s_imguiToolWindowVisible = false;
		return 0;
	}
	if ( uMsg == WM_DESTROY ) {
		return 0;
	}
	return DefWindowProc( hWnd, uMsg, wParam, lParam );
}

static bool CreateImGuiToolWindow() {
	WNDCLASS wc = {};
	wc.style = CS_OWNDC;
	wc.lpfnWndProc = ImGuiToolWndProc;
	wc.hInstance = win32.hInstance;
	wc.hCursor = LoadCursor( NULL, IDC_ARROW );
	wc.hbrBackground = (HBRUSH)( COLOR_BTNFACE + 1 );
	wc.lpszClassName = IMGUI_TOOL_WNDCLASS_NAME;
	if ( !RegisterClass( &wc ) ) {
		idLib::Warning( "Vis_ImGuiInit: could not register tool window class" );
		return false;
	}

	s_imguiHwnd = CreateWindowEx( 0, IMGUI_TOOL_WNDCLASS_NAME, "Visualizer Layer Editor",
		WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 500, 760,
		NULL, NULL, win32.hInstance, NULL );
	if ( s_imguiHwnd == NULL ) {
		idLib::Warning( "Vis_ImGuiInit: CreateWindowEx failed for tool window" );
		return false;
	}

	s_imguiHdc = GetDC( s_imguiHwnd );
	if ( s_imguiHdc == NULL ) {
		idLib::Warning( "Vis_ImGuiInit: GetDC failed for tool window" );
		DestroyWindow( s_imguiHwnd );
		s_imguiHwnd = NULL;
		return false;
	}

	PIXELFORMATDESCRIPTOR pfd = {};
	DescribePixelFormat( win32.hDC, win32.pixelformat, sizeof( pfd ), &pfd );
	if ( !SetPixelFormat( s_imguiHdc, win32.pixelformat, &pfd ) ) {
		idLib::Warning( "Vis_ImGuiInit: SetPixelFormat failed for tool window" );
		ReleaseDC( s_imguiHwnd, s_imguiHdc );
		s_imguiHdc = NULL;
		DestroyWindow( s_imguiHwnd );
		s_imguiHwnd = NULL;
		return false;
	}
	return true;
}

bool Vis_ImGuiIsInitialized() {
	return s_imguiInitialized;
}

bool Vis_ImGuiInit() {
	if ( s_imguiInitialized ) {
		return true;
	}
	if ( win32.hWnd == NULL || win32.hGLRC == NULL ) {
		idLib::Warning( "Vis_ImGuiInit: no window/GL context yet" );
		return false;
	}
	if ( !CreateImGuiToolWindow() ) {
		return false;
	}

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO & io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

	if ( !ImGui_ImplWin32_InitForOpenGL( s_imguiHwnd ) ) {
		idLib::Warning( "Vis_ImGuiInit: ImGui_ImplWin32_InitForOpenGL failed" );
		ImGui::DestroyContext();
		return false;
	}
	// this engine's GLSL backend targets #version 150 core (confirmed by
	// reading RenderProgs_GLSL.cpp's ConvertCG2GLSL output directly) --
	// matching it here keeps ImGui's own shaders on the same GL profile.
	if ( !ImGui_ImplOpenGL3_Init( "#version 150" ) ) {
		idLib::Warning( "Vis_ImGuiInit: ImGui_ImplOpenGL3_Init failed" );
		ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext();
		return false;
	}

	s_imguiInitialized = true;
	idLib::Printf( "Vis_ImGuiInit: ImGui %s initialized (Win32 + OpenGL3, separate tool window)\n", IMGUI_VERSION );
	return true;
}

void Vis_ImGuiShutdown() {
	if ( !s_imguiInitialized ) {
		return;
	}
	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	s_imguiInitialized = false;
	if ( s_imguiHdc != NULL ) {
		ReleaseDC( s_imguiHwnd, s_imguiHdc );
		s_imguiHdc = NULL;
	}
	if ( s_imguiHwnd != NULL ) {
		DestroyWindow( s_imguiHwnd );
		s_imguiHwnd = NULL;
	}
}

// Shows/hides the tool window to match vis_imguiEditor -- called every
// frame from Frame() regardless of the cvar's value (unlike
// Vis_ImGuiNewFrame/EndFrame, which only run while it's true) so flipping
// the cvar off actually hides the window instead of just freezing its
// last-drawn contents. Idempotent (diffs against the last known state)
// so this costs nothing beyond a bool compare on the common case.
void Vis_ImGuiSetToolWindowVisible( bool visible ) {
	if ( !s_imguiInitialized || s_imguiHwnd == NULL ) {
		return;
	}
	// PRD (post-M4 follow-up): "hide the cursor over the visualization
	// display but not the ImGui window." ShowCursor's hide count is
	// per-thread, not per-window (see common_frame.cpp's mouse-grab
	// condition, which this cvar feeds), so the only way to get per-window
	// cursor behavior out of this engine's single per-frame grab/release
	// decision point is to drive that decision from where the mouse
	// actually IS, not merely whether the tool window is open at all (that
	// would leave the cursor visible over the game view too, even while
	// the editor window just sits open unused in the background).
	// Deliberately geometric (WindowFromPoint against the live cursor
	// position), NOT GetForegroundWindow()/focus-based -- an earlier
	// attempt using foreground-window comparison looked correct in a
	// scripted SetForegroundWindow-driven test, but real mouse-driven
	// interaction didn't reliably show the same result, most likely
	// because Windows' focus-stealing-prevention rules (and this engine's
	// own WM_ACTIVATE-driven win32.activeApp bookkeeping in win_wndproc.cpp,
	// only updated when the GAME window itself receives WM_ACTIVATE) don't
	// guarantee the two top-level windows' active/foreground state changes
	// in lockstep the instant a real mouse click transfers focus between
	// them. Checking "what's physically under the cursor right now" has no
	// such timing dependency. GetAncestor(..., GA_ROOT) walks up in case
	// ImGui/the OS ever puts a child control under the cursor instead of
	// the top-level HWND itself. Runs every frame regardless of the
	// show/hide diff below.
	POINT cursorPt;
	HWND underCursor = ( GetCursorPos( &cursorPt ) ) ? WindowFromPoint( cursorPt ) : NULL;
	const bool overToolWindow = ( underCursor != NULL && GetAncestor( underCursor, GA_ROOT ) == s_imguiHwnd );
	cvarSystem->SetCVarBool( "vis_imguiEditorFocused", overToolWindow );

	if ( visible == s_imguiToolWindowVisible ) {
		return;
	}
	ShowWindow( s_imguiHwnd, visible ? SW_SHOW : SW_HIDE );
	s_imguiToolWindowVisible = visible;
}

void Vis_ImGuiNewFrame() {
	if ( !s_imguiInitialized ) {
		return;
	}
	ImGui_ImplOpenGL3_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();
}

// PRD M4: ImGui's own supported mechanism for multi-threaded rendering
// (ImDrawList::CloneOutput(), documented in imgui.h right next to the
// pointer it returns, referencing the imgui_club "imgui_threaded_rendering"
// example this mirrors) -- deep-copies the CmdBuffer/IdxBuffer/VtxBuffer
// each ImDrawList owns, which is the per-frame data ImGui::NewFrame() would
// otherwise reuse/overwrite out from under a backend thread still reading
// last frame's draw data. OwnerViewport/Textures are copied by pointer, NOT
// deep-copied -- OwnerViewport is unused in this single-viewport
// integration; Textures (ImGui's dynamic texture-update list, new in
// v1.92) is a narrow, documented gap: a race is only possible during an
// actual texture upload/update (e.g. first-frame font atlas creation, or if
// this integration ever dynamically adds fonts/images later), not on every
// frame's normal widget rendering.
static ImDrawData * CloneImDrawData( const ImDrawData * src ) {
	if ( src == NULL || !src->Valid ) {
		return NULL;
	}
	ImDrawData * dst = IM_NEW( ImDrawData )();
	dst->Valid = src->Valid;
	dst->CmdListsCount = src->CmdListsCount;
	dst->TotalIdxCount = src->TotalIdxCount;
	dst->TotalVtxCount = src->TotalVtxCount;
	dst->DisplayPos = src->DisplayPos;
	dst->DisplaySize = src->DisplaySize;
	dst->FramebufferScale = src->FramebufferScale;
	dst->OwnerViewport = src->OwnerViewport;
	dst->Textures = src->Textures;
	for ( int i = 0; i < src->CmdLists.Size; i++ ) {
		dst->CmdLists.push_back( src->CmdLists[i]->CloneOutput() );
	}
	return dst;
}

static void FreeClonedImDrawData( ImDrawData * data ) {
	if ( data == NULL ) {
		return;
	}
	for ( int i = 0; i < data->CmdLists.Size; i++ ) {
		IM_DELETE( data->CmdLists[i] );
	}
	IM_DELETE( data );
}

void Vis_ImGuiEndFrame() {
	if ( !s_imguiInitialized ) {
		return;
	}
	ImGui::Render();
	ImDrawData * clone = CloneImDrawData( ImGui::GetDrawData() );
	if ( clone != NULL ) {
		renderSystem->EnqueueImGuiRender( clone );
	}
}

// PRD M4: backend-thread handler for RC_IMGUI_RENDER, dispatched from
// gl_backend.cpp -- runs on whichever thread actually owns the GL context
// (see win_imgui.h's threading note). Frees the clone after submitting it;
// ownership transferred here from Vis_ImGuiEndFrame's EnqueueImGuiRender call.
void RB_ImGuiRender( const void * data ) {
	const imguiRenderCommand_t * cmd = (const imguiRenderCommand_t *)data;
	ImDrawData * drawData = (ImDrawData *)cmd->drawDataClone;
	if ( drawData != NULL && s_imguiHdc != NULL ) {
		// PRD: the tool window is a separate top-level window/HDC from the
		// game's own render surface, but shares the SAME GL context (see
		// the design note above s_imguiHwnd) -- rebind that context to the
		// tool window's HDC just for this submission, then rebind it back
		// to whatever this thread had bound before, so the game's own
		// subsequent GL calls this frame are unaffected.
		HDC prevHdc = qwglGetCurrentDC();
		HGLRC prevRc = qwglGetCurrentContext();
		if ( qwglMakeCurrent( s_imguiHdc, prevRc ) ) {
			int w = (int)drawData->DisplaySize.x;
			int h = (int)drawData->DisplaySize.y;
			if ( w > 0 && h > 0 ) {
				qglViewport( 0, 0, w, h );
				qglClearColor( 0.15f, 0.15f, 0.17f, 1.0f );
				qglClear( GL_COLOR_BUFFER_BIT );
			}
			ImGui_ImplOpenGL3_RenderDrawData( drawData );
			qwglSwapBuffers( s_imguiHdc );
			qwglMakeCurrent( prevHdc, prevRc );
		}
		FreeClonedImDrawData( drawData );
	}
}

bool Vis_ImGuiWndProcHandler( void * hwnd, unsigned int msg, unsigned long long wParam, long long lParam ) {
	if ( !s_imguiInitialized ) {
		return false;
	}
	return ImGui_ImplWin32_WndProcHandler( (HWND)hwnd, msg, (WPARAM)wParam, (LPARAM)lParam ) != 0;
}

bool Vis_ImGuiWantCaptureMouse() {
	return s_imguiInitialized && ImGui::GetIO().WantCaptureMouse;
}

bool Vis_ImGuiWantCaptureKeyboard() {
	return s_imguiInitialized && ImGui::GetIO().WantCaptureKeyboard;
}
