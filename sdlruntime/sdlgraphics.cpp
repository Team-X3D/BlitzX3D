#include "std.h"
#include "sdlgraphics.h"
#include "sdleffect.h"
#include "sdlruntime.h"
#include "asyncimage.h"
#include "../sdlgpu/sdl_gpu_texture.h"
#include "../sdlgpu/sdl_gpu_text.h"
#include "../sdlgpu/sdl_gpu_context.h"
#include "../sdlgpu/sdl_gpu_pipeline.h"
#include "../sdlgpu/sdl_gpu_shader.h"
#include "sdlutf8.h"
#include <cstring>
#include <SDL3_ttf/SDL_ttf.h>
#pragma comment (lib, "Dwmapi")
#include <dwmapi.h>

extern sdlRuntime* sdl_runtime;
static Debugger* debugger;

sdlGraphics::sdlGraphics(sdlRuntime* rt, IDirect3DDevice9Ex* dev, IDirect3DSurface9* front, IDirect3DSurface9* back, bool d3d, int w, int h) : runtime(rt), dir3dDev(dev), frontBuffer(front), backBuffer(back), gfx_lost(false), dummy_mesh(0), skin_vshader(nullptr), skin_decl(nullptr), skin_shader_load_failed(false), skin_caps_checked(-1) {

	if (dir3dDev) dir3dDev->AddRef();
	if (frontBuffer) frontBuffer->AddRef();
	if (backBuffer) backBuffer->AddRef();

	dir3d = rt->d3d;
	if (dir3d) dir3d->AddRef();
	present_params = rt->d3dpp;

	front_canvas = frontBuffer ? new sdlCanvas(this, frontBuffer, 0) : new sdlCanvas(this, w, h, 0);
	back_canvas = backBuffer ? new sdlCanvas(this, backBuffer, 0) : new sdlCanvas(this, w, h, 0);

	front_canvas->cls();
	back_canvas->cls();

	TTF_Init();

	std::string defaultFontPath;
	for (const char* face : { "Courier", "Courier New", "Arial", "Tahoma", "Microsoft Sans Serif" }) {
		defaultFontPath = UTF8::getSystemFontFile(face);
		if (!defaultFontPath.empty()) break;
	}
	def_font = defaultFontPath.empty() ? nullptr : this->loadFont(defaultFontPath, 12);
	front_canvas->setFont(def_font);
	back_canvas->setFont(def_font);

	D3DCAPS9 caps;
	if (dir3dDev && SUCCEEDED(dir3dDev->GetDeviceCaps(&caps))) {
		// simple for now, we should probably enumerate!!
		zbuffFmt = D3DFMT_D16;
	}
	else {
		zbuffFmt = D3DFMT_UNKNOWN;
	}

	for (int i = 0; i < 3; ++i)
		for (int k = 0; k < 256; ++k)
			gammaRamp[i][k] = (unsigned short)(k * 257);
}

sdlGraphics::~sdlGraphics() {
	ddUtil::releaseCopyScratch();
	releaseCopyScratchCanvas();
	releaseResolveScratch();
	while (scene_set.size()) freeScene(*scene_set.begin());
	while (movie_set.size()) closeMovie(*movie_set.begin());
	while (font_set.size()) freeFont(*font_set.begin());
	while (canvas_set.size()) freeCanvas(*canvas_set.begin());
	while (mesh_set.size()) freeMesh(*mesh_set.begin());
	if (skin_vshader) { skin_vshader->Release(); skin_vshader = nullptr; }
	if (skin_decl) { skin_decl->Release(); skin_decl = nullptr; }
	/*
	std::set<std::set<std::any>*>::iterator custom_set_it;
	for (custom_set_it = custom_set.begin(); custom_set_it != custom_set.end(); ++custom_set_it) {
		while ((*custom_set_it)->size()) {
			(*custom_set_it)->erase((*custom_set_it)->begin());
			delete* custom_set_it;
		}
	}
	*/

	for (auto it = font_res.begin(); it != font_res.end(); ++it) RemoveFontResource((*it).c_str());
	font_res.clear();

	delete antialias_canvas;
	delete back_canvas;
	delete front_canvas;

	TTF_Quit();

	if (dir3dDev) dir3dDev->Release();
	if (dir3d) dir3d->Release();
	if (frontBuffer && frontBuffer != backBuffer) frontBuffer->Release();
	if (backBuffer) backBuffer->Release();
}

void sdlGraphics::refreshAntialiasCanvas(int w, int h, D3DFORMAT fmt) {
	if (!antialias_canvas) return;

	antialias_canvas->releaseZBuffer();
	if (antialias_canvas->plain_surf) antialias_canvas->plain_surf->Release();
	antialias_canvas->plain_surf = nullptr;
	antialias_canvas->surf = nullptr;

	IDirect3DSurface9* target = nullptr;
	bool msaa = false;

	if (runtime->antialiasRequested() && dir3dDev && back_canvas && back_canvas->surf) {
		D3DFORMAT rtFmt = (fmt == D3DFMT_UNKNOWN) ? D3DFMT_X8R8G8B8 : fmt;
		BOOL windowed = present_params.Windowed ? TRUE : FALSE;
		DWORD quality = 0;
		D3DMULTISAMPLE_TYPE type = runtime->chooseMultisampleType(rtFmt, windowed, &quality);
		if (type != D3DMULTISAMPLE_NONE) {
			IDirect3DSurface9* rt = nullptr;
			if (SUCCEEDED(dir3dDev->CreateRenderTarget(w, h, rtFmt, type, quality, FALSE, &rt, nullptr)) && rt) {
				target = rt;
				msaa = true;
			}
		}
	}

	if (!target) {
		target = back_canvas->surf;
		target->AddRef();
		msaa = false;
	}

	antialias_canvas->surf = target;
	antialias_canvas->plain_surf = target;

	D3DSURFACE_DESC desc;
	target->GetDesc(&desc);
	antialias_canvas->format.setFormat(desc.Format);
	antialias_canvas->logical_w = desc.Width;
	antialias_canvas->logical_h = desc.Height;
	antialias_canvas->clip_rect.left = 0;
	antialias_canvas->clip_rect.top = 0;
	antialias_canvas->clip_rect.right = desc.Width;
	antialias_canvas->clip_rect.bottom = desc.Height;
	antialias_canvas->setViewport(0, 0, desc.Width, desc.Height);

	antialias_msaa = msaa;
	applied_antialias_request = runtime->antialiasRequested();

	if (zbuffFmt != D3DFMT_UNKNOWN) antialias_canvas->attachZBuffer();
}

bool sdlGraphics::ensureResolveScratch(int w, int h, D3DFORMAT fmt) {
	if (resolve_scratch && resolve_scratch_fmt == fmt && resolve_scratch_w >= w && resolve_scratch_h >= h) return true;
	releaseResolveScratch();
	if (FAILED(dir3dDev->CreateRenderTarget(w, h, fmt, D3DMULTISAMPLE_NONE, 0, FALSE, &resolve_scratch, nullptr)) || !resolve_scratch) {
		resolve_scratch = nullptr;
		return false;
	}
	resolve_scratch_w = w; resolve_scratch_h = h; resolve_scratch_fmt = fmt;
	return true;
}

void sdlGraphics::releaseResolveScratch() {
	if (resolve_scratch) { resolve_scratch->Release(); resolve_scratch = nullptr; }
	resolve_scratch_w = resolve_scratch_h = 0;
	resolve_scratch_fmt = D3DFMT_UNKNOWN;
}

void sdlGraphics::applyAntialiasChange() {
	if (!antialias_canvas || !back_canvas) return;
	if (applied_antialias_request == runtime->antialiasRequested()) return;
	D3DFORMAT fmt = present_params.BackBufferFormat;
	refreshAntialiasCanvas(back_canvas->getWidth(), back_canvas->getHeight(), fmt);
}

void sdlGraphics::resolveAntialias() {
	if (!antialias_msaa || !antialias_canvas || !antialias_canvas->surf || !back_canvas || !back_canvas->surf || !dir3dDev) return;
	dir3dDev->StretchRect(antialias_canvas->surf, nullptr, back_canvas->surf, nullptr, D3DTEXF_NONE);
}

sdlEffect* sdlGraphics::createEffect(const std::string& filename) {
	if (!runtime || !runtime->sdlGpu) {
		return nullptr;
	}
	std::string dir;
	size_t pos = filename.find_last_of("/\\");
	if (pos != std::string::npos) dir = filename.substr(0, pos + 1);
	sdlgpu::GpuShader* shader = sdlgpu::CreateShaderFromFile((SDL_GPUDevice*)runtime->sdlGpu, filename.c_str(), "VSMain", "PSMain", dir.c_str());
	if (!shader) {
		lastEffectError = sdlgpu::ShaderError();
		return nullptr;
	}
	lastEffectError.clear();
	sdlEffect* e = new sdlEffect(this, (SDL_GPUDevice*)runtime->sdlGpu, shader);
	effect_set.insert(e);
	return e;
}

sdlEffect* sdlGraphics::verifyEffect(sdlEffect* e) {
	return effect_set.count(e) ? e : nullptr;
}

void sdlGraphics::freeEffect(sdlEffect* e) {
	if (effect_set.erase(e)) delete e;
}

void sdlGraphics::clearEffects() {
	while (effect_set.size()) freeEffect(*effect_set.begin());
}

void sdlGraphics::setGamma(int r, int g, int b, float dr, float dg, float db) {
	gammaRamp[0][r & 255] = (unsigned short)(dr * 257.0f);
	gammaRamp[1][g & 255] = (unsigned short)(dg * 257.0f);
	gammaRamp[2][b & 255] = (unsigned short)(db * 257.0f);
}

void sdlGraphics::updateGamma(bool calibrate) {
	(void)calibrate;
	if (runtime && runtime->sdlGpu) {
		sdlgpu::SetGammaRamp((SDL_GPUDevice*)runtime->sdlGpu, (const unsigned short*)gammaRamp);
	}
}

void sdlGraphics::getGamma(int r, int g, int b, float* dr, float* dg, float* db) {
	*dr = gammaRamp[0][r & 255] / 257.0f;
	*dg = gammaRamp[1][g & 255] / 257.0f;
	*db = gammaRamp[2][b & 255] / 257.0f;
}

bool sdlGraphics::ensureD3DBegun() {
	if (d3dSceneOpen) return true;
	if (!dir3dDev) return false;
	if (dir3dDev->BeginScene() != D3D_OK) return false;
	d3dSceneOpen = true;
	return true;
}

void sdlGraphics::endD3DScene() {
	if (!d3dSceneOpen) return;
	d3dSceneOpen = false;
	if (dir3dDev) dir3dDev->EndScene();
}

bool sdlGraphics::restore() {
	if (!dir3dDev) return true;

	HRESULT hr = dir3dDev->CheckDeviceState(runtime->hwnd);
	if (hr == D3DERR_DEVICELOST || hr == D3DERR_DEVICEHUNG || hr == D3DERR_DEVICEREMOVED) return false;

	if (hr == D3DERR_DEVICENOTRESET || hr == S_PRESENT_MODE_CHANGED) {
		if (present_params.Windowed && !runtime->antialiasRequested()) {
			present_params.Flags |= D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
		}

		runtime->applyAntialiasToParams(present_params);

		ddUtil::releaseCopyScratch();
		releaseCopyScratchCanvas();
		releaseResolveScratch();
		hr = dir3dDev->ResetEx(&present_params, present_params.Windowed ? nullptr : &runtime->d3ddmEx);
		if (FAILED(hr) && present_params.MultiSampleType != D3DMULTISAMPLE_NONE) {
			present_params.MultiSampleType = D3DMULTISAMPLE_NONE;
			present_params.MultiSampleQuality = 0;
			present_params.Flags |= D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
			hr = dir3dDev->ResetEx(&present_params, present_params.Windowed ? nullptr : &runtime->d3ddmEx);
		}
		if (FAILED(hr)) return false;

		IDirect3DSurface9* newBack = nullptr;
		hr = dir3dDev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &newBack);
		if (FAILED(hr) || !newBack) return false;

		if (runtime->backBuffer) runtime->backBuffer->Release();
		runtime->backBuffer = newBack;
		if (runtime->stretchRT) { runtime->stretchRT->Release(); runtime->stretchRT = nullptr; }

		if (back_canvas && back_canvas->surf) {
			back_canvas->surf->Release();
			back_canvas->surf = newBack;
			newBack->AddRef();
		}
		if (front_canvas) {
			front_canvas->surf->Release();
			front_canvas->surf = newBack;
			newBack->AddRef();
		}

		if (antialias_canvas) {
			D3DSURFACE_DESC d;
			newBack->GetDesc(&d);
			refreshAntialiasCanvas(d.Width, d.Height, d.Format);
		}

		for (auto it = canvas_set.begin(); it != canvas_set.end(); ++it) {
			(*it)->restore();
			(*it)->restoreZBuffer();
		}

		if (back_canvas) back_canvas->restoreZBuffer();
		if (front_canvas && front_canvas != back_canvas) front_canvas->restoreZBuffer();

		for (auto it = mesh_set.begin(); it != mesh_set.end(); ++it) {
			(*it)->restore();
		}

		d3dSceneOpen = false;
		for (auto scene : scene_set) scene->invalidateD3DCaches();
		for (auto it = canvas_set.begin(); it != canvas_set.end(); ++it) (*it)->pushAllD3D();
		if (back_canvas) back_canvas->pushAllD3D();
		if (front_canvas && front_canvas != back_canvas) front_canvas->pushAllD3D();

		for (auto font : font_set) {
			for (auto atlas : font->atlases) {
				atlas->restore();
			}
			if (font->tempCanvas) {
				font->tempCanvas->restore();
			}
		}

		InvalidateRect(runtime->hwnd, nullptr, FALSE);
	}
	return true;
}

bool sdlGraphics::changeDisplayMode(int width, int height, bool fullscreen, bool borderless) {
	if (dir3dDev == nullptr && runtime && runtime->sdlWindow) {
		SDL_Window* win = (SDL_Window*)runtime->sdlWindow;
		sdlgpu::SetWindowFullscreen(win, fullscreen);
		if (!fullscreen) {
			sdlgpu::SizeWindowForClient(win, width, height);
			sdlgpu::CenterWindow(win);
		}
		runtime->setFullscreenState(fullscreen);
		return true;
	}
	if (!dir3dDev) return false;

	HWND hwnd = runtime->hwnd;

	if (fullscreen) {
		SetWindowLong(hwnd, GWL_STYLE, WS_VISIBLE | WS_POPUP);
		SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, width, height, SWP_FRAMECHANGED);
		ShowCursor(FALSE);
	}
	else if (borderless) {
		SetWindowLong(hwnd, GWL_STYLE, WS_VISIBLE | WS_POPUP);
		int dw = GetSystemMetrics(SM_CXSCREEN);
		int dh = GetSystemMetrics(SM_CYSCREEN);
		SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, dw, dh, SWP_FRAMECHANGED);
		width = dw;
		height = dh;
	}
	else {
		DWORD style = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE;
		SetWindowLong(hwnd, GWL_STYLE, style);
		RECT w_r, c_r;
		GetWindowRect(hwnd, &w_r);
		GetClientRect(hwnd, &c_r);
		int borderX = (w_r.right - w_r.left) - (c_r.right - c_r.left);
		int borderY = (w_r.bottom - w_r.top) - (c_r.bottom - c_r.top);
		int cx = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
		int cy = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
		MoveWindow(hwnd, cx, cy, width + borderX, height + borderY, TRUE);
	}

	if (runtime->backBuffer) {
		runtime->backBuffer->Release();
		runtime->backBuffer = nullptr;
	}
	if (runtime->frontBuffer && runtime->frontBuffer != runtime->backBuffer) {
		runtime->frontBuffer->Release();
		runtime->frontBuffer = nullptr;
	}

	present_params.BackBufferWidth = width;
	present_params.BackBufferHeight = height;
	present_params.Windowed = !fullscreen;
	if (fullscreen) {
		present_params.FullScreen_RefreshRateInHz = D3DPRESENT_RATE_DEFAULT;
		present_params.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
	}
	else {
		present_params.FullScreen_RefreshRateInHz = 0;
		present_params.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
	}

	memset(&runtime->d3ddmEx, 0, sizeof(runtime->d3ddmEx));
	runtime->d3ddmEx.Size = sizeof(D3DDISPLAYMODEEX);
	runtime->d3ddmEx.Width = width;
	runtime->d3ddmEx.Height = height;
	runtime->d3ddmEx.Format = present_params.BackBufferFormat;
	runtime->d3ddmEx.RefreshRate = 0;
	runtime->d3ddmEx.ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;

	runtime->applyAntialiasToParams(present_params);

	ddUtil::releaseCopyScratch();
	releaseCopyScratchCanvas();
	releaseResolveScratch();
	HRESULT hr = dir3dDev->ResetEx(&present_params, fullscreen ? &runtime->d3ddmEx : nullptr);
	if (FAILED(hr) && present_params.MultiSampleType != D3DMULTISAMPLE_NONE) {
		present_params.MultiSampleType = D3DMULTISAMPLE_NONE;
		present_params.MultiSampleQuality = 0;
		present_params.Flags |= D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
		hr = dir3dDev->ResetEx(&present_params, fullscreen ? &runtime->d3ddmEx : nullptr);
	}
	if (FAILED(hr)) {
		char buf[256];
		sprintf(buf, "ResetEx failed: 0x%08X", hr);
		runtime->debugLog(buf);
		return false;
	}

	IDirect3DSurface9* newBack = nullptr;
	hr = dir3dDev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &newBack);
	if (FAILED(hr) || !newBack) {
		runtime->debugLog("GetBackBuffer failed");
		return false;
	}
	runtime->backBuffer = newBack;
	runtime->frontBuffer = newBack;
	newBack->AddRef();
	newBack->AddRef();

	if (runtime->stretchRT) { runtime->stretchRT->Release(); runtime->stretchRT = nullptr; }

	auto updateCanvas = [&](sdlCanvas* canvas) {
		if (!canvas) return;
		if (canvas->surf) {
			canvas->surf->Release();
			canvas->surf = nullptr;
		}
		canvas->surf = newBack;
		newBack->AddRef();

		canvas->logical_w = width;
		canvas->logical_h = height;
		canvas->clip_rect.left = 0;
		canvas->clip_rect.top = 0;
		canvas->clip_rect.right = width;
		canvas->clip_rect.bottom = height;
		canvas->setViewport(0, 0, width, height);

		canvas->restoreZBuffer();
		};

	updateCanvas(front_canvas);
	updateCanvas(back_canvas);

	if (antialias_canvas) {
		refreshAntialiasCanvas(width, height, present_params.BackBufferFormat);
	}

	for (auto it = mesh_set.begin(); it != mesh_set.end(); ++it) {
		(*it)->restore();
	}
	for (auto font : font_set) {
		for (auto atlas : font->atlases) atlas->restore();
		if (font->tempCanvas) font->tempCanvas->restore();
	}

	InvalidateRect(hwnd, nullptr, FALSE);

	return true;
}

bool sdlGraphics::setDarkMode(bool mode) {
	if (!runtime) return false;
	HWND hwnd = runtime->hwnd;
	if (!hwnd || !IsWindow(hwnd)) return false;

	BOOL DARK_MODE = mode ? TRUE : FALSE;
	HRESULT hr = DwmSetWindowAttribute(hwnd, 20, &DARK_MODE, sizeof(DARK_MODE));
	if (FAILED(hr)) hr = DwmSetWindowAttribute(hwnd, 19, &DARK_MODE, sizeof(DARK_MODE));
	return SUCCEEDED(hr);
}

sdlCanvas* sdlGraphics::getFrontCanvas()const {
	return front_canvas;
}

sdlCanvas* sdlGraphics::getBackCanvas()const {
	return antialias_canvas ? antialias_canvas : back_canvas;
}

sdlFont* sdlGraphics::getDefaultFont()const {
	return def_font;
}

void sdlGraphics::vwait() { // stubby stbu stub
	// dirDraw->WaitForVerticalBlank(DDWAITVB_BLOCKBEGIN, 0);
}

sdlGraphics::DeviceState sdlGraphics::getDeviceState() {
	if (!dir3dDev) return DEVICE_OK;
	HRESULT hr = dir3dDev->CheckDeviceState(runtime->hwnd);
	if (hr == D3DERR_DEVICELOST || hr == D3DERR_DEVICEHUNG || hr == D3DERR_DEVICEREMOVED) return DEVICE_LOST;
	if (hr == D3DERR_DEVICENOTRESET || hr == S_PRESENT_MODE_CHANGED) return DEVICE_NEEDS_RESET;
	return DEVICE_OK;
}

void sdlGraphics::flip(bool vwait) {
	if (runtime) runtime->flip(vwait);
}

bool sdlGraphics::copySceneToTexture(sdlCanvas* dest, int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh) {
	if (!runtime || !runtime->sdlGpu) return false;
	SDL_GPUDevice* dev = (SDL_GPUDevice*)runtime->sdlGpu;
	for (sdlScene* scene : scene_set) {
		if (scene && scene->blitFrameToTexture(dev, dest, dx, dy, dw, dh, sx, sy, sw, sh)) return true;
	}
	return false;
}

void sdlGraphics::copy(sdlCanvas* dest, int dx, int dy, int dw, int dh, sdlCanvas* src, int sx, int sy, int sw, int sh) {
	if (dest->getSurface() && src->getSurface()) {
		ddUtil::copy(dir3dDev, dest->getSurface(), dx, dy, dw, dh, src->getSurface(), sx, sy, sw, sh);
		RECT r = { dx, dy, dx + dw, dy + dh };
		dest->damageD3D(r);
		return;
	}
	if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
	if (runtime && runtime->sdlGpu && src == getBackCanvas()
		&& (dest->getFlags() & (sdlCanvas::CANVAS_TEXTURE | sdlCanvas::CANVAS_TEX_RGB))) {
		if (copySceneToTexture(dest, dx, dy, dw, dh, sx, sy, sw, sh)) return;
	}
	if (!dest->lock() || !src->lockRO()) {
		if (dest->isLocked()) dest->unlock();
		if (src->isLocked()) src->unlock();
		return;
	}
	for (int y = 0; y < dh; ++y)
		for (int x = 0; x < dw; ++x)
			dest->copyPixelFast(dx + x, dy + y, src, sx + x * sw / dw, sy + y * sh / dh);
	src->unlock();
	dest->unlock();
	RECT r = { dx, dy, dx + dw, dy + dh };
	dest->damage(r);
}

bool sdlGraphics::ensureCopyScratch(int w, int h, D3DFORMAT fmt) {
	if (copy_scratch && (copy_scratch_fmt != fmt || copy_scratch_w < w || copy_scratch_h < h)) releaseCopyScratchCanvas();
	if (!copy_scratch) {
		IDirect3DTexture9* tex = nullptr;
		if (FAILED(dir3dDev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, fmt, D3DPOOL_DEFAULT, &tex, nullptr))) return false;
		copy_scratch = new sdlCanvas(this, tex, 0);
		copy_scratch_w = w; copy_scratch_h = h; copy_scratch_fmt = fmt;
	}
	return true;
}

void sdlGraphics::releaseCopyScratchCanvas() {
	if (copy_scratch) { delete copy_scratch; copy_scratch = nullptr; }
	copy_scratch_w = copy_scratch_h = 0;
	copy_scratch_fmt = D3DFMT_UNKNOWN;
}

int sdlGraphics::getScanLine() const { return 0; }

int sdlGraphics::getAvailVidmem() const { return 0; }

int sdlGraphics::getTotalVidmem() const { return 0; }

sdlMovie* sdlGraphics::openMovie(const std::string& file, int flags) {
	sdlMovie* movie = new sdlMovie(this, file);
	if (!movie->isValid()) {
		delete movie;
		return nullptr;
	}
	movie_set.insert(movie);
	return movie;
}

sdlMovie* sdlGraphics::verifyMovie(sdlMovie* m) {
	return movie_set.count(m) ? m : 0;
}

void sdlGraphics::closeMovie(sdlMovie* m) {
	if (movie_set.erase(m)) delete m;
}

static sdlCanvas* buildCpuCanvas(sdlGraphics* g, const DecodedImage& img, int flags, bool keepPixels) {
	if (img.w <= 0 || img.h <= 0 || img.rgba.empty()) return nullptr;
	if ((size_t)img.w * (size_t)img.h * 4 != img.rgba.size()) return nullptr;
	sdlCanvas* c = nullptr;
	try { c = new sdlCanvas(g, img.w, img.h, flags); }
	catch (...) { return nullptr; }

	bool seeded = false;
	if (!keepPixels && g->runtime && g->runtime->sdlGpu)
		seeded = sdlgpu::SeedCanvasTexture((SDL_GPUDevice*)g->runtime->sdlGpu, c,
			(unsigned)img.w, (unsigned)img.h, img.rgba.data());

	if (keepPixels || !seeded) {
		if (!c->lock()) { delete c; return nullptr; }
		bool hasMask = (flags & sdlCanvas::CANVAS_TEX_MASK) != 0;
		bool hasAlpha = (flags & sdlCanvas::CANVAS_TEX_ALPHA) != 0;
		const uint8_t* src = img.rgba.data();
		unsigned char* dst = c->getLockedSurf();
		int pitch = c->getLockedPitch();
		for (int y = 0; y < img.h; ++y) {
			unsigned char* row = dst + (size_t)y * pitch;
			for (int x = 0; x < img.w; ++x) {
				unsigned r = src[0], gg = src[1], b = src[2], a = src[3];
				if (hasMask) a = (r | gg | b) ? 255 : 0;
				else if (hasAlpha) { if (!img.hasAlpha) a = (r + gg + b) / 3; }
				else a = 255;
				c->format.setPixel(row + (size_t)x * 4, c->format.fromARGB((a << 24) | (r << 16) | (gg << 8) | b));
				src += 4;
			}
		}
		c->unlock();
	}
	return c;
}

sdlCanvas* sdlGraphics::createCanvas(int w, int h, int flags) {
	if (w <= 0 || h <= 0) return nullptr;
	if (runtime && runtime->sdlGpu) {
		sdlCanvas* c = nullptr;
		try { c = new sdlCanvas(this, w, h, flags); }
		catch (...) { return nullptr; }
		if (!c->lock()) { delete c; return nullptr; }
		c->unlock();
		canvas_set.insert(c);
		c->cls();
		c->cpu_keep = false;
		return c;
	}
	if (flags & sdlCanvas::CANVAS_TEX_CUBE) {
		int size = w > h ? w : h;
		IDirect3DCubeTexture9* cubeTex = ddUtil::createCubeTextureSurface(size, flags, this);
		if (!cubeTex) return nullptr;
		sdlCanvas* c = new sdlCanvas(this, cubeTex, flags);
		canvas_set.insert(c);
		c->cls();
		return c;
	}
	if (flags & sdlCanvas::CANVAS_TEXTURE) {
		IDirect3DTexture9* tex = ddUtil::createTextureSurface(w, h, flags, this, true);
		if (!tex) return nullptr;
		sdlCanvas* c = new sdlCanvas(this, tex, flags);
		canvas_set.insert(c);
		c->cls();
		return c;
	}
    IDirect3DSurface9* surf = ddUtil::createDisplaySurface(w, h, flags, this);
	if (!surf) return nullptr;
	sdlCanvas* c = new sdlCanvas(this, surf, flags);
	canvas_set.insert(c);
	c->cls();
	return c;
}

sdlCanvas* sdlGraphics::loadCanvas(const std::string& f, int flags) {
	if (runtime && runtime->sdlGpu) {
		auto img = DecodeImageFile(f);
		if (!img) return nullptr;
		if (!(flags & sdlCanvas::CANVAS_TEXTURE)) {
			if (img->hasAlpha) flags |= sdlCanvas::CANVAS_TEXTURE | sdlCanvas::CANVAS_TEX_ALPHA;
		}
		if ((flags & sdlCanvas::CANVAS_TEX_MASK) && !(flags & sdlCanvas::CANVAS_TEX_ALPHA))
			flags |= sdlCanvas::CANVAS_TEX_ALPHA;
		sdlCanvas* c = buildCpuCanvas(this, *img, flags, true);
		if (c) canvas_set.insert(c);
		return c;
	}
	if (!(flags & sdlCanvas::CANVAS_TEXTURE)) {
		if (ddUtil::hasActualAlpha(f)) {
			flags |= sdlCanvas::CANVAS_TEXTURE | sdlCanvas::CANVAS_TEX_ALPHA;
		} else if (flags & sdlCanvas::CANVAS_TEX_MIPMAP) {
			flags |= sdlCanvas::CANVAS_TEXTURE;
		}
	}
	if (flags & sdlCanvas::CANVAS_TEXTURE) {
		int srcW = 0, srcH = 0;
		IDirect3DTexture9* tex = ddUtil::loadTextureSurface(f, flags, this, true, &srcW, &srcH);
		if (!tex) return nullptr;
		sdlCanvas* c = new sdlCanvas(this, tex, flags);
		if (srcW > 0 && srcH > 0) c->setLogicalSize(srcW, srcH);
		canvas_set.insert(c);
		return c;
	}
	IDirect3DSurface9* surf = ddUtil::loadDisplaySurface(f, flags, this);
	if (!surf) return nullptr;
	sdlCanvas* c = new sdlCanvas(this, surf, flags);
	canvas_set.insert(c);
	return c;
}

sdlCanvas* sdlGraphics::createCanvasFromImage(const DecodedImage* img, int flags, bool keepPixels) {
	if (!img) return nullptr;
	if ((flags & sdlCanvas::CANVAS_TEX_MASK) && !(flags & sdlCanvas::CANVAS_TEX_ALPHA)) {
		flags |= sdlCanvas::CANVAS_TEX_ALPHA;
	}
	if (runtime && runtime->sdlGpu) {
		sdlCanvas* c = buildCpuCanvas(this, *img, flags, keepPixels);
		if (c) canvas_set.insert(c);
		return c;
	}
	int w = 0, h = 0;
	IDirect3DTexture9* tex = ddUtil::textureFromDecoded(img, flags, this, true, &w, &h);
	if (!tex) return nullptr;
	sdlCanvas* c = new sdlCanvas(this, tex, flags);
	if (w > 0 && h > 0) c->setLogicalSize(w, h);
	canvas_set.insert(c);
	return c;
}

sdlCanvas* sdlGraphics::verifyCanvas(sdlCanvas* c) {
	return canvas_set.count(c) || c == front_canvas || c == back_canvas || c == antialias_canvas ? c : 0;
}

void sdlGraphics::freeCanvas(sdlCanvas* c) {
	if (canvas_set.erase(c)) delete c;
}

int sdlGraphics::getWidth()const {
	return front_canvas->getWidth();
}

int sdlGraphics::getHeight()const {
	return front_canvas->getHeight();
}

int sdlGraphics::getDepth()const {
	return front_canvas->getDepth();
}

sdlFont* sdlGraphics::loadFont(std::string f, int height, bool bold, bool italic, bool underlined) {
	std::string t = f;
	if (f.find('.') == std::string::npos) {
		std::string sysFont = UTF8::getSystemFontFile(f);
		if (!sysFont.empty()) {
			t = sysFont;
			if (!font_res.count(t) && AddFontResource(t.c_str())) font_res.insert(t);
		}
	}

	sdlFont* newFont = new sdlFont(this, f, height, bold, italic, underlined);
	font_set.emplace(newFont);
	return newFont;
}

sdlFont* sdlGraphics::verifyFont(sdlFont* f) {
	return font_set.count(f) ? f : 0;
}

void sdlGraphics::freeFont(sdlFont* f) {
	if (font_set.erase(f)) delete f;
}

//////////////
// 3D STUFF //
//////////////

sdlScene* sdlGraphics::createScene(int flags) {
	if (scene_set.size()) return 0;

	if (dir3dDev) {
		D3DFORMAT depthFormats[] = { D3DFMT_D24S8, D3DFMT_D24X8, D3DFMT_D16, D3DFMT_D32 };
		bool zOk = false;
		for (int i = 0; i < 4; ++i) {
			zbuffFmt = depthFormats[i];
			if (back_canvas->attachZBuffer()) {
				zOk = true;
				break;
			}
		}
		if (!zOk) return 0;
	}

	sdlScene* scene = new sdlScene(this, back_canvas);
	scene_set.insert(scene);
	return scene;
}

sdlScene* sdlGraphics::verifyScene(sdlScene* s) { return scene_set.count(s) ? s : 0; }

void sdlGraphics::freeScene(sdlScene* scene) {
	if (!scene_set.erase(scene)) return;
	dummy_mesh = 0;
	while (mesh_set.size()) freeMesh(*mesh_set.begin());
	(antialias_canvas ? antialias_canvas : back_canvas)->releaseZBuffer();
	delete scene;
}

bool sdlGraphics::presentSceneSDL(struct SDL_GPUDevice* dev, struct SDL_Window* win) {
	if (!dev || !win) return false;
	for (sdlScene* scene : scene_set) {
		if (scene && scene->hasGpuImage()) {
			if (scene->presentGpuFrame(dev, win)) return true;
		}
	}
	return false;
}

bool sdlGraphics::presentSceneWithCanvas(struct SDL_GPUDevice* dev, struct SDL_Window* win, sdlCanvas* canvas) {
	if (!dev || !win) return false;
	for (sdlScene* scene : scene_set) {
		if (scene && (scene->hasGpuImage() || canvas)) {
			if (scene->presentGpuFrameWithCanvas(dev, win, canvas)) return true;
		}
	}
	if (canvas && scene_set.empty()) {
		sdlgpu::GpuSceneFrame empty{};
		if (sdlgpu::PresentSceneWithCanvas(dev, win, empty, canvas)) return true;
	}
	return false;
}

void sdlGraphics::setActiveCanvas(sdlCanvas* canvas) {
	if (!runtime || !runtime->sdlGpu) return;
	sdlgpu::SetActiveCanvasTarget((SDL_GPUDevice*)runtime->sdlGpu, canvas);
}

void sdlGraphics::adoptCanvas(sdlCanvas* c) {
	canvas_set.insert(c);
}

sdlMesh* sdlGraphics::createMesh(int max_verts, int max_tris, int flags) {
	sdlMesh* mesh = new sdlMesh(this, max_verts, max_tris, flags);
	mesh_set.insert(mesh);
	return mesh;
}

sdlMesh* sdlGraphics::verifyMesh(sdlMesh* m) {
	return mesh_set.count(m) ? m : 0;
}

void sdlGraphics::freeMesh(sdlMesh* mesh) {
	if (mesh_set.erase(mesh)) delete mesh;
}

// GPU SKINNING
static const char* SKIN_VSHADER_SRC =
"#define MAX_BONES 64\n"
"#define MAX_LIGHTS 8\n"
"\n"
"float4x3 boneTforms[MAX_BONES] : register(c0); \n"
"\n"
"float4x4 viewProj : register(c192);\n"
"\n"
"float4 lightPos[MAX_LIGHTS]     : register(c196); \n"
"float4 lightDiffuse[MAX_LIGHTS] : register(c204); \n"
"float4 lightAtten[MAX_LIGHTS]   : register(c212); \n"
"float4 lightDir[MAX_LIGHTS]     : register(c220); \n"
"\n"
"float4 ambientColor     : register(c228); \n"
"float4 materialDiffuse  : register(c229); \n"
"float4 materialSpecular : register(c230); \n"
"float4 eyePos            : register(c231); \n"
"\n"
"struct VS_INPUT {\n"
"    float3 pos      : POSITION;\n"
"    float3 normal   : NORMAL;\n"
"    float4 color    : COLOR0;\n"
"    float2 tex0     : TEXCOORD0;\n"
"    float2 tex1     : TEXCOORD1;\n"
"    float4 blendIdx : TEXCOORD2;\n"
"    float4 blendWgt : TEXCOORD3;\n"
"};\n"
"\n"
"struct VS_OUTPUT {\n"
"    float4 pos    : POSITION;\n"
"    float4 color  : COLOR0;\n"
"    float2 tex0   : TEXCOORD0;\n"
"    float2 tex1   : TEXCOORD1;\n"
"    float  fog    : FOG;\n"
"};\n"
"\n"
"VS_OUTPUT main(VS_INPUT IN) {\n"
"    VS_OUTPUT OUT;\n"
"\n"
"    float packedFlags = eyePos.w;\n"
"    bool useVertexColor = (fmod(packedFlags, 2.0) >= 1.0);\n"
"    bool useSpecular = (fmod(floor(packedFlags / 2.0), 2.0) >= 1.0);\n"
"\n"
"    float3 wPos = float3(0,0,0);\n"
"    float3 wNrm = float3(0,0,0);\n"
"    float totalWeight = 0;\n"
"\n"
"    [unroll]\n"
"    for (int i = 0; i < 4; ++i) {\n"
"        float w = IN.blendWgt[i];\n"
"        int   b = (int)IN.blendIdx[i];\n"
"        wPos += mul(float4(IN.pos, 1), boneTforms[b]) * w;\n"
"        wNrm += mul(IN.normal, (float3x3)boneTforms[b]) * w;\n"
"        totalWeight += w;\n"
"    }\n"
"    if (totalWeight <= 0.00001) {\n"
"        wPos = mul(float4(IN.pos, 1), boneTforms[0]);\n"
"        wNrm = mul(IN.normal, (float3x3)boneTforms[0]);\n"
"    }\n"
"    wNrm = normalize(wNrm);\n"
"\n"
"    OUT.pos = mul(float4(wPos, 1), viewProj);\n"
"    OUT.fog = OUT.pos.z;\n"
"    OUT.tex0 = IN.tex0;\n"
"    OUT.tex1 = IN.tex1;\n"
"\n"
"    float3 diffuseBase = useVertexColor ? IN.color.rgb : materialDiffuse.rgb;\n"
"    float3 lit = ambientColor.rgb * diffuseBase;\n"
"    float3 spec = float3(0,0,0);\n"
"    float3 toEye = normalize(eyePos.xyz - wPos);\n"
"\n"
"    [loop]\n"
"    for (int n = 0; n < MAX_LIGHTS; ++n) {\n"
"        if (n >= (int)ambientColor.w) break;\n"
"        float3 toLight;\n"
"        float atten = 1;\n"
"        if (lightPos[n].w < 0.5) {\n"
"            toLight = lightPos[n].xyz;\n"
"        } else {\n"
"            float3 delta = lightPos[n].xyz - wPos;\n"
"            float dist = length(delta);\n"
"            toLight = delta / max(dist, 0.0001);\n"
"            atten = 1.0 / max(1.0, 1.0 + lightAtten[n].x * dist);\n"
"            if (lightPos[n].w > 1.5) {\n"
"                float cosAng = dot(-toLight, lightDir[n].xyz);\n"
"                float spotT = saturate((cosAng - lightAtten[n].z) / max(lightAtten[n].y - lightAtten[n].z, 0.0001));\n"
"                atten *= pow(spotT, max(lightAtten[n].w, 0.0001));\n"
"            }\n"
"        }\n"
"        float ndotl = max(0, dot(wNrm, toLight));\n"
"        lit += lightDiffuse[n].rgb * diffuseBase * ndotl * atten;\n"
"        if (useSpecular && ndotl > 0) {\n"
"            float3 halfVec = normalize(toLight + toEye);\n"
"            float specPow = pow(max(0, dot(wNrm, halfVec)), max(materialSpecular.w, 1.0));\n"
"            spec += lightDiffuse[n].rgb * materialSpecular.rgb * specPow * atten;\n"
"        }\n"
"    }\n"
"\n"
"    OUT.color = float4(saturate(lit + spec), (useVertexColor ? IN.color.a : 1) * materialDiffuse.a);\n"
"    return OUT;\n"
"}\n";

bool sdlGraphics::skinningSupported() {
	if (skin_caps_checked == -1) {
		D3DCAPS9 caps;
		skin_caps_checked = 0;
		if (dir3dDev && SUCCEEDED(dir3dDev->GetDeviceCaps(&caps))) {
			if (caps.VertexShaderVersion >= D3DVS_VERSION(3, 0)) {
				skin_caps_checked = 1;
			}
		}
	}
	return skin_caps_checked == 1;
}

bool sdlGraphics::ensureSkinningShader() {
	if (skin_vshader && skin_decl) return true;
	if (skin_shader_load_failed) return false;

	if (!skinningSupported()) {
		runtime->debugLog("GPU skinning: vs_3_0 not supported, falling back to CPU");
		skin_shader_load_failed = true;
		return false;
	}

	if (!skin_decl) {
		if (!dir3dDev) return false;
		static const D3DVERTEXELEMENT9 decl[] = {
			{0, 0,  D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
			{0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL,   0},
			{0, 24, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
			{0, 28, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
			{0, 36, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
			{0, 44, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 2},
			{0, 60, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 3},
			D3DDECL_END()
		};
		if (FAILED(dir3dDev->CreateVertexDeclaration(decl, &skin_decl))) {
			runtime->debugLog("GPU skinning: CreateVertexDeclaration failed");
			skin_shader_load_failed = true;
			return false;
		}
	}

#if SDL_USE_LEGACY_D3DX
	ID3DXBuffer* code = nullptr;
	ID3DXBuffer* errors = nullptr;
	HRESULT hr = D3DXCompileShader(SKIN_VSHADER_SRC, (UINT)strlen(SKIN_VSHADER_SRC), nullptr, nullptr, "main", "vs_3_0", 0, &code, &errors, nullptr);
	if (FAILED(hr)) {
		if (errors) {
			runtime->debugLog("GPU skinning shader compilation failed:");
			runtime->debugLog((const char*)errors->GetBufferPointer());
			errors->Release();
		}
		else {
			runtime->debugLog("GPU skinning: D3DXCompileShader failed with no error buffer");
		}
		skin_shader_load_failed = true;
		return false;
	}
	if (errors) errors->Release();

	hr = dir3dDev->CreateVertexShader((const DWORD*)code->GetBufferPointer(), &skin_vshader);
	code->Release();
	if (FAILED(hr)) {
		runtime->debugLog("GPU skinning: CreateVertexShader failed");
		skin_shader_load_failed = true;
		return false;
	}

	runtime->debugLog("GPU skinning shader compiled successfully");
	return true;
#else
	skin_shader_load_failed = true;
	return false;
#endif
}
