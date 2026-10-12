#ifndef SDL_GPU_BUILTIN_SHADERS_H
#define SDL_GPU_BUILTIN_SHADERS_H

#include <SDL3/SDL_gpu.h>

#include "sdl_gpu_shader_data.h"

namespace sdlgpu {

const char* SDLSHADERDATA_API BuiltinShaderSource(const char* name);

SDL_GPUShader* SDLSHADERDATA_API CompileBuiltinShader(SDL_GPUDevice* dev, const char* name, const char* entry, bool vertex, const ShaderDefine* defines, unsigned defineCount, unsigned samplers, unsigned uniformBuffers, unsigned storageBuffers, unsigned storageTextures);

}

#endif
