#ifndef SDL_GPU_SHADER_DATA_H
#define SDL_GPU_SHADER_DATA_H

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#define SDLSHADERDATA_API __stdcall
#else
#define SDLSHADERDATA_API
#endif

namespace sdlgpu {

struct ShaderParam {
	unsigned offset = 0;
	unsigned size = 0;
	unsigned vec = 1;
	unsigned base = 0;
};

struct ShaderParamBuffer {
	unsigned binding = 0;
	bool fragment = true;
	unsigned dataSize = 0;
	std::unordered_map<std::string, ShaderParam> params;
};

struct ShaderStageAsset {
	std::vector<ShaderParamBuffer> ubos;
	std::unordered_map<std::string, unsigned> textures;
	unsigned samplerCount = 0;
	unsigned uniformCount = 0;
	unsigned storageBufferCount = 0;
	unsigned storageTextureCount = 0;
	std::vector<unsigned char> spirv;
};

struct ShaderAsset {
	ShaderStageAsset ps;
	ShaderStageAsset vs;
	bool hasVS = false;
};

struct ShaderDefine {
	const char* name;
	const char* value;
};

void SDLSHADERDATA_API InitShaderCompiler();

bool SDLSHADERDATA_API CompileHLSLToSPIRV(const char* source, const char* entry, bool vertex, const char* includeDir, const ShaderDefine* defines, unsigned defineCount, std::vector<unsigned char>& spirv, std::string& err);

bool SDLSHADERDATA_API ReflectSPIRV(const std::vector<unsigned char>& spirv, bool vertex, ShaderStageAsset& stage);

SDL_GPUShader* SDLSHADERDATA_API CreateGPUStageShader(SDL_GPUDevice* dev, const std::vector<unsigned char>& spirv, const char* entry, bool vertex, unsigned samplers, unsigned uniformBuffers, unsigned storageBuffers, unsigned storageTextures);

SDL_GPUShader* SDLSHADERDATA_API CompileStageShader(SDL_GPUDevice* dev, const char* source, const char* entry, bool vertex, const char* includeDir, const ShaderDefine* defines, unsigned defineCount, unsigned samplers, unsigned uniformBuffers, unsigned storageBuffers, unsigned storageTextures, std::string& err);

}

#endif
