#include "std.h"
#include "gxgraphics.h"
#include "gxeffect.h"
#include "gxruntime.h"
#include "gxshadercompat.h"
#include "asyncimage.h"
#include "sdlgpu/sdl_gpu_texture.h"
#include "sdlgpu/sdl_gpu_text.h"
#include "sdlgpu/sdl_gpu_context.h"
#include "../gxruntime/gxutf8.h"
#include <cstring>
#include <SDL3_ttf/SDL_ttf.h>
#pragma comment (lib, "Dwmapi")
#include <dwmapi.h>

extern gxRuntime* gx_runtime;
static Debugger* debugger;

gxGraphics::gxGraphics(gxRuntime* rt, IDirect3DDevice9Ex* dev, IDirect3DSurface9* front, IDirect3DSurface9* back, bool d3d, int w, int h) : runtime(rt), dir3dDev(dev), frontBuffer(front), backBuffer(back), gfx_lost(false), dummy_mesh(0), skin_vshader(nullptr), skin_decl(nullptr), skin_shader_load_failed(false), skin_caps_checked(-1) {

	if (dir3dDev) dir3dDev->AddRef();
	if (frontBuffer) frontBuffer->AddRef();
	if (backBuffer) backBuffer->AddRef();

	dir3d = rt->d3d;
	if (dir3d) dir3d->AddRef();
	present_params = rt->d3dpp;

	front_canvas = frontBuffer ? new gxCanvas(this, frontBuffer, 0) : new gxCanvas(this, w, h, 0);
	back_canvas = backBuffer ? new gxCanvas(this, backBuffer, 0) : new gxCanvas(this, w, h, 0);

	front_canvas->cls();
	back_canvas->cls();

	TTF_Init();

	HMODULE ntdllModule = GetModuleHandleW(L"ntdll.dll");
	running_on_wine = ntdllModule && GetProcAddress(ntdllModule, "wine_get_version");

	def_font = running_on_wine ? nullptr : this->loadFont(UTF8::getSystemFontFile("Courier"), 12);
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

	// todo: gamma
}

gxGraphics::~gxGraphics() {
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

	delete back_canvas;
	delete front_canvas;

	TTF_Quit();

	if (dir3dDev) dir3dDev->Release();
	if (dir3d) dir3d->Release();
	if (frontBuffer && frontBuffer != backBuffer) frontBuffer->Release();
	if (backBuffer) backBuffer->Release();
}

gxEffect* gxGraphics::createEffect(const std::string& filename) {
	(void)filename;
	lastEffectError = "Effects are not supported on the SDL-GPU renderer";
	return nullptr;

	/*
	ID3DXEffect* effect = nullptr;
	ID3DXBuffer* errors = nullptr;

	// BlitzPro shader support
	//
	// std::string converted;
	// HRESULT hr = E_FAIL;
	// if (convertShaderSource(dir3dDev, filename, converted)) {
	// 	hr = D3DXCreateEffect(dir3dDev, converted.data(), (UINT)converted.size(),
	// 		nullptr, nullptr, 0, nullptr, &effect, &errors);
	// }
	// if (FAILED(hr)) {
	// 	if (errors) { errors->Release(); errors = nullptr; }
	// 	hr = D3DXCreateEffectFromFile(dir3dDev, filename.c_str(), nullptr, nullptr, 0, nullptr, &effect, &errors);
	// }

	HRESULT hr = D3DXCreateEffectFromFile(dir3dDev, filename.c_str(), nullptr, nullptr, 0, nullptr, &effect, &errors);
	if (FAILED(hr)) {
		if (errors) {
			lastEffectError = (const char*)errors->GetBufferPointer();
			errors->Release();
		}
		else {
			lastEffectError = "Unknown error creating effect";
		}
		return nullptr;
	}
	lastEffectError.clear();
	gxEffect* e = new gxEffect(this, effect);
	effect_set.insert(e);
	return e;
	*/
}

gxEffect* gxGraphics::verifyEffect(gxEffect* e) {
	return effect_set.count(e) ? e : nullptr;
}

void gxGraphics::freeEffect(gxEffect* e) {
	if (effect_set.erase(e)) delete e;
}

void gxGraphics::clearEffects() {
	while (effect_set.size()) freeEffect(*effect_set.begin());
}

void gxGraphics::setGamma(int r, int g, int b, float dr, float dg, float db) {
	//bruh
}

void gxGraphics::updateGamma(bool calibrate) {
	//bruh
}

void gxGraphics::getGamma(int r, int g, int b, float* dr, float* dg, float* db) {
	//bruh
}

bool gxGraphics::ensureD3DBegun() {
	if (d3dSceneOpen) return true;
	if (!dir3dDev) return false;
	if (dir3dDev->BeginScene() != D3D_OK) return false;
	d3dSceneOpen = true;
	return true;
}

void gxGraphics::endD3DScene() {
	if (!d3dSceneOpen) return;
	d3dSceneOpen = false;
	if (dir3dDev) dir3dDev->EndScene();
}

bool gxGraphics::restore() {
	if (!dir3dDev) return true;

	HRESULT hr = dir3dDev->CheckDeviceState(runtime->hwnd);
	if (hr == D3DERR_DEVICELOST || hr == D3DERR_DEVICEHUNG || hr == D3DERR_DEVICEREMOVED) return false;

	if (hr == D3DERR_DEVICENOTRESET || hr == S_PRESENT_MODE_CHANGED) {
		if (present_params.Windowed && !runtime->antialiasRequested()) {
			present_params.Flags |= D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
		}

		runtime->applyAntialiasToParams(present_params);

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

		bool wasAliased = front_canvas && back_canvas && front_canvas->surf == back_canvas->surf;

		if (back_canvas && back_canvas->surf) {
			back_canvas->surf->Release();
			back_canvas->surf = newBack;
			newBack->AddRef();
		}
		if (front_canvas && wasAliased) {
			front_canvas->surf->Release();
			front_canvas->surf = newBack;
			newBack->AddRef();
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

bool gxGraphics::changeDisplayMode(int width, int height, bool fullscreen, bool borderless) {
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

	auto updateCanvas = [&](gxCanvas* canvas) {
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

bool gxGraphics::setDarkMode(bool mode) {
	if (!runtime) return false;
	HWND hwnd = runtime->hwnd;
	if (!hwnd || !IsWindow(hwnd)) return false;

	BOOL DARK_MODE = mode ? TRUE : FALSE;
	if (DwmSetWindowAttribute(hwnd, 20, &DARK_MODE, sizeof(DARK_MODE)) != S_OK) return false;
	if (DwmSetWindowAttribute(hwnd, 19, &DARK_MODE, sizeof(DARK_MODE)) != S_OK) return false;
	return true;
}

gxCanvas* gxGraphics::getFrontCanvas()const {
	return front_canvas;
}

gxCanvas* gxGraphics::getBackCanvas()const {
	return back_canvas;
}

gxFont* gxGraphics::getDefaultFont()const {
	return def_font;
}

void gxGraphics::vwait() { // stubby stbu stub
	// dirDraw->WaitForVerticalBlank(DDWAITVB_BLOCKBEGIN, 0);
}

gxGraphics::DeviceState gxGraphics::getDeviceState() {
	if (!dir3dDev) return DEVICE_OK;
	HRESULT hr = dir3dDev->CheckDeviceState(runtime->hwnd);
	if (hr == D3DERR_DEVICELOST || hr == D3DERR_DEVICEHUNG || hr == D3DERR_DEVICEREMOVED) return DEVICE_LOST;
	if (hr == D3DERR_DEVICENOTRESET || hr == S_PRESENT_MODE_CHANGED) return DEVICE_NEEDS_RESET;
	return DEVICE_OK;
}

void gxGraphics::flip(bool vwait) {
	if (runtime) runtime->flip(vwait);
}

bool gxGraphics::copySceneToTexture(gxCanvas* dest, int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh) {
	if (!runtime || !runtime->sdlGpu) return false;
	SDL_GPUDevice* dev = (SDL_GPUDevice*)runtime->sdlGpu;
	for (gxScene* scene : scene_set) {
		if (scene && scene->blitFrameToTexture(dev, dest, dx, dy, dw, dh, sx, sy, sw, sh)) return true;
	}
	return false;
}

void gxGraphics::copy(gxCanvas* dest, int dx, int dy, int dw, int dh, gxCanvas* src, int sx, int sy, int sw, int sh) {
	if (dest->getSurface() && src->getSurface()) {
		ddUtil::copy(dir3dDev, dest->getSurface(), dx, dy, dw, dh, src->getSurface(), sx, sy, sw, sh);
		RECT r = { dx, dy, dx + dw, dy + dh };
		dest->damageD3D(r);
		return;
	}
	if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
	if (runtime && runtime->sdlGpu && src == getBackCanvas() && (dest->getFlags() & gxCanvas::CANVAS_TEXTURE)) {
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

int gxGraphics::getScanLine() const { return 0; }

int gxGraphics::getAvailVidmem() const { return 0; }

int gxGraphics::getTotalVidmem() const { return 0; }

gxMovie* gxGraphics::openMovie(const std::string& file, int flags) {
	gxMovie* movie = new gxMovie(this, file);
	if (!movie->isValid()) {
		delete movie;
		return nullptr;
	}
	movie_set.insert(movie);
	return movie;
}

gxMovie* gxGraphics::verifyMovie(gxMovie* m) {
	return movie_set.count(m) ? m : 0;
}

void gxGraphics::closeMovie(gxMovie* m) {
	if (movie_set.erase(m)) delete m;
}

static gxCanvas* buildCpuCanvas(gxGraphics* g, const DecodedImage& img, int flags, bool keepPixels) {
	if (img.w <= 0 || img.h <= 0 || img.rgba.empty()) return nullptr;
	if ((size_t)img.w * (size_t)img.h * 4 != img.rgba.size()) return nullptr;
	gxCanvas* c = nullptr;
	try { c = new gxCanvas(g, img.w, img.h, flags); }
	catch (...) { return nullptr; }

	bool seeded = false;
	if (!keepPixels && g->runtime && g->runtime->sdlGpu)
		seeded = sdlgpu::SeedCanvasTexture((SDL_GPUDevice*)g->runtime->sdlGpu, c,
			(unsigned)img.w, (unsigned)img.h, img.rgba.data());

	if (keepPixels || !seeded) {
		if (!c->lock()) { delete c; return nullptr; }
		bool hasMask = (flags & gxCanvas::CANVAS_TEX_MASK) != 0;
		bool hasAlpha = (flags & gxCanvas::CANVAS_TEX_ALPHA) != 0;
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

gxCanvas* gxGraphics::createCanvas(int w, int h, int flags) {
	if (w <= 0 || h <= 0) return nullptr;
	if (runtime && runtime->sdlGpu) {
		gxCanvas* c = nullptr;
		try { c = new gxCanvas(this, w, h, flags); }
		catch (...) { return nullptr; }
		if (!c->lock()) { delete c; return nullptr; }
		c->unlock();
		canvas_set.insert(c);
		c->cls();
		c->cpu_keep = false;
		return c;
	}
	if (flags & gxCanvas::CANVAS_TEX_CUBE) {
		int size = w > h ? w : h;
		IDirect3DCubeTexture9* cubeTex = ddUtil::createCubeTextureSurface(size, flags, this);
		if (!cubeTex) return nullptr;
		gxCanvas* c = new gxCanvas(this, cubeTex, flags);
		canvas_set.insert(c);
		c->cls();
		return c;
	}
	if (flags & gxCanvas::CANVAS_TEXTURE) {
		IDirect3DTexture9* tex = ddUtil::createTextureSurface(w, h, flags, this, true);
		if (!tex) return nullptr;
		gxCanvas* c = new gxCanvas(this, tex, flags);
		canvas_set.insert(c);
		c->cls();
		return c;
	}
    IDirect3DSurface9* surf = ddUtil::createDisplaySurface(w, h, flags, this);
	if (!surf) return nullptr;
	gxCanvas* c = new gxCanvas(this, surf, flags);
	canvas_set.insert(c);
	c->cls();
	return c;
}

gxCanvas* gxGraphics::loadCanvas(const std::string& f, int flags) {
	if (runtime && runtime->sdlGpu) {
		auto img = DecodeImageFile(f);
		if (!img) return nullptr;
		if (!(flags & gxCanvas::CANVAS_TEXTURE)) {
			if (img->hasAlpha) flags |= gxCanvas::CANVAS_TEXTURE | gxCanvas::CANVAS_TEX_ALPHA;
		}
		if ((flags & gxCanvas::CANVAS_TEX_MASK) && !(flags & gxCanvas::CANVAS_TEX_ALPHA))
			flags |= gxCanvas::CANVAS_TEX_ALPHA;
		gxCanvas* c = buildCpuCanvas(this, *img, flags, true);
		if (c) canvas_set.insert(c);
		return c;
	}
	if (!(flags & gxCanvas::CANVAS_TEXTURE)) {
		if (ddUtil::hasActualAlpha(f)) {
			flags |= gxCanvas::CANVAS_TEXTURE | gxCanvas::CANVAS_TEX_ALPHA;
		}
	}
	if (flags & gxCanvas::CANVAS_TEXTURE) {
		int srcW = 0, srcH = 0;
		IDirect3DTexture9* tex = ddUtil::loadTextureSurface(f, flags, this, true, &srcW, &srcH);
		if (!tex) return nullptr;
		gxCanvas* c = new gxCanvas(this, tex, flags);
		if (srcW > 0 && srcH > 0) c->setLogicalSize(srcW, srcH);
		canvas_set.insert(c);
		return c;
	}
	IDirect3DSurface9* surf = ddUtil::loadDisplaySurface(f, flags, this);
	if (!surf) return nullptr;
	gxCanvas* c = new gxCanvas(this, surf, flags);
	canvas_set.insert(c);
	return c;
}

gxCanvas* gxGraphics::createCanvasFromImage(const DecodedImage* img, int flags, bool keepPixels) {
	if (!img) return nullptr;
	if ((flags & gxCanvas::CANVAS_TEX_MASK) && !(flags & gxCanvas::CANVAS_TEX_ALPHA)) {
		flags |= gxCanvas::CANVAS_TEX_ALPHA;
	}
	if (runtime && runtime->sdlGpu) {
		gxCanvas* c = buildCpuCanvas(this, *img, flags, keepPixels);
		if (c) canvas_set.insert(c);
		return c;
	}
	int w = 0, h = 0;
	IDirect3DTexture9* tex = ddUtil::textureFromDecoded(img, flags, this, true, &w, &h);
	if (!tex) return nullptr;
	gxCanvas* c = new gxCanvas(this, tex, flags);
	if (w > 0 && h > 0) c->setLogicalSize(w, h);
	canvas_set.insert(c);
	return c;
}

gxCanvas* gxGraphics::verifyCanvas(gxCanvas* c) {
	return canvas_set.count(c) || c == front_canvas || c == back_canvas ? c : 0;
}

void gxGraphics::freeCanvas(gxCanvas* c) {
	if (canvas_set.erase(c)) delete c;
}

int gxGraphics::getWidth()const {
	return front_canvas->getWidth();
}

int gxGraphics::getHeight()const {
	return front_canvas->getHeight();
}

int gxGraphics::getDepth()const {
	return front_canvas->getDepth();
}

gxFont* gxGraphics::loadFont(std::string f, int height, bool bold, bool italic, bool underlined) {
	std::string t;
	int n = f.find('.');
	if (n == std::string::npos) {
		t = fullfilename(f);
		if (!font_res.count(t) && AddFontResource(t.c_str())) font_res.insert(t);
		t = filenamefile(f.substr(0, n));
	}
	else {
		t = f;
	}

	gxFont* newFont = new gxFont(this, f, height, bold, italic, underlined);
	font_set.emplace(newFont);
	return newFont;
}

gxFont* gxGraphics::verifyFont(gxFont* f) {
	return font_set.count(f) ? f : 0;
}

void gxGraphics::freeFont(gxFont* f) {
	if (font_set.erase(f)) delete f;
}

//////////////
// 3D STUFF //
//////////////

gxScene* gxGraphics::createScene(int flags) {
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

	gxScene* scene = new gxScene(this, back_canvas);
	scene_set.insert(scene);
	return scene;
}

gxScene* gxGraphics::verifyScene(gxScene* s) { return scene_set.count(s) ? s : 0; }

void gxGraphics::freeScene(gxScene* scene) {
	if (!scene_set.erase(scene)) return;
	dummy_mesh = 0;
	while (mesh_set.size()) freeMesh(*mesh_set.begin());
	back_canvas->releaseZBuffer();
	delete scene;
}

bool gxGraphics::presentSceneSDL(struct SDL_GPUDevice* dev, struct SDL_Window* win) {
	if (!dev || !win) return false;
	for (gxScene* scene : scene_set) {
		if (scene && scene->hasGpuImage()) {
			if (scene->presentGpuFrame(dev, win)) return true;
		}
	}
	return false;
}

bool gxGraphics::presentSceneWithCanvas(struct SDL_GPUDevice* dev, struct SDL_Window* win, gxCanvas* canvas) {
	if (!dev || !win) return false;
	for (gxScene* scene : scene_set) {
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

void gxGraphics::setActiveCanvas(gxCanvas* canvas) {
	if (!runtime || !runtime->sdlGpu) return;
	sdlgpu::SetActiveCanvasTarget((SDL_GPUDevice*)runtime->sdlGpu, canvas);
}

void gxGraphics::adoptCanvas(gxCanvas* c) {
	canvas_set.insert(c);
}

gxMesh* gxGraphics::createMesh(int max_verts, int max_tris, int flags) {
	gxMesh* mesh = new gxMesh(this, max_verts, max_tris, flags);
	mesh_set.insert(mesh);
	return mesh;
}

gxMesh* gxGraphics::verifyMesh(gxMesh* m) {
	return mesh_set.count(m) ? m : 0;
}

void gxGraphics::freeMesh(gxMesh* mesh) {
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

bool gxGraphics::skinningSupported() {
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

bool gxGraphics::ensureSkinningShader() {
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

#if GX_USE_LEGACY_D3DX
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
