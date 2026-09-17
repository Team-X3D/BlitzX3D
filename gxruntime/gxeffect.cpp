#include "std.h"
#include "gxeffect.h"
#include "gxgraphics.h"

#if GX_USE_LEGACY_D3DX

gxEffect::gxEffect(gxGraphics* gfx, void* e)
    : graphics(gfx), effect(e) {
    ((ID3DXEffect*)effect)->AddRef();
}

gxEffect::~gxEffect() {
    if (effect) ((ID3DXEffect*)effect)->Release();
}

void gxEffect::onLostDevice() {
    if (effect) ((ID3DXEffect*)effect)->OnLostDevice();
}

void gxEffect::onResetDevice() {
    if (effect) ((ID3DXEffect*)effect)->OnResetDevice();
}

D3DXHANDLE gxEffect::getHandle(const std::string& name) {
    auto it = handleCache.find(name);
    if (it != handleCache.end()) return it->second;
    D3DXHANDLE h = ((ID3DXEffect*)effect)->GetParameterByName(nullptr, name.c_str());
    handleCache[name] = h;
    return h;
}

bool gxEffect::setFloat(const std::string& name, float value) {
    D3DXHANDLE h = getHandle(name);
    if (!h) return false;
    return SUCCEEDED(((ID3DXEffect*)effect)->SetFloat(h, value));
}

bool gxEffect::setVector(const std::string& name, const float vec[4]) {
    D3DXHANDLE h = getHandle(name);
    if (!h) return false;
    return SUCCEEDED(((ID3DXEffect*)effect)->SetFloatArray(h, vec, 4));
}

bool gxEffect::setMatrix(const std::string& name, const D3DXMATRIX& mat) {
    D3DXHANDLE h = getHandle(name);
    if (!h) return false;
    return SUCCEEDED(((ID3DXEffect*)effect)->SetMatrix(h, &mat));
}

void gxEffect::setMatrixBySemantic(const char* semantic, const D3DXMATRIX& mat) {
    if (!effect) return;
    ID3DXEffect* fx = (ID3DXEffect*)effect;
    D3DXHANDLE h = fx->GetParameterBySemantic(nullptr, semantic);
    if (h) fx->SetMatrix(h, &mat);
}

void gxEffect::setAutoMatrices(const D3DXMATRIX& world,
    const D3DXMATRIX& view,
    const D3DXMATRIX& proj) {
    D3DXMATRIX wv = world * view;
    D3DXMATRIX wvp = wv * proj;

    setMatrix("World", world);
    setMatrix("View", view);
    setMatrix("Projection", proj);
    setMatrix("WorldView", wv);
    setMatrix("WorldViewProj", wvp);

    setMatrixBySemantic("MATRIX_WORLD", world);
    setMatrixBySemantic("MATRIX_VIEW", view);
    setMatrixBySemantic("MATRIX_PROJECTION", proj);
    setMatrixBySemantic("MATRIX_WORLDVIEW", wv);
    setMatrixBySemantic("MATRIX_VIEWPROJ", wvp);
    setMatrixBySemantic("MATRIX_WORLDVIEWPROJ", wvp);
}

bool gxEffect::setTexture(const std::string& name, IDirect3DBaseTexture9* tex) {
    if (!effect) return false;
    D3DXHANDLE h = getHandle(name);
    if (!h) return false;
    HRESULT hr = ((ID3DXEffect*)effect)->SetTexture(h, tex);
    return SUCCEEDED(hr);
}

bool gxEffect::begin(unsigned* passes) {
    return SUCCEEDED(((ID3DXEffect*)effect)->Begin(passes, 0));
}

bool gxEffect::beginPass(unsigned pass) {
    return SUCCEEDED(((ID3DXEffect*)effect)->BeginPass(pass));
}

bool gxEffect::endPass() {
    return SUCCEEDED(((ID3DXEffect*)effect)->EndPass());
}

bool gxEffect::end() {
    return SUCCEEDED(((ID3DXEffect*)effect)->End());
}

#else

gxEffect::gxEffect(gxGraphics* gfx, void* e) : graphics(gfx), effect(e) {}
gxEffect::~gxEffect() {}
void gxEffect::onLostDevice() {}
void gxEffect::onResetDevice() {}
bool gxEffect::setFloat(const std::string&, float) { return false; }
bool gxEffect::setVector(const std::string&, const float[4]) { return false; }
bool gxEffect::setMatrix(const std::string&, const D3DXMATRIX&) { return false; }
void gxEffect::setMatrixBySemantic(const char*, const D3DXMATRIX&) {}
void gxEffect::setAutoMatrices(const D3DXMATRIX&, const D3DXMATRIX&, const D3DXMATRIX&) {}
bool gxEffect::setTexture(const std::string&, IDirect3DBaseTexture9*) { return false; }
bool gxEffect::begin(unsigned*) { return false; }
bool gxEffect::beginPass(unsigned) { return false; }
bool gxEffect::endPass() { return false; }
bool gxEffect::end() { return false; }

#endif
