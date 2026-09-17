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
	bool mips = false;
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

static unsigned MipLevels(unsigned w, unsigned h) {
	unsigned size = w > h ? w : h;
	unsigned levels = 1;
	for (unsigned s = size; s > 1; s >>= 1) ++levels;
	return levels;
}

static bool UploadTextureRGBAOn(SDL_GPUDevice* dev, SDL_GPUCommandBuffer* cmds, SDL_GPUTexture* tex, unsigned w, unsigned h, const void* px, bool cycle) {
	if (!dev || !cmds || !tex || !w || !h || !px) return false;
	Uint32 size = w * h * 4;
	SDL_GPUTransferBuffer* buf = AcquireUploadTransferBuffer(dev, size);
	if (!buf) return false;
	void* dst = SDL_MapGPUTransferBuffer(dev, buf, true);
	if (!dst) { ReleaseUploadTransferBuffer(dev, buf); return false; }
	memcpy(dst, px, size);
	SDL_UnmapGPUTransferBuffer(dev, buf);
	SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(cmds);
	if (!pass) { ReleaseUploadTransferBuffer(dev, buf); return false; }
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
	ReleaseUploadTransferBuffer(dev, buf);
	return true;
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
	bool mips = (canvas->getFlags() & ::gxCanvas::CANVAS_TEX_MIPMAP) != 0;
	SDL_GPUTexture* tex = CreateTexture2D(dev, w, h, mips);
	if (!tex) return false;
	SDL_GPUCommandBuffer* cmds = SDL_AcquireGPUCommandBuffer(dev);
	if (!cmds) { SDL_ReleaseGPUTexture(dev, tex); return false; }
	if (!UploadTextureRGBAOn(dev, cmds, tex, w, h, rgba, false)) {
		SDL_CancelGPUCommandBuffer(cmds);
		SDL_ReleaseGPUTexture(dev, tex);
		return false;
	}
	if (mips && MipLevels(w, h) > 1) SDL_GenerateMipmapsForGPUTexture(cmds, tex);
	if (!SDL_SubmitGPUCommandBuffer(cmds)) {
		SDL_ReleaseGPUTexture(dev, tex);
		return false;
	}
	CanvasTexEntry e;
	e.tex = tex; e.dev = dev; e.modCnt = canvas->getModify(); e.w = w; e.h = h; e.mips = mips;
	g_canvasTexMap[canvas] = e;
	canvas->releaseCPUBitsIfUnused();
	return true;
}

bool DownloadCanvasTexture(SDL_GPUDevice* dev, ::gxCanvas* canvas) {
	GpuLock lock;
	if (!dev || !canvas || !canvas->cpu_bits) return false;
	unsigned w = (unsigned)canvas->getWidth();
	unsigned h = (unsigned)canvas->getHeight();
	if (!w || !h || (unsigned)canvas->cpu_h != h || canvas->cpu_pitch <= 0) return false;
	bool cube = (canvas->getFlags() & ::gxCanvas::CANVAS_TEX_CUBE) != 0;
	unsigned faces = cube ? 6 : 1;
	SDL_GPUTexture* tex = nullptr;
	auto it = g_canvasTexMap.find(canvas);
	if (it != g_canvasTexMap.end() && it->second.tex && it->second.dev == dev) tex = it->second.tex;
	if (!tex) {
		auto ot = g_canvasOverlayMap.find(canvas);
		if (ot != g_canvasOverlayMap.end() && ot->second.tex && ot->second.dev == dev) tex = ot->second.tex;
	}
	if (!tex) return false;
	size_t faceBytes = (size_t)w * (size_t)h * 4;
	SDL_GPUCommandBuffer* cmds = SDL_AcquireGPUCommandBuffer(dev);
	if (!cmds) return false;
	SDL_GPUTransferBuffer* tb = AcquireDownloadTransferBuffer(dev, (Uint32)(faceBytes * faces));
	if (!tb) { SDL_CancelGPUCommandBuffer(cmds); return false; }
	SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmds);
	if (!cp) { ReleaseDownloadTransferBuffer(dev, tb); SDL_CancelGPUCommandBuffer(cmds); return false; }
	for (unsigned f = 0; f < faces; ++f) {
		SDL_GPUTextureRegion src{};
		src.texture = tex;
		src.mip_level = 0;
		src.layer = f;
		src.x = 0; src.y = 0; src.z = 0;
		src.w = w; src.h = h; src.d = 1;
		SDL_GPUTextureTransferInfo dst{};
		dst.transfer_buffer = tb;
		dst.offset = (Uint32)(faceBytes * f);
		dst.pixels_per_row = w;
		dst.rows_per_layer = h;
		SDL_DownloadFromGPUTexture(cp, &src, &dst);
	}
	SDL_EndGPUCopyPass(cp);
	SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmds);
	if (!fence) { ReleaseDownloadTransferBuffer(dev, tb); return false; }
	SDL_WaitForGPUFences(dev, true, &fence, 1);
	SDL_ReleaseGPUFence(dev, fence);
	void* mapped = SDL_MapGPUTransferBuffer(dev, tb, false);
	if (!mapped) { ReleaseDownloadTransferBuffer(dev, tb); return false; }
	int pitch = canvas->cpu_pitch;
	int bpp = canvas->format.getPitch();
	for (unsigned f = 0; f < faces; ++f) {
		const unsigned char* base = (const unsigned char*)mapped + faceBytes * f;
		unsigned char* plane = canvas->cpu_bits + (size_t)f * (size_t)pitch * (size_t)h;
		for (unsigned y = 0; y < h; ++y) {
			const unsigned char* srcRow = base + (size_t)y * w * 4;
			unsigned char* dstRow = plane + (size_t)y * pitch;
			for (unsigned x = 0; x < w; ++x) {
				unsigned r = srcRow[x * 4 + 0], g = srcRow[x * 4 + 1], b = srcRow[x * 4 + 2], a = srcRow[x * 4 + 3];
				canvas->format.setPixel(dstRow + (size_t)x * bpp, canvas->format.fromARGB((a << 24) | (r << 16) | (g << 8) | b));
			}
		}
	}
	SDL_UnmapGPUTransferBuffer(dev, tb);
	ReleaseDownloadTransferBuffer(dev, tb);
	return true;
}

static SDL_GPUTexture* CreateCubeTexture(SDL_GPUDevice* dev, unsigned size, bool mipmaps) {
	if (!dev || !size) return nullptr;
	if (!SDL_GPUTextureSupportsFormat(dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTURETYPE_CUBE,
		(SDL_GPUTextureUsageFlags)(SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER))) return nullptr;
	unsigned levels = 1;
	if (mipmaps) {
		for (unsigned s = size; s > 1; s >>= 1) ++levels;
	}
	SDL_GPUTextureCreateInfo info{};
	info.type = SDL_GPU_TEXTURETYPE_CUBE;
	info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
	info.width = size;
	info.height = size;
	info.layer_count_or_depth = 6;
	info.num_levels = levels;
	info.sample_count = SDL_GPU_SAMPLECOUNT_1;
	return SDL_CreateGPUTexture(dev, &info);
}

static bool UploadCubePixels(SDL_GPUDevice* dev, ::gxCanvas* canvas, SDL_GPUTexture* tex, unsigned w, unsigned h, bool mipmaps) {
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
			ConvertPixelsToRGBA(canvas->format, plane + (size_t)y * pitch, &rgba[(size_t)y * w * 4], w);
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
	if (mipmaps && MipLevels(w, h) > 1) SDL_GenerateMipmapsForGPUTexture(cmds, tex);
	return SDL_SubmitGPUCommandBuffer(cmds);
}

SDL_GPUTexture* EnsureCanvasCubeTexture(SDL_GPUDevice* dev, ::gxCanvas* canvas) {
	GpuLock lock;
	if (!dev || !canvas) return nullptr;
	unsigned w = (unsigned)canvas->getWidth();
	unsigned h = (unsigned)canvas->getHeight();
	if (!w || !h) return nullptr;
	bool mips = (canvas->getFlags() & ::gxCanvas::CANVAS_TEX_MIPMAP) != 0;
	auto it = g_canvasTexMap.find(canvas);
	if (it != g_canvasTexMap.end() && it->second.tex && it->second.rt && it->second.dev == dev && it->second.w == w && it->second.h == h && it->second.mips == mips)
		return it->second.tex;
	SDL_GPUTexture* old = nullptr;
	SDL_GPUDevice* oldDev = dev;
	if (it != g_canvasTexMap.end()) { old = it->second.tex; oldDev = it->second.dev; g_canvasTexMap.erase(it); }
	if (old) RetireTexture(oldDev, old);
	SDL_GPUTexture* tex = CreateCubeTexture(dev, w, mips);
	if (!tex) return nullptr;
	UploadCubePixels(dev, canvas, tex, w, h, mips);
	CanvasTexEntry e;
	e.tex = tex; e.dev = dev; e.modCnt = canvas->getModify(); e.w = w; e.h = h; e.rt = true; e.mips = mips;
	g_canvasTexMap[canvas] = e;
	canvas->releaseCPUBitsIfUnused();
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
	bool mips = (canvas->getFlags() & ::gxCanvas::CANVAS_TEX_MIPMAP) != 0;
	auto it = g_canvasTexMap.find(canvas);
	if (it != g_canvasTexMap.end() && it->second.tex && it->second.rt && it->second.dev == dev && it->second.w == w && it->second.h == h && it->second.mips == mips) {
		return it->second.tex;
	}
	if (it != g_canvasTexMap.end() && it->second.tex && it->second.dev == dev && it->second.modCnt == mod && it->second.w == w && it->second.h == h && it->second.mips == mips) {
		return it->second.tex;
	}
	SDL_GPUTexture* tex = nullptr;
	bool isNew = false;
	if (it != g_canvasTexMap.end() && it->second.tex && it->second.dev == dev && it->second.w == w && it->second.h == h && it->second.mips == mips) {
		tex = it->second.tex;
	}
	else {
		SDL_GPUTexture* old = nullptr;
		SDL_GPUDevice* oldDev = dev;
		if (it != g_canvasTexMap.end()) { old = it->second.tex; oldDev = it->second.dev; g_canvasTexMap.erase(it); }
		if (old) RetireTexture(oldDev, old);
		tex = CreateTexture2D(dev, w, h, mips);
		if (!tex) return nullptr;
		isNew = true;
	}

	static thread_local std::vector<unsigned char> rgba;
	try {
		rgba.resize((size_t)w * h * 4);
	} catch (...) { if (isNew && tex) SDL_ReleaseGPUTexture(dev, tex); return nullptr; }

	if (!canvas->ensureCPUBits()) { if (isNew && tex) SDL_ReleaseGPUTexture(dev, tex); return nullptr; }
	unsigned char* bits = canvas->cpu_bits;
	int pitch = canvas->cpu_pitch;
	if (!bits || pitch <= 0 || (unsigned)canvas->cpu_h != h) { if (isNew && tex) SDL_ReleaseGPUTexture(dev, tex); return nullptr; }
	for (unsigned y = 0; y < h; ++y) {
		ConvertPixelsToRGBA(canvas->format, bits + (size_t)y * pitch, &rgba[(size_t)y * w * 4], w);
	}

	SDL_GPUCommandBuffer* cmds = SDL_AcquireGPUCommandBuffer(dev);
	if (!cmds) { if (isNew && tex) SDL_ReleaseGPUTexture(dev, tex); return nullptr; }
	if (!UploadTextureRGBAOn(dev, cmds, tex, w, h, rgba.data(), !isNew)) {
		SDL_CancelGPUCommandBuffer(cmds);
		if (isNew && tex) SDL_ReleaseGPUTexture(dev, tex);
		return nullptr;
	}
	if (mips && MipLevels(w, h) > 1) SDL_GenerateMipmapsForGPUTexture(cmds, tex);
	if (!SDL_SubmitGPUCommandBuffer(cmds)) {
		if (isNew && tex) SDL_ReleaseGPUTexture(dev, tex);
		return nullptr;
	}
	CanvasTexEntry e;
	e.tex = tex; e.dev = dev; e.modCnt = mod; e.w = w; e.h = h; e.mips = mips;
	g_canvasTexMap[canvas] = e;
	canvas->releaseCPUBitsIfUnused();
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
	if (!canvas->ensureCPUBits()) return false;
	unsigned char* bits = canvas->cpu_bits;
	int pitch = canvas->cpu_pitch;
	if (!bits || pitch <= 0 || (unsigned)canvas->cpu_h != h) return false;
	static thread_local std::vector<unsigned char> rgba;
	try { rgba.resize((size_t)w * h * 4); }
	catch (...) { return false; }
	for (unsigned y = 0; y < h; ++y) {
		ConvertPixelsToRGBA(canvas->format, bits + (size_t)y * pitch, &rgba[(size_t)y * w * 4], w);
	}
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
	canvas->releaseCPUBitsIfUnused();
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
		tex = CreateTexture2D(dev, w, h, false);
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
		unsigned char* row = &rgba[(size_t)y * w * 4];
		ConvertPixelsToRGBA(canvas->format, canvas->getLockedSurf() + (size_t)y * canvas->getLockedPitch(), row, w);
		for (unsigned x = 0; x < w; ++x) {
			unsigned rgb = ((unsigned)row[x * 4] << 16) | ((unsigned)row[x * 4 + 1] << 8) | row[x * 4 + 2];
			row[x * 4 + 3] = (rgb == clsRgb) ? 0 : 255;
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
		tex = CreateTexture2D(dev, w, h, false);
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
	int srcBpp = canvas->format.getPitch();
	for (unsigned y = 0; y < uh; ++y) {
		unsigned char* row = &rgba[(size_t)y * uw * 4];
		ConvertPixelsToRGBA(canvas->format,
			canvas->getLockedSurf() + (size_t)(uy + y) * canvas->getLockedPitch() + (size_t)ux * srcBpp, row, uw);
		for (unsigned x = 0; x < uw; ++x) {
			unsigned rgb = ((unsigned)row[x * 4] << 16) | ((unsigned)row[x * 4 + 1] << 8) | row[x * 4 + 2];
			row[x * 4 + 3] = (rgb == clsRgb) ? 0 : 255;
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

SDL_GPUTexture* CreateTexture2D(SDL_GPUDevice* dev, unsigned w, unsigned h, bool mipmaps) {
	if (!dev || !w || !h) return nullptr;
	SDL_GPUTextureUsageFlags usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
	if (mipmaps) usage |= SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
	if (!SDL_GPUTextureSupportsFormat(dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTURETYPE_2D, usage)) return nullptr;
	SDL_GPUTextureCreateInfo info{};
	info.type = SDL_GPU_TEXTURETYPE_2D;
	info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	info.usage = usage;
	info.width = w;
	info.height = h;
	info.layer_count_or_depth = 1;
	info.num_levels = mipmaps ? MipLevels(w, h) : 1;
	info.sample_count = SDL_GPU_SAMPLECOUNT_1;
	return SDL_CreateGPUTexture(dev, &info);
}

bool UploadTextureRGBA(SDL_GPUDevice* dev, SDL_GPUTexture* tex, unsigned w, unsigned h, const void* px) {
	return UploadTextureRGBA(dev, tex, w, h, px, true);
}

bool UploadTextureRGBA(SDL_GPUDevice* dev, SDL_GPUTexture* tex, unsigned w, unsigned h, const void* px, bool cycle) {
	if (!dev || !tex || !w || !h || !px) return false;
	SDL_GPUCommandBuffer* cmds = SDL_AcquireGPUCommandBuffer(dev);
	if (!cmds) return false;
	if (!UploadTextureRGBAOn(dev, cmds, tex, w, h, px, cycle)) {
		SDL_CancelGPUCommandBuffer(cmds);
		return false;
	}
	return SDL_SubmitGPUCommandBuffer(cmds);
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

static SDL_GPUSampleCount ToSDLSamples(int n) {
	switch (n) {
		case 8: return SDL_GPU_SAMPLECOUNT_8;
		case 4: return SDL_GPU_SAMPLECOUNT_4;
		case 2: return SDL_GPU_SAMPLECOUNT_2;
		default: return SDL_GPU_SAMPLECOUNT_1;
	}
}

SDL_GPUTexture* CreateColorTargetMS(SDL_GPUDevice* dev, unsigned w, unsigned h, int sampleCount) {
	if (!dev || !w || !h) return nullptr;
	SDL_GPUSampleCount sc = ToSDLSamples(sampleCount);
	if (sc == SDL_GPU_SAMPLECOUNT_1) return CreateColorTarget(dev, w, h, 0.0f, 0.0f, 0.0f, 1.0f);
	SDL_GPUTextureFormat fmt = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	if (!SDL_GPUTextureSupportsFormat(dev, fmt, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET)) return nullptr;
	SDL_GPUTextureCreateInfo info{};
	info.type = SDL_GPU_TEXTURETYPE_2D;
	info.format = fmt;
	info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
	info.width = w;
	info.height = h;
	info.layer_count_or_depth = 1;
	info.num_levels = 1;
	info.sample_count = sc;
	return SDL_CreateGPUTexture(dev, &info);
}

SDL_GPUTexture* CreateDepthTarget(SDL_GPUDevice* dev, unsigned w, unsigned h, int formatValue) {
	return CreateDepthTarget(dev, w, h, formatValue, 1.0f, 0);
}

SDL_GPUTexture* CreateDepthTarget(SDL_GPUDevice* dev, unsigned w, unsigned h, int formatValue, float depth, unsigned char stencil) {
	return CreateDepthTarget(dev, w, h, formatValue, depth, stencil, 1);
}

SDL_GPUTexture* CreateDepthTarget(SDL_GPUDevice* dev, unsigned w, unsigned h, int formatValue, float depth, unsigned char stencil, int sampleCount) {
	if (!dev || !w || !h) return nullptr;
	SDL_GPUTextureFormat fmt = (SDL_GPUTextureFormat)formatValue;
	SDL_GPUSampleCount sc = ToSDLSamples(sampleCount);
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
	info.sample_count = sc;
	info.props = props;
	SDL_GPUTexture* tex = SDL_CreateGPUTexture(dev, &info);
	if (props) SDL_DestroyProperties(props);
	return tex;
}

}
