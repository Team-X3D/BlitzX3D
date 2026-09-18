#ifndef GXEFFECT_H
#define GXEFFECT_H

#include "d3dxmath.h"
#include <string>

namespace sdlgpu { struct GpuShader; }

class gxGraphics;
class gxCanvas;
struct SDL_GPUDevice;

class gxEffect {
public:
    gxEffect(gxGraphics* gfx, SDL_GPUDevice* dev, sdlgpu::GpuShader* shader);
    ~gxEffect();

    void onLostDevice();
    void onResetDevice();

    bool setFloat(const std::string& name, float value);
    bool setVector(const std::string& name, const float vec[4]);
    bool setMatrix(const std::string& name, const D3DXMATRIX& mat);
    void setMatrixBySemantic(const char* semantic, const D3DXMATRIX& mat);
    void setAutoMatrices(const D3DXMATRIX& world, const D3DXMATRIX& view, const D3DXMATRIX& proj);
    bool setTexture(const std::string& name, void* tex);
    bool setTextureCanvas(const std::string& name, gxCanvas* canvas);

    bool begin(unsigned* passes);
    bool beginPass(unsigned pass);
    bool endPass();
    bool end();

    sdlgpu::GpuShader* getGpuShader() const { return shader; }

private:
    gxGraphics* graphics;
    SDL_GPUDevice* dev;
    sdlgpu::GpuShader* shader;
};

#endif