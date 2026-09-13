#include "std.h"
#include "gxcanvas.h"
#include "gxgraphics.h"
#include "gxruntime.h"
#include "asmcoder.h"
#include "gxutf8.h"
#include "sdlgpu/sdl_gpu_texture.h"
#include "sdlgpu/sdl_gpu_text.h"
#include <SDL3/SDL_log.h>

static int canvas_cnt;

extern gxRuntime* gx_runtime;

static unsigned FWMS[] = {
    0xffffffff,0x7fffffff,0x3fffffff,0x1fffffff,
    0x0fffffff,0x07ffffff,0x03ffffff,0x01ffffff,
    0x00ffffff,0x007fffff,0x003fffff,0x001fffff,
    0x000fffff,0x0007ffff,0x0003ffff,0x0001ffff,
    0x0000ffff,0x00007fff,0x00003fff,0x00001fff,
    0x00000fff,0x000007ff,0x000003ff,0x000001ff,
    0x000000ff,0x0000007f,0x0000003f,0x0000001f,
    0x0000000f,0x00000007,0x00000003,0x00000001 };
static unsigned LWMS[] = {
    0x80000000,0xc0000000,0xe0000000,0xf0000000,
    0xf8000000,0xfc000000,0xfe000000,0xff000000,
    0xff800000,0xffc00000,0xffe00000,0xfff00000,
    0xfff80000,0xfffc0000,0xfffe0000,0xffff0000,
    0xffff8000,0xffffc000,0xffffe000,0xfffff000,
    0xfffff800,0xfffffc00,0xfffffe00,0xffffff00,
    0xffffff80,0xffffffc0,0xffffffe0,0xfffffff0,
    0xfffffff8,0xfffffffc,0xfffffffe,0xffffffff };

struct Rect : public RECT {
    Rect() {}
    Rect(int x, int y, int w, int h) { left = x; top = y; right = x + w; bottom = y + h; }
};

static bool clip(const RECT& viewport, RECT* d) {
    if (d->right <= d->left || d->bottom <= d->top ||
        d->left >= viewport.right || d->right <= viewport.left ||
        d->top >= viewport.bottom || d->bottom <= viewport.top) return false;
    if (d->left < viewport.left)   d->left = viewport.left;
    if (d->right > viewport.right)  d->right = viewport.right;
    if (d->top < viewport.top)    d->top = viewport.top;
    if (d->bottom > viewport.bottom) d->bottom = viewport.bottom;
    return true;
}

static bool clip(const RECT& viewport, RECT* d, RECT* s) {
    if (d->right <= d->left || d->bottom <= d->top ||
        d->left >= viewport.right || d->right <= viewport.left ||
        d->top >= viewport.bottom || d->bottom <= viewport.top) return false;
    int dx, dy;
    if ((dx = viewport.left - d->left) > 0) { d->left += dx; s->left += dx; }
    if ((dx = viewport.right - d->right) < 0) { d->right += dx; s->right += dx; }
    if ((dy = viewport.top - d->top) > 0) { d->top += dy; s->top += dy; }
    if ((dy = viewport.bottom - d->bottom) < 0) { d->bottom += dy; s->bottom += dy; }
    return true;
}

struct FillModeGuard {
    IDirect3DDevice9* dev;
    DWORD oldMode;
    FillModeGuard(IDirect3DDevice9* d) : dev(d) {
        if (dev) {
            dev->GetRenderState(D3DRS_FILLMODE, &oldMode);
            dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
        }
    }
    ~FillModeGuard() {
        if (dev) dev->SetRenderState(D3DRS_FILLMODE, oldMode);
    }
};

class FillRectGuard {
    IDirect3DDevice9* dev;
    IDirect3DSurface9* oldRT;
    IDirect3DSurface9* oldDS;
    bool active;

public:
    FillRectGuard(IDirect3DDevice9* d) : dev(d), oldRT(nullptr), oldDS(nullptr), active(false) {
        if (!dev) return;
        dev->GetRenderTarget(0, &oldRT);
        dev->GetDepthStencilSurface(&oldDS);
        active = true;
    }
    // this is retarded
    // but it works!
    ~FillRectGuard() {
        if (!active || !dev) return;
        dev->SetRenderTarget(0, oldRT);
        if (oldRT) oldRT->Release();
        if (oldDS) oldDS->Release();
        active = false;
    }

    FillRectGuard(const FillRectGuard&) = delete;
    FillRectGuard& operator=(const FillRectGuard&) = delete;
};

static inline void fillRectRows(unsigned char* base, int basePitch, int w, int h, unsigned nat, int pitch) {
    for (int y = 0; y < h; ++y) {
        unsigned char* row = base + y * basePitch;
        if (pitch == 4) {
            unsigned* p = (unsigned*)row;
            for (int x = 0; x < w; ++x) p[x] = nat;
        }
        else if (pitch == 2) {
            unsigned short val = (unsigned short)nat;
            unsigned short* p = (unsigned short*)row;
            for (int x = 0; x < w; ++x) p[x] = val;
        }
        else {
            unsigned char b0 = nat & 0xff;
            unsigned char b1 = (nat >> 8) & 0xff;
            unsigned char b2 = (nat >> 16) & 0xff;
            unsigned char* p = row;
            for (int x = 0; x < w; ++x) {
                p[0] = b0; p[1] = b1; p[2] = b2;
                p += 3;
            }
        }
    }
}

void gxCanvas::fillRect(const RECT& r, unsigned argb) {
    if (!lock()) return;
    int w = r.right - r.left;
    int h = r.bottom - r.top;
    if (w <= 0 || h <= 0) { unlock(); return; }
    unsigned nat = format.fromARGB(argb);
    int pitch = format.getPitch();
    unsigned char* base = locked_surf + r.top * locked_pitch + r.left * pitch;
    fillRectRows(base, locked_pitch, w, h, nat, pitch);
    ++mod_cnt;
    unlock();
}

void gxCanvas::allocCPUStore(int w, int h) {
    delete[] cpu_bits; cpu_bits = nullptr;
    cpu_w = w; cpu_h = h; cpu_pitch = 0;
    if (w <= 0 || h <= 0) return;
    int bpp = format.getPitch();
    if (bpp <= 0) return;
    cpu_pitch = w * bpp;
    cpu_bits = new unsigned char[(size_t)cpu_pitch * (size_t)h]();
}

bool gxCanvas::ensureTemp(int w, int h, int fmt) const {
    if (!graphics || !graphics->dir3dDev) return false;
    if (t_surf) {
        D3DSURFACE_DESC tdesc;
        if (SUCCEEDED(t_surf->GetDesc(&tdesc)) && (int)tdesc.Width == w && (int)tdesc.Height == h && (int)tdesc.Format == fmt)
            return true;
        t_surf->Release(); t_surf = nullptr;
    }
    return SUCCEEDED(graphics->dir3dDev->CreateOffscreenPlainSurface(
        w, h, (D3DFORMAT)fmt, D3DPOOL_SYSTEMMEM, &t_surf, nullptr));
}

bool gxCanvas::pullD3D() const {
    if (!d3d_dirty) return true;
    d3d_dirty = false;
    if (!cpu_bits || !surf || !graphics || !graphics->dir3dDev) return false;
    D3DSURFACE_DESC desc;
    if (FAILED(surf->GetDesc(&desc))) return false;
    int w = min<int>(cpu_w, (int)desc.Width), h = min<int>(cpu_h, (int)desc.Height);
    if (w <= 0 || h <= 0) return true;
    int bpp = format.getPitch();
    if (bpp <= 0) return false;
    if (desc.Usage & D3DUSAGE_RENDERTARGET) {
        if (!ensureTemp(desc.Width, desc.Height, (int)desc.Format)) return false;
        if (FAILED(graphics->dir3dDev->GetRenderTargetData(surf, t_surf))) return false;
        D3DLOCKED_RECT lr;
        if (FAILED(t_surf->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return false;
        int row = min(cpu_pitch, (int)lr.Pitch);
        if (row > w * bpp) row = w * bpp;
        for (int y = 0; y < h; ++y)
            memcpy(cpu_bits + (size_t)y * cpu_pitch, (unsigned char*)lr.pBits + (size_t)y * lr.Pitch, row);
        t_surf->UnlockRect();
    }
    else {
        D3DLOCKED_RECT lr;
        if (FAILED(surf->LockRect(&lr, nullptr, D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK))) return false;
        int row = min(cpu_pitch, (int)lr.Pitch);
        if (row > w * bpp) row = w * bpp;
        for (int y = 0; y < h; ++y)
            memcpy(cpu_bits + (size_t)y * cpu_pitch, (unsigned char*)lr.pBits + (size_t)y * lr.Pitch, row);
        surf->UnlockRect();
    }
    return true;
}

bool gxCanvas::pushRectD3D(const RECT& r) const {
    if (!cpu_bits || !surf || !graphics || !graphics->dir3dDev) return false;
    RECT c = r;
    if (c.left < 0) c.left = 0; if (c.top < 0) c.top = 0;
    if (c.right > cpu_w) c.right = cpu_w; if (c.bottom > cpu_h) c.bottom = cpu_h;
    if (c.right <= c.left || c.bottom <= c.top) return true;
    D3DSURFACE_DESC desc;
    if (FAILED(surf->GetDesc(&desc))) return false;
    int bpp = format.getPitch();
    if (bpp <= 0) return false;
    if (desc.Usage & D3DUSAGE_RENDERTARGET) {
        if (!ensureTemp(desc.Width, desc.Height, (int)desc.Format)) return false;
        D3DLOCKED_RECT lr;
        if (FAILED(t_surf->LockRect(&lr, &c, 0))) return false;
        int row = (c.right - c.left) * bpp;
        for (LONG y = c.top; y < c.bottom; ++y)
            memcpy((unsigned char*)lr.pBits + (size_t)(y - c.top) * lr.Pitch,
                cpu_bits + (size_t)y * cpu_pitch + (size_t)c.left * bpp, row);
        t_surf->UnlockRect();
        POINT pt = { c.left, c.top };
        return SUCCEEDED(graphics->dir3dDev->UpdateSurface(t_surf, &c, surf, &pt));
    }
    D3DLOCKED_RECT lr;
    if (FAILED(surf->LockRect(&lr, &c, 0))) return false;
    int row = (c.right - c.left) * bpp;
    for (LONG y = c.top; y < c.bottom; ++y)
        memcpy((unsigned char*)lr.pBits + (size_t)(y - c.top) * lr.Pitch,
            cpu_bits + (size_t)y * cpu_pitch + (size_t)c.left * bpp, row);
    surf->UnlockRect();
    return true;
}

struct QuadVertex {
    float x, y, z, rhw;
    float u, v;
};
static const DWORD QUAD_FVF = D3DFVF_XYZRHW | D3DFVF_TEX1;

gxCanvas::gxCanvas(gxGraphics* g, IDirect3DSurface9* s, int f) :
    graphics(g), plain_surf(s), tex(nullptr), cube_tex(nullptr), surf(s), z_surf(nullptr),
    flags(f), cube_mode(CUBEMODE_REFLECTION | CUBESPACE_WORLD),
    t_surf(nullptr), cm_mask(nullptr), locked_cnt(0), mod_cnt(0), remip_cnt(0),
    blit_tex(nullptr), blit_tex_mod_cnt(-1), blit_tex_mask(~0u), lock_is_rt(false), lock_ro(false), effect2D(nullptr), has_mask(false), sdlDirtyValid(false),
    blit_batch_depth(0), blit_batch_active(false), blit_batch_saved(nullptr) {
    memset(cube_surfs, 0, sizeof(cube_surfs));

    D3DSURFACE_DESC desc;
    surf->GetDesc(&desc);
    format.setFormat(desc.Format);
    allocCPUStore(desc.Width, desc.Height);

    clip_rect.left = clip_rect.top = 0;
    clip_rect.right = desc.Width;
    clip_rect.bottom = desc.Height;
    logical_w = desc.Width;
    logical_h = desc.Height;
    mipmapNeeded = (flags & CANVAS_TEX_MIPMAP) != 0;
    cm_pitch = (clip_rect.right + 31) / 32 + 1;
    setMask(0); setColor(~0); setClsColor(0);
    has_mask = false;
    setOrigin(0, 0); setHandle(0, 0);
    setFont(graphics->getDefaultFont());
    setViewport(0, 0, getWidth(), getHeight());
}

gxCanvas::gxCanvas(gxGraphics* g, IDirect3DTexture9* t, int f) :
    graphics(g), plain_surf(nullptr), tex(t), cube_tex(nullptr), surf(nullptr), z_surf(nullptr),
    flags(f), cube_mode(CUBEMODE_REFLECTION | CUBESPACE_WORLD),
    t_surf(nullptr), cm_mask(nullptr), locked_cnt(0), mod_cnt(0), remip_cnt(0),
    blit_tex(nullptr), blit_tex_mod_cnt(-1), blit_tex_mask(~0u), lock_is_rt(false), lock_ro(false), effect2D(nullptr), has_mask(false), sdlDirtyValid(false),
    blit_batch_depth(0), blit_batch_active(false), blit_batch_saved(nullptr) {
    memset(cube_surfs, 0, sizeof(cube_surfs));

    tex->GetSurfaceLevel(0, &surf);

    D3DSURFACE_DESC desc;
    surf->GetDesc(&desc);
    format.setFormat(desc.Format);
    allocCPUStore(desc.Width, desc.Height);

    clip_rect.left = clip_rect.top = 0;
    clip_rect.right = desc.Width;
    clip_rect.bottom = desc.Height;
    logical_w = desc.Width;
    logical_h = desc.Height;
    mipmapNeeded = (flags & CANVAS_TEX_MIPMAP) != 0;
    cm_pitch = (clip_rect.right + 31) / 32 + 1;
    setMask(0); setColor(~0); setClsColor(0);
    has_mask = false;
    setOrigin(0, 0); setHandle(0, 0);
    setFont(graphics->getDefaultFont());
    setViewport(0, 0, getWidth(), getHeight());

    if (flags & gxCanvas::CANVAS_TEX_MIPMAP) ddUtil::buildMipMaps(tex);
}

gxCanvas::gxCanvas(gxGraphics* g, IDirect3DCubeTexture9* ct, int f) :
    graphics(g), plain_surf(nullptr), tex(nullptr), cube_tex(ct), surf(nullptr), z_surf(nullptr),
    flags(f), cube_mode(CUBEMODE_REFLECTION | CUBESPACE_WORLD),
    t_surf(nullptr), cm_mask(nullptr), locked_cnt(0), mod_cnt(0), remip_cnt(0),
    blit_tex(nullptr), blit_tex_mod_cnt(-1), blit_tex_mask(~0u), lock_is_rt(false), lock_ro(false), effect2D(nullptr), has_mask(false), sdlDirtyValid(false),
    blit_batch_depth(0), blit_batch_active(false), blit_batch_saved(nullptr) {

    D3DCUBEMAP_FACES faceMap[6] = {
        D3DCUBEMAP_FACE_NEGATIVE_X,
        D3DCUBEMAP_FACE_POSITIVE_Z,
        D3DCUBEMAP_FACE_POSITIVE_X,
        D3DCUBEMAP_FACE_NEGATIVE_Z,
        D3DCUBEMAP_FACE_POSITIVE_Y,
        D3DCUBEMAP_FACE_NEGATIVE_Y
    };
    for (int k = 0; k < 6; ++k)
        cube_tex->GetCubeMapSurface(faceMap[k], 0, &cube_surfs[k]);
    surf = cube_surfs[2];

    D3DSURFACE_DESC desc;
    surf->GetDesc(&desc);
    format.setFormat(desc.Format);
    allocCPUStore(desc.Width, desc.Height);

    clip_rect.left = clip_rect.top = 0;
    clip_rect.right = desc.Width;
    clip_rect.bottom = desc.Height;
    logical_w = desc.Width;
    logical_h = desc.Height;
    mipmapNeeded = (flags & CANVAS_TEX_MIPMAP) != 0;
    cm_pitch = (clip_rect.right + 31) / 32 + 1;
    setMask(0); setColor(~0); setClsColor(0);
    has_mask = false;
    setOrigin(0, 0); setHandle(0, 0);
    setFont(graphics->getDefaultFont());
    setViewport(0, 0, getWidth(), getHeight());
}

gxCanvas::~gxCanvas() {
    sdlgpu::InvalidateCanvasTextures(this);
    sdlgpu::InvalidateTextAtlas(this);
    delete[] cm_mask;
    delete[] cpu_bits; cpu_bits = nullptr;
    if (locked_cnt) surf->UnlockRect();
    if (t_surf) t_surf->Release();
    if (blit_tex) { blit_tex->Release(); blit_tex = nullptr; }
    releaseZBuffer();

    for (int k = 0; k < 6; ++k) {
        if (cube_surfs[k]) { cube_surfs[k]->Release(); cube_surfs[k] = nullptr; }
    }

    if (tex && surf) { surf->Release(); surf = nullptr; }

    if (tex) { tex->Release();       tex = nullptr; }
    if (cube_tex) { cube_tex->Release();  cube_tex = nullptr; }
    if (plain_surf && !tex && !cube_tex) { plain_surf->Release(); plain_surf = nullptr; }
}

void gxCanvas::backup() {
    if (flags & CANVAS_TEX_CUBE) return;
    if (!surf) return;

    D3DSURFACE_DESC desc;
    if (FAILED(surf->GetDesc(&desc))) return;

    IDirect3DDevice9* dev = graphics->dir3dDev;
    if (!dev) return;

    if (t_surf) { t_surf->Release(); t_surf = nullptr; }
    if (FAILED(dev->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &t_surf, nullptr))) {
        return;
    }

    bool isRT = (desc.Usage & D3DUSAGE_RENDERTARGET) != 0;
    if (isRT) {
        dev->GetRenderTargetData(surf, t_surf);
    }
    else {
        D3DLOCKED_RECT srcLR, dstLR;
        if (SUCCEEDED(surf->LockRect(&srcLR, nullptr, D3DLOCK_READONLY))) {
            if (SUCCEEDED(t_surf->LockRect(&dstLR, nullptr, 0))) {
                for (UINT y = 0; y < desc.Height; ++y) {
                    memcpy((BYTE*)dstLR.pBits + y * dstLR.Pitch,
                        (BYTE*)srcLR.pBits + y * srcLR.Pitch,
                        min((UINT)dstLR.Pitch, (UINT)srcLR.Pitch));
                }
                t_surf->UnlockRect();
            }
            surf->UnlockRect();
        }
    }
}

void gxCanvas::restoreZBuffer() {
	if (z_surf) {
		releaseZBuffer();
		attachZBuffer();
	}
}

void gxCanvas::restore() {
    if (!t_surf) return;

    D3DSURFACE_DESC tdesc;
    t_surf->GetDesc(&tdesc);

    IDirect3DDevice9* dev = graphics->dir3dDev;
    if (!dev) return;

    if (tex) {
        if (surf) surf->Release();
        tex->Release();
    }
    else if (plain_surf) {
        plain_surf->Release();
    }
    tex = nullptr;
    surf = nullptr;
    plain_surf = nullptr;

    if (blit_tex) { blit_tex->Release(); blit_tex = nullptr; }
    blit_tex_mod_cnt = -1;

    IDirect3DTexture9* newTex = nullptr;
    if (FAILED(dev->CreateTexture(tdesc.Width, tdesc.Height, 1, D3DUSAGE_RENDERTARGET, tdesc.Format, D3DPOOL_DEFAULT, &newTex, nullptr))) return;

    IDirect3DSurface9* newSurf = nullptr;
    newTex->GetSurfaceLevel(0, &newSurf);

    RECT fullRect = { 0, 0, (LONG)tdesc.Width, (LONG)tdesc.Height };
    dev->UpdateSurface(t_surf, &fullRect, newSurf, nullptr);

    tex = newTex;
    surf = newSurf;
}

IDirect3DSurface9* gxCanvas::getSurface() const {
    return surf;
}

IDirect3DBaseTexture9* gxCanvas::getTexture() const {
    if (cube_tex) return cube_tex;
    if (tex)      return tex;
    return nullptr;
}

IDirect3DBaseTexture9* gxCanvas::getTexSurface() const {
    if (mod_cnt != remip_cnt && tex && (flags & CANVAS_TEX_MIPMAP) && mipmapNeeded) {
        ddUtil::buildMipMaps(tex);
    }
    remip_cnt = mod_cnt;
    return getTexture();
}

bool gxCanvas::clip(RECT* d) const {
    return ::clip(viewport, d);
}
bool gxCanvas::clip(RECT* d, RECT* s) const {
    return ::clip(viewport, d, s);
}

void gxCanvas::set2DEffect(gxEffect* effect) {
    effect2D = effect;
}

gxEffect* gxCanvas::get2DEffect() const {
    return effect2D;
}

void gxCanvas::updateBitMask(const RECT& r) const {
    int w = r.right - r.left; if (w <= 0) return;
    int h = r.bottom - r.top; if (h <= 0) return;

    lock();
    RECT t = r;
    t.left &= ~31;
    t.right = (t.right + 31) & ~31;
    w = (t.right - t.left) / 32;
    unsigned char* src_row = locked_surf + t.top * locked_pitch + t.left * format.getPitch();
    unsigned* dest_row = cm_mask + t.top * cm_pitch + t.left / 32;
    unsigned mask_argb = format.toARGB(mask_surf) & 0xffffff;

    while (h--) {
        unsigned* dest = dest_row;
        unsigned char* src = src_row;
        for (int c = 0; c < w; ++c) {
            unsigned mask = 0;
            for (int x = 0; x < 32; ++x) {
                unsigned pix = format.getPixel(src) & 0xffffff;
                mask = (mask << 1) | (pix != mask_argb);
                src += format.getPitch();
            }
            *dest++ = mask;
        }
        dest_row += cm_pitch;
        src_row += locked_pitch;
    }
    unlock();
}

void gxCanvas::setModify(int n) { mod_cnt = n; }
int  gxCanvas::getModify() const { return mod_cnt; }

bool gxCanvas::attachZBuffer() {
    if (z_surf) return true;
    D3DSURFACE_DESC desc;
    surf->GetDesc(&desc);
    IDirect3DDevice9* dev = graphics->dir3dDev;
    HRESULT hr = dev->CreateDepthStencilSurface(desc.Width, desc.Height, graphics->zbuffFmt, desc.MultiSampleType, desc.MultiSampleQuality, TRUE, &z_surf, nullptr);
    if (FAILED(hr) || !z_surf) {
        char buf[256];
        sprintf(buf, "CreateDepthStencilSurface failed: 0x%08X", hr);
        // MessageBoxA(NULL, buf, "Error", MB_OK);
        return false;
    }
    return true;
}

void gxCanvas::releaseZBuffer() {
    if (!z_surf) return;
    z_surf->Release();
    z_surf = nullptr;
}

void gxCanvas::damageImpl(const RECT& r, bool cpuSource) const {
    if (cpuSource) {
        if (d3d_dirty) pullD3D();
    }
    else {
        d3d_dirty = true;
    }
    ++mod_cnt;
    if (!sdlDirtyValid) { sdlDirtyRect = r; sdlDirtyValid = true; }
    else {
        if (r.left < sdlDirtyRect.left) sdlDirtyRect.left = r.left;
        if (r.top < sdlDirtyRect.top) sdlDirtyRect.top = r.top;
        if (r.right > sdlDirtyRect.right) sdlDirtyRect.right = r.right;
        if (r.bottom > sdlDirtyRect.bottom) sdlDirtyRect.bottom = r.bottom;
    }
    if (cpuSource) pushRectD3D(r);
    if (cm_mask) updateBitMask(r);
}

bool gxCanvas::pushAllD3D() const {
    if (!cpu_bits || cpu_w <= 0 || cpu_h <= 0) return false;
    RECT full = { 0, 0, cpu_w, cpu_h };
    if (!pushRectD3D(full)) return false;
    d3d_dirty = false;
    return true;
}

void gxCanvas::damage(const RECT& r) const {
    damageImpl(r, true);
}

void gxCanvas::damageD3D(const RECT& r) const {
    damageImpl(r, false);
}

void gxCanvas::damageScene(const RECT& r) const {
    d3d_dirty = false;
    if (cm_mask) updateBitMask(r);
}

void gxCanvas::setFont(gxFont* f) { font = f; }

void gxCanvas::setMask(unsigned argb) { mask_surf = format.fromARGB(argb); has_mask = true; }

void gxCanvas::setColor(unsigned argb) { color_argb = argb; color_surf = format.fromARGB(argb); }

void gxCanvas::setClsColor(unsigned argb) { clsColor_surf = format.fromARGB(argb); }

void gxCanvas::setOrigin(int x, int y) { origin_x = x; origin_y = y; }

void gxCanvas::setHandle(int x, int y) { handle_x = x; handle_y = y; }

void gxCanvas::setViewport(int x, int y, int w, int h) {
    Rect r(x, y, w, h);
    if (!::clip(clip_rect, &r)) r = Rect(0, 0, 0, 0);
    viewport = r;
}

struct GpuBack {
    struct SDL_GPUDevice* dev = nullptr;
    int cw = 0, ch = 0, vx = 0, vy = 0, vw = 0, vh = 0;
};

static bool gpuBackbuffer(gxCanvas* self, GpuBack& out) {
    if (!self) return false;
    gxGraphics* gfx = self->graphics;
    if (!gfx || !gfx->runtime || !gfx->runtime->sdlGpu) return false;
    if (self != gfx->getBackCanvas()) return false;
    int cw = self->getWidth(), ch = self->getHeight();
    if (cw <= 0 || ch <= 0) return false;
    out.dev = (struct SDL_GPUDevice*)gfx->runtime->sdlGpu;
    out.cw = cw;
    out.ch = ch;
    self->getViewport(&out.vx, &out.vy, &out.vw, &out.vh);
    return true;
}

static bool queueAbsRun(const GpuBack& b, int x0, int y0, int x1, int y1, unsigned argb) {
    if (x0 < b.vx) x0 = b.vx;
    if (y0 < b.vy) y0 = b.vy;
    if (x1 > b.vx + b.vw) x1 = b.vx + b.vw;
    if (y1 > b.vy + b.vh) y1 = b.vy + b.vh;
    if (x1 <= x0 || y1 <= y0) return true;
    return sdlgpu::QueueRectFilled(b.dev, (unsigned)b.cw, (unsigned)b.ch,
        (float)x0, (float)y0, (float)(x1 - x0), (float)(y1 - y0), argb);
}

static bool tryGpuRect(gxCanvas* self, int x, int y, int w, int h, unsigned argb, bool solid) {
    if (!self || w <= 0 || h <= 0) return true;
    GpuBack b;
    if (!gpuBackbuffer(self, b)) return false;
    int ox = 0, oy = 0;
    self->getOrigin(&ox, &oy);
    auto queueClipped = [&](int rx, int ry, int rw, int rh) -> bool {
        return queueAbsRun(b, rx + ox, ry + oy, rx + ox + rw, ry + oy + rh, argb);
    };
    if (solid) return queueClipped(x, y, w, h);
    if (!queueClipped(x, y, w, 1)) return false;
    if (!queueClipped(x, y + h - 1, w, 1)) return false;
    if (h > 2) {
        if (!queueClipped(x, y + 1, 1, h - 2)) return false;
        if (w > 1 && !queueClipped(x + w - 1, y + 1, 1, h - 2)) return false;
    }
    return true;
}

void gxCanvas::cls() {
    unsigned argb = format.toARGB(clsColor_surf);
    if (((argb >> 24) & 0xff) == 255) {
        GpuBack b;
        if (gpuBackbuffer(this, b)) {
            if (queueAbsRun(b, viewport.left, viewport.top, viewport.right, viewport.bottom, argb)) return;
        }
    }
    fillRect(viewport, argb);
    damage(viewport);
}

void gxCanvas::plot(int x, int y) {
    unsigned argb = format.toARGB(color_surf);
    if (tryGpuRect(this, x, y, 1, 1, argb, true)) return;
    x += origin_x; if (x < viewport.left || x >= viewport.right)  return;
    y += origin_y; if (y < viewport.top || y >= viewport.bottom) return;
    Rect dest(x, y, 1, 1);
    fillRect(dest, argb);
    damage(dest);
}

void gxCanvas::line(int x0, int y0, int x1, int y1) {
    int ddf, padj, sadj;
    int dx, dy, sx, sy, ax, ay;
    x0 += origin_x; y0 += origin_y;
    x1 += origin_x; y1 += origin_y;

    int cx0 = viewport.left, cx1 = viewport.right - 1;
    int cy0 = viewport.top, cy1 = viewport.bottom - 1;

    while (true) {
        int clip0 = 0, clip1 = 0;
        if (y0 > cy1) clip0 |= 1; else if (y0 < cy0) clip0 |= 2;
        if (x0 > cx1) clip0 |= 4; else if (x0 < cx0) clip0 |= 8;
        if (y1 > cy1) clip1 |= 1; else if (y1 < cy0) clip1 |= 2;
        if (x1 > cx1) clip1 |= 4; else if (x1 < cx0) clip1 |= 8;
        if ((clip0 | clip1) == 0) break;
        if ((clip0 & clip1) != 0) return;
        if ((clip0 & 1) == 1) { x0 = x0 + ((x1 - x0) * (cy1 - y0)) / (y1 - y0); y0 = cy1; continue; }
        if ((clip0 & 2) == 2) { x0 = x0 + ((x1 - x0) * (cy0 - y0)) / (y1 - y0); y0 = cy0; continue; }
        if ((clip0 & 4) == 4) { y0 = y0 + ((y1 - y0) * (cx1 - x0)) / (x1 - x0); x0 = cx1; continue; }
        if ((clip0 & 8) == 8) { y0 = y0 + ((y1 - y0) * (cx0 - x0)) / (x1 - x0); x0 = cx0; continue; }
        if ((clip1 & 1) == 1) { x1 = x0 + ((x1 - x0) * (cy1 - y0)) / (y1 - y0); y1 = cy1; continue; }
        if ((clip1 & 2) == 2) { x1 = x0 + ((x1 - x0) * (cy0 - y0)) / (y1 - y0); y1 = cy0; continue; }
        if ((clip1 & 4) == 4) { y1 = y0 + ((y1 - y0) * (cx1 - x0)) / (x1 - x0); x1 = cx1; continue; }
        if ((clip1 & 8) == 8) { y1 = y0 + ((y1 - y0) * (cx0 - x0)) / (x1 - x0); x1 = cx0; continue; }
    }
    dx = x1 - x0; dy = y1 - y0;
    if ((dx | dy) == 0) { plot(x0 - origin_x, y0 - origin_y); return; }
    if (dx == 0 || dy == 0) {
        GpuBack b;
        if (gpuBackbuffer(this, b)) {
            int lx0 = dx == 0 ? x0 : (x0 < x1 ? x0 : x1);
            int ly0 = dy == 0 ? y0 : (y0 < y1 ? y0 : y1);
            int lx1 = dx == 0 ? x0 + 1 : (x0 < x1 ? x1 + 1 : x0 + 1);
            int ly1 = dy == 0 ? y0 + 1 : (y0 < y1 ? y1 + 1 : y0 + 1);
            if (queueAbsRun(b, lx0, ly0, lx1, ly1, format.toARGB(color_surf))) return;
        }
    }
    if (dx >= 0) { sx = 1; ax = dx; }
    else { sx = -1; ax = -dx; }
    if (dy >= 0) { sy = 1; ay = dy; }
    else { sy = -1; ay = -dy; }
    int px0 = x0, py0 = y0, px1 = x1, py1 = y1;
    lock();
    if (ax > ay) {
        ddf = -ax; sadj = ax + ax; padj = ay + ay;
        while (ax-- >= 0) { setPixelFast(x0, y0, color_argb); x0 += sx; ddf += padj; if (ddf >= 0) { y0 += sy; ddf -= sadj; } }
    }
    else {
        ddf = -ay; sadj = ay + ay; padj = ax + ax;
        while (ay-- >= 0) { setPixelFast(x0, y0, color_argb); y0 += sy; ddf += padj; if (ddf >= 0) { x0 += sx; ddf -= sadj; } }
    }
    unlock();
    Rect dmg(px0 < px1 ? px0 : px1, py0 < py1 ? py0 : py1,
        (px0 < px1 ? px1 : px0) - (px0 < px1 ? px0 : px1) + 1,
        (py0 < py1 ? py1 : py0) - (py0 < py1 ? py0 : py1) + 1);
    damage(dmg);
}

static bool isRenderTarget(IDirect3DSurface9* s);


static bool tryGpuSprite(gxCanvas* self, const RECT& dest_r, gxCanvas* src, const RECT& src_r, unsigned tint, bool smooth) {
    if (!self || !src || src == self) return false;
    if (dest_r.right <= dest_r.left || dest_r.bottom <= dest_r.top) return true;
    if (src_r.right <= src_r.left || src_r.bottom <= src_r.top) return true;
    gxGraphics* gfx = self->graphics;
    if (!gfx || !gfx->runtime || !gfx->runtime->sdlGpu) return false;
    if (self != gfx->getBackCanvas()) return false;
    if (self->get2DEffect()) return false;
    struct SDL_GPUDevice* dev = (struct SDL_GPUDevice*)gfx->runtime->sdlGpu;
    struct SDL_GPUTexture* tex = (struct SDL_GPUTexture*)sdlgpu::GetCanvasTexture(dev, src);
    if (!tex) return false;
    int cw = self->getWidth(), ch = self->getHeight();
    int tw = src->getWidth(), th = src->getHeight();
    if (cw <= 0 || ch <= 0 || tw <= 0 || th <= 0) return false;
    sdlgpu::TextQuad q{};
    q.destX = (float)dest_r.left;
    q.destY = (float)dest_r.top;
    q.destW = (float)(dest_r.right - dest_r.left);
    q.destH = (float)(dest_r.bottom - dest_r.top);
    q.srcX = (float)src_r.left;
    q.srcY = (float)src_r.top;
    q.srcW = (float)(src_r.right - src_r.left);
    q.srcH = (float)(src_r.bottom - src_r.top);
    q.color = tint;
    return sdlgpu::QueueSpriteQuad(dev, tex, smooth, (unsigned)cw, (unsigned)ch, (unsigned)tw, (unsigned)th, &q);
}

void gxCanvas::rect(int x, int y, int w, int h, bool solid) {
    unsigned argb = format.toARGB(color_surf);
    if (tryGpuRect(this, x, y, w, h, argb, solid)) return;
    x += origin_x; y += origin_y;
    Rect dest(x, y, w, h);
    if (!clip(&dest)) return;
    if (solid) {
        fillRect(dest, argb);
        damage(dest);
        return;
    }
    Rect r1(x, y, w, 1);           if (clip(&r1)) fillRect(r1, argb);
    Rect r2(x, y, 1, h);           if (clip(&r2)) fillRect(r2, argb);
    Rect r3(x + w - 1, y, 1, h);   if (clip(&r3)) fillRect(r3, argb);
    Rect r4(x, y + h - 1, w, 1);   if (clip(&r4)) fillRect(r4, argb);
    damage(dest);
}

void gxCanvas::rectBlend(int x, int y, int w, int h, unsigned argb) {
    unsigned tintA = (argb >> 24) & 0xff;
    if (tintA == 255) { rect(x, y, w, h, true); return; }
    if (tintA == 0 || w <= 0 || h <= 0) return;
    if (tryGpuRect(this, x, y, w, h, argb, true)) return;
    x += origin_x; y += origin_y;
    Rect dest_r(x, y, w, h);
    if (!clip(&dest_r)) return;

    IDirect3DDevice9* dev = graphics ? graphics->dir3dDev : nullptr;
    if (!isRenderTarget(surf) || !dev) {
        if (!lock()) return;
        unsigned sR = (argb >> 16) & 0xff, sG = (argb >> 8) & 0xff, sB = argb & 0xff;
        unsigned invA = 255 - tintA;
        const PixelFormat& df = format;
        for (int yy = dest_r.top; yy < dest_r.bottom; ++yy) {
            for (int xx = dest_r.left; xx < dest_r.right; ++xx) {
                unsigned dstArgb = df.toARGB(getPixelFast(xx, yy));
                unsigned outR = (sR * tintA + ((dstArgb >> 16) & 0xff) * invA) / 255;
                unsigned outG = (sG * tintA + ((dstArgb >> 8) & 0xff) * invA) / 255;
                unsigned outB = (sB * tintA + (dstArgb & 0xff) * invA) / 255;
                unsigned outA = tintA + ((dstArgb >> 24) & 0xff) * invA / 255;
                setPixelFast(xx, yy, df.fromARGB((outA << 24) | (outR << 16) | (outG << 8) | outB));
            }
        }
        unlock();
        damage(dest_r);
        return;
    }

    bool ownBatch = !blit_batch_active;
    if (ownBatch) beginBlitBatch();
    if (!blit_batch_active) {
        if (ownBatch) endBlitBatch();
        rect(x - origin_x, y - origin_y, w, h, true);
        return;
    }

    IDirect3DBaseTexture9* oldTex = nullptr;
    DWORD oldFVF = 0, oldCOp, oldCArg1, oldAOp, oldAArg1;
    DWORD oldAB, oldSB, oldDB, oldAT, oldLight, oldZ;
    dev->GetTexture(0, &oldTex);
    dev->GetFVF(&oldFVF);
    dev->GetTextureStageState(0, D3DTSS_COLOROP, &oldCOp);
    dev->GetTextureStageState(0, D3DTSS_COLORARG1, &oldCArg1);
    dev->GetTextureStageState(0, D3DTSS_ALPHAOP, &oldAOp);
    dev->GetTextureStageState(0, D3DTSS_ALPHAARG1, &oldAArg1);
    dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &oldAB);
    dev->GetRenderState(D3DRS_SRCBLEND, &oldSB);
    dev->GetRenderState(D3DRS_DESTBLEND, &oldDB);
    dev->GetRenderState(D3DRS_ALPHATESTENABLE, &oldAT);
    dev->GetRenderState(D3DRS_LIGHTING, &oldLight);
    dev->GetRenderState(D3DRS_ZENABLE, &oldZ);

    dev->SetTexture(0, nullptr);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);

    struct DiffVert { float x, y, z, rhw; DWORD color; };
    float x0 = (float)dest_r.left - 0.5f, y0 = (float)dest_r.top - 0.5f;
    float x1 = (float)dest_r.right - 0.5f, y1 = (float)dest_r.bottom - 0.5f;
    DiffVert verts[4] = {
        { x0, y0, 0.0f, 1.0f, argb },
        { x1, y0, 0.0f, 1.0f, argb },
        { x0, y1, 0.0f, 1.0f, argb },
        { x1, y1, 0.0f, 1.0f, argb }
    };
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, verts, sizeof(DiffVert));

    dev->SetTexture(0, oldTex);
    if (oldTex) oldTex->Release();
    dev->SetFVF(oldFVF);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, oldCOp);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, oldCArg1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, oldAOp);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, oldAArg1);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, oldAB);
    dev->SetRenderState(D3DRS_SRCBLEND, oldSB);
    dev->SetRenderState(D3DRS_DESTBLEND, oldDB);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, oldAT);
    dev->SetRenderState(D3DRS_LIGHTING, oldLight);
    dev->SetRenderState(D3DRS_ZENABLE, oldZ);

    damageD3D(dest_r);
    if (ownBatch) endBlitBatch();
}

static bool tryGpuOval(gxCanvas* self, int x1, int y1, int w, int h, unsigned argb, bool solid) {
    if (!self || w <= 0 || h <= 0) return true;
    GpuBack b;
    if (!gpuBackbuffer(self, b)) return false;
    int ox = 0, oy = 0;
    self->getOrigin(&ox, &oy);
    x1 += ox; y1 += oy;
    int dx0 = x1 < b.vx ? b.vx : x1;
    int dy0 = y1 < b.vy ? b.vy : y1;
    int dx1 = x1 + w > b.vx + b.vw ? b.vx + b.vw : x1 + w;
    int dy1 = y1 + h > b.vy + b.vh ? b.vy + b.vh : y1 + h;
    if (dx1 <= dx0 || dy1 <= dy0) return true;
    float xr = w * .5f, yr = h * .5f, ar = (float)w / (float)h;
    float cx = x1 + xr + .5f, cy = y1 + yr - .5f, rsq = yr * yr;
    if (solid) {
        float y = (float)dy0 - cy;
        for (int t = dy0; t < dy1; ++y, ++t) {
            float x = sqrtf(rsq - y * y) * ar;
            int xa = (int)floor(cx - x), xb = (int)floor(cx + x);
            if (xb <= xa) continue;
            if (!queueAbsRun(b, xa, t, xb, t + 1, argb)) return false;
        }
        return true;
    }
    int p_xa, p_xb, t, hh = (int)floor(cy);
    float y;
    p_xa = p_xb = (int)cx;
    t = dy0; y = (float)t - cy;
    if (dy0 > y1) { --t; --y; }
    for (; t <= hh; ++y, ++t) {
        float x = sqrtf(rsq - y * y) * ar;
        int xa = (int)floor(cx - x), xb = (int)floor(cx + x);
        if (!queueAbsRun(b, xa, t, p_xa > xa + 1 ? p_xa : xa + 1, t + 1, argb)) return false;
        if (!queueAbsRun(b, p_xb, t, xb > p_xb + 1 ? xb : p_xb + 1, t + 1, argb)) return false;
        p_xa = xa; p_xb = xb;
    }
    p_xa = p_xb = (int)cx;
    t = dy1 - 1; y = (float)t - cy;
    if (dy1 < y1 + h) { ++t; ++y; }
    for (; t > hh; --y, --t) {
        float x = sqrtf(rsq - y * y) * ar;
        int xa = (int)floor(cx - x), xb = (int)floor(cx + x);
        if (!queueAbsRun(b, xa, t, p_xa > xa + 1 ? p_xa : xa + 1, t + 1, argb)) return false;
        if (!queueAbsRun(b, p_xb, t, xb > p_xb + 1 ? xb : p_xb + 1, t + 1, argb)) return false;
        p_xa = xa; p_xb = xb;
    }
    return true;
}

void gxCanvas::oval(int x1, int y1, int w, int h, bool solid) {
    if (tryGpuOval(this, x1, y1, w, h, format.toARGB(color_surf), solid)) return;
    x1 += origin_x; y1 += origin_y;
    Rect dest(x1, y1, w, h);
    if (!clip(&dest)) return;
    float xr = w * .5f, yr = h * .5f, ar = (float)w / (float)h;
    float cx = x1 + xr + .5f, cy = y1 + yr - .5f, rsq = yr * yr, y;
    unsigned argb = format.toARGB(color_surf);
    if (solid) {
        y = dest.top - cy;
        for (int t = dest.top; t < dest.bottom; ++y, ++t) {
            float x = sqrtf(rsq - y * y) * ar;
            int xa = (int)floor(cx - x), xb = (int)floor(cx + x);
            if (xb <= xa || xa >= viewport.right || xb <= viewport.left) continue;
            Rect dr; dr.top = t; dr.bottom = t + 1;
            dr.left = xa < viewport.left ? viewport.left : xa;
            dr.right = xb > viewport.right ? viewport.right : xb;
            fillRect(dr, argb);
        }
        damage(dest);
        return;
    }
    int p_xa, p_xb, t, hh = (int)floor(cy);
    p_xa = p_xb = (int)cx;
    t = dest.top; y = t - cy;
    if (dest.top > y1) { --t; --y; }
    for (; t <= hh; ++y, ++t) {
        float x = sqrtf(rsq - y * y) * ar;
        int xa = (int)floor(cx - x), xb = (int)floor(cx + x);
        Rect r1(xa, t, p_xa - xa, 1); if (r1.right <= r1.left)r1.right = r1.left + 1; if (clip(&r1)) fillRect(r1, argb);
        Rect r2(p_xb, t, xb - p_xb, 1); if (r2.left >= r2.right)r2.left = r2.right - 1; if (clip(&r2)) fillRect(r2, argb);
        p_xa = xa; p_xb = xb;
    }
    p_xa = p_xb = (int)cx;
    t = dest.bottom - 1; y = t - cy;
    if (dest.bottom < y1 + h) { ++t; ++y; }
    for (; t > hh; --y, --t) {
        float x = sqrtf(rsq - y * y) * ar;
        int xa = (int)floor(cx - x), xb = (int)floor(cx + x);
        Rect r1(xa, t, p_xa - xa, 1); if (r1.right <= r1.left)r1.right = r1.left + 1; if (clip(&r1)) fillRect(r1, argb);
        Rect r2(p_xb, t, xb - p_xb, 1); if (r2.left >= r2.right)r2.left = r2.right - 1; if (clip(&r2)) fillRect(r2, argb);
        p_xa = xa; p_xb = xb;
    }
    damage(dest);
}

static void drawQuadWithEffect(IDirect3DDevice9* dev, gxEffect* effect, IDirect3DTexture9* tex, const RECT& dst, const RECT& src, int texW, int texH, bool useAlpha = false, unsigned color_argb = 0xFFFFFFFF) {
    D3DXMATRIX proj, view, world;
    D3DXMatrixIdentity(&world);
    D3DXMatrixIdentity(&view);
    D3DVIEWPORT9 vp;
    dev->GetViewport(&vp);
    float w = (float)vp.Width;
    float h = (float)vp.Height;
    D3DXMatrixOrthoOffCenterLH(&proj, 0.0f, w, h, 0.0f, 0.0f, 1.0f);

    effect->setAutoMatrices(world, view, proj);
    if (tex) {
        effect->setTexture("tex0", tex);
        effect->setTexture("SceneTex", tex);
    }
    if (useAlpha) {
        float color[4] = {
            ((color_argb >> 16) & 0xFF) / 255.0f,
            ((color_argb >> 8) & 0xFF) / 255.0f,
            (color_argb & 0xFF) / 255.0f,
            ((color_argb >> 24) & 0xFF) / 255.0f
        };
        effect->setVector("color", color);
    }

    UINT passes;
    if (effect->begin(&passes)) {
        float x0 = (float)dst.left - 0.5f;
        float y0 = (float)dst.top - 0.5f;
        float x1 = (float)dst.right - 0.5f;
        float y1 = (float)dst.bottom - 0.5f;
        float u0 = (float)src.left / texW;
        float v0 = (float)src.top / texH;
        float u1 = (float)src.right / texW;
        float v1 = (float)src.bottom / texH;
        struct QuadVertex { float x, y, z, rhw; float u, v; };
        QuadVertex verts[4] = {
            { x0, y0, 0.0f, 1.0f, u0, v0 },
            { x1, y0, 0.0f, 1.0f, u1, v0 },
            { x0, y1, 0.0f, 1.0f, u0, v1 },
            { x1, y1, 0.0f, 1.0f, u1, v1 }
        };
        static const DWORD FVF = D3DFVF_XYZRHW | D3DFVF_TEX1;

        for (UINT p = 0; p < passes; ++p) {
            effect->beginPass(p);
            dev->SetFVF(FVF);
            dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, verts, sizeof(QuadVertex));
            effect->endPass();
        }
        effect->end();
    }
}

static IDirect3DTexture9* getOrBuildBlitTex(IDirect3DDevice9* dev, gxCanvas* src, unsigned maskRGB) {
    if (src->blit_tex && src->blit_tex_mod_cnt == src->mod_cnt && src->blit_tex_mask == maskRGB)
        return src->blit_tex;

    if (src->blit_tex) { src->blit_tex->Release(); src->blit_tex = nullptr; }

    int texW = src->clip_rect.right;
    int texH = src->clip_rect.bottom;
    int logW = src->logical_w;
    int logH = src->logical_h;

    IDirect3DTexture9* newTex = nullptr;
    if (FAILED(dev->CreateTexture(texW, texH, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &newTex, nullptr)))
        return nullptr;

    IDirect3DSurface9* texSurf = nullptr;
    if (FAILED(newTex->GetSurfaceLevel(0, &texSurf))) { newTex->Release(); return nullptr; }

    if (!src->lock()) {
        texSurf->Release();
        newTex->Release();
        return nullptr;
    }

    D3DLOCKED_RECT dstLR;
    if (FAILED(texSurf->LockRect(&dstLR, nullptr, 0))) {
        src->unlock();
        texSurf->Release();
        newTex->Release();
        return nullptr;
    }

    const unsigned char* srcBits = src->locked_surf;
    int srcPitch = src->locked_pitch;
    bool doMask = (maskRGB != ~0u);
    const PixelFormat& fmt = src->format;
    int pitch = fmt.getPitch();
    bool srcHasAlpha = fmt.hasAlphaMask();

    bool fastCopy = fmt.is8888();

    for (int y = 0; y < texH; ++y) {
        unsigned* dstRow = (unsigned*)((unsigned char*)dstLR.pBits + y * dstLR.Pitch);
        if (y >= logH) { memset(dstRow, 0, texW * sizeof(unsigned)); continue; }
        const unsigned char* srcRow = srcBits + y * srcPitch;

        if (fastCopy) {
            memcpy(dstRow, srcRow, logW * 4);
            if (!srcHasAlpha) {
                for (int x = 0; x < logW; ++x) dstRow[x] |= 0xff000000u;
            }
            else {
                for (int x = 0; x < logW; ++x)
                    if ((dstRow[x] >> 24) == 0) dstRow[x] = 0;
            }
            if (doMask) {
                for (int x = 0; x < logW; ++x)
                    if ((dstRow[x] & 0x00ffffffu) == maskRGB) dstRow[x] = 0x00000000u;
            }
        }
        else {
            for (int x = 0; x < logW; ++x) {
                unsigned argb = fmt.toARGB(fmt.getPixel((void*)(srcRow + x * pitch)));
                if (doMask && (argb & 0x00ffffffu) == maskRGB)
                    argb = 0x00000000u;
                else if (!srcHasAlpha) {
                    argb |= 0xff000000u;
                }
                else if ((argb >> 24) == 0) {
                    argb = 0;
                }
                dstRow[x] = argb;
            }
        }

        // zero out padding it never bleeds into samples!!
        if (logW < texW) memset(dstRow + logW, 0, (texW - logW) * sizeof(unsigned));
    }

    texSurf->UnlockRect();
    src->unlock();
    texSurf->Release();

    src->blit_tex = newTex;
    src->blit_tex_mod_cnt = src->mod_cnt;
    src->blit_tex_mask = maskRGB;
    return newTex;
}

static void drawBlitQuad(IDirect3DDevice9* dev,
    IDirect3DTexture9* tex,
    const RECT& dst,
    const RECT& srcRect,
    int texW, int texH)
{
    float x0 = (float)dst.left - 0.5f;
    float y0 = (float)dst.top - 0.5f;
    float x1 = (float)dst.right - 0.5f;
    float y1 = (float)dst.bottom - 0.5f;

    float u0 = (float)srcRect.left / texW;
    float v0 = (float)srcRect.top / texH;
    float u1 = (float)srcRect.right / texW;
    float v1 = (float)srcRect.bottom / texH;

    QuadVertex verts[4] = {
        { x0, y0, 0.0f, 1.0f, u0, v0 },
        { x1, y0, 0.0f, 1.0f, u1, v0 },
        { x0, y1, 0.0f, 1.0f, u0, v1 },
        { x1, y1, 0.0f, 1.0f, u1, v1 },
    };

    dev->SetTexture(0, tex);
    dev->SetFVF(QUAD_FVF);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, verts, sizeof(QuadVertex));
}

static void setupBlitRenderState(IDirect3DDevice9* dev, bool solid) {
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MIPMAPLODBIAS, 0);
    for (int s = 1; s < 8; ++s) {
        dev->SetTexture(s, nullptr);
        dev->SetTextureStageState(s, D3DTSS_COLOROP, D3DTOP_DISABLE);
        dev->SetTextureStageState(s, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    }
    if (!solid) {
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
        dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
        dev->SetRenderState(D3DRS_ALPHAREF, 0);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    }
    else {
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    }
}

struct SavedBlitState {
    IDirect3DSurface9* oldRT;
    IDirect3DSurface9* oldDS;
    IDirect3DBaseTexture9* oldTex;
    D3DVIEWPORT9 oldVP;
    DWORD oldZ, oldAlphaTest, oldAlphaFunc, oldAlphaRef, oldAlphaBlend;
    DWORD oldSrcBlend, oldDestBlend;
    DWORD oldLighting, oldTextureFactor;
    DWORD oldCOp, oldCArg1, oldCArg2, oldAOp, oldAArg1, oldAArg2, oldMag, oldMin, oldLodBias;
    IDirect3DBaseTexture9* extraTex[7];
    DWORD extraCOp[7], extraAOp[7];
};

static void disableExtraTextureStages(IDirect3DDevice9* dev) {
    for (int s = 1; s < 8; ++s) {
        dev->SetTexture(s, nullptr);
        dev->SetTextureStageState(s, D3DTSS_COLOROP, D3DTOP_DISABLE);
        dev->SetTextureStageState(s, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    }
}

static void saveBlitState(IDirect3DDevice9* dev, SavedBlitState& s) {
    dev->GetRenderTarget(0, &s.oldRT);
    dev->GetDepthStencilSurface(&s.oldDS);
    dev->GetTexture(0, &s.oldTex);
    for (int i = 0; i < 7; ++i) {
        dev->GetTexture(1 + i, &s.extraTex[i]);
        dev->GetTextureStageState(1 + i, D3DTSS_COLOROP, &s.extraCOp[i]);
        dev->GetTextureStageState(1 + i, D3DTSS_ALPHAOP, &s.extraAOp[i]);
    }
    dev->GetViewport(&s.oldVP);
    dev->GetRenderState(D3DRS_ZENABLE, &s.oldZ);
    dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &s.oldAlphaBlend);
    dev->GetRenderState(D3DRS_SRCBLEND, &s.oldSrcBlend);
    dev->GetRenderState(D3DRS_DESTBLEND, &s.oldDestBlend);
    dev->GetRenderState(D3DRS_ALPHATESTENABLE, &s.oldAlphaTest);
    dev->GetRenderState(D3DRS_ALPHAFUNC, &s.oldAlphaFunc);
    dev->GetRenderState(D3DRS_ALPHAREF, &s.oldAlphaRef);
    dev->GetRenderState(D3DRS_LIGHTING, &s.oldLighting);
    dev->GetRenderState(D3DRS_TEXTUREFACTOR, &s.oldTextureFactor);
    dev->GetTextureStageState(0, D3DTSS_COLOROP, &s.oldCOp);
    dev->GetTextureStageState(0, D3DTSS_COLORARG1, &s.oldCArg1);
    dev->GetTextureStageState(0, D3DTSS_COLORARG2, &s.oldCArg2);
    dev->GetTextureStageState(0, D3DTSS_ALPHAOP, &s.oldAOp);
    dev->GetTextureStageState(0, D3DTSS_ALPHAARG1, &s.oldAArg1);
    dev->GetTextureStageState(0, D3DTSS_ALPHAARG2, &s.oldAArg2);
    dev->GetSamplerState(0, D3DSAMP_MAGFILTER, &s.oldMag);
    dev->GetSamplerState(0, D3DSAMP_MINFILTER, &s.oldMin);
    dev->GetSamplerState(0, D3DSAMP_MIPMAPLODBIAS, &s.oldLodBias);
}

static void restoreBlitState(IDirect3DDevice9* dev, SavedBlitState& s) {
    dev->SetRenderTarget(0, s.oldRT);
    dev->SetDepthStencilSurface(s.oldDS);
    if (s.oldRT) s.oldRT->Release();
    if (s.oldDS) s.oldDS->Release();
    dev->SetViewport(&s.oldVP);
    dev->SetRenderState(D3DRS_ZENABLE, s.oldZ);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, s.oldAlphaBlend);
    dev->SetRenderState(D3DRS_SRCBLEND, s.oldSrcBlend);
    dev->SetRenderState(D3DRS_DESTBLEND, s.oldDestBlend);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, s.oldAlphaTest);
    dev->SetRenderState(D3DRS_ALPHAFUNC, s.oldAlphaFunc);
    dev->SetRenderState(D3DRS_ALPHAREF, s.oldAlphaRef);
    dev->SetRenderState(D3DRS_LIGHTING, s.oldLighting);
    dev->SetRenderState(D3DRS_TEXTUREFACTOR, s.oldTextureFactor);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, s.oldCOp);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, s.oldCArg1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, s.oldCArg2);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, s.oldAOp);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, s.oldAArg1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, s.oldAArg2);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, s.oldMag);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, s.oldMin);
    dev->SetSamplerState(0, D3DSAMP_MIPMAPLODBIAS, s.oldLodBias);
    dev->SetTexture(0, s.oldTex);
    if (s.oldTex) s.oldTex->Release();
    for (int i = 0; i < 7; ++i) {
        dev->SetTexture(1 + i, s.extraTex[i]);
        dev->SetTextureStageState(1 + i, D3DTSS_COLOROP, s.extraCOp[i]);
        dev->SetTextureStageState(1 + i, D3DTSS_ALPHAOP, s.extraAOp[i]);
        if (s.extraTex[i]) s.extraTex[i]->Release();
    }
}

static bool isRenderTarget(IDirect3DSurface9* s) {
    D3DSURFACE_DESC desc;
    return SUCCEEDED(s->GetDesc(&desc)) && (desc.Usage & D3DUSAGE_RENDERTARGET);
}

static bool isBoundAsRenderTarget(IDirect3DDevice9* dev, IDirect3DSurface9* s) {
    if (!dev || !s) return false;
    IDirect3DSurface9* cur = nullptr;
    dev->GetRenderTarget(0, &cur);
    if (!cur) return false;
    bool same = (cur == s);
    cur->Release();
    return same;
}

void gxCanvas::beginBlitBatch() const {
    if (blit_batch_depth++ > 0) return;

    IDirect3DDevice9* dev = graphics ? graphics->dir3dDev : nullptr;
    if (!dev || !isRenderTarget(surf)) {
        blit_batch_active = false;
        return;
    }

    SavedBlitState* saved = new SavedBlitState();
    saveBlitState(dev, *saved);
    blit_batch_saved = saved;

    dev->SetRenderTarget(0, surf);
    dev->SetDepthStencilSurface(nullptr);
    D3DVIEWPORT9 vp = { 0, 0, (DWORD)clip_rect.right, (DWORD)clip_rect.bottom, 0.0f, 1.0f };
    dev->SetViewport(&vp);

    dev->SetSamplerState(0, D3DSAMP_MIPMAPLODBIAS, 0);

    dev->BeginScene();
    disableExtraTextureStages(dev);
    blit_batch_active = true;
}

void gxCanvas::endBlitBatch() const {
    if (blit_batch_depth == 0) return;
    if (--blit_batch_depth > 0) return;

    if (blit_batch_active) {
        IDirect3DDevice9* dev = graphics->dir3dDev;
        dev->EndScene();
        SavedBlitState* saved = (SavedBlitState*)blit_batch_saved;
        restoreBlitState(dev, *saved);
        delete saved;
        blit_batch_saved = nullptr;
        blit_batch_active = false;
    }
}

static void cpuBlit(gxCanvas* dest, const RECT& dest_r, gxCanvas* src, const RECT& src_r, bool solid) {
    D3DSURFACE_DESC destDesc;
    bool destIsSysMem = SUCCEEDED(dest->surf->GetDesc(&destDesc)) && destDesc.Pool == D3DPOOL_SYSTEMMEM;

    if (!destIsSysMem) {
        IDirect3DDevice9* dev = dest->graphics->dir3dDev;
        if (dev) {
            RECT srcRect = { src_r.left, src_r.top, src_r.right, src_r.bottom };
            RECT destRect = { dest_r.left, dest_r.top, dest_r.right, dest_r.bottom };
            HRESULT hr = dev->StretchRect(src->getSurface(), &srcRect,
                dest->getSurface(), &destRect,
                D3DTEXF_LINEAR);
            if (SUCCEEDED(hr)) {
                dest->damageD3D(dest_r);
                return;
            }
        }
    }

    int dw = dest_r.right - dest_r.left;
    int dh = dest_r.bottom - dest_r.top;
    int sw = src_r.right - src_r.left;
    int sh = src_r.bottom - src_r.top;
    bool stretch = (dw != sw || dh != sh);
    bool srcPreLocked = src->isLocked();
    bool destPreLocked = dest->isLocked();

    unsigned char* srcBits;  int srcPitchBytes;
    unsigned char* dstBits;  int dstPitchBytes;

    if (srcPreLocked) {
        srcBits = src->getLockedSurf();
        srcPitchBytes = src->getLockedPitch();
    }
    else {
        if (!src->lock()) return;
        srcBits = src->getLockedSurf();
        srcPitchBytes = src->getLockedPitch();
    }

    if (destPreLocked) {
        dstBits = dest->getLockedSurf();
        dstPitchBytes = dest->getLockedPitch();
    }
    else {
        if (!dest->lock()) {
            if (!srcPreLocked) src->unlock();
            return;
        }
        dstBits = dest->getLockedSurf();
        dstPitchBytes = dest->getLockedPitch();
    }

    const PixelFormat& sf = src->format;
    const PixelFormat& df = dest->format;
    int sp = sf.getPitch(), dp = df.getPitch();

    unsigned maskRGB = solid ? ~0u : (sf.toARGB(src->mask_surf) & 0x00ffffffu);
    bool doMask = (maskRGB != ~0u);
    bool doAlpha = sf.hasAlphaMask();

    if (!stretch && !doMask && !doAlpha && sp == dp && sf.getDepth() == df.getDepth()) {
        int rowBytes = dw * sp;
        for (int y = 0; y < dh; ++y) {
            const unsigned char* srow = srcBits + (src_r.top + y) * srcPitchBytes + src_r.left * sp;
            unsigned char* drow = dstBits + (dest_r.top + y) * dstPitchBytes + dest_r.left * dp;
            memcpy(drow, srow, rowBytes);
        }
    }
    else {
        for (int y = 0; y < dh; ++y) {
            int sy = stretch ? (y * sh / dh) : y;
            const unsigned char* srow = srcBits + (src_r.top + sy) * srcPitchBytes + src_r.left * sp;
            unsigned char* drow = dstBits + (dest_r.top + y) * dstPitchBytes + dest_r.left * dp;
            for (int x = 0; x < dw; ++x) {
                int sx = stretch ? (x * sw / dw) : x;
                unsigned argb = sf.toARGB(sf.getPixel((void*)(srow + sx * sp)));
                if (doMask && (argb & 0x00ffffffu) == maskRGB) continue;
                if (!doAlpha) argb |= 0xff000000u;
                df.setPixel(drow + x * dp, df.fromARGB(argb));
            }
        }
    }

    if (!destPreLocked) dest->unlock();
    if (!srcPreLocked) src->unlock();
    dest->damage(dest_r);
}

void gxCanvas::blit(int x, int y, gxCanvas* src, int src_x, int src_y,
    int src_w, int src_h, bool solid)
{
    x += origin_x - src->handle_x;
    y += origin_y - src->handle_y;

    Rect dest_r(x, y, src_w, src_h);
    Rect src_r(src_x, src_y, src_w, src_h);

    if (!clip(&dest_r, &src_r)) return;
    if (!::clip(src->clip_rect, &src_r, &dest_r)) return;

    if (solid && tryGpuSprite(this, dest_r, src, src_r, 0xffffffff, false)) return;

    if (solid) {
        D3DSURFACE_DESC srcDesc, dstDesc;
        if (SUCCEEDED(src->surf->GetDesc(&srcDesc)) && SUCCEEDED(surf->GetDesc(&dstDesc))) {
            if ((srcDesc.Usage & D3DUSAGE_RENDERTARGET) && (dstDesc.Usage & D3DUSAGE_RENDERTARGET)) {
                ddUtil::copy(graphics->dir3dDev, surf, dest_r.left, dest_r.top, dest_r.right - dest_r.left, dest_r.bottom - dest_r.top,
                    src->surf, src_r.left, src_r.top, src_r.right - src_r.left, src_r.bottom - src_r.top);
                damageD3D(dest_r);
                return;
            }
        }
    }

    if (!isRenderTarget(surf)) {
        cpuBlit(this, dest_r, src, src_r, solid);
        damage(dest_r);
        return;
    }

    IDirect3DDevice9* dev = graphics->dir3dDev;
    if (!dev) return;

    unsigned maskRGB = solid ? ~0u : (src->format.toARGB(src->mask_surf) & 0x00ffffffu);
    IDirect3DTexture9* blitTex = nullptr;
    if (solid) {
        IDirect3DBaseTexture9* tex = src->getTexture();
        if (tex && !isBoundAsRenderTarget(dev, src->surf)) {
            blitTex = (IDirect3DTexture9*)tex;
        }
    }
    if (!blitTex) {
        blitTex = getOrBuildBlitTex(dev, src, maskRGB);
        if (!blitTex) return;
    }

    bool ownBatch = !blit_batch_active;
    if (ownBatch) beginBlitBatch();

    if (!blit_batch_active) {
        endBlitBatch();
        return;
    }

    FillModeGuard guard(dev);

    if (effect2D) {
        drawQuadWithEffect(dev, effect2D, blitTex, dest_r, src_r, src->clip_rect.right, src->clip_rect.bottom, false, 0);
    }
    else {
        setupBlitRenderState(dev, solid);
        drawBlitQuad(dev, blitTex, dest_r, src_r, src->clip_rect.right, src->clip_rect.bottom);
    }

    damageD3D(dest_r);

    if (ownBatch) endBlitBatch();
}

void gxCanvas::blitstretch(int x, int y, int w, int h,
    gxCanvas* src, int src_x, int src_y,
    int src_w, int src_h, bool solid)
{
    x += origin_x - src->handle_x;
    y += origin_y - src->handle_y;

    Rect dest_r(x, y, w, h);
    if (!::clip(viewport, &dest_r)) return;

    int clipLeft = dest_r.left - x;
    int clipTop = dest_r.top - y;
    int clipRight = dest_r.right - x;
    int clipBottom = dest_r.bottom - y;

    Rect src_r;
    src_r.left = src_x + clipLeft * src_w / w;
    src_r.top = src_y + clipTop * src_h / h;
    src_r.right = src_x + clipRight * src_w / w;
    src_r.bottom = src_y + clipBottom * src_h / h;

    if (!::clip(src->clip_rect, &src_r)) return;

    if (!src->hasMask() && tryGpuSprite(this, dest_r, src, src_r, 0xffffffff, true)) return;

    if (!isRenderTarget(surf)) {
        cpuBlit(this, dest_r, src, src_r, solid);
        damage(dest_r);
        return;
    }

    IDirect3DDevice9* dev = graphics->dir3dDev;
    if (!dev) return;

    bool useAlpha = (src->getFlags() & gxCanvas::CANVAS_TEX_ALPHA) != 0;
    bool useMask = src->hasMask();
    if (solid && !useAlpha && !useMask) {
        D3DSURFACE_DESC srcDesc, dstDesc;
        if (SUCCEEDED(src->surf->GetDesc(&srcDesc)) && SUCCEEDED(surf->GetDesc(&dstDesc))) {
            bool a32 = (srcDesc.Format == D3DFMT_A8R8G8B8 || srcDesc.Format == D3DFMT_X8R8G8B8);
            bool b32 = (dstDesc.Format == D3DFMT_A8R8G8B8 || dstDesc.Format == D3DFMT_X8R8G8B8);
            if (srcDesc.Format == dstDesc.Format || (a32 && b32)) {
                RECT srcRect = { src_r.left, src_r.top, src_r.right, src_r.bottom };
                RECT dstRect = { dest_r.left, dest_r.top, dest_r.right, dest_r.bottom };
                if (SUCCEEDED(dev->StretchRect(src->surf, &srcRect, surf, &dstRect, D3DTEXF_LINEAR))) {
                    damageD3D(dest_r);
                    return;
                }
            }
        }
    }

    FillModeGuard guard(dev);

    unsigned maskRGB = useMask ? (src->format.toARGB(src->mask_surf) & 0x00ffffffu) : ~0u;
    IDirect3DBaseTexture9* tex = src->getTexture();
    IDirect3DTexture9* builtTex = nullptr;
    if (!tex || useMask || isBoundAsRenderTarget(dev, src->surf)) {
        builtTex = getOrBuildBlitTex(dev, src, maskRGB);
        if (!builtTex) return;
        tex = builtTex;
    }

    SavedBlitState saved;
    saveBlitState(dev, saved);

    dev->SetRenderTarget(0, surf);
    dev->SetDepthStencilSurface(nullptr);

    D3DVIEWPORT9 vp = { 0, 0, (DWORD)clip_rect.right, (DWORD)clip_rect.bottom, 0.0f, 1.0f };
    dev->SetViewport(&vp);

    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);

    if (useAlpha) {
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    }
    else if (useMask) {
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
        dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
        dev->SetRenderState(D3DRS_ALPHAREF, 0);
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    }
    else {
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    }

    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MIPMAPLODBIAS, 0);

    disableExtraTextureStages(dev);

    dev->BeginScene();
    drawBlitQuad(dev, (IDirect3DTexture9*)tex, dest_r, src_r, src->clip_rect.right, src->clip_rect.bottom);
    dev->EndScene();

    restoreBlitState(dev, saved);
    damageD3D(dest_r);
}

static void cpuBlitAlpha(gxCanvas* dest, const RECT& dest_r, gxCanvas* src, const RECT& src_r, unsigned color_argb) {
    int dw = dest_r.right - dest_r.left;
    int dh = dest_r.bottom - dest_r.top;

    if (!dest->lock()) return;
    if (!src->lock()) { dest->unlock(); return; }

    unsigned tintR = (color_argb >> 16) & 0xff;
    unsigned tintG = (color_argb >> 8) & 0xff;
    unsigned tintB = color_argb & 0xff;
    unsigned tintA = (color_argb >> 24) & 0xff;

    const PixelFormat& sf = src->format;
    const PixelFormat& df = dest->format;

    for (int y = 0; y < dh; ++y) {
        for (int x = 0; x < dw; ++x) {
            unsigned srcArgb = sf.toARGB(src->getPixelFast(src_r.left + x, src_r.top + y));
            unsigned srcA = ((srcArgb >> 24) & 0xff) * tintA / 255;
            if (srcA == 0) continue;

            int dx = dest_r.left + x, dy = dest_r.top + y;
            unsigned dstArgb = df.toARGB(dest->getPixelFast(dx, dy));
            unsigned dstA = (dstArgb >> 24) & 0xff;
            unsigned outA = srcA + dstA * (255 - srcA) / 255;

            unsigned srcR = (srcArgb >> 16) & 0xff, srcG = (srcArgb >> 8) & 0xff, srcB = srcArgb & 0xff;
            unsigned dstR = (dstArgb >> 16) & 0xff, dstG = (dstArgb >> 8) & 0xff, dstB = dstArgb & 0xff;
            unsigned invA = 255 - srcA;
            unsigned outR = (tintR * srcA + dstR * invA) / 255;
            unsigned outG = (tintG * srcA + dstG * invA) / 255;
            unsigned outB = (tintB * srcA + dstB * invA) / 255;
            dest->setPixelFast(dx, dy, df.fromARGB((outA << 24) | (outR << 16) | (outG << 8) | outB));
        }
    }

    src->unlock();
    dest->unlock();
    dest->damage(dest_r);
}

void gxCanvas::blitAlpha(int x, int y, gxCanvas* src,
    int src_x, int src_y, int src_w, int src_h,
    unsigned color_argb, bool filter) {
    x += origin_x - src->handle_x;
    y += origin_y - src->handle_y;

    Rect dest_r(x, y, src_w, src_h);
    Rect src_r(src_x, src_y, src_w, src_h);

    if (!clip(&dest_r, &src_r)) return;
    if (!::clip(src->clip_rect, &src_r, &dest_r)) return;

    if (tryGpuSprite(this, dest_r, src, src_r, color_argb, filter)) return;

    if (!isRenderTarget(surf)) {
        cpuBlitAlpha(this, dest_r, src, src_r, color_argb);
        return;
    }

    IDirect3DDevice9* dev = graphics->dir3dDev;
    if (!dev) return;

    IDirect3DBaseTexture9* tex = src->getTexture();
    IDirect3DTexture9* builtTex = nullptr;
    if (!tex || isBoundAsRenderTarget(dev, src->surf)) {
        builtTex = getOrBuildBlitTex(dev, src, ~0u);
        if (!builtTex) return;
        tex = builtTex;
    }

    bool ownBatch = !blit_batch_active;
    if (ownBatch) beginBlitBatch();

    if (!blit_batch_active) {
        endBlitBatch();
        return;
    }

    FillModeGuard guard(dev);

    if (effect2D) {
        drawQuadWithEffect(dev, effect2D, (IDirect3DTexture9*)tex, dest_r, src_r, src->clip_rect.right, src->clip_rect.bottom, true, color_argb);
    }
    else {
        dev->SetRenderState(D3DRS_LIGHTING, FALSE);
        dev->SetRenderState(D3DRS_ZENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);

        dev->SetRenderState(D3DRS_TEXTUREFACTOR, color_argb);

        dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);

        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);

        if (filter) {
            dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        }
        else {
            dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        }

        drawBlitQuad(dev, (IDirect3DTexture9*)tex, dest_r, src_r, src->clip_rect.right, src->clip_rect.bottom);
    }

    damageD3D(dest_r);

    if (ownBatch) endBlitBatch();
}

void gxCanvas::text(int x, int y, const std::string& t) {
    int ty = y + origin_y;
    if (ty >= viewport.bottom) return;
    if (ty + font->getHeight() <= viewport.top) return;
    int tx = x + origin_x;
    if (tx >= viewport.right) return;
    int b = 0, w;
    while (b < (int)t.size() && tx + (w = font->charAdvance(UTF8::decodeCharacter(t.c_str(), b))) <= viewport.left) {
        tx += w; x += w;
        b += UTF8::measureCodepoint(t[b]);
    }
    int e = b;
    while (e < (int)t.size() && tx < viewport.right) {
        tx += font->charAdvance(UTF8::decodeCharacter(t.c_str(), e));
        e += UTF8::measureCodepoint(t[e]);
    }
    if (e > b) {
        bool gpuAttempted = graphics && graphics->runtime && graphics->runtime->sdlGpu && font && this == graphics->getBackCanvas();
        bool gpuText = gpuAttempted && font->renderGPU(graphics->runtime->sdlGpu, this, color_argb, x, y, t.substr(b, e - b));
        if (!gpuText) {
            if (gpuAttempted) {
                static bool logged = false;
                if (!logged) {
                    logged = true;
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "SDL GPU text queue failed, falling back to CPU text");
                }
            }
            beginBlitBatch();
            font->render(this, color_argb, x, y, t.substr(b, e - b));
            endBlitBatch();
        }
    }
}

int gxCanvas::getWidth()  const { return logical_w; }
int gxCanvas::getHeight() const { return logical_h; }
int gxCanvas::getDepth()  const { return format.getDepth(); }

void gxCanvas::getOrigin(int* x, int* y)  const { *x = origin_x; *y = origin_y; }
void gxCanvas::getHandle(int* x, int* y)  const { *x = handle_x; *y = handle_y; }

void gxCanvas::getViewport(int* x, int* y, int* w, int* h) const {
    *x = viewport.left; *y = viewport.top;
    *w = viewport.right - viewport.left; *h = viewport.bottom - viewport.top;
}

unsigned gxCanvas::getMask()     const { return format.toARGB(mask_surf); }
unsigned gxCanvas::getColor()    const { return format.toARGB(color_surf); }
unsigned gxCanvas::getClsColor() const { return format.toARGB(clsColor_surf); }

bool gxCanvas::collide(int x1, int y1, const gxCanvas* i2, int x2, int y2, bool solid) const {
    x1 -= handle_x; x2 -= i2->handle_x;
    if (x1 + clip_rect.right <= x2 || x1 >= x2 + i2->clip_rect.right) return false;
    y1 -= handle_y; y2 -= i2->handle_y;
    if (y1 + clip_rect.bottom <= y2 || y1 >= y2 + i2->clip_rect.bottom) return false;
    if (solid) return true;
    if (!cm_mask) { cm_mask = new unsigned[cm_pitch * clip_rect.bottom]; updateBitMask(clip_rect); }
    if (!i2->cm_mask) { i2->cm_mask = new unsigned[i2->cm_pitch * i2->clip_rect.bottom]; i2->updateBitMask(i2->clip_rect); }
    const gxCanvas* i1 = this;
    if (x1 > x2) { std::swap(x1, x2); std::swap(y1, y2); std::swap(i1, i2); }
    Rect r1, r2, ir;
    r1.left = x1; r1.top = y1; r1.right = x1 + i1->clip_rect.right; r1.bottom = y1 + i1->clip_rect.bottom;
    r2.left = x2; r2.top = y2; r2.right = x2 + i2->clip_rect.right; r2.bottom = y2 + i2->clip_rect.bottom;
    ir.left = r1.left > r2.left ? r1.left : r2.left; ir.right = r1.right < r2.right ? r1.right : r2.right;
    ir.top = r1.top > r2.top ? r1.top : r2.top;      ir.bottom = r1.bottom < r2.bottom ? r1.bottom : r2.bottom;
    unsigned* s1 = i1->cm_mask, * s2 = i2->cm_mask;
    int i1p = i1->cm_pitch, i2p = i2->cm_pitch;
    s1 += (ir.top - r1.top) * i1p;
    s2 += (ir.top - r2.top) * i2p;
    int startx = ir.left - r1.left, stopx = ir.right - r1.left - 1;
    int shr = startx & 31, shl = 32 - shr, cnt = stopx / 32 - startx / 32;
    unsigned lwm = LWMS[stopx & 31];
    s1 += startx / 32;
    for (int y = ir.top; y < ir.bottom; ++y) {
        unsigned p = 0, * row1 = s1, * row2 = s2;
        for (int x = 0; x < cnt; ++x) { unsigned n = *row2++; if (((n >> shr) | p) & *row1++) return true; p = shl < 32 ? n << shl : 0; }
        if (((*row2 >> shr) | p) & *row1 & lwm) return true;
        s1 += i1p; s2 += i2p;
    }
    return false;
}

bool gxCanvas::rect_collide(int x1, int y1, int x2, int y2, int w2, int h2, bool solid) const {
    x1 -= handle_x; if (x1 + clip_rect.right <= x2 || x1 >= x2 + w2) return false;
    y1 -= handle_y; if (y1 + clip_rect.bottom <= y2 || y1 >= y2 + h2) return false;
    if (solid) return true;
    Rect r1(x1, y1, clip_rect.right, clip_rect.bottom), r2(x2, y2, w2, h2), ir;
    ir.left = r1.left > r2.left ? r1.left : r2.left; ir.right = r1.right < r2.right ? r1.right : r2.right;
    ir.top = r1.top > r2.top ? r1.top : r2.top;      ir.bottom = r1.bottom < r2.bottom ? r1.bottom : r2.right;
    if (!cm_mask) { cm_mask = new unsigned[cm_pitch * clip_rect.bottom]; updateBitMask(clip_rect); }
    unsigned* s1 = cm_mask + (ir.top - r1.top) * cm_pitch;
    int startx = ir.left - r1.left, stopx = ir.right - r1.left - 1;
    int cnt = stopx / 32 - startx / 32;
    unsigned fwm = FWMS[startx & 31], lwm = LWMS[stopx & 31];
    if (!cnt) { fwm &= lwm; lwm = 0; }
    s1 += startx / 32;
    for (int h = ir.top; h < ir.bottom; ++h) {
        unsigned* row = s1;
        if (*row & fwm) return true;
        for (int x = 1; x < cnt; ++x) if (*++row) return true;
        if (lwm && (*++row & lwm)) return true;
        s1 += cm_pitch;
    }
    return false;
}

bool gxCanvas::lock() const {
    return lockImpl(false);
}

bool gxCanvas::lockRO() const {
    return lockImpl(true);
}

bool gxCanvas::lockImpl(bool ro) const {
    if (locked_cnt == 0) {
        lock_ro = ro;
        lock_is_rt = false;
        if (!cpu_bits) return false;
        if (d3d_dirty && !pullD3D()) return false;
        locked_pitch = cpu_pitch;
        locked_surf = cpu_bits;
        lock_mod_cnt = mod_cnt;
    }
    ++locked_cnt;
    return true;
}

void gxCanvas::unlock() const {
    if (locked_cnt == 0) return;

    if (locked_cnt == 1) {
        if (lock_mod_cnt != mod_cnt && cm_mask) {
            updateBitMask(clip_rect);
        }
    }
    --locked_cnt;
}

void gxCanvas::setPixel(int x, int y, unsigned argb) {
    x += origin_x; if (x < viewport.left || x >= viewport.right)  return;
    y += origin_y; if (y < viewport.top || y >= viewport.bottom) return;
    lock();
    setPixelFast(x, y, argb);
    unlock();
    Rect dmg(x, y, 1, 1);
    damage(dmg);
}

unsigned gxCanvas::getPixel(int x, int y) const {
    x += origin_x; if (x < viewport.left || x >= viewport.right)  return format.toARGB(mask_surf);
    y += origin_y; if (y < viewport.top || y >= viewport.bottom) return format.toARGB(mask_surf);
    lock();
    unsigned p = getPixelFast(x, y);
    unlock();
    return p;
}

void gxCanvas::copyPixelFast(int x, int y, gxCanvas* src, int src_x, int src_y) {
    if (format.getDepth() == src->format.getDepth()) {
        switch (format.getDepth()) {
        case 16: *(short*)(locked_surf + y * locked_pitch + x * 2) = *(short*)(src->locked_surf + src_y * src->locked_pitch + src_x * 2); ++mod_cnt; return;
        case 24: { unsigned char* p = locked_surf + y * locked_pitch + x * 3; unsigned char* t = src->locked_surf + src_y * src->locked_pitch + src_x * 3; *(short*)p = *(short*)t; *(char*)(p + 2) = *(char*)(t + 2); } ++mod_cnt; return;
        case 32: *(int*)(locked_surf + y * locked_pitch + x * 4) = *(int*)(src->locked_surf + src_y * src->locked_pitch + src_x * 4); ++mod_cnt; return;
        }
    }
    int sp = src->format.getPitch();
    int dp = format.getPitch();
    unsigned char* sPix = src->locked_surf + src_y * src->locked_pitch + src_x * sp;
    unsigned char* dPix = locked_surf + y * locked_pitch + x * dp;
    unsigned argb = src->format.toARGB(src->format.getPixel(sPix));
    format.setPixel(dPix, format.fromARGB(argb));
    ++mod_cnt;
}

void gxCanvas::copyPixel(int x, int y, gxCanvas* src, int src_x, int src_y) {
    x += origin_x; if (x < viewport.left || x >= viewport.right) return;
    y += origin_y; if (y < viewport.top || y >= viewport.bottom) return;
    src_x += src->origin_x; if (src_x < src->viewport.left || src_x >= src->viewport.right) return;
    src_y += src->origin_y; if (src_y < src->viewport.top || src_y >= src->viewport.bottom) return;
    lock(); src->lock();
    copyPixelFast(x, y, src, src_x, src_y);
    src->unlock(); unlock();
}

void gxCanvas::blitTForm(int x, int y, gxCanvas* src, int src_x, int src_y, int src_w, int src_h, float mat[2][2], bool filter) {
    if (src_w <= 0 || src_h <= 0) return;
    int hx, hy; src->getHandle(&hx, &hy);
    float ax = mat[0][0] * (-hx) + mat[0][1] * (-hy);
    float ay = mat[1][0] * (-hx) + mat[1][1] * (-hy);
    float baseX = (float)(x + origin_x) + ax;
    float baseY = (float)(y + origin_y) + ay;
    float rx = mat[0][0] * src_w;
    float ry = mat[1][0] * src_w;
    float sx = mat[0][1] * src_h;
    float sy = mat[1][1] * src_h;

    if (!isRenderTarget(surf)) {
        float det = mat[0][0] * mat[1][1] - mat[0][1] * mat[1][0];
        if (fabsf(det) < 1e-6f) return;
        float inv00 = mat[1][1] / det;
        float inv01 = -mat[0][1] / det;
        float inv10 = -mat[1][0] / det;
        float inv11 = mat[0][0] / det;
        float xs[4] = { baseX, baseX + rx, baseX + sx, baseX + rx + sx };
        float ys[4] = { baseY, baseY + ry, baseY + sy, baseY + ry + sy };
        float minx = xs[0], maxx = xs[0], miny = ys[0], maxy = ys[0];
        for (int k = 1; k < 4; ++k) { if (xs[k] < minx) minx = xs[k]; if (xs[k] > maxx) maxx = xs[k]; if (ys[k] < miny) miny = ys[k]; if (ys[k] > maxy) maxy = ys[k]; }
        int ix0 = (int)floorf(minx); int ix1 = (int)ceilf(maxx);
        int iy0 = (int)floorf(miny); int iy1 = (int)ceilf(maxy);
        if (ix0 < viewport.left) ix0 = viewport.left;
        if (iy0 < viewport.top) iy0 = viewport.top;
        if (ix1 > viewport.right) ix1 = viewport.right;
        if (iy1 > viewport.bottom) iy1 = viewport.bottom;
        if (ix0 >= ix1 || iy0 >= iy1) return;
        if (!src->lock()) return;
        if (!lock()) { src->unlock(); return; }
        int srcW = src->clip_rect.right, srcH = src->clip_rect.bottom;
        unsigned maskRGB = src->hasMask() ? (src->format.toARGB(src->mask_surf) & 0x00ffffffu) : ~0u;
        bool doMask = maskRGB != ~0u;
        bool doAlpha = src->format.hasAlphaMask() || (src->getFlags() & CANVAS_TEX_ALPHA);
        for (int dy = iy0; dy < iy1; ++dy) {
            for (int dx = ix0; dx < ix1; ++dx) {
                float fx = (dx + 0.5f) - baseX;
                float fy = (dy + 0.5f) - baseY;
                float ox = inv00 * fx + inv01 * fy;
                float oy = inv10 * fx + inv11 * fy;
                if (ox < 0.0f || ox >= (float)src_w || oy < 0.0f || oy >= (float)src_h) continue;
                float sxf = (float)src_x + ox;
                float syf = (float)src_y + oy;
                unsigned argb;
                if (!filter) {
                    int ix = (int)floorf(sxf); int iy = (int)floorf(syf);
                    if (ix < src_x) ix = src_x; else if (ix >= src_x + src_w) ix = src_x + src_w - 1;
                    if (iy < src_y) iy = src_y; else if (iy >= src_y + src_h) iy = src_y + src_h - 1;
                    if (ix < 0) ix = 0; else if (ix >= srcW) ix = srcW - 1;
                    if (iy < 0) iy = 0; else if (iy >= srcH) iy = srcH - 1;
                    argb = src->format.toARGB(src->getPixelFast(ix, iy));
                    if (doMask && (argb & 0x00ffffffu) == maskRGB) continue;
                    if (!doAlpha) argb |= 0xff000000u;
                } else {
                    int ix = (int)floorf(sxf); int iy = (int)floorf(syf);
                    float fxfrac = sxf - ix; float fyfrac = syf - iy;
                    auto getC = [&](int xx, int yy) -> unsigned {
                        if (xx < src_x) xx = src_x; else if (xx >= src_x + src_w) xx = src_x + src_w - 1;
                        if (yy < src_y) yy = src_y; else if (yy >= src_y + src_h) yy = src_y + src_h - 1;
                        if (xx < 0) xx = 0; else if (xx >= srcW) xx = srcW - 1;
                        if (yy < 0) yy = 0; else if (yy >= srcH) yy = srcH - 1;
                        unsigned c = src->format.toARGB(src->getPixelFast(xx, yy));
                        if (!doAlpha) c |= 0xff000000u;
                        return c;
                    };
                    unsigned c00 = getC(ix, iy); unsigned c10 = getC(ix+1, iy);
                    unsigned c01 = getC(ix, iy+1); unsigned c11 = getC(ix+1, iy+1);
                    if (doMask) {
                        auto masked = [&](unsigned c)->unsigned { return (c & 0x00ffffffu) == maskRGB ? 0 : c; };
                        c00 = masked(c00); c10 = masked(c10); c01 = masked(c01); c11 = masked(c11);
                    }
                    int w1 = (int)((1-fxfrac)*(1-fyfrac)*256); int w2 = (int)(fxfrac*(1-fyfrac)*256);
                    int w3 = (int)((1-fxfrac)*fyfrac*256); int w4 = (int)(fxfrac*fyfrac*256);
                    int a = ((c00>>24)&0xFF)*w1 + ((c10>>24)&0xFF)*w2 + ((c01>>24)&0xFF)*w3 + ((c11>>24)&0xFF)*w4;
                    int r = ((c00>>16)&0xFF)*w1 + ((c10>>16)&0xFF)*w2 + ((c01>>16)&0xFF)*w3 + ((c11>>16)&0xFF)*w4;
                    int g = ((c00>>8)&0xFF)*w1 + ((c10>>8)&0xFF)*w2 + ((c01>>8)&0xFF)*w3 + ((c11>>8)&0xFF)*w4;
                    int b = (c00&0xFF)*w1 + (c10&0xFF)*w2 + (c01&0xFF)*w3 + (c11&0xFF)*w4;
                    a = (a>>8)&0xFF; r=(r>>8)&0xFF; g=(g>>8)&0xFF; b=(b>>8)&0xFF;
                    if (a==0 && doMask) continue;
                    argb = (a<<24)|(r<<16)|(g<<8)|b;
                }
                format.setPixel(locked_surf + dy*locked_pitch + dx*format.getPitch(), format.fromARGB(argb));
            }
        }
        src->unlock(); unlock();
        RECT dmg = {ix0,iy0,ix1,iy1};
        damage(dmg);
        return;
    }
    IDirect3DDevice9* dev = graphics->dir3dDev;
    if (!dev) return;
    bool useAlpha = (src->getFlags() & CANVAS_TEX_ALPHA) != 0 || src->format.hasAlphaMask();
    bool useMask = src->hasMask();
    unsigned maskRGB = useMask ? (src->format.toARGB(src->mask_surf) & 0x00ffffffu) : ~0u;
    IDirect3DBaseTexture9* texBase = src->getTexture();
    IDirect3DTexture9* builtTex = nullptr;
    if (!texBase || useMask || isBoundAsRenderTarget(dev, src->surf)) {
        builtTex = getOrBuildBlitTex(dev, src, maskRGB);
        if (!builtTex) return;
        texBase = builtTex;
    }
    IDirect3DTexture9* tex = (IDirect3DTexture9*)texBase;
    int texW = src->clip_rect.right; int texH = src->clip_rect.bottom;
    if (texW <= 0) texW = src->getWidth(); if (texH <= 0) texH = src->getHeight();
    float u0 = (float)src_x / texW; float v0 = (float)src_y / texH;
    float u1 = (float)(src_x + src_w) / texW; float v1 = (float)(src_y + src_h) / texH;
    float p0x = baseX - 0.5f, p0y = baseY - 0.5f;
    float p1x = baseX + rx - 0.5f, p1y = baseY + ry - 0.5f;
    float p2x = baseX + sx - 0.5f, p2y = baseY + sy - 0.5f;
    float p3x = baseX + rx + sx - 0.5f, p3y = baseY + ry + sy - 0.5f;
    QuadVertex verts[4] = {
        { p0x, p0y, 0.0f, 1.0f, u0, v0 },
        { p1x, p1y, 0.0f, 1.0f, u1, v0 },
        { p2x, p2y, 0.0f, 1.0f, u0, v1 },
        { p3x, p3y, 0.0f, 1.0f, u1, v1 }
    };
    SavedBlitState saved; saveBlitState(dev, saved);
    dev->SetRenderTarget(0, surf);
    dev->SetDepthStencilSurface(nullptr);
    D3DVIEWPORT9 vp = { 0,0,(DWORD)clip_rect.right,(DWORD)clip_rect.bottom,0.0f,1.0f };
    dev->SetViewport(&vp);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    if (useAlpha) {
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    } else if (useMask) {
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
        dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
        dev->SetRenderState(D3DRS_ALPHAREF, 0);
        dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    } else {
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    }
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, filter ? D3DTEXF_LINEAR : D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, filter ? D3DTEXF_LINEAR : D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    disableExtraTextureStages(dev);
    if (effect2D) {
        D3DXMATRIX proj, view, world; D3DXMatrixIdentity(&world); D3DXMatrixIdentity(&view);
        D3DVIEWPORT9 curVP; dev->GetViewport(&curVP);
        float w = (float)curVP.Width, h = (float)curVP.Height;
        D3DXMatrixOrthoOffCenterLH(&proj, 0, w, h, 0, 0, 1);
        effect2D->setAutoMatrices(world, view, proj);
        effect2D->setTexture("tex0", tex);
        effect2D->setTexture("SceneTex", tex);
        UINT passes; if (effect2D->begin(&passes)) {
            for (UINT p=0;p<passes;++p){ effect2D->beginPass(p); dev->SetFVF(QUAD_FVF); dev->SetTexture(0, tex); dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, verts, sizeof(QuadVertex)); effect2D->endPass(); }
            effect2D->end();
        }
    } else {
        FillModeGuard guard(dev);
        dev->SetTexture(0, tex);
        dev->SetFVF(QUAD_FVF);
        dev->BeginScene();
        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, verts, sizeof(QuadVertex));
        dev->EndScene();
    }
    restoreBlitState(dev, saved);
    float xs[4] = { p0x, p1x, p2x, p3x };
    float ys[4] = { p0y, p1y, p2y, p3y };
    float minx = xs[0], maxx = xs[0], miny = ys[0], maxy = ys[0];
    for (int k=1;k<4;++k){ if(xs[k]<minx) minx=xs[k]; if(xs[k]>maxx) maxx=xs[k]; if(ys[k]<miny) miny=ys[k]; if(ys[k]>maxy) maxy=ys[k]; }
    RECT r; r.left = (LONG)floorf(minx); r.top = (LONG)floorf(miny); r.right = (LONG)ceilf(maxx); r.bottom = (LONG)ceilf(maxy);
    if (r.left < viewport.left) r.left = viewport.left; if (r.top < viewport.top) r.top = viewport.top;
    if (r.right > viewport.right) r.right = viewport.right; if (r.bottom > viewport.bottom) r.bottom = viewport.bottom;
    damageD3D(r);
}

void gxCanvas::setCubeMode(int mode) { cube_mode = mode; }

void gxCanvas::setCubeFace(int face) {
    if (face >= 0 && face < 6 && cube_surfs[face]) {
        surf = cube_surfs[face];
        d3d_dirty = true;
        ++mod_cnt;
    }
}