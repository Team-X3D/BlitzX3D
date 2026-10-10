#ifndef SDL_GPU_SHADER_H
#define SDL_GPU_SHADER_H

#include <SDL3/SDL_gpu.h>

#ifdef _WIN32
#define SDLSHADER_API __stdcall
#else
#define SDLSHADER_API
#endif

namespace sdlgpu {

struct GpuShader;

GpuShader* SDLSHADER_API CreateShaderFromFile(SDL_GPUDevice* dev, const char* path, const char* vsEntry, const char* psEntry, const char* includeDir);
GpuShader* SDLSHADER_API CreateShaderFromSource(SDL_GPUDevice* dev, const char* source, const char* vsEntry, const char* psEntry, const char* includeDir);
void SDLSHADER_API ReleaseShader(SDL_GPUDevice* dev, GpuShader* shader);
void SDLSHADER_API ReleaseAllShaders(SDL_GPUDevice* dev);
const char* SDLSHADER_API ShaderError();

bool SDLSHADER_API ShaderSetFloat(GpuShader* s, const char* name, float value);
bool SDLSHADER_API ShaderSetVector(GpuShader* s, const char* name, const float value[4]);
bool SDLSHADER_API ShaderSetMatrix(GpuShader* s, const char* name, const float value[16]);
bool SDLSHADER_API ShaderSetTexture(GpuShader* s, const char* name, SDL_GPUTexture* tex);

SDL_GPUGraphicsPipeline* SDLSHADER_API ShaderPipeline(GpuShader* s, SDL_GPUDevice* dev, int colorFormat, int depthFormat, int blend, int zMode, int cull, bool wireframe, int samples, bool skinned);
void SDLSHADER_API ShaderPushUniforms(GpuShader* s, SDL_GPUCommandBuffer* cmds);
void SDLSHADER_API ShaderBindTextures(GpuShader* s, SDL_GPUDevice* dev, SDL_GPURenderPass* pass);

}

#endif
