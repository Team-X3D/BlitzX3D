#include "sdl_gpu_pipeline.h"
#include "sdl_gpu_lock.h"
#include "sdl_gpu_mesh.h"
#include "sdl_gpu_upload.h"

#include "../std.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_properties.h>

#include "shaders/mesh_shaders.h"
#include "shaders/canvas_shaders.h"
#include "shaders/present_shaders.h"
#include "sdl_gpu_text.h"
#include "sdl_gpu_shader.h"

namespace sdlgpu {

	namespace {
		SDL_GPUDevice* g_blitDev = nullptr;
		SDL_GPUTexture* g_blitTex = nullptr;
		unsigned g_blitW = 0, g_blitH = 0;
		bool g_blitHasData = false;
	}

	static void TeardownBlit();
	void ClearTransferPool(SDL_GPUDevice* dev);

	static SDL_GPUShader* LoadShader(SDL_GPUDevice* dev, SDL_GPUShaderFormat fmt, SDL_GPUShaderStage stage, const char* entry, const uint8_t* code, size_t size, unsigned samplers = 0, unsigned uniformBuffers = 0, unsigned storageBuffers = 0) {
		SDL_GPUShaderCreateInfo info{};
		info.code = code;
		info.code_size = size;
		info.entrypoint = entry;
		info.format = fmt;
		info.stage = stage;
		info.num_samplers = samplers;
		info.num_storage_buffers = storageBuffers;
		info.num_uniform_buffers = uniformBuffers;
		return SDL_CreateGPUShader(dev, &info);
	}

	static void TeardownBlit() {
		GpuLock lock;
		if (g_blitTex && g_blitDev) SDL_ReleaseGPUTexture(g_blitDev, g_blitTex);
		g_blitTex = nullptr;
		g_blitDev = nullptr;
		g_blitW = g_blitH = 0;
		g_blitHasData = false;
	}

	static bool EnsureBlitTexture(SDL_GPUDevice* dev, unsigned w, unsigned h) {
		GpuLock lock;
		if (g_blitTex && g_blitDev == dev && g_blitW == w && g_blitH == h) return true;
		if (g_blitTex) {
			SDL_ReleaseGPUTexture(g_blitDev, g_blitTex);
			g_blitTex = nullptr;
			g_blitW = g_blitH = 0;
			g_blitHasData = false;
		}
		SDL_GPUTextureCreateInfo texInfo{};
		texInfo.type = SDL_GPU_TEXTURETYPE_2D;
		texInfo.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
		texInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
		texInfo.width = w;
		texInfo.height = h;
		texInfo.layer_count_or_depth = 1;
		texInfo.num_levels = 1;
		texInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;
		g_blitTex = SDL_CreateGPUTexture(dev, &texInfo);
		if (!g_blitTex) return false;
		g_blitDev = dev;
		g_blitW = w;
		g_blitH = h;
		g_blitHasData = false;
		return true;
	}

	bool PresentBlit(SDL_GPUDevice* dev, SDL_Window* win, float r, float g, float b, unsigned w, unsigned h, const void* px) {
		GpuLock lock;
		if (!dev || !win || !w || !h) return false;
		if (!EnsureBlitTexture(dev, w, h)) return false;

		SDL_GPUCommandBuffer* cmds = SDL_AcquireGPUCommandBuffer(dev);
		if (!cmds) return false;

		SDL_GPUTransferBuffer* buf = nullptr;
		bool didUpload = false;
		if (px) {
			Uint32 size = w * h * 4;
			buf = AcquireUploadTransferBuffer(dev, size);
			if (!buf) { SDL_CancelGPUCommandBuffer(cmds); return false; }
			void* dst = SDL_MapGPUTransferBuffer(dev, buf, true);
			if (!dst) {
				ReleaseUploadTransferBuffer(dev, buf);
				SDL_CancelGPUCommandBuffer(cmds);
				return false;
			}
			memcpy(dst, px, size);
			SDL_UnmapGPUTransferBuffer(dev, buf);

			SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmds);
			SDL_GPUTextureTransferInfo src{};
			src.transfer_buffer = buf;
			src.pixels_per_row = w;
			src.rows_per_layer = h;
			SDL_GPUTextureRegion reg{};
			reg.texture = g_blitTex;
			reg.w = w;
			reg.h = h;
			reg.d = 1;
			SDL_UploadToGPUTexture(copy, &src, &reg, true);
			SDL_EndGPUCopyPass(copy);
			didUpload = true;
		}

		SDL_GPUTexture* tex = nullptr;
		Uint32 sw = 0, sh = 0;
		if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmds, win, &tex, &sw, &sh)) {
			SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "SDL_WaitAndAcquireGPUSwapchainTexture failed: %s", SDL_GetError());
			SDL_CancelGPUCommandBuffer(cmds);
			if (buf) ReleaseUploadTransferBuffer(dev, buf);
			return false;
		}
		bool haveData = g_blitHasData || didUpload;
		if (tex) {
			bool gammaDone = haveData && GammaBlit(dev, cmds, g_blitTex, tex, sw, sh, SDL_GetGPUSwapchainTextureFormat(dev, win));
			if (!gammaDone) {
				if (haveData) {
					SDL_GPUBlitInfo blit{};
					blit.source.texture = g_blitTex;
					blit.source.w = w;
					blit.source.h = h;
					blit.destination.texture = tex;
					blit.destination.w = sw;
					blit.destination.h = sh;
					blit.load_op = SDL_GPU_LOADOP_CLEAR;
					blit.clear_color = SDL_FColor{ r, g, b, 1.0f };
					blit.flip_mode = SDL_FLIP_NONE;
					blit.filter = SDL_GPU_FILTER_LINEAR;
					blit.cycle = false;
					SDL_BlitGPUTexture(cmds, &blit);
				}
				else {
					SDL_GPUColorTargetInfo target{};
					target.texture = tex;
					target.load_op = SDL_GPU_LOADOP_CLEAR;
					target.store_op = SDL_GPU_STOREOP_STORE;
					target.clear_color = SDL_FColor{ r, g, b, 1.0f };
					SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmds, &target, 1, nullptr);
					if (pass) SDL_EndGPURenderPass(pass);
				}
			}
		}
		bool ok = SDL_SubmitGPUCommandBuffer(cmds);
		if (buf) ReleaseUploadTransferBuffer(dev, buf);
		if (ok && didUpload) g_blitHasData = true;
		return ok;
	}

	namespace {
		SDL_GPUDevice* g_meshDev = nullptr;
		SDL_GPUSampler* g_meshSamp = nullptr;
		SDL_GPUDevice* g_whiteDev = nullptr;
		SDL_GPUTexture* g_whiteTex = nullptr;
		struct MeshPipeKey {
			SDL_GPUTextureFormat format = SDL_GPU_TEXTUREFORMAT_INVALID;
			SDL_GPUTextureFormat depthFormat = SDL_GPU_TEXTUREFORMAT_INVALID;
			unsigned stride = 0;
			bool skinned = false;
			bool twoTex = false;
			int multiStages = 0;
			bool cube0 = false;
			bool cube1 = false;
			int blend = MESH_BLEND_REPLACE;
			int zMode = MESH_Z_NORMAL;
			SDL_GPUCullMode cullMode = SDL_GPU_CULLMODE_NONE;
			bool wireframe = false;
			int samples = 1;
			bool operator==(const MeshPipeKey& o) const {
				return format == o.format && depthFormat == o.depthFormat && stride == o.stride &&
					skinned == o.skinned && twoTex == o.twoTex && multiStages == o.multiStages &&
					cube0 == o.cube0 && cube1 == o.cube1 &&
					blend == o.blend && zMode == o.zMode && cullMode == o.cullMode && wireframe == o.wireframe &&
					samples == o.samples;
			}
			bool operator<(const MeshPipeKey& o) const {
				return std::tie(format, depthFormat, stride, skinned, twoTex, multiStages, cube0, cube1, blend, zMode, cullMode, wireframe, samples)
					< std::tie(o.format, o.depthFormat, o.stride, o.skinned, o.twoTex, o.multiStages, o.cube0, o.cube1, o.blend, o.zMode, o.cullMode, o.wireframe, o.samples);
			}
		};
		std::map<MeshPipeKey, SDL_GPUGraphicsPipeline*> g_meshPipes;
		struct MeshSampKey {
			unsigned pack = 0;
			int biasQ = 0;
			bool operator<(const MeshSampKey& o) const {
				return pack < o.pack || (pack == o.pack && biasQ < o.biasQ);
			}
		};
		std::map<MeshSampKey, SDL_GPUSampler*> g_meshSamps;

		SDL_GPURenderPass* g_lastMeshPass = nullptr;
		SDL_GPUGraphicsPipeline* g_lastMeshPipe = nullptr;
		int g_lastMeshSamplerCount = -1;
		SDL_GPUTexture* g_lastMeshTex[MESH_MAX_STAGES] = {};
		SDL_GPUSampler* g_lastMeshSamp[MESH_MAX_STAGES] = {};

		static SDL_GPUSampleCount ToSampleCount(int n) {
			switch (n) {
				case 8: return SDL_GPU_SAMPLECOUNT_8;
				case 4: return SDL_GPU_SAMPLECOUNT_4;
				case 2: return SDL_GPU_SAMPLECOUNT_2;
				default: return SDL_GPU_SAMPLECOUNT_1;
			}
		}

		SDL_GPUDevice* g_canvasDev = nullptr;
		SDL_GPUGraphicsPipeline* g_canvasPipe = nullptr;
		SDL_GPUSampler* g_canvasSamp = nullptr;
		SDL_GPUBuffer* g_canvasVB = nullptr;
		SDL_GPUTextureFormat g_canvasFormat = SDL_GPU_TEXTUREFORMAT_INVALID;

		SDL_GPUDevice* g_depthFmtDev = nullptr;
		SDL_GPUTextureFormat g_depthFmt = SDL_GPU_TEXTUREFORMAT_INVALID;

	}

	void InvalidateMeshState() {
		GpuLock lock;
		g_lastMeshPass = nullptr;
		g_lastMeshPipe = nullptr;
		g_lastMeshSamplerCount = -1;
	}

	static void TeardownMeshPipe() {
		GpuLock lock;
		for (auto& e : g_meshPipes) if (e.second && g_meshDev) SDL_ReleaseGPUGraphicsPipeline(g_meshDev, e.second);
		g_meshPipes.clear();
		for (auto& e : g_meshSamps) if (e.second && g_meshDev) SDL_ReleaseGPUSampler(g_meshDev, e.second);
		g_meshSamps.clear();
		if (g_meshSamp && g_meshDev) SDL_ReleaseGPUSampler(g_meshDev, g_meshSamp);
		g_meshSamp = nullptr;
		g_meshDev = nullptr;
		g_depthFmtDev = nullptr;
		g_depthFmt = SDL_GPU_TEXTUREFORMAT_INVALID;
	}

	static SDL_GPUSampler* EnsureMeshSampler(SDL_GPUDevice* dev, bool wrapU, bool wrapV, bool point, int aniso, float lodBias) {
		GpuLock lock;
		if (g_meshDev && g_meshDev != dev) TeardownMeshPipe();
		if (aniso < 1) aniso = 1;
		if (aniso > 16) aniso = 16;
		MeshSampKey key;
		key.pack = (wrapU ? 4u : 0u) | (wrapV ? 2u : 0u) | (point ? 1u : 0u) | ((unsigned)aniso << 3);
		key.biasQ = (int)lroundf(lodBias * 256.0f);
		auto it = g_meshSamps.find(key);
		if (it != g_meshSamps.end()) return it->second;
		SDL_GPUSamplerCreateInfo samp{};
		samp.min_filter = point ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
		samp.mag_filter = point ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
		samp.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
		samp.address_mode_u = wrapU ? SDL_GPU_SAMPLERADDRESSMODE_REPEAT : SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		samp.address_mode_v = wrapV ? SDL_GPU_SAMPLERADDRESSMODE_REPEAT : SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		samp.max_lod = 1000.0f;
		samp.mip_lod_bias = (float)key.biasQ / 256.0f;
		if (aniso > 1) {
			samp.enable_anisotropy = true;
			samp.max_anisotropy = (float)aniso;
		}
		SDL_GPUSampler* s = SDL_CreateGPUSampler(dev, &samp);
		if (!s) return nullptr;
		g_meshDev = dev;
		g_meshSamps[key] = s;
		return s;
	}

	static void TeardownCanvas() {
		GpuLock lock;
		if (g_canvasVB && g_canvasDev) SDL_ReleaseGPUBuffer(g_canvasDev, g_canvasVB);
		if (g_canvasPipe && g_canvasDev) SDL_ReleaseGPUGraphicsPipeline(g_canvasDev, g_canvasPipe);
		if (g_canvasSamp && g_canvasDev) SDL_ReleaseGPUSampler(g_canvasDev, g_canvasSamp);
		g_canvasVB = nullptr;
		g_canvasPipe = nullptr;
		g_canvasSamp = nullptr;
		g_canvasDev = nullptr;
		g_canvasFormat = SDL_GPU_TEXTUREFORMAT_INVALID;
	}

	static bool EnsureCanvasVertices(SDL_GPUDevice* dev);

	static void TeardownWhiteTexture() {
		GpuLock lock;
		if (g_whiteTex && g_whiteDev) SDL_ReleaseGPUTexture(g_whiteDev, g_whiteTex);
		g_whiteTex = nullptr;
		g_whiteDev = nullptr;
	}

	static SDL_GPUTextureFormat PickMeshDepthFormat(SDL_GPUDevice* dev) {
		GpuLock lock;
		if (dev && dev == g_depthFmtDev && g_depthFmt != SDL_GPU_TEXTUREFORMAT_INVALID)
			return g_depthFmt;
		SDL_GPUTextureFormat picked = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
		if (dev) {
			const SDL_GPUTextureFormat order[] = {
				SDL_GPU_TEXTUREFORMAT_D24_UNORM,
				SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT,
				SDL_GPU_TEXTUREFORMAT_D16_UNORM,
				SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
				SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT,
			};
			for (SDL_GPUTextureFormat f : order) {
				if (SDL_GPUTextureSupportsFormat(dev, f, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)) {
					picked = f;
					break;
				}
			}
			g_depthFmtDev = dev;
			g_depthFmt = picked;
		}
		return picked;
	}

	int MeshDepthFormat(SDL_GPUDevice* dev) {
		return (int)PickMeshDepthFormat(dev);
	}

	int SceneColorFormat() {
		return (int)SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	}

	static SDL_GPUTexture* EnsureWhiteTexture(SDL_GPUDevice* dev);

	SDL_GPUTexture* GetWhiteTexture(SDL_GPUDevice* dev) {
		return EnsureWhiteTexture(dev);
	}

	SDL_GPUSampler* GetDefaultMeshSampler(SDL_GPUDevice* dev) {
		GpuLock lock;
		if (!dev) return nullptr;
		if (g_meshDev && g_meshDev != dev) TeardownMeshPipe();
		if (!g_meshSamp) {
			SDL_GPUSamplerCreateInfo samp{};
			samp.min_filter = SDL_GPU_FILTER_LINEAR;
			samp.mag_filter = SDL_GPU_FILTER_LINEAR;
			samp.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
			samp.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
			samp.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
			samp.max_lod = 1000.0f;
			g_meshSamp = SDL_CreateGPUSampler(dev, &samp);
			if (!g_meshSamp) return nullptr;
			g_meshDev = dev;
		}
		return g_meshSamp;
	}

	static SDL_GPUTexture* EnsureWhiteTexture(SDL_GPUDevice* dev) {
		GpuLock lock;
		if (g_whiteTex && g_whiteDev == dev) return g_whiteTex;
		TeardownWhiteTexture();
		SDL_GPUTextureCreateInfo info{};
		info.type = SDL_GPU_TEXTURETYPE_2D;
		info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
		info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
		info.width = 1;
		info.height = 1;
		info.layer_count_or_depth = 1;
		info.num_levels = 1;
		info.sample_count = SDL_GPU_SAMPLECOUNT_1;
		SDL_GPUTexture* tex = SDL_CreateGPUTexture(dev, &info);
		if (!tex) return nullptr;

		unsigned char white[4] = { 255, 255, 255, 255 };
		SDL_GPUTransferBuffer* buf = AcquireUploadTransferBuffer(dev, 4);
		if (!buf) { SDL_ReleaseGPUTexture(dev, tex); return nullptr; }
		void* dst = SDL_MapGPUTransferBuffer(dev, buf, true);
		if (!dst) { ReleaseUploadTransferBuffer(dev, buf); SDL_ReleaseGPUTexture(dev, tex); return nullptr; }
		memcpy(dst, white, 4);
		SDL_UnmapGPUTransferBuffer(dev, buf);

		SDL_GPUCommandBuffer* cmds = SDL_AcquireGPUCommandBuffer(dev);
		if (!cmds) { ReleaseUploadTransferBuffer(dev, buf); SDL_ReleaseGPUTexture(dev, tex); return nullptr; }
		SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmds);
		SDL_GPUTextureTransferInfo src{};
		src.transfer_buffer = buf;
		src.pixels_per_row = 1;
		src.rows_per_layer = 1;
		SDL_GPUTextureRegion reg{};
		reg.texture = tex;
		reg.w = 1;
		reg.h = 1;
		reg.d = 1;
		SDL_UploadToGPUTexture(copy, &src, &reg, true);
		SDL_EndGPUCopyPass(copy);
		bool ok = SDL_SubmitGPUCommandBuffer(cmds);
		ReleaseUploadTransferBuffer(dev, buf);
		if (!ok) { SDL_ReleaseGPUTexture(dev, tex); return nullptr; }

		g_whiteDev = dev;
		g_whiteTex = tex;
		return g_whiteTex;
	}

	static SDL_GPUShaderFormat CachedShaderFormats(SDL_GPUDevice* dev) {
		GpuLock lock;
		static SDL_GPUDevice* cachedDev = nullptr;
		static SDL_GPUShaderFormat cached = SDL_GPU_SHADERFORMAT_INVALID;
		if (dev && dev == cachedDev) return cached;
		cached = dev ? SDL_GetGPUShaderFormats(dev) : SDL_GPU_SHADERFORMAT_INVALID;
		cachedDev = dev;
		return cached;
	}

	static SDL_GPUGraphicsPipeline* EnsureMeshPipe(SDL_GPUDevice* dev, SDL_Window* win, unsigned stride, bool skinned, bool twoTex, int colorFormatOverride, int depthFormatOverride, int blendMode, int zMode, SDL_GPUCullMode cullMode, bool wireframe, int multiStages = 0, bool cube0 = false, bool cube1 = false, int samples = 1) {
		GpuLock lock;
		SDL_GPUTextureFormat fmt = colorFormatOverride ? (SDL_GPUTextureFormat)colorFormatOverride : SDL_GetGPUSwapchainTextureFormat(dev, win);
		SDL_GPUTextureFormat depthFmt = depthFormatOverride ? (SDL_GPUTextureFormat)depthFormatOverride : PickMeshDepthFormat(dev);
		if (g_meshDev && g_meshDev != dev) TeardownMeshPipe();
		if (blendMode < MESH_BLEND_REPLACE || blendMode > MESH_BLEND_ADD) blendMode = MESH_BLEND_ALPHA;
		if (zMode < MESH_Z_NORMAL || zMode > MESH_Z_CMPONLY) zMode = MESH_Z_NORMAL;
		if (multiStages < 0) multiStages = 0;
		if (multiStages > MESH_MAX_STAGES) multiStages = MESH_MAX_STAGES;
		if (multiStages > 0) { twoTex = false; cube0 = false; cube1 = false; }

		MeshPipeKey key{ fmt, depthFmt, stride, skinned, twoTex, multiStages, cube0, cube1, blendMode, zMode, cullMode, wireframe, samples };
		{
			auto found = g_meshPipes.find(key);
			if (found != g_meshPipes.end()) return found->second;
		}

		SDL_GPUShaderFormat supported = CachedShaderFormats(dev);
		const uint8_t* vsCode = nullptr;
		const uint8_t* psCode = nullptr;
		size_t vsSize = 0, psSize = 0;
		const char* vsEntry = skinned ? "VSMainSkinned" : "VSMain";
		int psKind = 0;
		const char* psEntry;
		if (multiStages > 0) {
			psEntry = "PSMainMulti";
		} else {
			if (cube0) psKind = twoTex ? (cube1 ? 4 : 2) : 1;
			else if (cube1) psKind = 3;
			psEntry = psKind == 1 ? "PSMainCube"
				: psKind == 2 ? "PSMainCubeTex"
				: psKind == 3 ? "PSMainTexCube"
				: psKind == 4 ? "PSMainCubeCube"
				: "PSMainCube";
		}
		SDL_GPUShaderFormat useFmt = SDL_GPU_SHADERFORMAT_INVALID;
		if (supported & SDL_GPU_SHADERFORMAT_SPIRV) {
			useFmt = SDL_GPU_SHADERFORMAT_SPIRV;
			if (skinned) { vsCode = kSkinVS_SPIRV; vsSize = kSkinVS_SPIRV_size; }
			else { vsCode = kMeshVS_SPIRV; vsSize = kMeshVS_SPIRV_size; }
			if (multiStages > 0) {
				static const uint8_t* const multiCode[] = {
					kMeshPS1_SPIRV, kMeshPS2_SPIRV, kMeshPS3_SPIRV, kMeshPS4_SPIRV,
					kMeshPS5_SPIRV, kMeshPS6_SPIRV, kMeshPS7_SPIRV, kMeshPS8_SPIRV };
				static const size_t multiSize[] = {
					kMeshPS1_SPIRV_size, kMeshPS2_SPIRV_size, kMeshPS3_SPIRV_size, kMeshPS4_SPIRV_size,
					kMeshPS5_SPIRV_size, kMeshPS6_SPIRV_size, kMeshPS7_SPIRV_size, kMeshPS8_SPIRV_size };
				psCode = multiCode[multiStages - 1]; psSize = multiSize[multiStages - 1];
			}
			else switch (psKind) {
				case 1: psCode = kMeshPSCube_SPIRV; psSize = kMeshPSCube_SPIRV_size; break;
				case 2: psCode = kMeshPSCubeTex_SPIRV; psSize = kMeshPSCubeTex_SPIRV_size; break;
				case 3: psCode = kMeshPSTexCube_SPIRV; psSize = kMeshPSTexCube_SPIRV_size; break;
				default: psCode = kMeshPSCubeCube_SPIRV; psSize = kMeshPSCubeCube_SPIRV_size; break;
			}
		}
		else if (supported & SDL_GPU_SHADERFORMAT_DXIL) {
			useFmt = SDL_GPU_SHADERFORMAT_DXIL;
			if (skinned) { vsCode = kSkinVS_DXIL; vsSize = kSkinVS_DXIL_size; }
			else { vsCode = kMeshVS_DXIL; vsSize = kMeshVS_DXIL_size; }
			if (multiStages > 0) {
				static const uint8_t* const multiCode[] = {
					kMeshPS1_DXIL, kMeshPS2_DXIL, kMeshPS3_DXIL, kMeshPS4_DXIL,
					kMeshPS5_DXIL, kMeshPS6_DXIL, kMeshPS7_DXIL, kMeshPS8_DXIL };
				static const size_t multiSize[] = {
					kMeshPS1_DXIL_size, kMeshPS2_DXIL_size, kMeshPS3_DXIL_size, kMeshPS4_DXIL_size,
					kMeshPS5_DXIL_size, kMeshPS6_DXIL_size, kMeshPS7_DXIL_size, kMeshPS8_DXIL_size };
				psCode = multiCode[multiStages - 1]; psSize = multiSize[multiStages - 1];
			}
			else switch (psKind) {
				case 1: psCode = kMeshPSCube_DXIL; psSize = kMeshPSCube_DXIL_size; break;
				case 2: psCode = kMeshPSCubeTex_DXIL; psSize = kMeshPSCubeTex_DXIL_size; break;
				case 3: psCode = kMeshPSTexCube_DXIL; psSize = kMeshPSTexCube_DXIL_size; break;
				default: psCode = kMeshPSCubeCube_DXIL; psSize = kMeshPSCubeCube_DXIL_size; break;
			}
		}
		if (useFmt == SDL_GPU_SHADERFORMAT_INVALID) {
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "No supported mesh shader format: %u", (unsigned)supported);
			return nullptr;
		}

		SDL_GPUShader* vs = LoadShader(dev, useFmt, SDL_GPU_SHADERSTAGE_VERTEX, vsEntry, vsCode, vsSize, 0, 1, skinned ? 1 : 0);
		if (!vs) return nullptr;
		unsigned fragSamplers = multiStages > 0 ? (unsigned)multiStages : (twoTex ? 2 : 1);
		SDL_GPUShader* ps = LoadShader(dev, useFmt, SDL_GPU_SHADERSTAGE_FRAGMENT, psEntry, psCode, psSize, fragSamplers, 1);
		if (!ps) { SDL_ReleaseGPUShader(dev, vs); return nullptr; }

		static_assert(sizeof(float) * 3 == 12, "layout");
		static_assert(sizeof(unsigned) == 4, "layout");
		SDL_GPUVertexBufferDescription vb{};
		vb.slot = 0;
		vb.pitch = stride;
		vb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
		SDL_GPUVertexAttribute attrs[7]{};
		attrs[0].location = 0; attrs[0].buffer_slot = 0; attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3; attrs[0].offset = 0;
		attrs[1].location = 1; attrs[1].buffer_slot = 0; attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3; attrs[1].offset = 12;
		attrs[2].location = 2; attrs[2].buffer_slot = 0; attrs[2].format = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM; attrs[2].offset = 24;
		attrs[3].location = 3; attrs[3].buffer_slot = 0; attrs[3].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2; attrs[3].offset = 28;
		attrs[4].location = 4; attrs[4].buffer_slot = 0; attrs[4].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2; attrs[4].offset = 36;
		unsigned attrCount = 5;
		if (skinned) {
			attrs[5].location = 5; attrs[5].buffer_slot = 0; attrs[5].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4; attrs[5].offset = 44;
			attrs[6].location = 6; attrs[6].buffer_slot = 0; attrs[6].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4; attrs[6].offset = 60;
			attrCount = 7;
		}
		SDL_GPUVertexInputState vin{};
		vin.vertex_buffer_descriptions = &vb;
		vin.num_vertex_buffers = 1;
		vin.vertex_attributes = attrs;
		vin.num_vertex_attributes = attrCount;

		SDL_GPUColorTargetDescription target{};
		target.format = fmt;
		if (blendMode != MESH_BLEND_REPLACE) {
			target.blend_state.enable_blend = true;
			target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
			target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
			if (blendMode == MESH_BLEND_MULTIPLY) {
				target.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_DST_COLOR;
				target.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
				target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_DST_ALPHA;
				target.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
			}
			else if (blendMode == MESH_BLEND_ADD) {
				target.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
				target.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
				target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
				target.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
			}
			else {
				target.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
				target.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
				target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
				target.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
			}
		}

		SDL_GPUGraphicsPipelineCreateInfo info{};
		info.vertex_shader = vs;
		info.fragment_shader = ps;
		info.vertex_input_state = vin;
		info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
		info.rasterizer_state.fill_mode = wireframe ? SDL_GPU_FILLMODE_LINE : SDL_GPU_FILLMODE_FILL;
		info.rasterizer_state.cull_mode = cullMode;
		info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;
		info.rasterizer_state.enable_depth_clip = true;
		info.rasterizer_state.enable_depth_bias = false;
		info.rasterizer_state.depth_bias_constant_factor = 0.0f;
		info.rasterizer_state.depth_bias_clamp = 0.0f;
		info.rasterizer_state.depth_bias_slope_factor = 0.0f;
		info.multisample_state.sample_count = ToSampleCount(samples);
		info.multisample_state.sample_mask = 0;

		info.depth_stencil_state.enable_depth_test = (zMode != MESH_Z_DISABLE);
		info.depth_stencil_state.enable_depth_write = (zMode == MESH_Z_NORMAL);
		info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
		info.depth_stencil_state.enable_stencil_test = false;
		info.depth_stencil_state.back_stencil_state.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
		info.depth_stencil_state.front_stencil_state.compare_op = SDL_GPU_COMPAREOP_ALWAYS;

		info.target_info.num_color_targets = 1;
		info.target_info.color_target_descriptions = &target;
		info.target_info.has_depth_stencil_target = true;
		info.target_info.depth_stencil_format = depthFmt;

		SDL_PropertiesID pipeProps = SDL_CreateProperties();
		if (pipeProps) SDL_SetStringProperty(pipeProps, SDL_PROP_GPU_GRAPHICSPIPELINE_CREATE_NAME_STRING, "b3d_mesh");
		info.props = pipeProps;
		SDL_GPUGraphicsPipeline* newPipe = SDL_CreateGPUGraphicsPipeline(dev, &info);
		if (pipeProps) SDL_DestroyProperties(pipeProps);
		info.props = 0;
		SDL_ReleaseGPUShader(dev, vs);
		SDL_ReleaseGPUShader(dev, ps);
		if (!newPipe) {
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_CreateGPUGraphicsPipeline mesh failed: %s", SDL_GetError());
			return nullptr;
		}
		if (g_meshPipes.size() >= 512) {
			SDL_GPUGraphicsPipeline* old = g_meshPipes.begin()->second;
			if (old) SDL_ReleaseGPUGraphicsPipeline(dev, old);
			g_meshPipes.erase(g_meshPipes.begin());
		}

		if (!g_meshSamp) {
			SDL_GPUSamplerCreateInfo samp{};
			samp.min_filter = SDL_GPU_FILTER_LINEAR;
			samp.mag_filter = SDL_GPU_FILTER_LINEAR;
			samp.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
			samp.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
			samp.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
			samp.max_lod = 1000.0f;
			g_meshSamp = SDL_CreateGPUSampler(dev, &samp);
			if (!g_meshSamp) { SDL_ReleaseGPUGraphicsPipeline(dev, newPipe); return nullptr; }
		}

		g_meshDev = dev;
		g_meshPipes.emplace(key, newPipe);
		return newPipe;
	}

	struct MeshFragUniforms {
		float stage1[4];
		float mat0A[4];
		float mat0B[4];
		float mat1A[4];
		float mat1B[4];
		float bump[4];
		float flat[4];
		float stage[MESH_MAX_STAGES][4];
		float matA[MESH_MAX_STAGES][4];
		float matB[MESH_MAX_STAGES][4];
		float bumpEnv[MESH_MAX_STAGES][4];
	};

	static void PushMeshFragUniforms(SDL_GPUCommandBuffer* cmds, const MeshDrawParams& p) {
		MeshFragUniforms fu{};
		memcpy(fu.stage1, p.stage1, sizeof(fu.stage1));
		memcpy(fu.mat0A, p.uvMat0A, sizeof(fu.mat0A));
		memcpy(fu.mat0B, p.uvMat0B, sizeof(fu.mat0B));
		memcpy(fu.mat1A, p.uvMat1A, sizeof(fu.mat1A));
		memcpy(fu.mat1B, p.uvMat1B, sizeof(fu.mat1B));
		memcpy(fu.bump, p.bumpMat, sizeof(fu.bump));
		fu.flat[0] = p.flat;
		int n = p.stageCount;
		if (n < 0) n = 0;
		if (n > MESH_MAX_STAGES) n = MESH_MAX_STAGES;
		for (int i = 0; i < n; ++i) {
			const MeshStage& s = p.stages[i];
			fu.stage[i][0] = (float)s.blend;
			fu.stage[i][1] = s.useUV1 ? 1.0f : 0.0f;
			fu.stage[i][2] = s.alpha ? 1.0f : 0.0f;
			memcpy(fu.matA[i], s.matA, sizeof(fu.matA[i]));
			memcpy(fu.matB[i], s.matB, sizeof(fu.matB[i]));
			memcpy(fu.bumpEnv[i], s.bump, sizeof(fu.bumpEnv[i]));
		}
		SDL_PushGPUFragmentUniformData(cmds, 0, &fu, (unsigned)sizeof(fu));
	}

	void DrawMesh(SDL_GPUDevice* dev, SDL_Window* win, SDL_GPUCommandBuffer* cmds, SDL_GPURenderPass* pass, GpuMesh* mesh, const float* uniforms, unsigned uniformBytes, unsigned indexCount, unsigned startIndex, int firstVertex, int colorFormat, int depthFormat, const MeshDrawParams& p, int samples) {
		GpuLock lock;
		if (!dev || !cmds || !pass || !mesh || !uniforms || !uniformBytes || !indexCount) return;
		if (!colorFormat && !win) return;
		if (startIndex + indexCount > mesh->maxTris * 3u) return;
		if (firstVertex < 0 || (unsigned)firstVertex >= mesh->maxVerts) return;
		if (uniformBytes > 4096) return;
		if (!mesh->verts || !mesh->indices) return;
		if (CachedShaderFormats(dev) == SDL_GPU_SHADERFORMAT_INVALID) return;
		bool skinned = p.boneBuf != nullptr;
		if (p.shader) {
			SDL_GPUTextureFormat fmt = colorFormat ? (SDL_GPUTextureFormat)colorFormat : (win ? SDL_GetGPUSwapchainTextureFormat(dev, win) : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
			SDL_GPUTextureFormat depthFmt = depthFormat ? (SDL_GPUTextureFormat)depthFormat : PickMeshDepthFormat(dev);
			SDL_GPUGraphicsPipeline* pipe = ShaderPipeline(p.shader, dev, (int)fmt, (int)depthFmt, p.blend, p.zMode, (int)p.cull, p.wireframe, samples, skinned);
			if (!pipe) return;
			InvalidateMeshState();
			SDL_BindGPUGraphicsPipeline(pass, pipe);
			ShaderBindTextures(p.shader, dev, pass);
			SDL_PushGPUVertexUniformData(cmds, 0, uniforms, uniformBytes);
			ShaderPushUniforms(p.shader, cmds);
			if (skinned) SDL_BindGPUVertexStorageBuffers(pass, 0, &p.boneBuf, 1);
			SDL_GPUBufferBinding vb{};
			vb.buffer = mesh->verts;
			SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
			SDL_GPUBufferBinding ib{};
			ib.buffer = mesh->indices;
			SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_16BIT);
			SDL_DrawGPUIndexedPrimitives(pass, indexCount, 1, startIndex, firstVertex, 0);
			return;
		}
		bool cube0 = p.cube0 && p.tex != nullptr;
		bool cube1 = p.cube1 && p.tex1 != nullptr;
		bool multi = !cube0 && !cube1;
		bool twoTex = !multi && p.tex1 != nullptr;
		int multiStages = 0;
		if (multi) {
			multiStages = p.stageCount;
			if (multiStages < 1) multiStages = 1;
			if (multiStages > MESH_MAX_STAGES) multiStages = MESH_MAX_STAGES;
		}

		SDL_GPUGraphicsPipeline* meshPipe = EnsureMeshPipe(dev, win, mesh->vertStride, skinned, twoTex, colorFormat, depthFormat, p.blend, p.zMode, p.cull, p.wireframe, multiStages, cube0, cube1, samples);
		if (!meshPipe) return;
		if (!g_meshSamp) return;

		SDL_GPUTexture* boundTex = p.tex;
		if (!boundTex) {
			boundTex = EnsureWhiteTexture(dev);
			if (!boundTex) return;
		}

		SDL_GPUTextureSamplerBinding binds[MESH_MAX_STAGES]{};
		unsigned samplerCount = 0;
		if (multi) {
			samplerCount = (unsigned)multiStages;
			for (unsigned i = 0; i < samplerCount; ++i) {
				const MeshStage& s = p.stages[i];
				SDL_GPUTexture* t = s.tex;
				if (!t) t = boundTex;
				SDL_GPUSampler* sm = EnsureMeshSampler(dev, s.wrapU, s.wrapV, s.point, p.aniso, p.lodBias);
				binds[i].texture = t;
				binds[i].sampler = sm ? sm : g_meshSamp;
			}
		}
		else {
			SDL_GPUSampler* samp0 = EnsureMeshSampler(dev, p.wrapU0, p.wrapV0, p.point0, p.aniso, p.lodBias);
			binds[0].texture = boundTex;
			binds[0].sampler = samp0 ? samp0 : g_meshSamp;
			samplerCount = 1;
			if (twoTex) {
				SDL_GPUSampler* samp1 = EnsureMeshSampler(dev, p.wrapU1, p.wrapV1, p.point1, p.aniso, p.lodBias);
				binds[1].texture = p.tex1;
				binds[1].sampler = samp1 ? samp1 : binds[0].sampler;
				samplerCount = 2;
			}
			if (!binds[0].sampler) return;
		}

		bool bound = pass == g_lastMeshPass && meshPipe == g_lastMeshPipe && (int)samplerCount == g_lastMeshSamplerCount;
		if (bound) {
			for (unsigned i = 0; i < samplerCount; ++i) {
				if (binds[i].texture != g_lastMeshTex[i] || binds[i].sampler != g_lastMeshSamp[i]) { bound = false; break; }
			}
		}
		if (!bound) {
			SDL_BindGPUGraphicsPipeline(pass, meshPipe);
			SDL_BindGPUFragmentSamplers(pass, 0, binds, samplerCount);
			g_lastMeshPass = pass;
			g_lastMeshPipe = meshPipe;
			g_lastMeshSamplerCount = (int)samplerCount;
			for (unsigned i = 0; i < samplerCount; ++i) {
				g_lastMeshTex[i] = binds[i].texture;
				g_lastMeshSamp[i] = binds[i].sampler;
			}
		}

		SDL_PushGPUVertexUniformData(cmds, 0, uniforms, uniformBytes);
		PushMeshFragUniforms(cmds, p);
		if (skinned) SDL_BindGPUVertexStorageBuffers(pass, 0, &p.boneBuf, 1);
		SDL_GPUBufferBinding vb{};
		vb.buffer = mesh->verts;
		SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
		SDL_GPUBufferBinding ib{};
		ib.buffer = mesh->indices;
		SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_16BIT);
		SDL_DrawGPUIndexedPrimitives(pass, indexCount, 1, startIndex, firstVertex, 0);
	}

	static bool EnsureCanvasPipeline(SDL_GPUDevice* dev, SDL_GPUTextureFormat swapFormat) {
		GpuLock lock;
		if (g_canvasPipe && g_canvasSamp && g_canvasVB && g_canvasDev == dev && g_canvasFormat == swapFormat) return true;
		TeardownCanvas();
		SDL_GPUShaderFormat sup = SDL_GetGPUShaderFormats(dev);
		const uint8_t* vsCode = nullptr; const uint8_t* psCode = nullptr; size_t vsSize = 0, psSize = 0;
		SDL_GPUShaderFormat use = SDL_GPU_SHADERFORMAT_INVALID;
		if (sup & SDL_GPU_SHADERFORMAT_SPIRV) { use = SDL_GPU_SHADERFORMAT_SPIRV; vsCode = kCanvasVS_SPIRV; vsSize = kCanvasVS_SPIRV_size; psCode = kCanvasPS_SPIRV; psSize = kCanvasPS_SPIRV_size; }
		else if (sup & SDL_GPU_SHADERFORMAT_DXIL) { use = SDL_GPU_SHADERFORMAT_DXIL; vsCode = kCanvasVS_DXIL; vsSize = kCanvasVS_DXIL_size; psCode = kCanvasPS_DXIL; psSize = kCanvasPS_DXIL_size; }
		if (use == SDL_GPU_SHADERFORMAT_INVALID) {
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "No supported canvas shader format: %u", (unsigned)sup);
			return false;
		}
		SDL_GPUShader* vs = LoadShader(dev, use, SDL_GPU_SHADERSTAGE_VERTEX, "VSMain", vsCode, vsSize, 0, 0);
		if (!vs) {
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Canvas VS failed: %s", SDL_GetError());
			return false;
		}
		SDL_GPUShader* ps = LoadShader(dev, use, SDL_GPU_SHADERSTAGE_FRAGMENT, "PSMain", psCode, psSize, 1, 0);
		if (!ps) { SDL_ReleaseGPUShader(dev, vs); SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Canvas PS failed: %s", SDL_GetError()); return false; }
		SDL_GPUVertexBufferDescription vb{}; vb.slot = 0; vb.pitch = 16; vb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
		SDL_GPUVertexAttribute attrs[2]{};
		attrs[0].location = 0; attrs[0].buffer_slot = 0; attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2; attrs[0].offset = 0;
		attrs[1].location = 1; attrs[1].buffer_slot = 0; attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2; attrs[1].offset = 8;
		SDL_GPUVertexInputState vin{}; vin.vertex_buffer_descriptions = &vb; vin.num_vertex_buffers = 1; vin.vertex_attributes = attrs; vin.num_vertex_attributes = 2;
		SDL_GPUColorTargetDescription tgt{}; tgt.format = swapFormat;
		tgt.blend_state.enable_blend = true;
		tgt.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA; tgt.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA; tgt.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
		tgt.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE; tgt.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA; tgt.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
		SDL_GPUGraphicsPipelineCreateInfo info{}; info.vertex_shader = vs; info.fragment_shader = ps; info.vertex_input_state = vin;
		info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
		info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL; info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
		info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
		info.rasterizer_state.enable_depth_clip = true;
		info.rasterizer_state.enable_depth_bias = false;
		info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
		info.multisample_state.sample_mask = 0;
		info.depth_stencil_state.enable_depth_test = false; info.depth_stencil_state.enable_depth_write = false;
		info.depth_stencil_state.enable_stencil_test = false;
		info.depth_stencil_state.back_stencil_state.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
		info.depth_stencil_state.front_stencil_state.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
		info.target_info.num_color_targets = 1; info.target_info.color_target_descriptions = &tgt; info.target_info.has_depth_stencil_target = false;
		SDL_PropertiesID canvasProps = SDL_CreateProperties();
		if (canvasProps) SDL_SetStringProperty(canvasProps, SDL_PROP_GPU_GRAPHICSPIPELINE_CREATE_NAME_STRING, "b3d_canvas");
		info.props = canvasProps;
		g_canvasPipe = SDL_CreateGPUGraphicsPipeline(dev, &info);
		if (canvasProps) SDL_DestroyProperties(canvasProps);
		info.props = 0;
		SDL_ReleaseGPUShader(dev, vs); SDL_ReleaseGPUShader(dev, ps);
		if (!g_canvasPipe) {
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Canvas pipeline failed: %s", SDL_GetError());
			return false;
		}
		SDL_GPUSamplerCreateInfo samp{}; samp.min_filter = SDL_GPU_FILTER_NEAREST; samp.mag_filter = SDL_GPU_FILTER_NEAREST; samp.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE; samp.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		g_canvasSamp = SDL_CreateGPUSampler(dev, &samp);
		if (!g_canvasSamp) { TeardownCanvas(); return false; }
		g_canvasDev = dev; g_canvasFormat = swapFormat;
		return EnsureCanvasVertices(dev);
	}

	static bool EnsureCanvasVertices(SDL_GPUDevice* dev) {
		GpuLock lock;
		if (g_canvasVB && g_canvasDev == dev) return true;
		if (g_canvasVB) { SDL_ReleaseGPUBuffer(g_canvasDev, g_canvasVB); g_canvasVB = nullptr; }
		struct V { float x, y, u, v; };
		V verts[6] = { {-1,-1,0,1},{1,-1,1,1},{1,1,1,0},{-1,-1,0,1},{1,1,1,0},{-1,1,0,0} };
		SDL_GPUBufferCreateInfo bi{}; bi.usage = SDL_GPU_BUFFERUSAGE_VERTEX; bi.size = sizeof(verts);
		g_canvasVB = SDL_CreateGPUBuffer(dev, &bi);
		if (!g_canvasVB) return false;
		SDL_GPUTransferBuffer* tb = AcquireUploadTransferBuffer(dev, sizeof(verts));
		if (!tb) { SDL_ReleaseGPUBuffer(dev, g_canvasVB); g_canvasVB = nullptr; return false; }
		void* dst = SDL_MapGPUTransferBuffer(dev, tb, true);
		if (!dst) { ReleaseUploadTransferBuffer(dev, tb); SDL_ReleaseGPUBuffer(dev, g_canvasVB); g_canvasVB = nullptr; return false; }
		memcpy(dst, verts, sizeof(verts)); SDL_UnmapGPUTransferBuffer(dev, tb);
		SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(dev); if (!cb) { ReleaseUploadTransferBuffer(dev, tb); SDL_ReleaseGPUBuffer(dev, g_canvasVB); g_canvasVB = nullptr; return false; }
		SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cb);
		SDL_GPUTransferBufferLocation src{}; src.transfer_buffer = tb;
		SDL_GPUBufferRegion reg{}; reg.buffer = g_canvasVB; reg.size = sizeof(verts);
		SDL_UploadToGPUBuffer(cp, &src, &reg, true);
		SDL_EndGPUCopyPass(cp);
		bool ok = SDL_SubmitGPUCommandBuffer(cb);
		ReleaseUploadTransferBuffer(dev, tb);
		if (!ok) { SDL_ReleaseGPUBuffer(dev, g_canvasVB); g_canvasVB = nullptr; return false; }
		return true;
	}

	void DrawCanvasOverlay(SDL_GPUDevice* dev, SDL_Window* win, SDL_GPURenderPass* pass, SDL_GPUTexture* tex) {
		GpuLock lock;
		if (!dev || !pass || !tex) return;
		SDL_GPUTextureFormat fmt = win ? SDL_GetGPUSwapchainTextureFormat(dev, win) : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
		if (!EnsureCanvasPipeline(dev, fmt)) return;
		if (!EnsureCanvasVertices(dev)) return;
		if (!g_canvasPipe || !g_canvasVB || !g_canvasSamp) return;
		InvalidateMeshState();
		SDL_BindGPUGraphicsPipeline(pass, g_canvasPipe);
		SDL_GPUBufferBinding vb{}; vb.buffer = g_canvasVB; SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
		SDL_GPUTextureSamplerBinding b{}; b.texture = tex; b.sampler = g_canvasSamp;
		SDL_BindGPUFragmentSamplers(pass, 0, &b, 1);
		SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
	}

	namespace {
		SDL_GPUDevice* g_gammaDev = nullptr;
		SDL_GPUTexture* g_gammaLut = nullptr;
		SDL_GPUGraphicsPipeline* g_gammaPipe = nullptr;
		SDL_GPUDevice* g_gammaPipeDev = nullptr;
		SDL_GPUTextureFormat g_gammaFmt = SDL_GPU_TEXTUREFORMAT_INVALID;
		SDL_GPUSampler* g_gammaSrcSamp = nullptr;
		SDL_GPUSampler* g_gammaLutSamp = nullptr;
		SDL_GPUTexture* g_gammaComp = nullptr;
		SDL_GPUDevice* g_gammaCompDev = nullptr;
		Uint32 g_gammaCompW = 0, g_gammaCompH = 0;
		SDL_GPUTextureFormat g_gammaCompFmt = SDL_GPU_TEXTUREFORMAT_INVALID;
		bool g_gammaActive = false;

		void TeardownGamma() {
			GpuLock lock;
			if (g_gammaComp && g_gammaCompDev) SDL_ReleaseGPUTexture(g_gammaCompDev, g_gammaComp);
			g_gammaComp = nullptr; g_gammaCompDev = nullptr;
			g_gammaCompW = g_gammaCompH = 0; g_gammaCompFmt = SDL_GPU_TEXTUREFORMAT_INVALID;
			if (g_gammaPipe && g_gammaPipeDev) SDL_ReleaseGPUGraphicsPipeline(g_gammaPipeDev, g_gammaPipe);
			g_gammaPipe = nullptr; g_gammaPipeDev = nullptr; g_gammaFmt = SDL_GPU_TEXTUREFORMAT_INVALID;
			if (g_gammaLut && g_gammaDev) SDL_ReleaseGPUTexture(g_gammaDev, g_gammaLut);
			g_gammaLut = nullptr;
			if (g_gammaSrcSamp && g_gammaDev) SDL_ReleaseGPUSampler(g_gammaDev, g_gammaSrcSamp);
			if (g_gammaLutSamp && g_gammaDev) SDL_ReleaseGPUSampler(g_gammaDev, g_gammaLutSamp);
			g_gammaSrcSamp = nullptr; g_gammaLutSamp = nullptr;
			g_gammaDev = nullptr;
			g_gammaActive = false;
		}

		bool EnsureGammaSamplers(SDL_GPUDevice* dev) {
			if (!g_gammaSrcSamp) {
				SDL_GPUSamplerCreateInfo s{};
				s.min_filter = SDL_GPU_FILTER_LINEAR; s.mag_filter = SDL_GPU_FILTER_LINEAR;
				s.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
				s.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
				s.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
				s.max_lod = 0.0f;
				g_gammaSrcSamp = SDL_CreateGPUSampler(dev, &s);
				if (!g_gammaSrcSamp) return false;
			}
			if (!g_gammaLutSamp) {
				SDL_GPUSamplerCreateInfo s{};
				s.min_filter = SDL_GPU_FILTER_NEAREST; s.mag_filter = SDL_GPU_FILTER_NEAREST;
				s.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
				s.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
				s.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
				s.max_lod = 0.0f;
				g_gammaLutSamp = SDL_CreateGPUSampler(dev, &s);
				if (!g_gammaLutSamp) return false;
			}
			return true;
		}

		bool EnsureGammaPipeline(SDL_GPUDevice* dev, SDL_GPUTextureFormat fmt) {
			if (g_gammaPipe && g_gammaPipeDev == dev && g_gammaFmt == fmt) return true;
			if (g_gammaPipe && g_gammaPipeDev) SDL_ReleaseGPUGraphicsPipeline(g_gammaPipeDev, g_gammaPipe);
			g_gammaPipe = nullptr; g_gammaFmt = SDL_GPU_TEXTUREFORMAT_INVALID;
			SDL_GPUShaderFormat sup = CachedShaderFormats(dev);
			const uint8_t* vsCode = nullptr; const uint8_t* psCode = nullptr;
			size_t vsSize = 0, psSize = 0;
			SDL_GPUShaderFormat use = SDL_GPU_SHADERFORMAT_INVALID;
			if (sup & SDL_GPU_SHADERFORMAT_SPIRV) { use = SDL_GPU_SHADERFORMAT_SPIRV; vsCode = kCanvasVS_SPIRV; vsSize = kCanvasVS_SPIRV_size; psCode = kPresentPS_SPIRV; psSize = kPresentPS_SPIRV_size; }
			else if (sup & SDL_GPU_SHADERFORMAT_DXIL) { use = SDL_GPU_SHADERFORMAT_DXIL; vsCode = kCanvasVS_DXIL; vsSize = kCanvasVS_DXIL_size; psCode = kPresentPS_DXIL; psSize = kPresentPS_DXIL_size; }
			if (use == SDL_GPU_SHADERFORMAT_INVALID) return false;
			SDL_GPUShader* vs = LoadShader(dev, use, SDL_GPU_SHADERSTAGE_VERTEX, "VSMain", vsCode, vsSize, 0, 0);
			if (!vs) return false;
			SDL_GPUShader* ps = LoadShader(dev, use, SDL_GPU_SHADERSTAGE_FRAGMENT, "PSMain", psCode, psSize, 2, 0);
			if (!ps) { SDL_ReleaseGPUShader(dev, vs); return false; }
			SDL_GPUVertexBufferDescription vb{}; vb.slot = 0; vb.pitch = 16; vb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
			SDL_GPUVertexAttribute attrs[2]{};
			attrs[0].location = 0; attrs[0].buffer_slot = 0; attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2; attrs[0].offset = 0;
			attrs[1].location = 1; attrs[1].buffer_slot = 0; attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2; attrs[1].offset = 8;
			SDL_GPUVertexInputState vin{}; vin.vertex_buffer_descriptions = &vb; vin.num_vertex_buffers = 1; vin.vertex_attributes = attrs; vin.num_vertex_attributes = 2;
			SDL_GPUColorTargetDescription tgt{}; tgt.format = fmt;
			SDL_GPUGraphicsPipelineCreateInfo info{};
			info.vertex_shader = vs; info.fragment_shader = ps; info.vertex_input_state = vin;
			info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
			info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL; info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
			info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
			info.rasterizer_state.enable_depth_clip = true;
			info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
			info.depth_stencil_state.enable_depth_test = false; info.depth_stencil_state.enable_depth_write = false;
			info.depth_stencil_state.enable_stencil_test = false;
			info.depth_stencil_state.back_stencil_state.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
			info.depth_stencil_state.front_stencil_state.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
			info.target_info.num_color_targets = 1; info.target_info.color_target_descriptions = &tgt;
			info.target_info.has_depth_stencil_target = false;
			SDL_PropertiesID props = SDL_CreateProperties();
			if (props) SDL_SetStringProperty(props, SDL_PROP_GPU_GRAPHICSPIPELINE_CREATE_NAME_STRING, "b3d_gamma");
			info.props = props;
			g_gammaPipe = SDL_CreateGPUGraphicsPipeline(dev, &info);
			if (props) SDL_DestroyProperties(props);
			SDL_ReleaseGPUShader(dev, vs); SDL_ReleaseGPUShader(dev, ps);
			if (!g_gammaPipe) return false;
			g_gammaPipeDev = dev; g_gammaFmt = fmt;
			return true;
		}
	}

	void SetGammaRamp(SDL_GPUDevice* dev, const unsigned short* ramp) {
		GpuLock lock;
		if (!dev || !ramp) return;
		if (g_gammaDev && g_gammaDev != dev) TeardownGamma();
		g_gammaDev = dev;
		bool ident = true;
		for (int ch = 0; ch < 3 && ident; ++ch) {
			for (int i = 0; i < 256; ++i) {
				if (ramp[ch * 256 + i] != (unsigned short)(i * 257)) { ident = false; break; }
			}
		}
		g_gammaActive = false;
		if (ident) return;
		if (!g_gammaLut) {
			SDL_GPUTextureCreateInfo ti{};
			ti.type = SDL_GPU_TEXTURETYPE_2D;
			ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
			ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
			ti.width = 256; ti.height = 1; ti.layer_count_or_depth = 1; ti.num_levels = 1;
			ti.sample_count = SDL_GPU_SAMPLECOUNT_1;
			g_gammaLut = SDL_CreateGPUTexture(dev, &ti);
			if (!g_gammaLut) return;
		}
		unsigned char px[256 * 4];
		for (int i = 0; i < 256; ++i) {
			px[i * 4 + 0] = (unsigned char)(ramp[i] >> 8);
			px[i * 4 + 1] = (unsigned char)(ramp[256 + i] >> 8);
			px[i * 4 + 2] = (unsigned char)(ramp[512 + i] >> 8);
			px[i * 4 + 3] = 255;
		}
		SDL_GPUCommandBuffer* cmds = SDL_AcquireGPUCommandBuffer(dev);
		if (!cmds) return;
		SDL_GPUTransferBuffer* tb = AcquireUploadTransferBuffer(dev, sizeof(px));
		if (!tb) { SDL_CancelGPUCommandBuffer(cmds); return; }
		void* dst = SDL_MapGPUTransferBuffer(dev, tb, true);
		if (!dst) { ReleaseUploadTransferBuffer(dev, tb); SDL_CancelGPUCommandBuffer(cmds); return; }
		memcpy(dst, px, sizeof(px));
		SDL_UnmapGPUTransferBuffer(dev, tb);
		SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmds);
		if (!cp) { ReleaseUploadTransferBuffer(dev, tb); SDL_CancelGPUCommandBuffer(cmds); return; }
		SDL_GPUTextureTransferInfo src{};
		src.transfer_buffer = tb;
		src.pixels_per_row = 256;
		src.rows_per_layer = 1;
		SDL_GPUTextureRegion reg{};
		reg.texture = g_gammaLut;
		reg.w = 256; reg.h = 1; reg.d = 1;
		SDL_UploadToGPUTexture(cp, &src, &reg, true);
		SDL_EndGPUCopyPass(cp);
		bool ok = SDL_SubmitGPUCommandBuffer(cmds);
		ReleaseUploadTransferBuffer(dev, tb);
		if (ok) g_gammaActive = true;
	}

	bool GammaActive(SDL_GPUDevice* dev) {
		GpuLock lock;
		return g_gammaActive && g_gammaDev == dev && g_gammaLut != nullptr;
	}

	SDL_GPUTexture* AcquireGammaComposite(SDL_GPUDevice* dev, SDL_GPUTextureFormat fmt, unsigned w, unsigned h) {
		GpuLock lock;
		if (!dev || !w || !h) return nullptr;
		if (g_gammaComp && g_gammaCompDev == dev && g_gammaCompW == w && g_gammaCompH == h && g_gammaCompFmt == fmt) return g_gammaComp;
		if (g_gammaComp && g_gammaCompDev) SDL_ReleaseGPUTexture(g_gammaCompDev, g_gammaComp);
		g_gammaComp = nullptr;
		SDL_GPUTextureCreateInfo ti{};
		ti.type = SDL_GPU_TEXTURETYPE_2D;
		ti.format = fmt;
		ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
		ti.width = w; ti.height = h; ti.layer_count_or_depth = 1; ti.num_levels = 1;
		ti.sample_count = SDL_GPU_SAMPLECOUNT_1;
		g_gammaComp = SDL_CreateGPUTexture(dev, &ti);
		if (!g_gammaComp) { g_gammaCompDev = nullptr; return nullptr; }
		g_gammaCompDev = dev; g_gammaCompW = w; g_gammaCompH = h; g_gammaCompFmt = fmt;
		return g_gammaComp;
	}

	bool GammaBlit(SDL_GPUDevice* dev, SDL_GPUCommandBuffer* cmds, SDL_GPUTexture* src, SDL_GPUTexture* dst, unsigned w, unsigned h, SDL_GPUTextureFormat fmt) {
		GpuLock lock;
		if (!dev || !cmds || !src || !dst || !g_gammaLut || !g_gammaActive) return false;
		if (!EnsureCanvasPipeline(dev, fmt)) return false;
		if (!g_canvasVB) return false;
		if (!EnsureGammaSamplers(dev)) return false;
		if (!EnsureGammaPipeline(dev, fmt)) return false;
		SDL_GPUColorTargetInfo ci{};
		ci.texture = dst;
		ci.load_op = SDL_GPU_LOADOP_DONT_CARE;
		ci.store_op = SDL_GPU_STOREOP_STORE;
		ci.cycle = false;
		SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmds, &ci, 1, nullptr);
		if (!pass) return false;
		SDL_GPUViewport vp{};
		vp.x = 0; vp.y = 0; vp.w = (float)w; vp.h = (float)h;
		vp.min_depth = 0.0f; vp.max_depth = 1.0f;
		SDL_SetGPUViewport(pass, &vp);
		SDL_Rect sc{};
		sc.x = 0; sc.y = 0; sc.w = (int)w; sc.h = (int)h;
		SDL_SetGPUScissor(pass, &sc);
		InvalidateMeshState();
		SDL_BindGPUGraphicsPipeline(pass, g_gammaPipe);
		SDL_GPUBufferBinding vb{}; vb.buffer = g_canvasVB;
		SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
		SDL_GPUTextureSamplerBinding binds[2]{};
		binds[0].texture = src; binds[0].sampler = g_gammaSrcSamp;
		binds[1].texture = g_gammaLut; binds[1].sampler = g_gammaLutSamp;
		SDL_BindGPUFragmentSamplers(pass, 0, binds, 2);
		SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
		SDL_EndGPURenderPass(pass);
		return true;
	}

	void TeardownPipelines() {
		ShutdownUploads();
		GpuLock lock;
		TeardownBlit();
		TeardownMeshPipe();
		TeardownCanvas();
		TeardownGamma();
		TeardownWhiteTexture();
		TeardownText();
		ReleaseBones(nullptr);
		ClearTransferPool(nullptr);
	}

	namespace {
		struct PooledEntry {
			SDL_GPUDevice* dev = nullptr;
			SDL_GPUTransferBuffer* buf = nullptr;
			Uint32 size = 0;
			SDL_GPUTransferBufferUsage usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
		};
		std::vector<PooledEntry> g_transferPool;
		std::unordered_map<SDL_GPUTransferBuffer*, PooledEntry> g_transferSizes;
	}

	void ClearTransferPool(SDL_GPUDevice* dev) {
		GpuLock lock;
		for (auto it = g_transferPool.begin(); it != g_transferPool.end(); ) {
			if (!dev || it->dev == dev) {
				SDL_ReleaseGPUTransferBuffer(it->dev, it->buf);
				g_transferSizes.erase(it->buf);
				it = g_transferPool.erase(it);
			} else ++it;
		}
		if (!dev) {
			g_transferPool.clear();
			g_transferSizes.clear();
		}
	}

	static SDL_GPUTransferBuffer* AcquireTransferBuffer(SDL_GPUDevice* dev, Uint32 size, SDL_GPUTransferBufferUsage usage) {
		GpuLock lock;
		if (!dev || !size) return nullptr;
		for (auto it = g_transferPool.begin(); it != g_transferPool.end(); ++it) {
			if (it->dev == dev && it->usage == usage && it->size >= size) {
				SDL_GPUTransferBuffer* buf = it->buf;
				g_transferPool.erase(it);
				g_transferSizes.erase(buf);
				return buf;
			}
		}
		SDL_GPUTransferBufferCreateInfo ci{};
		ci.usage = usage;
		ci.size = size;
		SDL_GPUTransferBuffer* buf = SDL_CreateGPUTransferBuffer(dev, &ci);
		if (buf) g_transferSizes[buf] = PooledEntry{ dev, buf, size, usage };
		return buf;
	}

	SDL_GPUTransferBuffer* AcquireUploadTransferBuffer(SDL_GPUDevice* dev, Uint32 size) {
		return AcquireTransferBuffer(dev, size, SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD);
	}

	SDL_GPUTransferBuffer* AcquireDownloadTransferBuffer(SDL_GPUDevice* dev, Uint32 size) {
		return AcquireTransferBuffer(dev, size, SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD);
	}

	static void ReleaseTransferBuffer(SDL_GPUDevice* dev, SDL_GPUTransferBuffer* buf) {
		GpuLock lock;
		if (!dev || !buf) return;
		auto sit = g_transferSizes.find(buf);
		Uint32 size = (sit != g_transferSizes.end() && sit->second.dev == dev) ? sit->second.size : 0;
		SDL_GPUTransferBufferUsage usage = (sit != g_transferSizes.end()) ? sit->second.usage : SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
		static const Uint32 kMaxPooledBytes = 4u * 1024u * 1024u;
		if (!size || size > kMaxPooledBytes) {
			g_transferSizes.erase(buf);
			SDL_ReleaseGPUTransferBuffer(dev, buf);
			return;
		}
		if (g_transferPool.size() >= 8) {
			SDL_GPUTransferBuffer* old = g_transferPool.front().buf;
			SDL_GPUDevice* oldDev = g_transferPool.front().dev;
			SDL_ReleaseGPUTransferBuffer(oldDev, old);
			g_transferSizes.erase(old);
			g_transferPool.erase(g_transferPool.begin());
		}
		g_transferPool.push_back(PooledEntry{ dev, buf, size, usage });
	}

	void ReleaseUploadTransferBuffer(SDL_GPUDevice* dev, SDL_GPUTransferBuffer* buf) {
		ReleaseTransferBuffer(dev, buf);
	}

	void ReleaseDownloadTransferBuffer(SDL_GPUDevice* dev, SDL_GPUTransferBuffer* buf) {
		ReleaseTransferBuffer(dev, buf);
	}

}