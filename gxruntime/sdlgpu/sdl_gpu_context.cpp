#include "sdl_gpu_context.h"
#include "sdl_gpu_lock.h"

#include "../std.h"

#include <dinput.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_log.h>

#include "../gxruntime.h"
#include "../gxgraphics.h"
#include "../gxinput.h"

#include <unordered_map>
#include <mutex>

namespace sdlgpu {

static bool EnsureVideoInit() {
	if ((SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) != 0) return true;
	return SDL_InitSubSystem(SDL_INIT_VIDEO);
}

SDL_Window* CreateGameWindow(int clientW, int clientH, bool resizable, bool borderless, bool fullscreen, const char* title) {
	if (!EnsureVideoInit()) return nullptr;

	SDL_WindowFlags flags = SDL_WINDOW_HIDDEN;
	if (fullscreen) {
		flags |= SDL_WINDOW_FULLSCREEN;
	}
	else {
		if (resizable) flags |= SDL_WINDOW_RESIZABLE;
		if (borderless) flags |= SDL_WINDOW_BORDERLESS;
	}

	SDL_Window* win = SDL_CreateWindow(title ? title : " ", clientW, clientH, flags);
	if (!win) return nullptr;

	if (HWND hwnd = (HWND)GetHWND(win)) {
		SetClassLongPtrW(hwnd, GCLP_HBRBACKGROUND, (LONG_PTR)GetStockObject(BLACK_BRUSH));
	}

	if (fullscreen) {
		SDL_DisplayID disp = SDL_GetDisplayForWindow(win);
		int count = 0;
		SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(disp, &count);
		if (modes) {
			const SDL_DisplayMode* best = nullptr;
			for (int i = 0; i < count && modes[i]; ++i) {
				if ((int)modes[i]->w == clientW && (int)modes[i]->h == clientH) { best = modes[i]; break; }
			}
			if (best) SDL_SetWindowFullscreenMode(win, best);
			SDL_free(modes);
		}
	}
	else {
		CenterWindow(win);
	}
	SDL_StartTextInput(win);
	return win;
}

void SetWindowFullscreen(SDL_Window* win, bool fullscreen) {
	if (!win) return;
	SDL_SetWindowFullscreen(win, fullscreen);
}

void* GetHWND(SDL_Window* win) {
	if (!win) return nullptr;
	SDL_PropertiesID props = SDL_GetWindowProperties(win);
	if (!props) return nullptr;
	return SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
}

void DestroyGameWindow(SDL_Window* win) {
	if (!win) return;
	SDL_StopTextInput(win);
	SDL_DestroyWindow(win);
	// do not SDL_QuitSubSystem here!!!!!!!!!
}

void SetWindowTitle(SDL_Window* win, const char* title) {
	if (!win) return;
	SDL_SetWindowTitle(win, title ? title : " ");
}

void SizeWindowForClient(SDL_Window* win, int clientW, int clientH) {
	if (!win) return;
	SDL_SetWindowSize(win, clientW, clientH);
}

void CenterWindow(SDL_Window* win) {
	if (!win) return;
	SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
}

void ShowGameWindow(SDL_Window* win) {
	if (!win) return;
	SDL_ShowWindow(win);
}

void SetCursorVisible(bool vis) {
	if (vis) SDL_ShowCursor();
	else SDL_HideCursor();
}

SDL_GPUDevice* CreateGPUDevice() {
	SDL_PropertiesID props = SDL_CreateProperties();
	if (!props) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_CreateProperties failed: %s", SDL_GetError());
		return nullptr;
	}
#ifdef _DEBUG
	SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_VERBOSE_BOOLEAN, true);
#else
	SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_VERBOSE_BOOLEAN, false);
#endif
	SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true);
	SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_DXIL_BOOLEAN, true);
	// SDL_SetStringProperty(props, SDL_PROP_GPU_DEVICE_CREATE_NAME_STRING, "vulkan");
#ifdef _DEBUG
	SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN, true);
#else
	SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN, false);
#endif
#ifdef _DEBUG
	SDL_SetLogPriority(SDL_LOG_CATEGORY_GPU, SDL_LOG_PRIORITY_VERBOSE);
#endif
	SDL_GPUDevice* dev = SDL_CreateGPUDeviceWithProperties(props);
	SDL_DestroyProperties(props);
	if (dev) SDL_SetGPUAllowedFramesInFlight(dev, 3);
	if (!dev) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_CreateGPUDeviceWithProperties failed: %s", SDL_GetError());
		return nullptr;
	}
	return dev;
}

void DestroyGPUDevice(SDL_GPUDevice* dev) {
	if (!dev) return;
	SDL_DestroyGPUDevice(dev);
}

namespace {
	static std::unordered_map<SDL_Window*, SDL_GPUPresentMode> s_vsyncLastMode;
	static std::once_flag s_motionFlag;
}

bool ClaimWindow(SDL_GPUDevice* dev, SDL_Window* win) {
	if (!dev || !win) return false;
	GpuLock lock;
	if (!SDL_ClaimWindowForGPUDevice(dev, win)) return false;
	if (SDL_SetGPUSwapchainParameters(dev, win, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC))
		s_vsyncLastMode[win] = SDL_GPU_PRESENTMODE_VSYNC;
	return true;
}

void ReleaseWindow(SDL_GPUDevice* dev, SDL_Window* win) {
	if (!dev || !win) return;
	GpuLock lock;
	s_vsyncLastMode.erase(win);
	SDL_ReleaseWindowFromGPUDevice(dev, win);
}

void SetVSync(SDL_GPUDevice* dev, SDL_Window* win, bool vsync) {
	if (!dev || !win) return;
	GpuLock lock;
	SDL_GPUPresentMode want = vsync ? SDL_GPU_PRESENTMODE_VSYNC : SDL_GPU_PRESENTMODE_IMMEDIATE;
	auto it = s_vsyncLastMode.find(win);
	SDL_GPUPresentMode cur = (it != s_vsyncLastMode.end()) ? it->second : SDL_GPU_PRESENTMODE_VSYNC;
	if (want == cur && it != s_vsyncLastMode.end()) return;
	if (want != SDL_GPU_PRESENTMODE_VSYNC && !SDL_WindowSupportsGPUPresentMode(dev, win, want)) {
		if (want == SDL_GPU_PRESENTMODE_IMMEDIATE && SDL_WindowSupportsGPUPresentMode(dev, win, SDL_GPU_PRESENTMODE_MAILBOX)) {
			SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Present mode IMMEDIATE not supported, falling back to MAILBOX");
			want = SDL_GPU_PRESENTMODE_MAILBOX;
		} else {
			SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Present mode %d not supported, keeping VSYNC", (int)want);
			want = SDL_GPU_PRESENTMODE_VSYNC;
		}
		if (want == cur && it != s_vsyncLastMode.end()) return;
	}
	if (SDL_SetGPUSwapchainParameters(dev, win, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, want)) {
		s_vsyncLastMode[win] = want;
	} else {
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "SDL_SetGPUSwapchainParameters failed: %s", SDL_GetError());
	}
}

bool PresentSwapchain(SDL_GPUDevice* dev, SDL_Window* win, float r, float g, float b) {
	if (!dev || !win) return false;
	SDL_GPUCommandBuffer* cmds = SDL_AcquireGPUCommandBuffer(dev);
	if (!cmds) {
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "SDL_AcquireGPUCommandBuffer failed: %s", SDL_GetError());
		return false;
	}
	SDL_GPUTexture* tex = nullptr;
	Uint32 w = 0, h = 0;
	if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmds, win, &tex, &w, &h)) {
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "SDL_WaitAndAcquireGPUSwapchainTexture failed: %s", SDL_GetError());
		SDL_CancelGPUCommandBuffer(cmds);
		return false;
	}
	if (tex) {
		SDL_GPUColorTargetInfo target{};
		target.texture = tex;
		target.load_op = SDL_GPU_LOADOP_CLEAR;
		target.store_op = SDL_GPU_STOREOP_STORE;
		target.clear_color = SDL_FColor{ r, g, b, 1.0f };
		SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmds, &target, 1, nullptr);
		if (pass) SDL_EndGPURenderPass(pass);
	}
	if (!SDL_SubmitGPUCommandBuffer(cmds)) {
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "SDL_SubmitGPUCommandBuffer failed: %s", SDL_GetError());
		return false;
	}
	return true;
}

int SdlScancodeToDIK(int sc) {
	switch (sc) {
	case SDL_SCANCODE_ESCAPE: return DIK_ESCAPE;
	case SDL_SCANCODE_1: return DIK_1;
	case SDL_SCANCODE_2: return DIK_2;
	case SDL_SCANCODE_3: return DIK_3;
	case SDL_SCANCODE_4: return DIK_4;
	case SDL_SCANCODE_5: return DIK_5;
	case SDL_SCANCODE_6: return DIK_6;
	case SDL_SCANCODE_7: return DIK_7;
	case SDL_SCANCODE_8: return DIK_8;
	case SDL_SCANCODE_9: return DIK_9;
	case SDL_SCANCODE_0: return DIK_0;
	case SDL_SCANCODE_MINUS: return DIK_MINUS;
	case SDL_SCANCODE_EQUALS: return DIK_EQUALS;
	case SDL_SCANCODE_BACKSPACE: return DIK_BACK;
	case SDL_SCANCODE_TAB: return DIK_TAB;
	case SDL_SCANCODE_Q: return DIK_Q;
	case SDL_SCANCODE_W: return DIK_W;
	case SDL_SCANCODE_E: return DIK_E;
	case SDL_SCANCODE_R: return DIK_R;
	case SDL_SCANCODE_T: return DIK_T;
	case SDL_SCANCODE_Y: return DIK_Y;
	case SDL_SCANCODE_U: return DIK_U;
	case SDL_SCANCODE_I: return DIK_I;
	case SDL_SCANCODE_O: return DIK_O;
	case SDL_SCANCODE_P: return DIK_P;
	case SDL_SCANCODE_LEFTBRACKET: return DIK_LBRACKET;
	case SDL_SCANCODE_RIGHTBRACKET: return DIK_RBRACKET;
	case SDL_SCANCODE_RETURN: return DIK_RETURN;
	case SDL_SCANCODE_LCTRL: return DIK_LCONTROL;
	case SDL_SCANCODE_A: return DIK_A;
	case SDL_SCANCODE_S: return DIK_S;
	case SDL_SCANCODE_D: return DIK_D;
	case SDL_SCANCODE_F: return DIK_F;
	case SDL_SCANCODE_G: return DIK_G;
	case SDL_SCANCODE_H: return DIK_H;
	case SDL_SCANCODE_J: return DIK_J;
	case SDL_SCANCODE_K: return DIK_K;
	case SDL_SCANCODE_L: return DIK_L;
	case SDL_SCANCODE_SEMICOLON: return DIK_SEMICOLON;
	case SDL_SCANCODE_APOSTROPHE: return DIK_APOSTROPHE;
	case SDL_SCANCODE_GRAVE: return DIK_GRAVE;
	case SDL_SCANCODE_LSHIFT: return DIK_LSHIFT;
	case SDL_SCANCODE_BACKSLASH: return DIK_BACKSLASH;
	case SDL_SCANCODE_Z: return DIK_Z;
	case SDL_SCANCODE_X: return DIK_X;
	case SDL_SCANCODE_C: return DIK_C;
	case SDL_SCANCODE_V: return DIK_V;
	case SDL_SCANCODE_B: return DIK_B;
	case SDL_SCANCODE_N: return DIK_N;
	case SDL_SCANCODE_M: return DIK_M;
	case SDL_SCANCODE_COMMA: return DIK_COMMA;
	case SDL_SCANCODE_PERIOD: return DIK_PERIOD;
	case SDL_SCANCODE_SLASH: return DIK_SLASH;
	case SDL_SCANCODE_RSHIFT: return DIK_RSHIFT;
	case SDL_SCANCODE_KP_MULTIPLY: return DIK_MULTIPLY;
	case SDL_SCANCODE_LALT: return DIK_LMENU;
	case SDL_SCANCODE_SPACE: return DIK_SPACE;
	case SDL_SCANCODE_CAPSLOCK: return DIK_CAPITAL;
	case SDL_SCANCODE_F1: return DIK_F1;
	case SDL_SCANCODE_F2: return DIK_F2;
	case SDL_SCANCODE_F3: return DIK_F3;
	case SDL_SCANCODE_F4: return DIK_F4;
	case SDL_SCANCODE_F5: return DIK_F5;
	case SDL_SCANCODE_F6: return DIK_F6;
	case SDL_SCANCODE_F7: return DIK_F7;
	case SDL_SCANCODE_F8: return DIK_F8;
	case SDL_SCANCODE_F9: return DIK_F9;
	case SDL_SCANCODE_F10: return DIK_F10;
	case SDL_SCANCODE_NUMLOCKCLEAR: return DIK_NUMLOCK;
	case SDL_SCANCODE_SCROLLLOCK: return DIK_SCROLL;
	case SDL_SCANCODE_KP_7: return DIK_NUMPAD7;
	case SDL_SCANCODE_KP_8: return DIK_NUMPAD8;
	case SDL_SCANCODE_KP_9: return DIK_NUMPAD9;
	case SDL_SCANCODE_KP_MINUS: return DIK_SUBTRACT;
	case SDL_SCANCODE_KP_4: return DIK_NUMPAD4;
	case SDL_SCANCODE_KP_5: return DIK_NUMPAD5;
	case SDL_SCANCODE_KP_6: return DIK_NUMPAD6;
	case SDL_SCANCODE_KP_PLUS: return DIK_ADD;
	case SDL_SCANCODE_KP_1: return DIK_NUMPAD1;
	case SDL_SCANCODE_KP_2: return DIK_NUMPAD2;
	case SDL_SCANCODE_KP_3: return DIK_NUMPAD3;
	case SDL_SCANCODE_KP_0: return DIK_NUMPAD0;
	case SDL_SCANCODE_KP_PERIOD: return DIK_DECIMAL;
	case SDL_SCANCODE_F11: return DIK_F11;
	case SDL_SCANCODE_F12: return DIK_F12;
	case SDL_SCANCODE_RCTRL: return DIK_RCONTROL;
	case SDL_SCANCODE_KP_DIVIDE: return DIK_DIVIDE;
	case SDL_SCANCODE_RALT: return DIK_RMENU;
	case SDL_SCANCODE_KP_ENTER: return DIK_NUMPADENTER;
	case SDL_SCANCODE_UP: return DIK_UP;
	case SDL_SCANCODE_DOWN: return DIK_DOWN;
	case SDL_SCANCODE_LEFT: return DIK_LEFT;
	case SDL_SCANCODE_RIGHT: return DIK_RIGHT;
	case SDL_SCANCODE_INSERT: return DIK_INSERT;
	case SDL_SCANCODE_DELETE: return DIK_DELETE;
	case SDL_SCANCODE_HOME: return DIK_HOME;
	case SDL_SCANCODE_END: return DIK_END;
	case SDL_SCANCODE_PAGEUP: return DIK_PRIOR;
	case SDL_SCANCODE_PAGEDOWN: return DIK_NEXT;
	case SDL_SCANCODE_LGUI: return DIK_LWIN;
	case SDL_SCANCODE_RGUI: return DIK_RWIN;
	case SDL_SCANCODE_APPLICATION: return DIK_APPS;
	default: return 0;
	}
}

static void ForwardMouseMove(SDL_Window* win, gxRuntime* rt, int px, int py) {
	if (!rt || !rt->input) return;
	int x = px, y = py;
	if (rt->graphics && win) {
		int ww = 0, wh = 0;
		if (!SDL_GetWindowSizeInPixels(win, &ww, &wh)) SDL_GetWindowSize(win, &ww, &wh);
		int gw = rt->graphics->getWidth();
		int gh = rt->graphics->getHeight();
		if (ww > 0 && wh > 0 && gw > 0 && gh > 0 && (ww != gw || wh != gh)) {
			x = x * gw / ww;
			y = y * gh / wh;
		}
		if (x < 0) x = 0;
		else if (x >= gw) x = gw - 1;
		if (y < 0) y = 0;
		else if (y >= gh) y = gh - 1;
	}
	rt->input->wm_mousemove(x, y);
}

void PumpEvents(SDL_Window* win, gxRuntime* rt) {
	if (!win || !rt) return;
	std::call_once(s_motionFlag, [] { SDL_SetEventEnabled(SDL_EVENT_MOUSE_MOTION, false); });
	SDL_Event ev;
	while (SDL_PollEvent(&ev)) {
		switch (ev.type) {
		case SDL_EVENT_QUIT:
		case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
			rt->asyncEnd();
			return;
		case SDL_EVENT_KEY_DOWN:
			if (!ev.key.repeat && rt->input) {
				if (int dik = SdlScancodeToDIK((int)ev.key.scancode)) rt->input->wm_keydown(dik);
			}
			break;
		case SDL_EVENT_KEY_UP:
			if (rt->input) {
				if (int dik = SdlScancodeToDIK((int)ev.key.scancode)) rt->input->wm_keyup(dik);
			}
			break;
		case SDL_EVENT_TEXT_INPUT:
			if (rt->input && ev.text.text) {
				const char* p = ev.text.text;
				while (*p) {
					unsigned char c = (unsigned char)*p;
					int cp = 0; int len = 0;
					if (c < 0x80) { cp = c; len = 1; }
					else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
					else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
					else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
					else { ++p; continue; }
					for (int i = 1; i < len; ++i) {
						unsigned char cc = (unsigned char)p[i];
						if ((cc & 0xC0) != 0x80) { len = i; break; }
						cp = (cp << 6) | (cc & 0x3F);
					}
					rt->input->wm_char(cp, 1);
					p += len;
					if (len <= 0) ++p;
				}
			}
			break;
		case SDL_EVENT_MOUSE_BUTTON_DOWN:
			if (rt->input) {
				switch (ev.button.button) {
				case SDL_BUTTON_LEFT: rt->input->wm_mousedown(1); break;
				case SDL_BUTTON_RIGHT: rt->input->wm_mousedown(2); break;
				case SDL_BUTTON_MIDDLE: rt->input->wm_mousedown(3); break;
				case SDL_BUTTON_X1: rt->input->wm_mousedown(5); break;
				case SDL_BUTTON_X2: rt->input->wm_mousedown(4); break;
				default: break;
				}
				ForwardMouseMove(win, rt, (int)ev.button.x, (int)ev.button.y);
			}
			break;
		case SDL_EVENT_MOUSE_BUTTON_UP:
			if (rt->input) {
				switch (ev.button.button) {
				case SDL_BUTTON_LEFT: rt->input->wm_mouseup(1); break;
				case SDL_BUTTON_RIGHT: rt->input->wm_mouseup(2); break;
				case SDL_BUTTON_MIDDLE: rt->input->wm_mouseup(3); break;
				case SDL_BUTTON_X1: rt->input->wm_mouseup(5); break;
				case SDL_BUTTON_X2: rt->input->wm_mouseup(4); break;
				default: break;
				}
				ForwardMouseMove(win, rt, (int)ev.button.x, (int)ev.button.y);
			}
			break;
		case SDL_EVENT_MOUSE_WHEEL:
			if (rt->input) rt->input->wm_mousewheel((int)(ev.wheel.y * 120.0f));
			break;
		default:
			break;
		}
	}
	if (rt->input && (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS)) {
		float mx = 0.0f, my = 0.0f;
		SDL_GetMouseState(&mx, &my);
		ForwardMouseMove(win, rt, (int)mx, (int)my);
	}
}

}
