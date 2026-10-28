#include "std.h"
#include "sdleffect.h"
#include "sdlgraphics.h"
#include "sdlcanvas.h"
#include "sdlruntime.h"
#include "../sdlgpu/sdl_gpu_shader.h"
#include "../sdlgpu/sdl_gpu_texture.h"

static void transposeMat(const D3DXMATRIX& mat, float out[16]) {
	out[0] = mat._11; out[1] = mat._21; out[2] = mat._31; out[3] = mat._41;
	out[4] = mat._12; out[5] = mat._22; out[6] = mat._32; out[7] = mat._42;
	out[8] = mat._13; out[9] = mat._23; out[10] = mat._33; out[11] = mat._43;
	out[12] = mat._14; out[13] = mat._24; out[14] = mat._34; out[15] = mat._44;
}

sdlEffect::sdlEffect(sdlGraphics* gfx, SDL_GPUDevice* d, sdlgpu::GpuShader* s)
    : graphics(gfx), dev(d), shader(s) {}

sdlEffect::~sdlEffect() {
    if (shader && dev) sdlgpu::ReleaseShader(dev, shader);
    shader = nullptr;
}

void sdlEffect::onLostDevice() {}
void sdlEffect::onResetDevice() {}

bool sdlEffect::setFloat(const std::string& name, float value) {
    return shader && sdlgpu::ShaderSetFloat(shader, name.c_str(), value);
}

bool sdlEffect::setVector(const std::string& name, const float vec[4]) {
    return shader && sdlgpu::ShaderSetVector(shader, name.c_str(), vec);
}

bool sdlEffect::setMatrix(const std::string& name, const D3DXMATRIX& mat) {
    if (!shader) return false;
    float t[16];
    transposeMat(mat, t);
    return sdlgpu::ShaderSetMatrix(shader, name.c_str(), t);
}

void sdlEffect::setMatrixBySemantic(const char* semantic, const D3DXMATRIX& mat) {
    if (!shader || !semantic) return;
    const char* param = sdlgpu::ShaderSemanticParam(shader, semantic);
    if (param) setMatrix(param, mat);
}

void sdlEffect::setAutoMatrices(const D3DXMATRIX& world, const D3DXMATRIX& view, const D3DXMATRIX& proj) {
    if (!shader) return;

    D3DXMATRIX wv, wvp;
    D3DXMatrixMultiply(&wv, &world, &view);
    D3DXMatrixMultiply(&wvp, &wv, &proj);

    setMatrix("World", world);
    setMatrix("View", view);
    setMatrix("Projection", proj);
    setMatrix("WorldView", wv);
    setMatrix("WorldViewProj", wvp);

    struct AutoParam { const char* semantic; const D3DXMATRIX* mat; };
    const AutoParam autoParams[] = {
        { "MATRIX_WORLD", &world },
        { "MATRIX_VIEW", &view },
        { "MATRIX_PROJECTION", &proj },
        { "MATRIX_WORLDVIEW", &wv },
        { "MATRIX_VIEWPROJ", &wvp },
        { "MATRIX_WORLDVIEWPROJ", &wvp },
        { "WORLD", &world },
        { "VIEW", &view },
        { "PROJECTION", &proj },
        { "WORLDVIEW", &wv },
        { "VIEWPROJ", &wvp },
        { "WORLDVIEWPROJ", &wvp },
    };
    for (const AutoParam& ap : autoParams) setMatrixBySemantic(ap.semantic, *ap.mat);
}

bool sdlEffect::setTexture(const std::string& name, void* tex) {
    if (!shader || !tex) return false;
    SDL_GPUTexture* gpu = (SDL_GPUTexture*)tex;
    if (sdlgpu::ShaderSetTexture(shader, name.c_str(), gpu)) return true;
    return setTextureIndexByName(name, gpu);
}

bool sdlEffect::setTextureIndexByName(const std::string& name, SDL_GPUTexture* tex) {
    if (!shader) return false;
    unsigned count = sdlgpu::ShaderFxSamplerCount(shader);
    for (unsigned i = 0; i < count; ++i) {
        sdlgpu::FxSamplerInfo info{};
        if (!sdlgpu::ShaderFxSampler(shader, i, &info)) continue;
        if ((info.name && name == info.name) || (info.texture && info.texture[0] && name == info.texture)) {
            return sdlgpu::ShaderSetTextureIndex(shader, info.stage, tex);
        }
    }
    return false;
}

bool sdlEffect::setTextureCanvas(const std::string& name, sdlCanvas* canvas) {
    if (!shader || !canvas || !dev) return false;
    SDL_GPUTexture* tex = sdlgpu::GetCanvasTexture(dev, canvas);
    if (!tex) return false;
    return setTexture(name, tex);
}

void sdlEffect::bindStageTextureRaw(unsigned stage, SDL_GPUTexture* tex) {
    if (!shader) return;
    unsigned count = sdlgpu::ShaderFxSamplerCount(shader);
    if (stage < count) {
        sdlgpu::FxSamplerInfo info{};
        if (sdlgpu::ShaderFxSampler(shader, stage, &info) && info.texture && info.texture[0]) return;
    }
    sdlgpu::ShaderSetTextureIndex(shader, stage, tex);
}

void sdlEffect::bindStageTexture(unsigned stage, sdlCanvas* canvas) {
    if (!dev) return;
    SDL_GPUTexture* tex = canvas ? sdlgpu::GetCanvasTexture(dev, canvas) : nullptr;
    bindStageTextureRaw(stage, tex);
}

bool sdlEffect::begin(unsigned* passes) { if (passes) *passes = 1; return shader != nullptr; }
bool sdlEffect::beginPass(unsigned) { return shader != nullptr; }
bool sdlEffect::endPass() { return shader != nullptr; }
bool sdlEffect::end() { return shader != nullptr; }
