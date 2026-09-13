#include "sdl_gpu_pipeline.h"
#include "sdl_gpu_mesh.h"

#include "../std.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_log.h>

#include "shaders/mesh_shaders.h"
#include "shaders/canvas_shaders.h"
#include "sdl_gpu_text.h"

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
		if (g_blitTex && g_blitDev) SDL_ReleaseGPUTexture(g_blitDev, g_blitTex);
		g_blitTex = nullptr;
		g_blitDev = nullptr;
		g_blitW = g_blitH = 0;
		g_blitHasData = false;
	}

	static bool EnsureBlitTexture(SDL_GPUDevice* dev, unsigned w, unsigned h) {
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
			int blend = MESH_BLEND_REPLACE;
			int zMode = MESH_Z_NORMAL;
			SDL_GPUCullMode cullMode = SDL_GPU_CULLMODE_NONE;
			bool wireframe = false;
			bool operator==(const MeshPipeKey& o) const {
				return format == o.format && depthFormat == o.depthFormat && stride == o.stride &&
					skinned == o.skinned && twoTex == o.twoTex &&
					blend == o.blend && zMode == o.zMode && cullMode == o.cullMode && wireframe == o.wireframe;
			}
		};
		struct MeshPipeEntry { MeshPipeKey key; SDL_GPUGraphicsPipeline* pipe = nullptr; };
		std::vector<MeshPipeEntry> g_meshPipes;

		SDL_GPUDevice* g_canvasDev = nullptr;
		SDL_GPUGraphicsPipeline* g_canvasPipe = nullptr;
		SDL_GPUSampler* g_canvasSamp = nullptr;
		SDL_GPUBuffer* g_canvasVB = nullptr;
		SDL_GPUTextureFormat g_canvasFormat = SDL_GPU_TEXTUREFORMAT_INVALID;

	}

	static void TeardownMeshPipe() {
		for (auto& e : g_meshPipes) if (e.pipe && g_meshDev) SDL_ReleaseGPUGraphicsPipeline(g_meshDev, e.pipe);
		g_meshPipes.clear();
		if (g_meshSamp && g_meshDev) SDL_ReleaseGPUSampler(g_meshDev, g_meshSamp);
		g_meshSamp = nullptr;
		g_meshDev = nullptr;
	}

	static void TeardownCanvas() {
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
		if (g_whiteTex && g_whiteDev) SDL_ReleaseGPUTexture(g_whiteDev, g_whiteTex);
		g_whiteTex = nullptr;
		g_whiteDev = nullptr;
	}

	static SDL_GPUTextureFormat PickMeshDepthFormat(SDL_GPUDevice* dev) {
		if (SDL_GPUTextureSupportsFormat(dev, SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET))
			return SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
		if (SDL_GPUTextureSupportsFormat(dev, SDL_GPU_TEXTUREFORMAT_D24_UNORM, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET))
			return SDL_GPU_TEXTUREFORMAT_D24_UNORM;
		if (SDL_GPUTextureSupportsFormat(dev, SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET))
			return SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
		if (SDL_GPUTextureSupportsFormat(dev, SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET))
			return SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;
		if (SDL_GPUTextureSupportsFormat(dev, SDL_GPU_TEXTUREFORMAT_D16_UNORM, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET))
			return SDL_GPU_TEXTUREFORMAT_D16_UNORM;
		return SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
	}

	int MeshDepthFormat(SDL_GPUDevice* dev) {
		return (int)PickMeshDepthFormat(dev);
	}

	int SceneColorFormat() {
		return (int)SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	}

	static SDL_GPUTexture* EnsureWhiteTexture(SDL_GPUDevice* dev) {
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

	static SDL_GPUGraphicsPipeline* EnsureMeshPipe(SDL_GPUDevice* dev, SDL_Window* win, unsigned stride, bool skinned, bool twoTex, int colorFormatOverride, int depthFormatOverride, int blendMode, int zMode, SDL_GPUCullMode cullMode, bool wireframe) {
		SDL_GPUTextureFormat fmt = colorFormatOverride ? (SDL_GPUTextureFormat)colorFormatOverride : SDL_GetGPUSwapchainTextureFormat(dev, win);
		SDL_GPUTextureFormat depthFmt = depthFormatOverride ? (SDL_GPUTextureFormat)depthFormatOverride : PickMeshDepthFormat(dev);
		if (g_meshDev && g_meshDev != dev) TeardownMeshPipe();
		if (blendMode < MESH_BLEND_REPLACE || blendMode > MESH_BLEND_ADD) blendMode = MESH_BLEND_ALPHA;
		if (zMode < MESH_Z_NORMAL || zMode > MESH_Z_CMPONLY) zMode = MESH_Z_NORMAL;

		MeshPipeKey key{ fmt, depthFmt, stride, skinned, twoTex, blendMode, zMode, cullMode, wireframe };
		for (auto& e : g_meshPipes) {
			if (e.key == key) return e.pipe;
		}

		SDL_GPUShaderFormat supported = SDL_GetGPUShaderFormats(dev);
		const uint8_t* vsCode = nullptr;
		const uint8_t* psCode = nullptr;
		size_t vsSize = 0, psSize = 0;
		const char* vsEntry = skinned ? "VSMainSkinned" : "VSMain";
		const char* psEntry = twoTex ? "PSMain2Tex" : "PSMain";
		SDL_GPUShaderFormat useFmt = SDL_GPU_SHADERFORMAT_INVALID;
		if (supported & SDL_GPU_SHADERFORMAT_SPIRV) {
			useFmt = SDL_GPU_SHADERFORMAT_SPIRV;
			if (skinned) { vsCode = kSkinVS_SPIRV; vsSize = kSkinVS_SPIRV_size; }
			else { vsCode = kMeshVS_SPIRV; vsSize = kMeshVS_SPIRV_size; }
			if (twoTex) { psCode = kMeshPS2_SPIRV; psSize = kMeshPS2_SPIRV_size; }
			else { psCode = kMeshPS_SPIRV; psSize = kMeshPS_SPIRV_size; }
		}
		else if (supported & SDL_GPU_SHADERFORMAT_DXIL) {
			useFmt = SDL_GPU_SHADERFORMAT_DXIL;
			if (skinned) { vsCode = kSkinVS_DXIL; vsSize = kSkinVS_DXIL_size; }
			else { vsCode = kMeshVS_DXIL; vsSize = kMeshVS_DXIL_size; }
			if (twoTex) { psCode = kMeshPS2_DXIL; psSize = kMeshPS2_DXIL_size; }
			else { psCode = kMeshPS_DXIL; psSize = kMeshPS_DXIL_size; }
		}
		if (useFmt == SDL_GPU_SHADERFORMAT_INVALID) {
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "No supported mesh shader format: %u", (unsigned)supported);
			return nullptr;
		}

		SDL_GPUShader* vs = LoadShader(dev, useFmt, SDL_GPU_SHADERSTAGE_VERTEX, vsEntry, vsCode, vsSize, 0, 1, skinned ? 1 : 0);
		if (!vs) return nullptr;
		SDL_GPUShader* ps = LoadShader(dev, useFmt, SDL_GPU_SHADERSTAGE_FRAGMENT, psEntry, psCode, psSize, twoTex ? 2 : 1, twoTex ? 1 : 0);
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
		info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
		info.rasterizer_state.cull_mode = cullMode;
		info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;
		info.rasterizer_state.enable_depth_clip = true;
		info.rasterizer_state.enable_depth_bias = false;
		info.rasterizer_state.depth_bias_constant_factor = 0.0f;
		info.rasterizer_state.depth_bias_clamp = 0.0f;
		info.rasterizer_state.depth_bias_slope_factor = 0.0f;
		info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
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

		SDL_GPUGraphicsPipeline* newPipe = SDL_CreateGPUGraphicsPipeline(dev, &info);
		SDL_ReleaseGPUShader(dev, vs);
		SDL_ReleaseGPUShader(dev, ps);
		if (!newPipe) {
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_CreateGPUGraphicsPipeline mesh failed: %s", SDL_GetError());
			return nullptr;
		}
		if (g_meshPipes.size() >= 24) {
			SDL_GPUGraphicsPipeline* old = g_meshPipes.front().pipe;
			if (old) SDL_ReleaseGPUGraphicsPipeline(dev, old);
			g_meshPipes.erase(g_meshPipes.begin());
		}

		if (!g_meshSamp) {
			SDL_GPUSamplerCreateInfo samp{};
			samp.min_filter = SDL_GPU_FILTER_LINEAR;
			samp.mag_filter = SDL_GPU_FILTER_LINEAR;
			samp.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
			samp.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
			g_meshSamp = SDL_CreateGPUSampler(dev, &samp);
			if (!g_meshSamp) { SDL_ReleaseGPUGraphicsPipeline(dev, newPipe); return nullptr; }
		}

		g_meshDev = dev;
		g_meshPipes.push_back({ key, newPipe });
		return newPipe;
	}

	void DrawMesh(SDL_GPUDevice* dev, SDL_Window* win, SDL_GPUCommandBuffer* cmds, SDL_GPURenderPass* pass, GpuMesh* mesh, const float* uniforms, unsigned uniformBytes, unsigned indexCount, unsigned startIndex, int firstVertex, int colorFormat, int depthFormat, const MeshDrawParams& p) {
		if (!dev || !cmds || !pass || !mesh || !uniforms || !uniformBytes || !indexCount) return;
		if (!colorFormat && !win) return;
		if (startIndex + indexCount > mesh->maxTris * 3u) return;
		if (firstVertex < 0 || (unsigned)firstVertex >= mesh->maxVerts) return;
		if (uniformBytes > 4096) return;
		if (!mesh->verts || !mesh->indices) return;
		if (SDL_GetGPUShaderFormats(dev) == SDL_GPU_SHADERFORMAT_INVALID) return;
		bool skinned = p.boneBuf != nullptr;
		bool twoTex = p.tex1 != nullptr;

		SDL_GPUGraphicsPipeline* meshPipe = EnsureMeshPipe(dev, win, mesh->vertStride, skinned, twoTex, colorFormat, depthFormat, p.blend, p.zMode, p.cull, p.wireframe);
		if (!meshPipe) return;
		if (!g_meshSamp) return;

		SDL_GPUTexture* boundTex = p.tex;
		if (!boundTex) {
			boundTex = EnsureWhiteTexture(dev);
			if (!boundTex) return;
		}

		SDL_BindGPUGraphicsPipeline(pass, meshPipe);
		SDL_PushGPUVertexUniformData(cmds, 0, uniforms, uniformBytes);
		SDL_GPUBufferBinding vb{};
		vb.buffer = mesh->verts;
		SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
		SDL_GPUBufferBinding ib{};
		ib.buffer = mesh->indices;
		SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_16BIT);
		SDL_GPUTextureSamplerBinding bind{};
		bind.texture = boundTex;
		bind.sampler = g_meshSamp;
		SDL_BindGPUFragmentSamplers(pass, 0, &bind, 1);
		SDL_DrawGPUIndexedPrimitives(pass, indexCount, 1, startIndex, firstVertex, 0);
	}

	static bool EnsureCanvasPipeline(SDL_GPUDevice* dev, SDL_GPUTextureFormat swapFormat) {
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
		g_canvasPipe = SDL_CreateGPUGraphicsPipeline(dev, &info);
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
		if (!dev || !pass || !tex) return;
		SDL_GPUTextureFormat fmt = win ? SDL_GetGPUSwapchainTextureFormat(dev, win) : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
		if (!EnsureCanvasPipeline(dev, fmt)) return;
		if (!EnsureCanvasVertices(dev)) return;
		if (!g_canvasPipe || !g_canvasVB || !g_canvasSamp) return;
		SDL_BindGPUGraphicsPipeline(pass, g_canvasPipe);
		SDL_GPUBufferBinding vb{}; vb.buffer = g_canvasVB; SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
		SDL_GPUTextureSamplerBinding b{}; b.texture = tex; b.sampler = g_canvasSamp;
		SDL_BindGPUFragmentSamplers(pass, 0, &b, 1);
		SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
	}

	void TeardownPipelines() {
		TeardownBlit();
		TeardownMeshPipe();
		TeardownCanvas();
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
		};
		std::vector<PooledEntry> g_transferPool;
		std::unordered_map<SDL_GPUTransferBuffer*, PooledEntry> g_transferSizes;
	}

	void ClearTransferPool(SDL_GPUDevice* dev) {
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

	SDL_GPUTransferBuffer* AcquireUploadTransferBuffer(SDL_GPUDevice* dev, Uint32 size) {
		if (!dev || !size) return nullptr;
		for (auto it = g_transferPool.begin(); it != g_transferPool.end(); ++it) {
			if (it->dev == dev && it->size >= size) {
				SDL_GPUTransferBuffer* buf = it->buf;
				g_transferPool.erase(it);
				return buf;
			}
		}
		SDL_GPUTransferBufferCreateInfo ci{};
		ci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
		ci.size = size;
		SDL_GPUTransferBuffer* buf = SDL_CreateGPUTransferBuffer(dev, &ci);
		if (buf) g_transferSizes[buf] = PooledEntry{ dev, buf, size };
		return buf;
	}
	void ReleaseUploadTransferBuffer(SDL_GPUDevice* dev, SDL_GPUTransferBuffer* buf) {
		if (!dev || !buf) return;
		auto sit = g_transferSizes.find(buf);
		Uint32 size = (sit != g_transferSizes.end() && sit->second.dev == dev) ? sit->second.size : 0;
		if (!size) {
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
		g_transferPool.push_back(PooledEntry{ dev, buf, size });
	}

}