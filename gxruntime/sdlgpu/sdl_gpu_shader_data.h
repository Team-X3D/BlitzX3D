#ifndef SDL_GPU_SHADER_DATA_H
#define SDL_GPU_SHADER_DATA_H

#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#define GXSHADERDATA_API __stdcall
#else
#define GXSHADERDATA_API
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
	std::vector<unsigned char> dxil;
	std::vector<unsigned char> spirv;
};

struct ShaderAsset {
	ShaderStageAsset ps;
	ShaderStageAsset vs;
	bool hasVS = false;
};

std::string GXSHADERDATA_API ShaderAssetStem(const std::string& sourcePath);
bool GXSHADERDATA_API CompileShader(const char* source, const char* sourcePath, const char* vsEntry, const char* psEntry, const char* includeDir, bool wantDxil, bool wantSpirv, ShaderAsset& out, std::string& err);
bool GXSHADERDATA_API SaveShaderAsset(const std::string& stem, const ShaderAsset& asset, std::string& err);
bool GXSHADERDATA_API LoadShaderAsset(const std::string& stem, ShaderAsset& out, std::string& err);
bool GXSHADERDATA_API ShaderAssetExists(const std::string& stem);
bool GXSHADERDATA_API RuntimeCompilerAvailable();

}

#endif
