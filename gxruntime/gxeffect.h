#ifndef GXEFFECT_H
#define GXEFFECT_H

#include "d3dxmath.h"
#include <string>
#include <unordered_map>

class gxGraphics;

class gxEffect {
public:
    gxEffect(gxGraphics* gfx, void* effect);
    ~gxEffect();

    void onLostDevice();
    void onResetDevice();

    bool setFloat(const std::string& name, float value);
    bool setVector(const std::string& name, const float vec[4]);
    bool setMatrix(const std::string& name, const D3DXMATRIX& mat);
    void setMatrixBySemantic(const char* semantic, const D3DXMATRIX& mat);
    void setAutoMatrices(const D3DXMATRIX& world, const D3DXMATRIX& view, const D3DXMATRIX& proj);
    bool setTexture(const std::string& name, IDirect3DBaseTexture9* tex);

    bool begin(unsigned* passes);
    bool beginPass(unsigned pass);
    bool endPass();
    bool end();

    void* getEffect() const { return effect; }

private:
    gxGraphics* graphics;
    void* effect;

#if GX_USE_LEGACY_D3DX
    std::unordered_map<std::string, D3DXHANDLE> handleCache;
    D3DXHANDLE getHandle(const std::string& name);
#endif
};

#endif