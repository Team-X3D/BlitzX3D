#include "sdl_gpu_shader_data.h"

#include <SDL3/SDL.h>
#include <SDL3_shadercross/SDL_shadercross.h>
#include <spirv_cross_c.h>

#include <cstring>
#include <mutex>

namespace sdlgpu {

	void SDLSHADERDATA_API InitShaderCompiler() {
		static std::once_flag once;
		std::call_once(once, [] {
			SDL_ShaderCross_Init();
		});
	}

	bool SDLSHADERDATA_API CompileHLSLToSPIRV(const char* source, const char* entry, bool vertex, const char* includeDir, const ShaderDefine* defines, unsigned defineCount, std::vector<unsigned char>& spirv, std::string& err) {
		InitShaderCompiler();
		if (!source || !entry || !entry[0]) {
			err = "Missing shader source or entry point";
			return false;
		}

		SDL_ShaderCross_HLSL_Define defs[64]{};
		unsigned n = defineCount;
		if (n > 64) n = 64;
		for (unsigned i = 0; i < n; ++i) {
			defs[i].name = (char*)defines[i].name;
			defs[i].value = (char*)defines[i].value;
		}

		SDL_ShaderCross_HLSL_Info info{};
		info.source = source;
		info.entrypoint = entry;
		info.include_dir = (includeDir && includeDir[0]) ? includeDir : nullptr;
		info.defines = n ? defs : nullptr;
		info.shader_stage = vertex ? SDL_SHADERCROSS_SHADERSTAGE_VERTEX : SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT;
		info.props = 0;

		size_t size = 0;
		void* buf = SDL_ShaderCross_CompileSPIRVFromHLSL(&info, &size);
		if (!buf || size == 0) {
			err = SDL_GetError();
			if (err.empty()) err = "HLSL compilation failed";
			return false;
		}
		spirv.assign((unsigned char*)buf, (unsigned char*)buf + size);
		SDL_free(buf);
		return true;
	}

	SDL_GPUShader* SDLSHADERDATA_API CreateGPUStageShader(SDL_GPUDevice* dev, const std::vector<unsigned char>& spirv, const char* entry, bool vertex, unsigned samplers, unsigned uniformBuffers, unsigned storageBuffers, unsigned storageTextures) {
		if (!dev || spirv.empty()) return nullptr;
		InitShaderCompiler();

		SDL_ShaderCross_SPIRV_Info info{};
		info.bytecode = spirv.data();
		info.bytecode_size = spirv.size();
		info.entrypoint = entry;
		info.shader_stage = vertex ? SDL_SHADERCROSS_SHADERSTAGE_VERTEX : SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT;
		info.props = 0;

		SDL_ShaderCross_GraphicsShaderResourceInfo ri{};
		ri.num_samplers = samplers;
		ri.num_uniform_buffers = uniformBuffers;
		ri.num_storage_buffers = storageBuffers;
		ri.num_storage_textures = storageTextures;

		return SDL_ShaderCross_CompileGraphicsShaderFromSPIRV(dev, &info, &ri, 0);
	}

	SDL_GPUShader* SDLSHADERDATA_API CompileStageShader(SDL_GPUDevice* dev, const char* source, const char* entry, bool vertex, const char* includeDir, const ShaderDefine* defines, unsigned defineCount, unsigned samplers, unsigned uniformBuffers, unsigned storageBuffers, unsigned storageTextures, std::string& err) {
		std::vector<unsigned char> spirv;
		if (!CompileHLSLToSPIRV(source, entry, vertex, includeDir, defines, defineCount, spirv, err)) return nullptr;
		SDL_GPUShader* shader = CreateGPUStageShader(dev, spirv, entry, vertex, samplers, uniformBuffers, storageBuffers, storageTextures);
		if (!shader) err = SDL_GetError();
		return shader;
	}

	bool SDLSHADERDATA_API ReflectSPIRV(const std::vector<unsigned char>& spv, bool vertex, ShaderStageAsset& stage) {
		if (spv.empty()) return false;
		spvc_context ctx = nullptr;
		if (spvc_context_create(&ctx) < 0) return false;
		spvc_parsed_ir ir = nullptr;
		if (spvc_context_parse_spirv(ctx, (const SpvId*)spv.data(), spv.size() / sizeof(SpvId), &ir) < 0) {
			spvc_context_destroy(ctx);
			return false;
		}
		spvc_compiler comp = nullptr;
		if (spvc_context_create_compiler(ctx, SPVC_BACKEND_NONE, ir, SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &comp) < 0) {
			spvc_context_destroy(ctx);
			return false;
		}
		spvc_resources res;
		if (spvc_compiler_create_shader_resources(comp, &res) < 0) {
			spvc_context_destroy(ctx);
			return false;
		}
		const spvc_reflected_resource* list = nullptr;
		size_t n = 0;
		if (spvc_resources_get_resource_list_for_type(res, SPVC_RESOURCE_TYPE_UNIFORM_BUFFER, &list, &n) == 0) {
			stage.uniformCount = (unsigned)n;
			for (size_t i = 0; i < n; ++i) {
				unsigned binding = spvc_compiler_get_decoration(comp, list[i].id, SpvDecorationBinding);
				if (vertex && binding == 0) continue;
				ShaderParamBuffer pb;
				pb.binding = binding;
				pb.fragment = !vertex;
				spvc_type type = spvc_compiler_get_type_handle(comp, list[i].base_type_id);
				size_t structSize = 0;
				spvc_compiler_get_declared_struct_size(comp, type, &structSize);
				pb.dataSize = (unsigned)structSize;
				unsigned members = spvc_type_get_num_member_types(type);
				for (unsigned m = 0; m < members; ++m) {
					const char* name = spvc_compiler_get_member_name(comp, list[i].base_type_id, m);
					if (!name || !name[0]) continue;
					size_t msize = 0;
					spvc_compiler_get_declared_struct_member_size(comp, type, m, &msize);
					spvc_type mt = spvc_compiler_get_type_handle(comp, spvc_type_get_member_type(type, m));
					ShaderParam p;
					p.offset = spvc_compiler_get_member_decoration(comp, list[i].base_type_id, m, SpvDecorationOffset);
					p.size = (unsigned)msize;
					p.vec = spvc_type_get_vector_size(mt);
					if (p.vec == 0) p.vec = 1;
					p.base = spvc_type_get_basetype(mt);
					pb.params[name] = p;
				}
				stage.ubos.push_back(std::move(pb));
			}
		}
		const spvc_reflected_resource* imgs = nullptr;
		size_t ni = 0;
		if (spvc_resources_get_resource_list_for_type(res, SPVC_RESOURCE_TYPE_SEPARATE_IMAGE, &imgs, &ni) == 0) {
			for (size_t i = 0; i < ni; ++i) {
				unsigned binding = spvc_compiler_get_decoration(comp, imgs[i].id, SpvDecorationBinding);
				if (imgs[i].name && imgs[i].name[0]) stage.textures[imgs[i].name] = binding;
				if (binding + 1 > stage.samplerCount) stage.samplerCount = binding + 1;
			}
		}
		const spvc_reflected_resource* simgs = nullptr;
		size_t nsi = 0;
		if (spvc_resources_get_resource_list_for_type(res, SPVC_RESOURCE_TYPE_SAMPLED_IMAGE, &simgs, &nsi) == 0) {
			for (size_t i = 0; i < nsi; ++i) {
				unsigned binding = spvc_compiler_get_decoration(comp, simgs[i].id, SpvDecorationBinding);
				if (simgs[i].name && simgs[i].name[0]) stage.textures[simgs[i].name] = binding;
				if (binding + 1 > stage.samplerCount) stage.samplerCount = binding + 1;
			}
		}
		const spvc_reflected_resource* smps = nullptr;
		size_t ns = 0;
		if (spvc_resources_get_resource_list_for_type(res, SPVC_RESOURCE_TYPE_SEPARATE_SAMPLERS, &smps, &ns) == 0) {
			if (ns > stage.samplerCount) stage.samplerCount = (unsigned)ns;
		}
		const spvc_reflected_resource* sb = nullptr;
		size_t nsb = 0;
		if (spvc_resources_get_resource_list_for_type(res, SPVC_RESOURCE_TYPE_STORAGE_BUFFER, &sb, &nsb) == 0) stage.storageBufferCount = (unsigned)nsb;
		const spvc_reflected_resource* st = nullptr;
		size_t nst = 0;
		if (spvc_resources_get_resource_list_for_type(res, SPVC_RESOURCE_TYPE_STORAGE_IMAGE, &st, &nst) == 0) stage.storageTextureCount = (unsigned)nst;
		spvc_context_destroy(ctx);
		return true;
	}

}
