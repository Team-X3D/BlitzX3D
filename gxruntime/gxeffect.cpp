#include "std.h"
#include "gxeffect.h"
#include "gxgraphics.h"
#include "gxcanvas.h"
#include "gxruntime.h"
#include "sdlgpu/sdl_gpu_shader.h"
#include "sdlgpu/sdl_gpu_texture.h"

gxEffect::gxEffect(gxGraphics* gfx, SDL_GPUDevice* d, sdlgpu::GpuShader* s)
    : graphics(gfx), dev(d), shader(s) {}

gxEffect::~gxEffect() {
    if (shader && dev) sdlgpu::ReleaseShader(dev, shader);
    shader = nullptr;
}

void gxEffect::onLostDevice() {}
void gxEffect::onResetDevice() {}
void gxEffect::setMatrixBySemantic(const char*, const D3DXMATRIX&) {}
void gxEffect::setAutoMatrices(const D3DXMATRIX&, const D3DXMATRIX&, const D3DXMATRIX&) {}
bool gxEffect::begin(unsigned*) { return false; }
bool gxEffect::beginPass(unsigned) { return false; }
bool gxEffect::endPass() { return false; }
bool gxEffect::end() { return false; }

bool gxEffect::setFloat(const std::string& name, float value) {
    return shader && sdlgpu::ShaderSetFloat(shader, name.c_str(), value);
}

bool gxEffect::setVector(const std::string& name, const float vec[4]) {
    return shader && sdlgpu::ShaderSetVector(shader, name.c_str(), vec);
}

bool gxEffect::setMatrix(const std::string& name, const D3DXMATRIX& mat) {
    return shader && sdlgpu::ShaderSetMatrix(shader, name.c_str(), &mat._11);
}

bool gxEffect::setTexture(const std::string&, void*) {
    return false;
}

bool gxEffect::setTextureCanvas(const std::string& name, gxCanvas* canvas) {
    if (!shader || !canvas || !dev) return false;
    SDL_GPUTexture* tex = sdlgpu::GetCanvasTexture(dev, canvas);
    return tex && sdlgpu::ShaderSetTexture(shader, name.c_str(), tex);
}
