#include "std.h"
#include "sdleffect.h"
#include "sdlgraphics.h"
#include "sdlcanvas.h"
#include "sdlruntime.h"
#include "../sdlgpu/sdl_gpu_shader.h"
#include "../sdlgpu/sdl_gpu_texture.h"

sdlEffect::sdlEffect(sdlGraphics* gfx, SDL_GPUDevice* d, sdlgpu::GpuShader* s)
    : graphics(gfx), dev(d), shader(s) {}

sdlEffect::~sdlEffect() {
    if (shader && dev) sdlgpu::ReleaseShader(dev, shader);
    shader = nullptr;
}

void sdlEffect::onLostDevice() {}
void sdlEffect::onResetDevice() {}
void sdlEffect::setMatrixBySemantic(const char*, const D3DXMATRIX&) {}
void sdlEffect::setAutoMatrices(const D3DXMATRIX&, const D3DXMATRIX&, const D3DXMATRIX&) {}
bool sdlEffect::begin(unsigned*) { return false; }
bool sdlEffect::beginPass(unsigned) { return false; }
bool sdlEffect::endPass() { return false; }
bool sdlEffect::end() { return false; }

bool sdlEffect::setFloat(const std::string& name, float value) {
    return shader && sdlgpu::ShaderSetFloat(shader, name.c_str(), value);
}

bool sdlEffect::setVector(const std::string& name, const float vec[4]) {
    return shader && sdlgpu::ShaderSetVector(shader, name.c_str(), vec);
}

bool sdlEffect::setMatrix(const std::string& name, const D3DXMATRIX& mat) {
    return shader && sdlgpu::ShaderSetMatrix(shader, name.c_str(), &mat._11);
}

bool sdlEffect::setTexture(const std::string&, void*) {
    return false;
}

bool sdlEffect::setTextureCanvas(const std::string& name, sdlCanvas* canvas) {
    if (!shader || !canvas || !dev) return false;
    SDL_GPUTexture* tex = sdlgpu::GetCanvasTexture(dev, canvas);
    return tex && sdlgpu::ShaderSetTexture(shader, name.c_str(), tex);
}

