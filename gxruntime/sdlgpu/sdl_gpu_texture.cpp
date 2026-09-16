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
	bool rt = false;
} ;

static std::unordered_map< ::gxCanvas*, CanvasTexEntry> g_canvasTexMap;
static std::unordered_map< ::gxCanvas*, CanvasTexEntry> g_canvasOverlayMap;

static struct CanvasDepthEntry {
	SDL_GPUTexture* tex = nullptr;
	SDL_GPUDevice* dev = nullptr;
	unsigned w = 0, h = 0;
	int format = 0;
};

static std::unordered_map< ::gxCanvas*, CanvasDepthEntry> g_canvasDepthMap;

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
	for (auto it = g_canvasDepthMap.begin(); it != g_canvasDepthMap.end(); ) {
		if (!dev || it->second.dev == dev) {
			if (it->second.tex) SDL_ReleaseGPUTexture(it->second.dev, it->second.tex);
			it = g_canvasDepthMap.erase(it);
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
	auto dt = g_canvasDepthMap.find(canvas);
	if (dt != g_canvasDepthMap.end()) {
		if (dt->second.tex && dt->second.dev) SDL_ReleaseGPUTexture(dt->second.dev, dt->second.tex);
		g_canvasDepthMap.erase(dt);
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

static SDL_GPUTexture* CreateCubeTexture(SDL_GPUDevice* dev, unsigned size) {
	if (!dev || !size) return nullptr;
	if (!SDL_GPUTextureSupportsFormat(dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTURETYPE_CUBE,
		(SDL_GPUTextureUsageFlags)(SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER))) return nullptr;
	SDL_GPUTextureCreateInfo info{};
	info.type = SDL_GPU_TEXTURETYPE_CUBE;
	info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
	info.width = size;
	info.height = size;
	info.layer_count_or_depth = 6;
	info.num_levels = 1;
	info.sample_count = SDL_GPU_SAMPLECOUNT_1;
	return SDL_CreateGPUTexture(dev, &info);
}

static bool UploadCubePixels(SDL_GPUDevice* dev, ::gxCanvas* canvas, SDL_GPUTexture* tex, unsigned w, unsigned h) {
	if (!dev || !canvas || !tex) return false;
	const unsigned char* bits = canvas->cpu_bits;
	int pitch = canvas->cpu_pitch;
	if (!bits || pitch <= 0 || (unsigned)canvas->cpu_h != h) return false;
	size_t faceBytes = (size_t)pitch * (size_t)h;
	SDL_GPUCommandBuffer* cmds = SDL_AcquireGPUCommandBuffer(dev);
	if (!cmds) return false;
	SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmds);
	if (!cp) { SDL_CancelGPUCommandBuffer(cmds); return false; }
	static thread_local std::vector<unsigned char> rgba;
	try { rgba.resize((size_t)w * (size_t)h * 4); }
	catch (...) { SDL_EndGPUCopyPass(cp); SDL_CancelGPUCommandBuffer(cmds); return false; }
	for (unsigned face = 0; face < 6; ++face) {
		const unsigned char* plane = bits + faceBytes * face;
		for (unsigned y = 0; y < h; ++y) {
			for (unsigned x = 0; x < w; ++x) {
				unsigned argb = canvas->format.getPixel(const_cast<unsigned char*>(plane) + (size_t)y * pitch + (size_t)x * canvas->format.getPitch());
				unsigned char* dst = &rgba[((size_t)y * w + x) * 4];
				dst[0] = (argb >> 16) & 0xff;
				dst[1] = (argb >> 8) & 0xff;
				dst[2] = argb & 0xff;
				dst[3] = (argb >> 24) & 0xff;
			}
		}
		SDL_GPUTransferBuffer* tb = AcquireUploadTransferBuffer(dev, (Uint32)rgba.size());
		if (!tb) { SDL_EndGPUCopyPass(cp); SDL_CancelGPUCommandBuffer(cmds); return false; }
		void* mapped = SDL_MapGPUTransferBuffer(dev, tb, true);
		if (!mapped) { ReleaseUploadTransferBuffer(dev, tb); SDL_EndGPUCopyPass(cp); SDL_CancelGPUCommandBuffer(cmds); return false; }
		memcpy(mapped, rgba.data(), rgba.size());
		SDL_UnmapGPUTransferBuffer(dev, tb);
		SDL_GPUTextureTransferInfo src{};
		src.transfer_buffer = tb;
		src.pixels_per_row = w;
		src.rows_per_layer = h;
		SDL_GPUTextureRegion reg{};
		reg.texture = tex;
		reg.mip_level = 0;
		reg.layer = face;
		reg.x = 0; reg.y = 0; reg.z = 0;
		reg.w = w; reg.h = h; reg.d = 1;
		SDL_UploadToGPUTexture(cp, &src, &reg, false);
		ReleaseUploadTransferBuffer(dev, tb);
	}
	SDL_EndGPUCopyPass(cp);
	return SDL_SubmitGPUCommandBuffer(cmds);
}

SDL_GPUTexture* EnsureCanvasCubeTexture(SDL_GPUDevice* dev, ::gxCanvas* canvas) {
	GpuLock lock;
	if (!dev || !canvas) return nullptr;
	unsigned w = (unsigned)canvas->getWidth();
	unsigned h = (unsigned)canvas->getHeight();
	if (!w || !h) return nullptr;
	auto it = g_canvasTexMap.find(canvas);
	if (it != g_canvasTexMap.end() && it->second.tex && it->second.rt && it->second.dev == dev && it->second.w == w && it->second.h == h)
		return it->second.tex;
	SDL_GPUTexture* old = nullptr;
	SDL_GPUDevice* oldDev = dev;
	if (it != g_canvasTexMap.end()) { old = it->second.tex; oldDev = it->second.dev; g_canvasTexMap.erase(it); }
	if (old) RetireTexture(oldDev, old);
	SDL_GPUTexture* tex = CreateCubeTexture(dev, w);
	if (!tex) return nullptr;
	UploadCubePixels(dev, canvas, tex, w, h);
	CanvasTexEntry e;
	e.tex = tex; e.dev = dev; e.modCnt = canvas->getModify(); e.w = w; e.h = h; e.rt = true;
	g_canvasTexMap[canvas] = e;
	return tex;
}

SDL_GPUTexture* GetCanvasTexture(SDL_GPUDevice* dev, ::gxCanvas* canvas) {
	GpuLock lock;
	if (!dev || !canvas) return nullptr;
	if (canvas->getFlags() & ::gxCanvas::CANVAS_TEX_CUBE) return EnsureCanvasCubeTexture(dev, canvas);
	unsigned w = (unsigned)canvas->getWidth();
	unsigned h = (unsigned)canvas->getHeight();
	if (!w || !h) return nullptr;
	int mod = canvas->getModify();
	auto it = g_canvasTexMap.find(canvas);
	if (it != g_canvasTexMap.end() && it->second.tex && it->second.rt && it->second.dev == dev && it->second.w == w && it->second.h == h) {
		return it->second.tex;
	}
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

static SDL_GPUTexture* CreateRenderTarget2D(SDL_GPUDevice* dev, unsigned w, unsigned h) {
	if (!dev || !w || !h) return nullptr;
	if (!SDL_GPUTextureSupportsFormat(dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTURETYPE_2D,
		(SDL_GPUTextureUsageFlags)(SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER))) return nullptr;
	SDL_GPUTextureCreateInfo info{};
	info.type = SDL_GPU_TEXTURETYPE_2D;
	info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
	info.width = w;
	info.height = h;
	info.layer_count_or_depth = 1;
	info.num_levels = 1;
	info.sample_count = SDL_GPU_SAMPLECOUNT_1;
	return SDL_CreateGPUTexture(dev, &info);
}

static bool UploadCanvasPixels(SDL_GPUDevice* dev, ::gxCanvas* canvas, SDL_GPUTexture* tex, unsigned w, unsigned h) {
	if (!canvas->lockRO()) return false;
	static thread_local std::vector<unsigned char> rgba;
	try { rgba.resize((size_t)w * h * 4); }
	catch (...) { canvas->unlock(); return false; }
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
	return UploadTextureRGBA(dev, tex, w, h, rgba.data(), false);
}

SDL_GPUTexture* EnsureCanvasRenderTarget(SDL_GPUDevice* dev, ::gxCanvas* canvas) {
	GpuLock lock;
	if (!dev || !canvas) return nullptr;
	unsigned w = (unsigned)canvas->getWidth();
	unsigned h = (unsigned)canvas->getHeight();
	if (!w || !h) return nullptr;
	auto it = g_canvasTexMap.find(canvas);
	if (it != g_canvasTexMap.end() && it->second.tex && it->second.rt && it->second.dev == dev && it->second.w == w && it->second.h == h)
		return it->second.tex;
	SDL_GPUTexture* old = nullptr;
	SDL_GPUDevice* oldDev = dev;
	if (it != g_canvasTexMap.end()) { old = it->second.tex; oldDev = it->second.dev; g_canvasTexMap.erase(it); }
	if (old) RetireTexture(oldDev, old);
	SDL_GPUTexture* tex = CreateRenderTarget2D(dev, w, h);
	if (!tex) return nullptr;
	UploadCanvasPixels(dev, canvas, tex, w, h);
	CanvasTexEntry e;
	e.tex = tex; e.dev = dev; e.modCnt = canvas->getModify(); e.w = w; e.h = h; e.rt = true;
	g_canvasTexMap[canvas] = e;
	return tex;
}

SDL_GPUTexture* EnsureCanvasDepthTarget(SDL_GPUDevice* dev, ::gxCanvas* canvas, unsigned w, unsigned h) {
	GpuLock lock;
	if (!dev || !canvas || !w || !h) return nullptr;
	int fmt = MeshDepthFormat(dev);
	auto it = g_canvasDepthMap.find(canvas);
	if (it != g_canvasDepthMap.end() && it->second.tex && it->second.dev == dev && it->second.w == w && it->second.h == h && it->second.format == fmt)
		return it->second.tex;
	if (it != g_canvasDepthMap.end()) {
		if (it->second.tex && it->second.dev) SDL_ReleaseGPUTexture(it->second.dev, it->second.tex);
		g_canvasDepthMap.erase(it);
	}
	SDL_GPUTexture* tex = CreateDepthTarget(dev, w, h, fmt);
	if (!tex) return nullptr;
	CanvasDepthEntry e;
	e.tex = tex; e.dev = dev; e.w = w; e.h = h; e.format = fmt;
	g_canvasDepthMap[canvas] = e;
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
	unsigned ux = 0, uy = 0, uw = w, uh = h;
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
