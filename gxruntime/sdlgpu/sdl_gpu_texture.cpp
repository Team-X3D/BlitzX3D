#include "sdl_gpu_texture.h"
#include "sdl_gpu_lock.h"
#include "sdl_gpu_text.h"

#include "../std.h"
#include "../gxcanvas.h"
#include "sdl_gpu_pipeline.h"

#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_properties.h>

namespace sdlgpu {

static struct CanvasTexEntry {
	SDL_GPUTexture* tex = nullptr;
	SDL_GPUDevice* dev = nullptr;
	int modCnt = -1;
	unsigned w = 0, h = 0;
} ;

static std::unordered_map< ::gxCanvas*, CanvasTexEntry> g_canvasTexMap;
static std::unordered_map< ::gxCanvas*, CanvasTexEntry> g_canvasOverlayMap;

static void RetireTexture(SDL_GPUDevice* dev, SDL_GPUTexture* tex) {
	if (!dev || !tex) return;
	InvalidatePendingTexture(tex);
	SDL_ReleaseGPUTexture(dev, tex);
}

void TeardownTexturePools(SDL_GPUDevice* dev) {
	GpuLock lock;
	for (auto it = g_canvasTexMap.begin(); it != g_canvasTexMap.end(); ) {
		if (!dev || it->second.dev == dev) {
			if (it->second.tex) {
				InvalidatePendingTexture(it->second.tex);
				SDL_ReleaseGPUTexture(it->second.dev, it->second.tex);
			}
			it = g_canvasTexMap.erase(it);
		} else ++it;
	}
	for (auto it = g_canvasOverlayMap.begin(); it != g_canvasOverlayMap.end(); ) {
		if (!dev || it->second.dev == dev) {
			if (it->second.tex) {
				InvalidatePendingTexture(it->second.tex);
				SDL_ReleaseGPUTexture(it->second.dev, it->second.tex);
			}
			it = g_canvasOverlayMap.erase(it);
		} else ++it;
	}
}

void InvalidateCanvasTextures(::gxCanvas* canvas) {
	GpuLock lock;
	if (!canvas) return;
	auto it = g_canvasTexMap.find(canvas);
	if (it != g_canvasTexMap.end()) {
		if (it->second.tex) RetireTexture(it->second.dev, it->second.tex);
		g_canvasTexMap.erase(it);
	}
	auto jt = g_canvasOverlayMap.find(canvas);
	if (jt != g_canvasOverlayMap.end()) {
		if (jt->second.tex) RetireTexture(jt->second.dev, jt->second.tex);
		g_canvasOverlayMap.erase(jt);
	}
}

bool SeedCanvasTexture(SDL_GPUDevice* dev, ::gxCanvas* canvas, unsigned w, unsigned h, const void* rgba) {
	GpuLock lock;
	if (!dev || !canvas || !w || !h || !rgba) return false;
	if ((unsigned)canvas->getWidth() != w || (unsigned)canvas->getHeight() != h) return false;
	auto it = g_canvasTexMap.find(canvas);
	if (it != g_canvasTexMap.end() && it->second.tex && it->second.dev == dev) return true;
	SDL_GPUTexture* old = nullptr;
	SDL_GPUDevice* oldDev = dev;
	if (it != g_canvasTexMap.end()) { old = it->second.tex; oldDev = it->second.dev; g_canvasTexMap.erase(it); }
	if (old) RetireTexture(oldDev, old);
	SDL_GPUTexture* tex = CreateTexture2D(dev, w, h);
	if (!tex) return false;
	if (!UploadTextureRGBA(dev, tex, w, h, rgba, false)) {
		SDL_ReleaseGPUTexture(dev, tex);
		return false;
	}
	CanvasTexEntry e;
	e.tex = tex; e.dev = dev; e.modCnt = canvas->getModify(); e.w = w; e.h = h;
	g_canvasTexMap[canvas] = e;
	return true;
}

SDL_GPUTexture* GetCanvasTexture(SDL_GPUDevice* dev, ::gxCanvas* canvas) {
	GpuLock lock;
	if (!dev || !canvas) return nullptr;
	unsigned w = (unsigned)canvas->getWidth();
	unsigned h = (unsigned)canvas->getHeight();
	if (!w || !h) return nullptr;
	int mod = canvas->getModify();
	auto it = g_canvasTexMap.find(canvas);
	if (it != g_canvasTexMap.end() && it->second.tex && it->second.dev == dev && it->second.modCnt == mod && it->second.w == w && it->second.h == h) {
		return it->second.tex;
	}
	SDL_GPUTexture* tex = nullptr;
	bool isNew = false;
	if (it != g_canvasTexMap.end() && it->second.tex && it->second.dev == dev && it->second.w == w && it->second.h == h) {
		tex = it->second.tex;
	}
	else {
		SDL_GPUTexture* old = nullptr;
		SDL_GPUDevice* oldDev = dev;
		if (it != g_canvasTexMap.end()) { old = it->second.tex; oldDev = it->second.dev; g_canvasTexMap.erase(it); }
		if (old) RetireTexture(oldDev, old);
		tex = CreateTexture2D(dev, w, h);
		if (!tex) return nullptr;
		isNew = true;
	}

	static thread_local std::vector<unsigned char> rgba;
	try {
		rgba.resize((size_t)w * h * 4);
	} catch (...) { if (isNew && tex) SDL_ReleaseGPUTexture(dev, tex); return nullptr; }

	if (!canvas->lockRO()) { if (isNew && tex) SDL_ReleaseGPUTexture(dev, tex); return nullptr; }
	for (unsigned y = 0; y < h; ++y) {
		for (unsigned x = 0; x < w; ++x) {
			unsigned argb = canvas->getPixelFast((int)x, (int)y);
			unsigned char* dst = &rgba[(size_t)(y * w + x) * 4];
			dst[0] = (argb >> 16) & 0xff;
			dst[1] = (argb >> 8) & 0xff;
			dst[2] = argb & 0xff;
			dst[3] = (argb >> 24) & 0xff;
		}
	}
	canvas->unlock();

	if (!UploadTextureRGBA(dev, tex, w, h, rgba.data(), !isNew)) {
		if (isNew && tex) SDL_ReleaseGPUTexture(dev, tex);
		return nullptr;
	}
	CanvasTexEntry e;
	e.tex = tex; e.dev = dev; e.modCnt = mod; e.w = w; e.h = h;
	g_canvasTexMap[canvas] = e;
	return tex;
}

SDL_GPUTexture* GetCanvasOverlayTexture(SDL_GPUDevice* dev, ::gxCanvas* canvas) {
	GpuLock lock;
	if (!dev || !canvas) return nullptr;
	unsigned w = (unsigned)canvas->getWidth();
	unsigned h = (unsigned)canvas->getHeight();
	if (!w || !h) return nullptr;
	int mod = canvas->getModify();
	auto it = g_canvasOverlayMap.find(canvas);
	if (it != g_canvasOverlayMap.end() && it->second.tex && it->second.dev == dev && it->second.modCnt == mod && it->second.w == w && it->second.h == h) {
		return it->second.tex;
	}
	SDL_GPUTexture* tex = nullptr;
	bool isNew = false;
	if (it != g_canvasOverlayMap.end() && it->second.tex && it->second.dev == dev && it->second.w == w && it->second.h == h) {
		tex = it->second.tex;
	}
	else {
		SDL_GPUTexture* old = nullptr;
		SDL_GPUDevice* oldDev = dev;
		if (it != g_canvasOverlayMap.end()) { old = it->second.tex; oldDev = it->second.dev; g_canvasOverlayMap.erase(it); }
		if (old) RetireTexture(oldDev, old);
		tex = CreateTexture2D(dev, w, h);
		if (!tex) return nullptr;
		isNew = true;
	}
	static thread_local std::vector<unsigned char> rgba;
	try {
		rgba.resize((size_t)w * h * 4);
	} catch (...) { if (isNew && tex) SDL_ReleaseGPUTexture(dev, tex); return nullptr; }
	if (!canvas->lockRO()) { if (isNew && tex) SDL_ReleaseGPUTexture(dev, tex); return nullptr; }
	unsigned clsRgb = canvas->getClsColor() & 0x00ffffff;
	for (unsigned y = 0; y < h; ++y) {
		for (unsigned x = 0; x < w; ++x) {
			unsigned argb = canvas->getPixelFast((int)x, (int)y);
			unsigned rgb = argb & 0x00ffffff;
			unsigned a = (rgb == clsRgb) ? 0 : 255;
			unsigned char* dst = &rgba[(size_t)(y * w + x) * 4];
			dst[0] = (argb >> 16) & 0xff;
			dst[1] = (argb >> 8) & 0xff;
			dst[2] = argb & 0xff;
			dst[3] = (unsigned char)a;
		}
	}
	canvas->unlock();
	if (!UploadTextureRGBA(dev, tex, w, h, rgba.data(), !isNew)) { if (isNew && tex) SDL_ReleaseGPUTexture(dev, tex); return nullptr; }
	CanvasTexEntry e; e.tex = tex; e.dev = dev; e.modCnt = mod; e.w = w; e.h = h;
	g_canvasOverlayMap[canvas] = e;
	return tex;
}

SDL_GPUTexture* GetCanvasOverlayTextureBatched(SDL_GPUDevice* dev, ::gxCanvas* canvas, SDL_GPUCommandBuffer* cmds, bool* didUpload) {
	GpuLock lock;
	if (didUpload) *didUpload = false;
	if (!dev || !canvas) return nullptr;
	unsigned w = (unsigned)canvas->getWidth();
	unsigned h = (unsigned)canvas->getHeight();
	if (!w || !h) return nullptr;
	int mod = canvas->getModify();
	auto it = g_canvasOverlayMap.find(canvas);
	if (it != g_canvasOverlayMap.end() && it->second.tex && it->second.dev == dev && it->second.modCnt == mod && it->second.w == w && it->second.h == h) {
		return it->second.tex;
	}
	if (!cmds) return GetCanvasOverlayTexture(dev, canvas);
	SDL_GPUTexture* tex = nullptr;
	bool isNew = false;
	if (it != g_canvasOverlayMap.end() && it->second.tex && it->second.dev == dev && it->second.w == w && it->second.h == h) {
		tex = it->second.tex;
	} else {
		SDL_GPUTexture* old = nullptr;
		SDL_GPUDevice* oldDev = dev;
		if (it != g_canvasOverlayMap.end()) { old = it->second.tex; oldDev = it->second.dev; g_canvasOverlayMap.erase(it); }
		if (old) RetireTexture(oldDev, old);
		tex = CreateTexture2D(dev, w, h);
		if (!tex) return nullptr;
		isNew = true;
	}
	RECT dr{};
	bool hasDirty = !isNew && canvas->getSDLDirtyRect(dr);
	unsigned ux = 0, uy = 0, uw = w, uh = h;
	if (hasDirty) {
		int x0 = dr.left < 0 ? 0 : dr.left, y0 = dr.top < 0 ? 0 : dr.top;
		int x1 = dr.right > (int)w ? (int)w : dr.right, y1 = dr.bottom > (int)h ? (int)h : dr.bottom;
		if (x1 <= x0 || y1 <= y0) hasDirty = false;
		else { ux = (unsigned)x0; uy = (unsigned)y0; uw = (unsigned)(x1 - x0); uh = (unsigned)(y1 - y0); }
	}
	static thread_local std::vector<unsigned char> rgba;
	try {
		rgba.resize((size_t)uw * uh * 4);
	} catch (...) { if (isNew && tex) SDL_ReleaseGPUTexture(dev, tex); return nullptr; }
	if (!canvas->lockRO()) { if (isNew && tex) SDL_ReleaseGPUTexture(dev, tex); return nullptr; }
	unsigned clsRgb = canvas->getClsColor() & 0x00ffffff;
	for (unsigned y = 0; y < uh; ++y) {
		for (unsigned x = 0; x < uw; ++x) {
			unsigned argb = canvas->getPixelFast((int)(ux + x), (int)(uy + y));
			unsigned char* dst = &rgba[(size_t)(y * uw + x) * 4];
			dst[0] = (argb >> 16) & 0xff;
			dst[1] = (argb >> 8) & 0xff;
			dst[2] = argb & 0xff;
			dst[3] = ((argb & 0x00ffffff) == clsRgb) ? 0 : 255;
		}
	}
	canvas->unlock();
	Uint32 size = uw * uh * 4;
	SDL_GPUTransferBuffer* buf = AcquireUploadTransferBuffer(dev, size);
	if (!buf) { if (isNew && tex) SDL_ReleaseGPUTexture(dev, tex); return nullptr; }
	void* dst = SDL_MapGPUTransferBuffer(dev, buf, true);
	if (!dst) { ReleaseUploadTransferBuffer(dev, buf); if (isNew && tex) SDL_ReleaseGPUTexture(dev, tex); return nullptr; }
	memcpy(dst, rgba.data(), size);
	SDL_UnmapGPUTransferBuffer(dev, buf);
	SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmds);
	if (!copy) { ReleaseUploadTransferBuffer(dev, buf); if (isNew && tex) SDL_ReleaseGPUTexture(dev, tex); return nullptr; }
	SDL_GPUTextureTransferInfo src{};
	src.transfer_buffer = buf;
	src.pixels_per_row = uw;
	src.rows_per_layer = uh;
	SDL_GPUTextureRegion reg{};
	reg.texture = tex;
	reg.x = ux;
	reg.y = uy;
	reg.w = uw;
	reg.h = uh;
	reg.d = 1;
	SDL_UploadToGPUTexture(copy, &src, &reg, !isNew);
	SDL_EndGPUCopyPass(copy);
	ReleaseUploadTransferBuffer(dev, buf);
	canvas->clearSDLDirty();
	if (didUpload) *didUpload = true;
	CanvasTexEntry e; e.tex = tex; e.dev = dev; e.modCnt = mod; e.w = w; e.h = h;
	g_canvasOverlayMap[canvas] = e;
	return tex;
}

SDL_GPUTexture* CreateTexture2D(SDL_GPUDevice* dev, unsigned w, unsigned h) {
	if (!dev || !w || !h) return nullptr;
	if (!SDL_GPUTextureSupportsFormat(dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_SAMPLER)) return nullptr;
	SDL_GPUTextureCreateInfo info{};
	info.type = SDL_GPU_TEXTURETYPE_2D;
	info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
	info.width = w;
	info.height = h;
	info.layer_count_or_depth = 1;
	info.num_levels = 1;
	info.sample_count = SDL_GPU_SAMPLECOUNT_1;
	return SDL_CreateGPUTexture(dev, &info);
}

bool UploadTextureRGBA(SDL_GPUDevice* dev, SDL_GPUTexture* tex, unsigned w, unsigned h, const void* px) {
	return UploadTextureRGBA(dev, tex, w, h, px, true);
}

bool UploadTextureRGBA(SDL_GPUDevice* dev, SDL_GPUTexture* tex, unsigned w, unsigned h, const void* px, bool cycle) {
	if (!dev || !tex || !w || !h || !px) return false;
	Uint32 size = w * h * 4;

	SDL_GPUTransferBuffer* buf = AcquireUploadTransferBuffer(dev, size);
	if (!buf) return false;

	void* dst = SDL_MapGPUTransferBuffer(dev, buf, true);
	if (!dst) { ReleaseUploadTransferBuffer(dev, buf); return false; }
	memcpy(dst, px, size);
	SDL_UnmapGPUTransferBuffer(dev, buf);

	SDL_GPUCommandBuffer* cmds = SDL_AcquireGPUCommandBuffer(dev);
	if (!cmds) { ReleaseUploadTransferBuffer(dev, buf); return false; }
	SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(cmds);

	SDL_GPUTextureTransferInfo src{};
	src.transfer_buffer = buf;
	src.pixels_per_row = w;
	src.rows_per_layer = h;

	SDL_GPUTextureRegion dstReg{};
	dstReg.texture = tex;
	dstReg.w = w;
	dstReg.h = h;
	dstReg.d = 1;

	SDL_UploadToGPUTexture(pass, &src, &dstReg, cycle);
	SDL_EndGPUCopyPass(pass);
	bool ok = SDL_SubmitGPUCommandBuffer(cmds);
	ReleaseUploadTransferBuffer(dev, buf);
	return ok;
}

void ReleaseTexture(SDL_GPUDevice* dev, SDL_GPUTexture* tex) {
	if (!dev || !tex) return;
	SDL_ReleaseGPUTexture(dev, tex);
}

SDL_GPUTexture* CreateColorTarget(SDL_GPUDevice* dev, unsigned w, unsigned h) {
	return CreateColorTarget(dev, w, h, 0.0f, 0.0f, 0.0f, 1.0f);
}

SDL_GPUTexture* CreateColorTarget(SDL_GPUDevice* dev, unsigned w, unsigned h, float r, float g, float b, float a) {
	if (!dev || !w || !h) return nullptr;
	if (!SDL_GPUTextureSupportsFormat(dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTURETYPE_2D, (SDL_GPUTextureUsageFlags)(SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER))) return nullptr;
	SDL_PropertiesID props = SDL_CreateProperties();
	if (props) {
		SDL_SetFloatProperty(props, SDL_PROP_GPU_TEXTURE_CREATE_D3D12_CLEAR_R_FLOAT, r);
		SDL_SetFloatProperty(props, SDL_PROP_GPU_TEXTURE_CREATE_D3D12_CLEAR_G_FLOAT, g);
		SDL_SetFloatProperty(props, SDL_PROP_GPU_TEXTURE_CREATE_D3D12_CLEAR_B_FLOAT, b);
		SDL_SetFloatProperty(props, SDL_PROP_GPU_TEXTURE_CREATE_D3D12_CLEAR_A_FLOAT, a);
	}
	SDL_GPUTextureCreateInfo info{};
	info.type = SDL_GPU_TEXTURETYPE_2D;
	info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
	info.width = w;
	info.height = h;
	info.layer_count_or_depth = 1;
	info.num_levels = 1;
	info.sample_count = SDL_GPU_SAMPLECOUNT_1;
	info.props = props;
	SDL_GPUTexture* tex = SDL_CreateGPUTexture(dev, &info);
	if (props) SDL_DestroyProperties(props);
	return tex;
}

SDL_GPUTexture* CreateDepthTarget(SDL_GPUDevice* dev, unsigned w, unsigned h, int formatValue) {
	return CreateDepthTarget(dev, w, h, formatValue, 1.0f, 0);
}

SDL_GPUTexture* CreateDepthTarget(SDL_GPUDevice* dev, unsigned w, unsigned h, int formatValue, float depth, unsigned char stencil) {
	if (!dev || !w || !h) return nullptr;
	SDL_GPUTextureFormat fmt = (SDL_GPUTextureFormat)formatValue;
	if (!SDL_GPUTextureSupportsFormat(dev, fmt, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)) return nullptr;
	SDL_PropertiesID props = SDL_CreateProperties();
	if (props) {
		SDL_SetFloatProperty(props, SDL_PROP_GPU_TEXTURE_CREATE_D3D12_CLEAR_DEPTH_FLOAT, depth);
		SDL_SetNumberProperty(props, SDL_PROP_GPU_TEXTURE_CREATE_D3D12_CLEAR_STENCIL_NUMBER, stencil);
	}
	SDL_GPUTextureCreateInfo info{};
	info.type = SDL_GPU_TEXTURETYPE_2D;
	info.format = fmt;
	info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
	info.width = w;
	info.height = h;
	info.layer_count_or_depth = 1;
	info.num_levels = 1;
	info.sample_count = SDL_GPU_SAMPLECOUNT_1;
	info.props = props;
	SDL_GPUTexture* tex = SDL_CreateGPUTexture(dev, &info);
	if (props) SDL_DestroyProperties(props);
	return tex;
}

}
