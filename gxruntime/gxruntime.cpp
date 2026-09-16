#include "std.h"
#include "gxruntime.h"
#include "zmouse.h"
#include <shellapi.h>

#include "../gxruntime/gxutf8.h"

#include <sstream>
#include <shellapi.h>

#include "bass.h"

#include "sdlgpu/sdl_gpu_context.h"
#include "sdlgpu/sdl_gpu_pipeline.h"
#include "sdlgpu/sdl_gpu_texture.h"

static bool SetModernDPIAwareness() {
	HMODULE hShcore = LoadLibraryW(L"shcore.dll");
	if (!hShcore) return false;

	typedef HRESULT(WINAPI* SetProcessDpiAwarenessFunc)(int);
	SetProcessDpiAwarenessFunc pSetProcessDpiAwareness = (SetProcessDpiAwarenessFunc)GetProcAddress(hShcore, "SetProcessDpiAwareness");

	if (!pSetProcessDpiAwareness) {
		FreeLibrary(hShcore);
		return false;
	}

	const int PROCESS_PER_MONITOR_DPI_AWARE = 2;
	HRESULT hr = pSetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);
	FreeLibrary(hShcore);

	return SUCCEEDED(hr);
}

void SetAppDPIAware() {
	if (SetModernDPIAwareness()) { return; }
	//fallback for old ass pcs
	SetProcessDPIAware();
}

static void DebugMsg(const char* msg) {
	// MessageBoxA(NULL, msg, "Graphics Debug", MB_OK);
}

static void DebugMsg(const std::string& msg) {
	// MessageBoxA(NULL, msg.c_str(), "Graphics Debug", MB_OK);
}

static DWORD pickVertexProcessingFlag(IDirect3D9* d3d, UINT adapter) {
	D3DCAPS9 caps;
	if (FAILED(d3d->GetDeviceCaps(adapter, D3DDEVTYPE_HAL, &caps))) {
		return D3DCREATE_SOFTWARE_VERTEXPROCESSING;
	}
	if (!(caps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT)) {
		return D3DCREATE_SOFTWARE_VERTEXPROCESSING;
	}
	return D3DCREATE_HARDWARE_VERTEXPROCESSING;
}

struct gxRuntime::GfxMode {
	D3DDISPLAYMODE mode;
};

struct gxRuntime::GfxDriver {
	D3DADAPTER_IDENTIFIER9 identifier;
	std::vector<GfxMode*> modes;
	UINT adapter;
};

static const int static_ws = WS_VISIBLE | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
static const int scaled_ws = WS_VISIBLE | WS_CAPTION | WS_SYSMENU | WS_SIZEBOX | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;

static std::string app_title;
static std::string app_close;
static gxRuntime* runtime;
static bool busy, suspended;
static volatile bool run_flag;
static Debugger* debugger;

typedef int(_stdcall* LibFunc)(const void* in, int in_sz, void* out, int out_sz);

struct gxDll {
	HINSTANCE hinst;
	std::map<std::string, LibFunc> funcs;
};

static std::map<std::string, gxDll*> libs;

static CRITICAL_SECTION g_gfxCS;

static LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

//current gfx mode
//
//0=NONE
//1=SCALED WINDOW
//2=FIXED SIZE WINDOW
//3=EXCLUSIVE
//
static int gfx_mode;
static int border_mode;
static bool gfx_lost;
static bool auto_suspend;

//for modes 1 and 2
static int mod_cnt;
static MMRESULT timerID;
static std::set<gxTimer*> timers;

enum { WM_STOP = WM_APP + 1, WM_RUN, WM_END };

////////////////////
// STATIC STARTUP //
////////////////////
gxRuntime* gxRuntime::openRuntime(HINSTANCE hinst, const std::string& cmd_line, Debugger* d) {
	SetAppDPIAware();
	if(runtime) return 0;

	//create debugger
	debugger = d;

	WNDCLASS wndclass = { 0 };
	wndclass.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
	wndclass.lpfnWndProc = ::windowProc;
	wndclass.hInstance = hinst;
	wndclass.lpszClassName = "Blitz Runtime Class";
	wndclass.hCursor = (HCURSOR)LoadCursor(0, IDC_ARROW);
	wndclass.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
	RegisterClass(&wndclass);

	gfx_mode = GMODE_NONE;
	busy = suspended = false;
	run_flag = true;

	HWND hwnd = CreateWindowEx(0, "Blitz Runtime Class", " ", WS_CAPTION, 0, 0, 0, 0, 0, 0, 0, 0);
	UpdateWindow(hwnd);

	runtime = new gxRuntime(hinst, cmd_line, hwnd);
	InitializeCriticalSection(&g_gfxCS);
	return runtime;
}

void gxRuntime::closeRuntime(gxRuntime* r) {
	if (!runtime || runtime != r) return;
	for (auto it = libs.begin(); it != libs.end(); ++it)
		FreeLibrary(it->second->hinst);
	libs.clear();

	EnterCriticalSection(&g_gfxCS);
	gxRuntime* whatARottenWayToDie = runtime;
	runtime = 0;
	LeaveCriticalSection(&g_gfxCS);
	delete whatARottenWayToDie;

	DeleteCriticalSection(&g_gfxCS);
}


//////////////////////////
// RUNTIME CONSTRUCTION //
//////////////////////////
gxRuntime::gxRuntime(HINSTANCE hi, const std::string& cl, HWND hw) :
	hinst(hi), cmd_line(cl), hwnd(hw), curr_driver(0), enum_all(false),
	pointer_visible(true), audio(0), input(0), graphics(0), fileSystem(0), use_di(false),
	d3d(0), d3dDevice(0), backBuffer(0), frontBuffer(0),
	stretchRT(0), stretchRT_w(0), stretchRT_h(0) {

	CoInitialize(0);



	memset(&d3ddmEx, 0, sizeof(d3ddmEx));
	d3ddmEx.Size = sizeof(D3DDISPLAYMODEEX);

	if (FAILED(Direct3DCreate9Ex(D3D_SDK_VERSION, &d3d))) {
		d3d = nullptr;
		debugLog("Direct3D9 not available");
	}

	enumGfx();

	TIMECAPS tc;
	timeGetDevCaps(&tc, sizeof(tc));
	timeBeginPeriod(tc.wPeriodMin);

	memset(&osinfo, 0, sizeof(osinfo));
	osinfo.dwOSVersionInfoSize = sizeof(osinfo);

	HMODULE osinfodll = LoadLibraryA("ntdll.dll");
	if (osinfodll) {
		typedef void (WINAPI* RtlGetVersionFunc) (OSVERSIONINFO*);
		RtlGetVersionFunc RtlGetVersion = (RtlGetVersionFunc)GetProcAddress(osinfodll, "RtlGetVersion");
		if(RtlGetVersion) RtlGetVersion(&osinfo);
		FreeLibrary(osinfodll);
	}

	memset(&statex, 0, sizeof(statex));
	statex.dwLength = sizeof(statex);
	GlobalMemoryStatusEx(&statex);

	memset(&devmode, 0, sizeof(devmode));
	devmode.dmSize = sizeof(devmode);
	EnumDisplaySettings(NULL, ENUM_CURRENT_SETTINGS, &devmode);
}

gxRuntime::~gxRuntime() {
	while(timers.size()) freeTimer(*timers.begin());
	if(audio) closeAudio(audio);
	if(graphics) closeGraphics(graphics);
	if(input) closeInput(input);
	destroySDLWindow();
	TIMECAPS tc;
	timeGetDevCaps(&tc, sizeof(tc));
	timeEndPeriod(tc.wPeriodMin);
	denumGfx();
	DestroyWindow(hwnd);
	UnregisterClass("Blitz Runtime Class", hinst);



	CoUninitialize();
}

void gxRuntime::pauseAudio() {
	if(audio) audio->pause();
}

void gxRuntime::resumeAudio() {
	if(audio) audio->resume();
}

void gxRuntime::restoreGraphics() {
	if (!graphics) return;
	gxGraphics::DeviceState state = graphics->getDeviceState();
	if (state == gxGraphics::DEVICE_NEEDS_RESET || state == gxGraphics::DEVICE_LOST) {
		gfx_lost = !graphics->restore();
	}
	else {
		gfx_lost = false;
	}
}

void gxRuntime::resetInput() {
	if(input) input->reset();
}

void gxRuntime::acquireInput() {
	if(!input) return;
	if(gfx_mode == GMODE_EXCLUSIVE) {
		if(use_di) {
			use_di = input->acquire();
		}
		else {
		}
	}
	input->reset();
}

void gxRuntime::unacquireInput() {
	if(!input) return;
	if(gfx_mode == GMODE_EXCLUSIVE && use_di) input->unacquire();
	input->reset();
}

/////////////
// SUSPEND //
/////////////
void gxRuntime::suspend() {
	busy = true;
	pauseAudio();
	unacquireInput();
	if (GetCapture() == hwnd) ReleaseCapture();
	suspended = true;
	busy = false;

	if (gfx_mode == GMODE_EXCLUSIVE) {
		if (usingSDLWindow()) sdlgpu::SetCursorVisible(true);
		else ShowCursor(1);
	}

	if(debugger) debugger->debugStop();
}

////////////
// RESUME //
////////////
void gxRuntime::resume() {
	if (gfx_mode == GMODE_EXCLUSIVE) {
		if (usingSDLWindow()) sdlgpu::SetCursorVisible(false);
		else ShowCursor(0);
	}
	busy = true;
	acquireInput();
	restoreGraphics();
	resumeAudio();
	suspended = false;
	busy = false;

	if(debugger) debugger->debugRun();
}

///////////////////
// FORCE SUSPEND //
///////////////////
void gxRuntime::forceSuspend() {
	if (gfx_mode == GMODE_EXCLUSIVE && !usingSDLWindow()) {
		ShowWindow(hwnd, SW_MINIMIZE);
		SetForegroundWindow(GetDesktopWindow());
	}
	else {
		suspend();
	}
}

//////////////////
// FORCE RESUME //
//////////////////
void gxRuntime::forceResume() {
	if (gfx_mode == GMODE_EXCLUSIVE && !usingSDLWindow()) {
		SetForegroundWindow(hwnd);
		ShowWindow(hwnd, SW_SHOWMAXIMIZED);
	}
	else {
		resume();
	}
}

///////////
// PAINT //
///////////
void gxRuntime::paint() {
	if (!d3dDevice || !backBuffer) return;

	gxGraphics::DeviceState state = graphics->getDeviceState();
	if (state == gxGraphics::DEVICE_LOST) {
		gfx_lost = true;
		return;
	}
	if (state == gxGraphics::DEVICE_NEEDS_RESET) {
		if (!graphics->restore()) {
			gfx_lost = true;
			return;
		}
		gfx_lost = false;
	}

	switch (gfx_mode) {
	case GMODE_SCALED: {
		if (!graphics) break;
		gxCanvas* f = graphics->getFrontCanvas();
		if (!f) break;
		IDirect3DSurface9* canvasSurf = f->getSurface();
		if (!canvasSurf) break;

		if (canvasSurf != backBuffer) {
			D3DSURFACE_DESC srcDesc, dstDesc;
			bool canUpdate = SUCCEEDED(canvasSurf->GetDesc(&srcDesc)) &&
				SUCCEEDED(backBuffer->GetDesc(&dstDesc)) &&
				srcDesc.MultiSampleType == D3DMULTISAMPLE_NONE &&
				dstDesc.MultiSampleType == D3DMULTISAMPLE_NONE;
			if (canUpdate) {
				POINT pt = { 0, 0 };
				RECT full = { 0, 0, (LONG)srcDesc.Width, (LONG)srcDesc.Height };
				d3dDevice->UpdateSurface(canvasSurf, &full, backBuffer, &pt);
			}
		}

		HRESULT hr = d3dDevice->Present(NULL, NULL, NULL, NULL);
		if (hr == D3DERR_DEVICELOST || hr == D3DERR_DEVICEHUNG || hr == D3DERR_DEVICEREMOVED) {
			gfx_lost = true;
		}
		break;
	}
	case GMODE_FIXED: {
		if (!graphics) break;
		gxCanvas* f = graphics->getFrontCanvas();
		if (!f) break;
		IDirect3DSurface9* canvasSurf = f->getSurface();
		if (!canvasSurf) break;

		if (canvasSurf != backBuffer) {
			D3DSURFACE_DESC srcDesc, dstDesc;
			bool canUpdate = SUCCEEDED(canvasSurf->GetDesc(&srcDesc)) &&
				SUCCEEDED(backBuffer->GetDesc(&dstDesc)) &&
				srcDesc.MultiSampleType == D3DMULTISAMPLE_NONE &&
				dstDesc.MultiSampleType == D3DMULTISAMPLE_NONE;
			if (canUpdate) {
				RECT src, dest;
				GetClientRect(hwnd, &dest);
				src.left = src.top = 0;
				src.right = dest.right - dest.left;
				src.bottom = dest.bottom - dest.top;

				POINT pt = { dest.left, dest.top };
				d3dDevice->UpdateSurface(canvasSurf, &src, backBuffer, &pt);
			}
		}

		HRESULT hr = d3dDevice->Present(NULL, NULL, NULL, NULL);
		if (hr == D3DERR_DEVICELOST || hr == D3DERR_DEVICEHUNG || hr == D3DERR_DEVICEREMOVED) {
			gfx_lost = true;
		}
		break;
	}
	case GMODE_EXCLUSIVE: {
		HRESULT hr = d3dDevice->Present(NULL, NULL, NULL, NULL);
		if (hr == D3DERR_DEVICELOST || hr == D3DERR_DEVICEHUNG || hr == D3DERR_DEVICEREMOVED) {
			gfx_lost = true;
		}
		break;
	}
	}
}


//////////
// FLIP //
//////////

void gxRuntime::flip(bool vwait) {
	pumpSDLWindowEvents();
	MSG msg;
	while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
		if (msg.message == WM_STOP) { if (!suspended) forceSuspend(); continue; }
		if (msg.message == WM_RUN) { if (suspended) forceResume(); continue; }
		if (msg.message == WM_END) { debugger = 0; run_flag = false; return; }
		if (msg.message == WM_MOUSEMOVE) {
			MSG peek;
			while (PeekMessage(&peek, msg.hwnd, WM_MOUSEMOVE, WM_MOUSEMOVE, PM_REMOVE)) {
				msg = peek;
			}
		}
		TranslateMessage(&msg);
		DispatchMessage(&msg);
		if (!run_flag) {
			return;
		}
	}

	if (!graphics) return;

	if (suspended) {
		MSG m;
		while (suspended && run_flag) {
			if (!GetMessageW(&m, 0, 0, 0)) break;
			switch (m.message) {
			case WM_STOP:
				if (!suspended) forceSuspend();
				break;
			case WM_RUN:
				if (suspended) forceResume();
				break;
			case WM_END:
				debugger = 0;
				run_flag = false;
				break;
			default:
				TranslateMessage(&m);
				DispatchMessageW(&m);
			}
		}
		if (!run_flag || !graphics) return;
	}

	if (d3dDevice) {
		gxGraphics::DeviceState state = graphics->getDeviceState();
		if (state == gxGraphics::DEVICE_LOST) {
			gfx_lost = true;
			return;
		}
		if (state == gxGraphics::DEVICE_NEEDS_RESET) {
			if (!graphics->restore()) {
				gfx_lost = true;
				return;
			}
			gfx_lost = false;
		}
	}

	if (sdlGpu && sdlWindow) {
		if (sceneBeganSinceFlip) {
			vwaitPending = true;
			vwaitValue = vwait;
		}
		else {
			sdlgpu::SetVSync(sdlGpu, sdlWindow, vwait);
		}
		sceneBeganSinceFlip = false;
		gxCanvas* back = graphics ? graphics->getBackCanvas() : nullptr;
		if (graphics && graphics->presentSceneWithCanvas(sdlGpu, sdlWindow, back)) return;
		unsigned argb = graphics ? graphics->getBackCanvas()->getClsColor() : 0;
		float r = ((argb >> 16) & 0xff) / 255.0f;
		float g = ((argb >> 8) & 0xff) / 255.0f;
		float b = (argb & 0xff) / 255.0f;
		back = graphics ? graphics->getBackCanvas() : nullptr;
		bool blitted = false;
		if (back && back->lockRO()) {
			int w = back->getWidth(), h = back->getHeight();
			static std::vector<unsigned> px;
			static int lastMod = -1;
			static int lastW = 0, lastH = 0;
			const void* up = nullptr;
			if (back->getModify() != lastMod || w != lastW || h != lastH) {
				px.resize((size_t)w * (size_t)h);
				for (int y = 0; y < h; ++y) {
					for (int x = 0; x < w; ++x) {
						unsigned p = back->getPixelFast(x, y);
						px[(size_t)y * (size_t)w + (size_t)x] =
							(p & 0xff00ff00) | ((p >> 16) & 0xff) | ((p << 16) & 0xff0000);
					}
				}
				up = px.data();
				lastMod = back->getModify();
				lastW = w;
				lastH = h;
			}
			back->unlock();
			blitted = sdlgpu::PresentBlit(sdlGpu, sdlWindow, r, g, b, (unsigned)w, (unsigned)h, up);
		}
		if (!blitted) sdlgpu::PresentSwapchain(sdlGpu, sdlWindow, r, g, b);
		return;
	}

	HRESULT hr = d3dDevice->Present(NULL, NULL, NULL, NULL);
	if (hr == D3DERR_DEVICELOST || hr == D3DERR_DEVICEHUNG || hr == D3DERR_DEVICEREMOVED) {
		gfx_lost = true;
		return;
	}
}

////////////////
// MOVE MOUSE //
////////////////
void gxRuntime::moveMouse(int x, int y) {
	POINT p;
	RECT rect;
	switch(gfx_mode) {
		case GMODE_SCALED:
			GetClientRect(hwnd, &rect);
			x = x * (rect.right - rect.left) / graphics->getWidth();
			y = y * (rect.bottom - rect.top) / graphics->getHeight();
		case GMODE_FIXED:
			p.x = x; p.y = y; ClientToScreen(hwnd, &p); x = p.x; y = p.y;
			break;
		case GMODE_EXCLUSIVE:
			if(use_di) return;
			break;
		default:
			return;
	}
	SetCursorPos(x, y);
}

/////////////////
// WINDOW PROC //
/////////////////
LRESULT gxRuntime::windowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {

	if(busy) {
		return DefWindowProc(hwnd, msg, wparam, lparam);
	}

	PAINTSTRUCT ps;

	//handle 'special' messages!
	switch(msg) {
		case WM_PAINT:
			BeginPaint(hwnd, &ps);
			paint();
			EndPaint(hwnd, &ps);
			return DefWindowProc(hwnd, msg, wparam, lparam);
		case WM_ERASEBKGND:
			return gfx_mode ? GMODE_SCALED : DefWindowProc(hwnd, msg, wparam, lparam);
		case WM_CLOSE:
			if(app_close.size()) {
				int n = MessageBox(hwnd, app_close.c_str(), app_title.c_str(), MB_OKCANCEL | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
				if(n != IDOK) return 0;
			}
			asyncEnd();
			return 0;
		case WM_SETCURSOR:
			if(!suspended) {
				if(gfx_mode == GMODE_EXCLUSIVE) {
					SetCursor(0);
					return 1;
				}
				else if(!pointer_visible) {
					POINT p;
					GetCursorPos(&p);
					ScreenToClient(hwnd, &p);
					RECT r; GetClientRect(hwnd, &r);
					if(p.x >= 0 && p.y >= 0 && p.x < r.right && p.y < r.bottom) {
						SetCursor(0);
						return 1;
					}
				}
			}
			break;
		case WM_ACTIVATEAPP:
			if(auto_suspend) {
				if(wparam) {
					if(suspended) resume();
				}
				else {
					if(!suspended) suspend();
				}
			}
			break;
	}

	if(!input || suspended) return DefWindowProc(hwnd, msg, wparam, lparam);

	if(gfx_mode == GMODE_EXCLUSIVE && use_di) {
		use_di = input->acquire();
		return DefWindowProc(hwnd, msg, wparam, lparam);
	}

	static const int MK_ALLBUTTONS = MK_LBUTTON | MK_RBUTTON | MK_MBUTTON;

	//handle input messages
	switch(msg) {
		case WM_LBUTTONDOWN:
			input->wm_mousedown(1);
			SetCapture(hwnd);
			break;
		case WM_LBUTTONUP:
			input->wm_mouseup(1);
			if(!(wparam & MK_ALLBUTTONS)) ReleaseCapture();
			break;
		case WM_RBUTTONDOWN:
			input->wm_mousedown(2);
			SetCapture(hwnd);
			break;
		case WM_RBUTTONUP:
			input->wm_mouseup(2);
			if(!(wparam & MK_ALLBUTTONS)) ReleaseCapture();
			break;
		case WM_MBUTTONDOWN:
			input->wm_mousedown(3);
			SetCapture(hwnd);
			break;
		case WM_XBUTTONDOWN:
			if (HIWORD(wparam) == XBUTTON1) input->wm_mousedown(5); // don't ask me why
			else if (HIWORD(wparam) == XBUTTON2) input->wm_mousedown(4);
			SetCapture(hwnd);
			break;
		case WM_XBUTTONUP:
			if (HIWORD(wparam) == XBUTTON1) input->wm_mouseup(5);
			else if (HIWORD(wparam) == XBUTTON2) input->wm_mouseup(4);
			if (!(wparam & MK_ALLBUTTONS)) ReleaseCapture();
			break;
		case WM_MBUTTONUP:
			input->wm_mouseup(3);
			if(!(wparam & MK_ALLBUTTONS)) ReleaseCapture();
			break;
		case WM_MOUSEMOVE:
			if(!graphics) break;
			if(gfx_mode == GMODE_EXCLUSIVE && !use_di) {
				POINT p; GetCursorPos(&p);
				input->wm_mousemove(p.x, p.y);
			}
			else {
				int x = (short)(lparam & 0xffff), y = lparam >> 16;
				if(gfx_mode == GMODE_SCALED) {
					RECT rect; GetClientRect(hwnd, &rect);
					x = x * graphics->getWidth() / (rect.right - rect.left);
					y = y * graphics->getHeight() / (rect.bottom - rect.top);
				}
				if(x < 0) x = 0;
				else if(x >= graphics->getWidth()) x = graphics->getWidth() - 1;
				if(y < 0) y = 0;
				else if(y >= graphics->getHeight()) y = graphics->getHeight() - 1;
				input->wm_mousemove(x, y);
			}
			break;
		case WM_MOUSEWHEEL:
			input->wm_mousewheel((short)HIWORD(wparam));
			break;
		case WM_KEYDOWN:case WM_SYSKEYDOWN:
			if(lparam & 0x40000000) break;
			if(int n = ((lparam >> 17) & 0x80) | ((lparam >> 16) & 0x7f)) input->wm_keydown(n);
			break;
		case WM_KEYUP:case WM_SYSKEYUP:
			if(int n = ((lparam >> 17) & 0x80) | ((lparam >> 16) & 0x7f)) input->wm_keyup(n);
			break;
		case WM_CHAR:
			input->wm_char(wparam, lparam);
			break;
		default:
			return DefWindowProc(hwnd, msg, wparam, lparam);
	}

	return 0;
}

static LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
	if(runtime) return runtime->windowProc(hwnd, msg, wparam, lparam);
	return DefWindowProc(hwnd, msg, wparam, lparam);
}

//////////////////////////////
//STOP FROM EXTERNAL SOURCE //
//////////////////////////////
void gxRuntime::asyncStop() {
	PostMessage(hwnd, WM_STOP, 0, 0);
}

//////////////////////////////
//RUN  FROM EXTERNAL SOURCE //
//////////////////////////////
void gxRuntime::asyncRun() {
	PostMessage(hwnd, WM_RUN, 0, 0);
}

//////////////////////////////
// END FROM EXTERNAL SOURCE //
//////////////////////////////
void gxRuntime::asyncEnd() {
	PostMessage(hwnd, WM_END, 0, 0);
}

//////////
// IDLE //
//////////
void gxRuntime::maybePresentConsole() {
	if (!run_flag || busy || suspended || !sdlGpu || !sdlWindow || !graphics) return;
	gxCanvas* f = graphics->getFrontCanvas();
	if (!f) return;
	if (f != console_canvas) { console_canvas = f; console_mod = f->getModify(); return; }
	if (f->getModify() == console_mod) return;
	if (sceneBeganSinceFlip) return;
	console_mod = f->getModify();
	graphics->presentSceneWithCanvas(sdlGpu, sdlWindow, f);
}

bool gxRuntime::idle() {
	for(;;) {
		pumpSDLWindowEvents();
		MSG msg;
		BOOL success = 0;
		if(suspended && run_flag) {
			success = GetMessageW(&msg, 0, 0, 0);
		}
		else {
			if(!PeekMessageW(&msg, 0, 0, 0, PM_REMOVE)) {
				maybePresentConsole();
				return run_flag;
			}
		}

		if (msg.message == WM_MOUSEMOVE) {
			MSG peek;
			while (PeekMessageW(&peek, msg.hwnd, WM_MOUSEMOVE, WM_MOUSEMOVE, PM_REMOVE)) {
				msg = peek;
			}
		}

		switch(msg.message) {
			case WM_STOP:
				if(!suspended) forceSuspend();
				break;
			case WM_RUN:
				if(suspended) forceResume();
				break;
			case WM_END:
				debugger = 0;
				run_flag = false;
				break;
			default:
				TranslateMessage(&msg);
				DispatchMessageW(&msg);
		}
	}
	return run_flag;
}

///////////
// DELAY //
///////////
bool gxRuntime::delay(int ms) {
	int t = timeGetTime() + ms;
	for(;;) {
		if(!idle()) return false;
		int d = t - timeGetTime();	//how long left to wait
		if(d <= 0) return true;
		if(d > 100) d = 100;
		Sleep(d);
	}
}

///////////////
// DEBUGSTMT //
///////////////
bool gxRuntime::debugStmt(int pos, const char* file) {
	return debugger ? debugger->debugStmt(pos, file) : true;
}

///////////////
// DEBUGSTOP //
///////////////
void gxRuntime::debugStop() {
	if(!suspended) forceSuspend();
}

////////////////
// DEBUGENTER //
////////////////
void gxRuntime::debugEnter(void* frame, void* env, const char* func) {
	if(debugger) debugger->debugEnter(frame, env, func);
}

////////////////
// DEBUGLEAVE //
////////////////
void gxRuntime::debugLeave() {
	if(debugger) debugger->debugLeave();
}

////////////////
// DEBUGERROR //
////////////////
void gxRuntime::debugError(const char* t) {
	if(!debugger) return;
	Debugger* d = debugger;
	asyncEnd();
	if(!suspended) {
		forceSuspend();
	}
	d->debugMsg(UTF8::convertToUtf8(t).c_str(), true);
}

///////////////
// DEBUGINFO //
///////////////
void gxRuntime::debugInfo(const char* t) {
	if(!debugger) return;
	Debugger* d = debugger;
	asyncEnd();
	if(!suspended) {
		forceSuspend();
	}
	d->debugMsg(UTF8::convertToUtf8(t).c_str(), false);
}

//////////////
// DEBUGLOG //
//////////////
void gxRuntime::debugLog(const char* t) {
	if(debugger) debugger->debugLog(t);
}

//debugsys
void gxRuntime::debugSys(void* msg) {
	if (debugger) debugger->debugSys(msg);
}

/////////////////////////
// RETURN COMMAND LINE //
/////////////////////////
std::string gxRuntime::commandLine() {
	return cmd_line;
}

/////////////
// EXECUTE //
/////////////
bool gxRuntime::execute(const std::string& cmd_line) {

	if(!cmd_line.size()) return false;

	//convert cmd_line to cmd and params
	std::string cmd = cmd_line, params;
	while(cmd.size() && cmd[0] == ' ') cmd = cmd.substr(1);
	if(cmd.find('\"') == 0) {
		int n = cmd.find('\"', 1);
		if(n != std::string::npos) {
			params = cmd.substr(n + 1);
			cmd = cmd.substr(1, n - 1);
		}
	}
	else {
		int n = cmd.find(' ');
		if(n != std::string::npos) {
			params = cmd.substr(n + 1);
			cmd = cmd.substr(0, n);
		}
	}
	while(params.size() && params[0] == ' ') params = params.substr(1);
	while(params.size() && params[params.size() - 1] == ' ') params = params.substr(0, params.size() - 1);

	SetForegroundWindow(GetDesktopWindow());
	return (int)ShellExecute(GetDesktopWindow(), 0, cmd.c_str(), params.size() ? params.c_str() : 0, 0, SW_SHOW) > 32;
}

///////////////
// APP TITLE //
///////////////
void gxRuntime::setTitle(const std::string& t, const std::string& e) {
	app_title = t;
	app_close = e;
	SetWindowTextW(hwnd, UTF8::convertToUtf16(app_title).c_str());
	if (usingSDLWindow()) sdlgpu::SetWindowTitle(sdlWindow, app_title.c_str());
}

void gxRuntime::pumpSDLWindowEvents() {
	if (!sdlWindow) return;
	sdlgpu::PumpEvents(sdlWindow, this);
}

void gxRuntime::destroySDLWindow() {
	sdlgpu::TeardownPipelines();
	if (sdlGpu) sdlgpu::TeardownTexturePools(sdlGpu);
	if (sdlGpu && sdlWindow) sdlgpu::ReleaseWindow(sdlGpu, sdlWindow);
	if (sdlGpu) {
		sdlgpu::DestroyGPUDevice(sdlGpu);
		sdlGpu = nullptr;
	}
	if (!sdlWindow) return;
	SDL_Window* win = sdlWindow;
	sdlWindow = nullptr;
	sdlgpu::DestroyGameWindow(win);
	if (savedHwnd) {
		hwnd = savedHwnd;
		savedHwnd = nullptr;
		ShowWindow(hwnd, SW_SHOW);
	}
}

void gxRuntime::setFullscreenState(bool fullscreen) {
	if (!usingSDLWindow()) return;
	if (fullscreen) {
		gfx_mode = GMODE_EXCLUSIVE;
		auto_suspend = true;
		sdlgpu::SetCursorVisible(false);
	}
	else {
		gfx_mode = GMODE_FIXED;
		auto_suspend = false;
		sdlgpu::SetCursorVisible(pointer_visible);
	}
}

//////////////////
// GETMILLISECS //
//////////////////
int gxRuntime::getMilliSecs() {
	return timeGetTime() & 0x7FFFFFFF;
}

////////////////
// MEMORYINFO //
////////////////
int gxRuntime::getMemoryLoad() {
	GlobalMemoryStatusEx(&statex);
	return statex.dwMemoryLoad;
}

int gxRuntime::getTotalPhys() {
	return statex.ullTotalPhys / 1024;
}

int gxRuntime::getAvailPhys() {
	return statex.ullAvailPhys / 1024;
}

int gxRuntime::getTotalVirtual() {
	return statex.ullTotalVirtual / 1024;
}

int gxRuntime::getAvailVirtual() {
	return statex.ullAvailVirtual / 1024;
}

/////////////////////
// POINTER VISIBLE //
/////////////////////
void gxRuntime::setPointerVisible(bool vis) {
	if(pointer_visible == vis) return;

	pointer_visible = vis;
	if(gfx_mode == GMODE_EXCLUSIVE && !usingSDLWindow()) return;

	if (usingSDLWindow()) {
		sdlgpu::SetCursorVisible(vis);
		return;
	}

	//force a WM_SETCURSOR
	POINT pt;
	GetCursorPos(&pt);
	SetCursorPos(pt.x, pt.y);
}

/////////////////
// AUDIO SETUP //
/////////////////
gxAudio* gxRuntime::openAudio(int flags) {
	if(audio) return 0;

	static const int mixrates[] = { 96000, 48000, 44100 };
	bool ok = false;
	for (int i = 0; i < sizeof(mixrates) / sizeof(mixrates[0]); ++i) {
		if (BASS_Init(-1, mixrates[i], 0, hwnd, 0)) {
			ok = true;
			break;
		}
	}
	if (!ok) {
		return 0;
	}
	BASS_Start();

	audio = new gxAudio(this);
	return audio;
}

void gxRuntime::closeAudio(gxAudio* a) {
	if(!audio || audio != a) return;
	delete audio;
	audio = 0;
}

/////////////////
// INPUT SETUP //
/////////////////
gxInput* gxRuntime::openInput(int flags) {
	if(input) return 0;

	IDirectInput8* di;
	if(DirectInput8Create(hinst, DIRECTINPUT_VERSION, IID_IDirectInput8, (void**)&di, 0) >= 0) {
		input = new gxInput(this, di);
		acquireInput();
	}
	else {
		runtime->debugLog("Failed to create DirectInput.");
	}
	return input;
}

void gxRuntime::closeInput(gxInput* i) {
	if(!input || input != i) return;
	unacquireInput();
	delete input;
	input = 0;
}

/////////////////////////////////////////////////////
// TIMER CALLBACK FOR AUTOREFRESH OF WINDOWED MODE //
/////////////////////////////////////////////////////
static void CALLBACK timerCallback(UINT, UINT, DWORD, DWORD, DWORD) {
	bool post = false;
	HWND target = NULL;
	EnterCriticalSection(&g_gfxCS);
	if (gfx_mode && runtime && runtime->graphics) {
		gxCanvas* f = runtime->graphics->getFrontCanvas();
		if (f && f->getModify() != mod_cnt) {
			mod_cnt = f->getModify();
			post = true;
			target = runtime->hwnd;
		}
	}
	LeaveCriticalSection(&g_gfxCS);
	if (post && target) PostMessage(target, WM_PAINT, 0, 0);
}

////////////////////
// GRAPHICS SETUP //
////////////////////
void gxRuntime::backupWindowState() {
	GetWindowRect(hwnd, &t_rect);
	t_style = GetWindowLong(hwnd, GWL_STYLE);
}

void gxRuntime::restoreWindowState() {
	SetWindowLong(hwnd, GWL_STYLE, t_style);
	SetWindowPos(
		hwnd, 0, t_rect.left, t_rect.top,
		t_rect.right - t_rect.left, t_rect.bottom - t_rect.top,
		SWP_NOZORDER | SWP_FRAMECHANGED);
}

bool gxRuntime::setDisplayMode(int w, int h, int d, bool d3d) {
	D3DFORMAT format = D3DFMT_UNKNOWN;
	if (d == 32) format = D3DFMT_X8R8G8B8;
	else if (d == 24) format = D3DFMT_R8G8B8;
	else if (d == 16) format = D3DFMT_R5G6B5;
	else return false;

	HRESULT hr = this->d3d->CheckDeviceType(curr_driver->adapter, D3DDEVTYPE_HAL, format, format, FALSE);
	if (FAILED(hr)) return false;

	d3dpp.BackBufferWidth = w;
	d3dpp.BackBufferHeight = h;
	d3dpp.BackBufferFormat = format;
	return true;
}

void gxRuntime::applyAntialiasToParams(D3DPRESENT_PARAMETERS& pp) {
	pp.MultiSampleType = D3DMULTISAMPLE_NONE;
	pp.MultiSampleQuality = 0;

	if (!requested_antialias) return;

	pp.SwapEffect = D3DSWAPEFFECT_DISCARD;

	D3DFORMAT fmt = pp.BackBufferFormat;
	if (fmt == D3DFMT_UNKNOWN) fmt = D3DFMT_X8R8G8B8;

	BOOL windowed = pp.Windowed ? TRUE : FALSE;

	D3DMULTISAMPLE_TYPE type = D3DMULTISAMPLE_4_SAMPLES;
	DWORD quality = 0;
	HRESULT hr = d3d->CheckDeviceMultiSampleType(curr_driver->adapter, D3DDEVTYPE_HAL, fmt, windowed, type, &quality);
	if (FAILED(hr) || quality == 0) {
		type = D3DMULTISAMPLE_2_SAMPLES;
		quality = 0;
		hr = d3d->CheckDeviceMultiSampleType(curr_driver->adapter, D3DDEVTYPE_HAL, fmt, windowed, type, &quality);
		if (FAILED(hr) || quality == 0) return;
	}

	pp.MultiSampleType = type;
	pp.MultiSampleQuality = quality > 0 ? quality - 1 : 0;
	pp.Flags &= ~D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
}

gxGraphics* gxRuntime::openWindowedGraphics(int w, int h, int d, bool d3d) {
	if (!d3d) return 0;

	if (sdlGpu) {
		if (!(timerID = timeSetEvent(100, 10, timerCallback, 0, TIME_PERIODIC))) {
			DebugMsg("timeSetEvent failed!");
			timerID = 0;
		}
		return new gxGraphics(this, nullptr, nullptr, nullptr, d3d, w, h);
	}

	ZeroMemory(&d3dpp, sizeof(d3dpp));
	d3dpp.Windowed = TRUE;
	d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
	d3dpp.Flags = D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
	d3dpp.EnableAutoDepthStencil = FALSE;
	d3dpp.BackBufferCount = 1;
	d3dpp.BackBufferWidth = w;
	d3dpp.BackBufferHeight = h;
	d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

	D3DDISPLAYMODE mode;
	if (FAILED(this->d3d->GetAdapterDisplayMode(curr_driver->adapter, &mode))) return 0;

	d3dpp.BackBufferFormat = (mode.Format == D3DFMT_R8G8B8 || mode.Format == D3DFMT_A8R8G8B8 || mode.Format == D3DFMT_X8R8G8B8) ? mode.Format : D3DFMT_X8R8G8B8;

	applyAntialiasToParams(d3dpp);

	DWORD vp_flag = pickVertexProcessingFlag(this->d3d, curr_driver->adapter);
	if (FAILED(this->d3d->CreateDeviceEx(curr_driver->adapter, D3DDEVTYPE_HAL, hwnd, vp_flag, &d3dpp, nullptr, &d3dDevice))) {
		if (d3dpp.MultiSampleType != D3DMULTISAMPLE_NONE) {
			d3dpp.MultiSampleType = D3DMULTISAMPLE_NONE;
			d3dpp.MultiSampleQuality = 0;
			d3dpp.Flags |= D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
			if (FAILED(this->d3d->CreateDeviceEx(curr_driver->adapter, D3DDEVTYPE_HAL, hwnd, vp_flag, &d3dpp, nullptr, &d3dDevice))) return 0;
		}
		else {
			return 0;
		}
	}

	if (FAILED(d3dDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer))) {
		d3dDevice->Release(); d3dDevice = 0;
		return 0;
	}

	frontBuffer = backBuffer;
	frontBuffer->AddRef();

	// Do we need this timer stuff anymore?
	if (!(timerID = timeSetEvent(100, 10, timerCallback, 0, TIME_PERIODIC))) {
		DebugMsg("timeSetEvent failed!");
		timerID = 0;
	}

	return new gxGraphics(this, d3dDevice, frontBuffer, backBuffer, d3d, w, h);
}

gxGraphics* gxRuntime::openExclusiveGraphics(int w, int h, int d, bool d3d) {
	if (!d3d) return 0;

	D3DFORMAT format;
	if (d == 0) {
		D3DDISPLAYMODE mode;
		if (FAILED(this->d3d->GetAdapterDisplayMode(curr_driver->adapter, &mode))) return 0;
		format = mode.Format;
	}
	else if (d == 32) format = D3DFMT_X8R8G8B8;
	else if (d == 24) format = D3DFMT_R8G8B8;
	else if (d == 16) format = D3DFMT_R5G6B5;
	else return 0;

	if (FAILED(this->d3d->CheckDeviceType(curr_driver->adapter, D3DDEVTYPE_HAL, format, format, FALSE))) {
		DebugMsg("openExclusiveGraphics: CheckDeviceType failed for requested format");
		return 0;
	}

	ZeroMemory(&d3dpp, sizeof(d3dpp));
	d3dpp.Windowed = FALSE;
	d3dpp.BackBufferWidth = w;
	d3dpp.BackBufferHeight = h;
	d3dpp.BackBufferFormat = format;
	d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
	d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
	d3dpp.BackBufferCount = 1;
	d3dpp.EnableAutoDepthStencil = FALSE;
	d3dpp.Flags = D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;

	memset(&d3ddmEx, 0, sizeof(d3ddmEx));
	d3ddmEx.Size = sizeof(D3DDISPLAYMODEEX);
	d3ddmEx.Width = w;
	d3ddmEx.Height = h;
	d3ddmEx.Format = format;
	d3ddmEx.RefreshRate = 0;
	d3ddmEx.ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;

	applyAntialiasToParams(d3dpp);

	DWORD vp_flag = pickVertexProcessingFlag(this->d3d, curr_driver->adapter);
	if (FAILED(this->d3d->CreateDeviceEx(curr_driver->adapter, D3DDEVTYPE_HAL, hwnd, vp_flag, &d3dpp, &d3ddmEx, &d3dDevice))) {
		if (d3dpp.MultiSampleType != D3DMULTISAMPLE_NONE) {
			d3dpp.MultiSampleType = D3DMULTISAMPLE_NONE;
			d3dpp.MultiSampleQuality = 0;
			d3dpp.Flags |= D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
			if (FAILED(this->d3d->CreateDeviceEx(curr_driver->adapter, D3DDEVTYPE_HAL, hwnd, vp_flag, &d3dpp, &d3ddmEx, &d3dDevice))) {
				return 0;
			}
		}
		else {
			return 0;
		}
	}

	if (FAILED(d3dDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer))) {
		d3dDevice->Release();
		d3dDevice = 0;
		return 0;
	}

	// front is same as back in exclusive
	frontBuffer = backBuffer;
	frontBuffer->AddRef();

	return new gxGraphics(this, d3dDevice, frontBuffer, backBuffer, d3d, w, h);
}

gxGraphics* gxRuntime::openGraphics(int w, int h, int d, int driver, int flags) {
	std::stringstream ss;
	ss << "openGraphics called: " << w << "x" << h << " d=" << d
		<< " driver=" << driver << " flags=0x" << std::hex << flags;
	DebugMsg(ss.str());

	if (graphics) {
		DebugMsg("ERROR: graphics already open!");
		return 0;
	}
	busy = true;

	bool d3d = (flags & gxGraphics::GRAPHICS_3D) != 0;
	bool windowed = (flags & gxGraphics::GRAPHICS_WINDOWED) != 0;

	ss.str("");
	ss << "d3d flag=" << d3d << " windowed=" << windowed;
	DebugMsg(ss.str());

	if (!d3d) {
		DebugMsg("ERROR: GRAPHICS_3D flag not set!");
		busy = false;
		return 0;
	}

	if (!this->d3d) {
		DebugMsg("ERROR: Direct3D9 object is null! Direct3DCreate9 failed in constructor.");
		busy = false;
		return 0;
	}

	curr_driver = drivers[driver];

	bool fullscreen = !windowed;

	bool sdlReady = false;
	if (!sdlWindow) {
		bool resizable = (flags & gxGraphics::GRAPHICS_SCALED) != 0;
		bool borderless = (flags & gxGraphics::GRAPHICS_BORDERLESS) != 0;
		std::string title = app_title.size() ? app_title : " ";
		SDL_Window* win = sdlgpu::CreateGameWindow(w, h, resizable, borderless, fullscreen, title.c_str());
		if (win) {
			HWND sdlHwnd = (HWND)sdlgpu::GetHWND(win);
			if (sdlHwnd) {
				savedHwnd = hwnd;
				ShowWindow(savedHwnd, SW_HIDE);
				sdlWindow = win;
				hwnd = sdlHwnd;
				sdlGpu = sdlgpu::CreateGPUDevice();
				if (!sdlGpu) DebugMsg("SDL GPU device create failed");
				else if (!sdlgpu::ClaimWindow(sdlGpu, win)) {
					DebugMsg("SDL GPU claim window failed");
					sdlgpu::DestroyGPUDevice(sdlGpu);
					sdlGpu = nullptr;
				}
				if (sdlGpu) {
					if (!fullscreen) {
						sdlgpu::SizeWindowForClient(win, w, h);
						sdlgpu::CenterWindow(win);
					}
					sdlgpu::PresentSwapchain(sdlGpu, win, 0.0f, 0.0f, 0.0f);
					sdlgpu::ShowGameWindow(win);
					sdlgpu::SetCursorVisible(fullscreen ? false : pointer_visible);
					sdlReady = true;
				}
			}
			if (!sdlReady) sdlgpu::DestroyGameWindow(win);
		}
		if (!sdlReady) DebugMsg("SDL window create failed");
	}
	else {
		sdlReady = (sdlGpu != nullptr);
	}

	if (sdlReady) {
		DebugMsg("Attempting openWindowedGraphics (SDL)...");
		graphics = openWindowedGraphics(w, h, d, d3d);
		if (!graphics) {
			DebugMsg("openWindowedGraphics failed on SDL");
			destroySDLWindow();
			sdlReady = false;
		}
	}

	if (!graphics) {
		if (windowed) {
			DebugMsg("Attempting openWindowedGraphics...");
			graphics = openWindowedGraphics(w, h, d, d3d);
		}
		else {
			DebugMsg("Attempting openExclusiveGraphics...");
			backupWindowState();
			SetWindowLong(hwnd, GWL_STYLE, WS_VISIBLE | WS_POPUP);
			SetWindowPos(hwnd, 0, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
			ShowCursor(0);
			graphics = openExclusiveGraphics(w, h, d, d3d);
			if (!graphics) {
				DebugMsg("openExclusiveGraphics FAILED");
				ShowCursor(1);
				restoreWindowState();
			}
		}
	}

	if (graphics) {
		if (!windowed) {
			DebugMsg(usingSDLWindow() ? "SDL fullscreen graphics SUCCESS" : "openExclusiveGraphics SUCCESS");
			gfx_mode = GMODE_EXCLUSIVE;
			auto_suspend = true;
			if (!usingSDLWindow()) SetCursorPos(0, 0);
			acquireInput();
		}
		else {
			DebugMsg("openWindowedGraphics SUCCESS");
			gfx_mode = (flags & gxGraphics::GRAPHICS_SCALED) ? GMODE_SCALED : GMODE_FIXED;
			auto_suspend = (flags & gxGraphics::GRAPHICS_AUTOSUSPEND) != 0;
			border_mode = (flags & gxGraphics::GRAPHICS_BORDERLESS) ? 1 : 0;

			if (usingSDLWindow()) {
				sdlgpu::SizeWindowForClient(sdlWindow, w, h);
				if (border_mode == 1 && gfx_mode == GMODE_SCALED) {
					sdlgpu::CenterWindow(sdlWindow);
				}
				if (app_title.size()) sdlgpu::SetWindowTitle(sdlWindow, app_title.c_str());
			}
			else {
				if (border_mode == 0)
					SetWindowLong(hwnd, GWL_STYLE, (gfx_mode == GMODE_SCALED) ? scaled_ws : static_ws);
				else
					SetWindowLong(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
				SetWindowPos(hwnd, 0, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

				if (border_mode == 1 && gfx_mode == GMODE_SCALED) {
					MoveWindow(hwnd, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), true);
				}
				else {
					RECT w_r, c_r;
					GetWindowRect(hwnd, &w_r);
					GetClientRect(hwnd, &c_r);
					int tw = (w_r.right - w_r.left) - (c_r.right - c_r.left);
					int th = (w_r.bottom - w_r.top) - (c_r.bottom - c_r.top);
					int cx = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
					int cy = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
					MoveWindow(hwnd, cx, cy, w + tw, h + th, true);
				}
			}
		}
	}

	gfx_lost = false;
	busy = false;

	if (!graphics) {
		DebugMsg("openGraphics FINAL: Returning NULL (graphics creation failed)");
	}
	else {
		DebugMsg("openGraphics FINAL: Success");
	}

	return graphics;
}

void gxRuntime::closeGraphics(gxGraphics* g) {
	if (!graphics || graphics != g) return;
	bool wasSDL = usingSDLWindow();
	auto_suspend = false;
	busy = true;

	unacquireInput();
	if (timerID) { timeKillEvent(timerID); timerID = 0; }
	if (d3dDevice) {
		if (frontBuffer && frontBuffer != backBuffer) frontBuffer->Release();
		if (backBuffer) backBuffer->Release();
		if (stretchRT) { stretchRT->Release(); stretchRT = 0; stretchRT_w = stretchRT_h = 0; }
		d3dDevice->Release();
		d3dDevice = 0;
		backBuffer = 0;
		frontBuffer = 0;
	}
	destroySDLWindow();
	EnterCriticalSection(&g_gfxCS);
	gxGraphics* old_graphics = graphics;
	graphics = 0;
	LeaveCriticalSection(&g_gfxCS);

	old_graphics->dir3dDev = nullptr;
	old_graphics->frontBuffer = nullptr;
	old_graphics->backBuffer = nullptr;
	delete old_graphics;

	if (gfx_mode == GMODE_EXCLUSIVE && !wasSDL) {
		ShowCursor(1);
		restoreWindowState();
	}
	gfx_mode = GMODE_NONE;
	gfx_lost = false;
	busy = false;
}

bool gxRuntime::graphicsLost() {
	if (!graphics || !d3dDevice) return false;

	gxGraphics::DeviceState state = graphics->getDeviceState();
	if (state == gxGraphics::DEVICE_OK) {
		gfx_lost = false;
		return false;
	}

	bool ok = graphics->restore();
	gfx_lost = !ok;
	return !ok;
}

bool gxRuntime::focus() {
	//return suspended;
	return GetFocus();
}

int gxRuntime::desktopWidth() {
	//	return GetSystemMetrics(SM_CXSCREEN);
	return devmode.dmPelsWidth;
}

int gxRuntime::desktopHeight() {
	//	return GetSystemMetrics(SM_CYSCREEN);
	return devmode.dmPelsHeight;
}

gxFileSystem* gxRuntime::openFileSystem(int flags) {
	if(fileSystem) return 0;

	fileSystem = new gxFileSystem();
	return fileSystem;
}

void gxRuntime::closeFileSystem(gxFileSystem* f) {
	if(!fileSystem || fileSystem != f) return;

	delete fileSystem;
	fileSystem = 0;
}

////////////////////
// GFX ENUM STUFF //
////////////////////
void gxRuntime::enumGfx() {
	denumGfx();
	if (!d3d) return;
	static const D3DFORMAT kFormats[] = { D3DFMT_X8R8G8B8, D3DFMT_R5G6B5, D3DFMT_A8R8G8B8 };
	UINT adapterCount = d3d->GetAdapterCount();
	for (UINT i = 0; i < adapterCount; ++i) {
		D3DADAPTER_IDENTIFIER9 id;
		if (SUCCEEDED(d3d->GetAdapterIdentifier(i, 0, &id))) {
			GfxDriver* d = new GfxDriver;
			d->adapter = i;
			d->identifier = id;
			for (D3DFORMAT fmt : kFormats) {
				UINT modeCount = d3d->GetAdapterModeCount(i, fmt);
				for (UINT j = 0; j < modeCount; ++j) {
					D3DDISPLAYMODE mode;
					if (SUCCEEDED(d3d->EnumAdapterModes(i, fmt, j, &mode))) {
						GfxMode* m = new GfxMode;
						m->mode = mode;
						d->modes.push_back(m);
					}
				}
			}
			drivers.push_back(d);
		}
	}
}

void gxRuntime::denumGfx() {
	for (auto d : drivers) {
		for (auto m : d->modes) delete m;
		delete d;
	}
	drivers.clear();
}

int gxRuntime::numGraphicsDrivers() {
	if(!enum_all) {
		enum_all = true;
		enumGfx();
	}
	return drivers.size();
}

void gxRuntime::graphicsDriverInfo(int driver, std::string* name, int* caps) {
	GfxDriver* d = drivers[driver];
	*name = d->identifier.Description;
	*caps = 0;
	if (d->identifier.DeviceId) *caps |= GFXMODECAPS_3D;
}

int gxRuntime::numGraphicsModes(int driver) {
	return drivers[driver]->modes.size();
}

void gxRuntime::graphicsModeInfo(int driver, int mode, int* w, int* h, int* d, int* caps) {
	GfxDriver* drv = drivers[driver];
	GfxMode* m = drv->modes[mode];
	*w = m->mode.Width;
	*h = m->mode.Height;
	switch (m->mode.Format) {
	case D3DFMT_X8R8G8B8: *d = 32; break;
	case D3DFMT_R8G8B8:   *d = 24; break;
	case D3DFMT_R5G6B5:   *d = 16; break;
	default:              *d = 0; break;
	}
	*caps = GFXMODECAPS_3D;
}

void gxRuntime::windowedModeInfo(int* caps) {
	*caps = GFXMODECAPS_3D;
}

gxTimer* gxRuntime::createTimer(int hertz) {
	gxTimer* t = new gxTimer(this, hertz);
	timers.insert(t);
	return t;
}

void gxRuntime::freeTimer(gxTimer* t) {
	if(!timers.count(t)) return;
	timers.erase(t);
	delete t;
}

static std::string toDir(std::string t) {
	if(t.size() && t[t.size() - 1] != '\\') t += '\\';
	return t;
}

std::string gxRuntime::systemProperty(const std::string& p) {
	char buff[MAX_PATH + 1];
	std::string t = tolower(p);
	if(t == "os") {
		switch(osinfo.dwMajorVersion) {
			case 6:
				switch(osinfo.dwMinorVersion) {
					case 0:return "Windows Vista";
					case 1:return "Windows 7";
					case 2:return "Windows 8";
					case 3:return "Windows 8.1";
				}
				break;
			case 10:
				if(osinfo.dwBuildNumber >= 22000) return "Windows 11";
				return "Windows 10";
				break;
		}
	}
	else if(t == "cpuname") {
		//Uses the __cpuid intrinsic to get the brand name.
		//-------RESOURCES-------
		//https://en.wikipedia.org/wiki/CPUID#EAX=80000002h,80000003h,80000004h:_Processor_Brand_String
		//https://docs.microsoft.com/en-us/cpp/intrinsics/cpuid-cpuidex?view=msvc-160

		std::string cpuBrand;
		uint32_t regs[4];
		int numberOfExtendedFlags;

		__cpuid((int*)regs, 0x80000000);
		numberOfExtendedFlags = regs[0];

		if(numberOfExtendedFlags >= 0x80000004) {
			__cpuid((int*)regs, 0x80000002);
			cpuBrand += std::string((const char*)&regs[0], 4);
			cpuBrand += std::string((const char*)&regs[1], 4);
			cpuBrand += std::string((const char*)&regs[2], 4);
			cpuBrand += std::string((const char*)&regs[3], 4);

			__cpuid((int*)regs, 0x80000003);
			cpuBrand += std::string((const char*)&regs[0], 4);
			cpuBrand += std::string((const char*)&regs[1], 4);
			cpuBrand += std::string((const char*)&regs[2], 4);
			cpuBrand += std::string((const char*)&regs[3], 4);

			__cpuid((int*)regs, 0x80000004);
			cpuBrand += std::string((const char*)&regs[0], 4);
			cpuBrand += std::string((const char*)&regs[1], 4);
			cpuBrand += std::string((const char*)&regs[2], 4);
			cpuBrand += std::string((const char*)&regs[3], 4);
		}
		else cpuBrand = getenv("PROCESSOR_IDENTIFIER"); //Should never happen, modern CPUs implement the brand name.

		return cpuBrand;
	}
	else if(t == "cpuarch") {
		SYSTEM_INFO systemInfo;
		GetNativeSystemInfo(&systemInfo);

		switch(systemInfo.wProcessorArchitecture) {
			case PROCESSOR_ARCHITECTURE_AMD64:
				return "AMD64";
			case PROCESSOR_ARCHITECTURE_INTEL:
				return "x86";
				//Maybe someone runs the game under the x86 emulation layers of ARM Windows, detect it.
			case PROCESSOR_ARCHITECTURE_ARM:
				return "ARM32";
			case PROCESSOR_ARCHITECTURE_ARM64:
				return "ARM64";
				//-------------------------------------------------------------------------------------
			default:
				return "Unknown";
		}
	}
	else if(t == "osbuild") {
		return itoa((int)osinfo.dwBuildNumber);
	}
	else if(t == "appdir") {
		if(GetModuleFileName(0, buff, MAX_PATH)) {
			std::string t = buff;
			int n = t.find_last_of('\\');
			if(n != std::string::npos) t = t.substr(0, n);
			return toDir(t);
		}
	}
	else if(t == "appfile") {
		if(GetModuleFileName(0, buff, MAX_PATH)) return buff;
	}
	else if(t == "apphwnd") {
		return itoa((int)hwnd);
	}
	else if(t == "apphinstance") {
		return itoa((int)hinst);
	}
	else if(t == "windowsdir") {
		if(GetWindowsDirectory(buff, MAX_PATH)) return toDir(buff);
	}
	else if(t == "systemdir") {
		if(GetSystemDirectory(buff, MAX_PATH)) return toDir(buff);
	}
	else if(t == "tempdir") {
		if(GetTempPath(MAX_PATH, buff)) return toDir(buff);
	}
	else if(t == "direct3d7" || t == "direct3d8" || t == "direct3d9" || t == "direct3d") {
		if(graphics) return itoa((int)graphics->dir3d);
	}
	else if(t == "direct3ddevice7" || t == "direct3ddevice8" || t == "direct3ddevice9" || t == "direct3ddevice") {
		if(graphics) return itoa((int)graphics->dir3dDev);
	}
	else if(t == "directdraw7" || t == "directdraw" || t == "directdraw8") {
		if(graphics) return itoa((int)graphics->dir3d);
	}
	else if(t == "directinput7") {
		if(input) return itoa((int)input->dirInput);
	}
	else if(t == "blitzversion") {
		return itoa((VERSION & 0xffff) / 1000) + "." + itoa((VERSION & 0xffff) % 1000);
	}
	return "";
}

void gxRuntime::calculateDPI() {
	if ((this->scale_x == .0f) && (this->scale_y == .0f)) {
		HDC hdc = GetDC(GetDesktopWindow());
		this->scale_x = GetDeviceCaps(hdc, LOGPIXELSX) / 96.0f;
		this->scale_y = GetDeviceCaps(hdc, LOGPIXELSY) / 96.0f;
		ReleaseDC(GetDesktopWindow(), hdc);
	}
}

void gxRuntime::enableDirectInput(bool enable) {
	if(use_di = enable) {
		acquireInput();
	}
	else {
		unacquireInput();
	}
}

int gxRuntime::callDll(const std::string& dll, const std::string& func, const void* in, int in_sz, void* out, int out_sz) {

	std::map<std::string, gxDll*>::const_iterator lib_it = libs.find(dll);

	if(lib_it == libs.end()) {
		HINSTANCE h = LoadLibrary(dll.c_str());
		if(!h) return 0;
		gxDll* t = new gxDll;
		t->hinst = h;
		lib_it = libs.insert(make_pair(dll, t)).first;
	}

	gxDll* t = lib_it->second;
	std::map<std::string, LibFunc>::const_iterator fun_it = t->funcs.find(func);

	if(fun_it == t->funcs.end()) {
		LibFunc f = (LibFunc)GetProcAddress(t->hinst, func.c_str());
		if(!f) return 0;
		fun_it = t->funcs.insert(make_pair(func, f)).first;
	}

	static void* save_esp;

	_asm {
		mov[save_esp], esp
	};

	int n = fun_it->second(in, in_sz, out, out_sz);

	_asm {
		mov esp, [save_esp]
	};

	return n;
}