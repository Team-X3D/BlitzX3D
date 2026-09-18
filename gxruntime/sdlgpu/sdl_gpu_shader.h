#ifndef SDL_GPU_SHADER_H
#define SDL_GPU_SHADER_H

#include <SDL3/SDL_gpu.h>

#ifdef _WIN32
#define GXSHADER_API __stdcall
#else
#define GXSHADER_API
#endif

namespace sdlgpu {

struct GpuShader;

GpuShader* GXSHADER_API CreateShaderFromFile(SDL_GPUDevice* dev, const char* path, const char* vsEntry, const char* psEntry, const char* includeDir);
GpuShader* GXSHADER_API CreateShaderFromSource(SDL_GPUDevice* dev, const char* source, const char* vsEntry, const char* psEntry, const char* includeDir);
void GXSHADER_API ReleaseShader(SDL_GPUDevice* dev, GpuShader* shader);
void GXSHADER_API ReleaseAllShaders(SDL_GPUDevice* dev);
const char* GXSHADER_API ShaderError();

bool GXSHADER_API ShaderSetFloat(GpuShader* s, const char* name, float value);
bool GXSHADER_API ShaderSetVector(GpuShader* s, const char* name, const float value[4]);
bool GXSHADER_API ShaderSetMatrix(GpuShader* s, const char* name, const float value[16]);
bool GXSHADER_API ShaderSetTexture(GpuShader* s, const char* name, SDL_GPUTexture* tex);

SDL_GPUGraphicsPipeline* GXSHADER_API ShaderPipeline(GpuShader* s, SDL_GPUDevice* dev, int colorFormat, int depthFormat, int blend, int zMode, int cull, bool wireframe, int samples, bool skinned);
void GXSHADER_API ShaderPushUniforms(GpuShader* s, SDL_GPUCommandBuffer* cmds);
void GXSHADER_API ShaderBindTextures(GpuShader* s, SDL_GPUDevice* dev, SDL_GPURenderPass* pass);

}

#endif
