#include "sdl_gpu_shader.h"
#include "sdl_gpu_lock.h"
#include "sdl_gpu_pipeline.h"
#include "sdl_gpu_shader_data.h"

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_log.h>

#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "shaders/mesh_shaders.h"

namespace sdlgpu {

	namespace {
		std::string g_shaderError;
	}

	struct RuntimeParamBuffer {
		unsigned binding = 0;
		bool fragment = true;
		std::vector<unsigned char> data;
		std::unordered_map<std::string, ShaderParam> params;
	};

	struct ShaderPipeKey {
		SDL_GPUTextureFormat color = SDL_GPU_TEXTUREFORMAT_INVALID;
		SDL_GPUTextureFormat depth = SDL_GPU_TEXTUREFORMAT_INVALID;
		int blend = 0;
		int z = 0;
		int cull = 0;
		bool wire = false;
		int samples = 1;
		bool skinned = false;
		bool operator<(const ShaderPipeKey& o) const {
			return std::tie(color, depth, blend, z, cull, wire, samples, skinned)
				< std::tie(o.color, o.depth, o.blend, o.z, o.cull, o.wire, o.samples, o.skinned);
		}
	};

	struct GpuShader {
		SDL_GPUDevice* dev = nullptr;
		SDL_GPUShader* vs = nullptr;
		SDL_GPUShader* ps = nullptr;
		SDL_GPUShader* builtinVS = nullptr;
		SDL_GPUShader* builtinVSSkinned = nullptr;
		bool builtin = true;
		std::vector<RuntimeParamBuffer> ubos;
		std::unordered_map<std::string, std::pair<unsigned, ShaderParam>> paramIndex;
		std::unordered_map<std::string, unsigned> texIndex;
		std::unordered_map<unsigned, SDL_GPUTexture*> texBind;
		unsigned samplerCount = 0;
		std::map<ShaderPipeKey, SDL_GPUGraphicsPipeline*> pipes;
	};

	static GpuShader* CreateFromAsset(SDL_GPUDevice* dev, const ShaderAsset& asset, const char* psEntry, const char* vsEntry) {
		SDL_GPUShaderFormat sup = SDL_GetGPUShaderFormats(dev);
		bool dxil = (sup & SDL_GPU_SHADERFORMAT_DXIL) != 0;
		SDL_GPUShaderFormat fmt = dxil ? SDL_GPU_SHADERFORMAT_DXIL : SDL_GPU_SHADERFORMAT_SPIRV;

		GpuShader* s = new GpuShader();
		s->dev = dev;

		const std::vector<unsigned char>& psBytes = dxil ? asset.ps.dxil : asset.ps.spirv;
		if (psBytes.empty()) {
			g_shaderError = dxil ? "Precompiled pixel shader missing DXIL" : "Precompiled pixel shader missing SPIR-V";
			delete s;
			return nullptr;
		}
		SDL_GPUShaderCreateInfo ci{};
		ci.code = psBytes.data();
		ci.code_size = psBytes.size();
		ci.entrypoint = psEntry;
		ci.format = fmt;
		ci.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
		ci.num_samplers = asset.ps.samplerCount;
		ci.num_storage_textures = asset.ps.storageTextureCount;
		ci.num_storage_buffers = asset.ps.storageBufferCount;
		ci.num_uniform_buffers = asset.ps.uniformCount;
		s->ps = SDL_CreateGPUShader(dev, &ci);
		if (!s->ps) {
			g_shaderError = SDL_GetError();
			delete s;
			return nullptr;
		}

		if (asset.hasVS) {
			const std::vector<unsigned char>& vsBytes = dxil ? asset.vs.dxil : asset.vs.spirv;
			if (vsBytes.empty()) {
				g_shaderError = dxil ? "Precompiled vertex shader missing DXIL" : "Precompiled vertex shader missing SPIR-V";
				SDL_ReleaseGPUShader(dev, s->ps);
				delete s;
				return nullptr;
			}
			SDL_GPUShaderCreateInfo vci{};
			vci.code = vsBytes.data();
			vci.code_size = vsBytes.size();
			vci.entrypoint = vsEntry;
			vci.format = fmt;
			vci.stage = SDL_GPU_SHADERSTAGE_VERTEX;
			vci.num_samplers = asset.vs.samplerCount;
			vci.num_storage_textures = asset.vs.storageTextureCount;
			vci.num_storage_buffers = asset.vs.storageBufferCount;
			vci.num_uniform_buffers = asset.vs.uniformCount;
			s->vs = SDL_CreateGPUShader(dev, &vci);
			if (!s->vs) {
				g_shaderError = SDL_GetError();
				SDL_ReleaseGPUShader(dev, s->ps);
				delete s;
				return nullptr;
			}
			s->builtin = false;
		}

		auto addStage = [&](const ShaderStageAsset& stage) {
			for (const ShaderParamBuffer& pb : stage.ubos) {
				RuntimeParamBuffer rb;
				rb.binding = pb.binding;
				rb.fragment = pb.fragment;
				rb.data.assign(((pb.dataSize + 15) / 16) * 16, 0);
				rb.params = pb.params;
				s->ubos.push_back(std::move(rb));
			}
		};
		addStage(asset.ps);
		if (asset.hasVS) addStage(asset.vs);
		for (unsigned i = 0; i < s->ubos.size(); ++i)
			for (auto& kv : s->ubos[i].params)
				s->paramIndex.emplace(kv.first, std::make_pair(i, kv.second));

		for (auto& kv : asset.ps.textures) s->texIndex[kv.first] = kv.second;
		s->samplerCount = asset.ps.samplerCount;
		return s;
	}

	static GpuShader* CompileFromSource(SDL_GPUDevice* dev, const char* source, const char* sourcePath, const char* vsEntry, const char* psEntry, const char* includeDir) {
		SDL_GPUShaderFormat sup = SDL_GetGPUShaderFormats(dev);
		bool dxil = (sup & SDL_GPU_SHADERFORMAT_DXIL) != 0;
		bool spirv = (sup & SDL_GPU_SHADERFORMAT_SPIRV) != 0;
		if (!dxil && !spirv) {
			g_shaderError = "No supported shader format on this device";
			return nullptr;
		}
		ShaderAsset asset;
		std::string err;
		if (!CompileShader(source, sourcePath, vsEntry, psEntry, includeDir, dxil, spirv, asset, err)) {
			g_shaderError = err;
			return nullptr;
		}
		return CreateFromAsset(dev, asset, psEntry, vsEntry);
	}

	GpuShader* GXSHADER_API CreateShaderFromSource(SDL_GPUDevice* dev, const char* source, const char* vsEntry, const char* psEntry, const char* includeDir) {
		if (!dev || !source) return nullptr;
		g_shaderError.clear();
		return CompileFromSource(dev, source, nullptr, vsEntry, psEntry, includeDir);
	}

	GpuShader* GXSHADER_API CreateShaderFromFile(SDL_GPUDevice* dev, const char* path, const char* vsEntry, const char* psEntry, const char* includeDir) {
		if (!dev || !path) return nullptr;
		g_shaderError.clear();
		std::string stem = ShaderAssetStem(path);
		if (ShaderAssetExists(stem)) {
			ShaderAsset asset;
			std::string err;
			if (!LoadShaderAsset(stem, asset, err)) {
				g_shaderError = err;
				return nullptr;
			}
			return CreateFromAsset(dev, asset, psEntry, vsEntry);
		}
		std::ifstream f(path, std::ios::binary);
		if (!f) {
			g_shaderError = "Unable to open shader file";
			return nullptr;
		}
		std::ostringstream ss;
		ss << f.rdbuf();
		std::string src = ss.str();
		if (src.size() >= 3 && (unsigned char)src[0] == 0xEF && (unsigned char)src[1] == 0xBB && (unsigned char)src[2] == 0xBF)
			src.erase(0, 3);
		return CompileFromSource(dev, src.c_str(), path, vsEntry, psEntry, includeDir);
	}

	static SDL_GPUShader* MakeBuiltinVS(SDL_GPUDevice* dev, bool skinned) {
		SDL_GPUShaderFormat sup = SDL_GetGPUShaderFormats(dev);
		const uint8_t* code = nullptr;
		size_t size = 0;
		SDL_GPUShaderFormat fmt = SDL_GPU_SHADERFORMAT_INVALID;
		const char* entry = skinned ? "VSMainSkinned" : "VSMain";
		if (sup & SDL_GPU_SHADERFORMAT_DXIL) {
			fmt = SDL_GPU_SHADERFORMAT_DXIL;
			code = skinned ? kSkinVS_DXIL : kMeshVS_DXIL;
			size = skinned ? kSkinVS_DXIL_size : kMeshVS_DXIL_size;
		}
		else if (sup & SDL_GPU_SHADERFORMAT_SPIRV) {
			fmt = SDL_GPU_SHADERFORMAT_SPIRV;
			code = skinned ? kSkinVS_SPIRV : kMeshVS_SPIRV;
			size = skinned ? kSkinVS_SPIRV_size : kMeshVS_SPIRV_size;
		}
		if (!code) return nullptr;
		SDL_GPUShaderCreateInfo ci{};
		ci.code = code;
		ci.code_size = size;
		ci.entrypoint = entry;
		ci.format = fmt;
		ci.stage = SDL_GPU_SHADERSTAGE_VERTEX;
		ci.num_uniform_buffers = 1;
		ci.num_storage_buffers = skinned ? 1 : 0;
		return SDL_CreateGPUShader(dev, &ci);
	}

	SDL_GPUGraphicsPipeline* GXSHADER_API ShaderPipeline(GpuShader* s, SDL_GPUDevice* dev, int colorFormat, int depthFormat, int blend, int zMode, int cull, bool wireframe, int samples, bool skinned) {
		if (!s || !dev) return nullptr;
		GpuLock lock;
		ShaderPipeKey key;
		key.color = (SDL_GPUTextureFormat)colorFormat;
		key.depth = (SDL_GPUTextureFormat)depthFormat;
		key.blend = blend;
		key.z = zMode;
		key.cull = cull;
		key.wire = wireframe;
		key.samples = samples;
		key.skinned = skinned;
		auto found = s->pipes.find(key);
		if (found != s->pipes.end()) return found->second;

		SDL_GPUShader* vs = s->vs;
		if (!vs) {
			if (skinned) {
				if (!s->builtinVSSkinned) s->builtinVSSkinned = MakeBuiltinVS(dev, true);
				vs = s->builtinVSSkinned;
			}
			else {
				if (!s->builtinVS) s->builtinVS = MakeBuiltinVS(dev, false);
				vs = s->builtinVS;
			}
		}
		if (!vs || !s->ps) return nullptr;

		SDL_GPUVertexBufferDescription vb{};
		vb.slot = 0;
		vb.pitch = skinned ? 76 : 44;
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
		target.format = (SDL_GPUTextureFormat)colorFormat;
		if (blend != MESH_BLEND_REPLACE) {
			target.blend_state.enable_blend = true;
			target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
			target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
			if (blend == MESH_BLEND_MULTIPLY) {
				target.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_DST_COLOR;
				target.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
				target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_DST_ALPHA;
				target.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
			}
			else if (blend == MESH_BLEND_ADD) {
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
		info.fragment_shader = s->ps;
		info.vertex_input_state = vin;
		info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
		info.rasterizer_state.fill_mode = wireframe ? SDL_GPU_FILLMODE_LINE : SDL_GPU_FILLMODE_FILL;
		info.rasterizer_state.cull_mode = (SDL_GPUCullMode)cull;
		info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;
		info.rasterizer_state.enable_depth_clip = true;
		info.rasterizer_state.enable_depth_bias = false;
		info.multisample_state.sample_count = samples == 8 ? SDL_GPU_SAMPLECOUNT_8 : samples == 4 ? SDL_GPU_SAMPLECOUNT_4 : samples == 2 ? SDL_GPU_SAMPLECOUNT_2 : SDL_GPU_SAMPLECOUNT_1;
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
		info.target_info.depth_stencil_format = (SDL_GPUTextureFormat)depthFormat;

		SDL_PropertiesID pipeProps = SDL_CreateProperties();
		if (pipeProps) SDL_SetStringProperty(pipeProps, SDL_PROP_GPU_GRAPHICSPIPELINE_CREATE_NAME_STRING, "b3d_shader");
		info.props = pipeProps;
		SDL_GPUGraphicsPipeline* pipe = SDL_CreateGPUGraphicsPipeline(dev, &info);
		if (pipeProps) SDL_DestroyProperties(pipeProps);
		if (!pipe) {
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_CreateGPUGraphicsPipeline shader failed: %s", SDL_GetError());
			return nullptr;
		}
		s->pipes[key] = pipe;
		return pipe;
	}

	void GXSHADER_API ShaderPushUniforms(GpuShader* s, SDL_GPUCommandBuffer* cmds) {
		if (!s || !cmds) return;
		for (auto& b : s->ubos) {
			if (b.data.empty()) continue;
			if (b.fragment) SDL_PushGPUFragmentUniformData(cmds, b.binding, b.data.data(), (unsigned)b.data.size());
			else SDL_PushGPUVertexUniformData(cmds, b.binding, b.data.data(), (unsigned)b.data.size());
		}
	}

	void GXSHADER_API ShaderBindTextures(GpuShader* s, SDL_GPUDevice* dev, SDL_GPURenderPass* pass) {
		if (!s || !dev || !pass || !s->samplerCount) return;
		SDL_GPUTexture* white = GetWhiteTexture(dev);
		SDL_GPUSampler* samp = GetDefaultMeshSampler(dev);
		SDL_GPUTextureSamplerBinding binds[8]{};
		unsigned n = s->samplerCount;
		if (n > 8) n = 8;
		for (unsigned i = 0; i < n; ++i) {
			auto it = s->texBind.find(i);
			binds[i].texture = (it != s->texBind.end() && it->second) ? it->second : white;
			binds[i].sampler = samp;
		}
		SDL_BindGPUFragmentSamplers(pass, 0, binds, n);
	}

	static ShaderParam* FindParam(GpuShader* s, const char* name, unsigned& bufIndex) {
		auto it = s->paramIndex.find(name);
		if (it == s->paramIndex.end()) return nullptr;
		bufIndex = it->second.first;
		return &it->second.second;
	}

	bool GXSHADER_API ShaderSetFloat(GpuShader* s, const char* name, float value) {
		if (!s || !name) return false;
		unsigned bi = 0;
		ShaderParam* p = FindParam(s, name, bi);
		if (!p) return false;
		std::vector<unsigned char>& d = s->ubos[bi].data;
		if (p->offset + sizeof(float) > d.size()) return false;
		memcpy(d.data() + p->offset, &value, sizeof(float));
		return true;
	}

	bool GXSHADER_API ShaderSetVector(GpuShader* s, const char* name, const float value[4]) {
		if (!s || !name || !value) return false;
		unsigned bi = 0;
		ShaderParam* p = FindParam(s, name, bi);
		if (!p) return false;
		std::vector<unsigned char>& d = s->ubos[bi].data;
		unsigned bytes = p->size < 16 ? p->size : 16;
		if (p->offset + bytes > d.size()) return false;
		memcpy(d.data() + p->offset, value, bytes);
		return true;
	}

	bool GXSHADER_API ShaderSetMatrix(GpuShader* s, const char* name, const float value[16]) {
		if (!s || !name || !value) return false;
		unsigned bi = 0;
		ShaderParam* p = FindParam(s, name, bi);
		if (!p) return false;
		std::vector<unsigned char>& d = s->ubos[bi].data;
		unsigned bytes = p->size < 64 ? p->size : 64;
		if (p->offset + bytes > d.size()) return false;
		memcpy(d.data() + p->offset, value, bytes);
		return true;
	}

	bool GXSHADER_API ShaderSetTexture(GpuShader* s, const char* name, SDL_GPUTexture* tex) {
		if (!s || !name) return false;
		auto it = s->texIndex.find(name);
		if (it == s->texIndex.end()) return false;
		s->texBind[it->second] = tex;
		return true;
	}

	const char* GXSHADER_API ShaderError() {
		return g_shaderError.c_str();
	}

	void GXSHADER_API ReleaseShader(SDL_GPUDevice* dev, GpuShader* s) {
		if (!s) return;
		GpuLock lock;
		for (auto& kv : s->pipes) if (kv.second && dev) SDL_ReleaseGPUGraphicsPipeline(dev, kv.second);
		s->pipes.clear();
		if (dev) {
			if (s->vs) SDL_ReleaseGPUShader(dev, s->vs);
			if (s->ps) SDL_ReleaseGPUShader(dev, s->ps);
			if (s->builtinVS) SDL_ReleaseGPUShader(dev, s->builtinVS);
			if (s->builtinVSSkinned) SDL_ReleaseGPUShader(dev, s->builtinVSSkinned);
		}
		delete s;
	}

	void GXSHADER_API ReleaseAllShaders(SDL_GPUDevice* dev) {
		(void)dev;
	}

}
