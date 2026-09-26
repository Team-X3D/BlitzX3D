#include "std.h"
#include "gxcanvas.h"
#include "gxgraphics.h"
#include "gxruntime.h"
#include "asmcoder.h"
#include "gxutf8.h"
#include "sdlgpu/sdl_gpu_texture.h"
#include "sdlgpu/sdl_gpu_text.h"
#include <SDL3/SDL_log.h>
#include <emmintrin.h>

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

static inline void fillRectRows(unsigned char* base, int basePitch, int w, int h, unsigned nat, int pitch) {
    if (pitch == 4) {
        __m128i v = _mm_set1_epi32((int)nat);
        int m = w & ~3, tail = w & 3;
        for (int y = 0; y < h; ++y) {
            unsigned* p = (unsigned*)(base + y * basePitch);
            for (int x = 0; x < m; x += 4) _mm_storeu_si128((__m128i*)(p + x), v);
            for (int x = 0; x < tail; ++x) p[m + x] = nat;
        }
        return;
    }
    for (int y = 0; y < h; ++y) {
        unsigned char* row = base + y * basePitch;
        if (pitch == 2) {
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

void gxCanvas::allocCPUStore(int w, int h) const {
    sizeCPUStore(w, h);
    if (cpu_pitch <= 0 || cpu_h <= 0) return;
    size_t planes = (flags & CANVAS_TEX_CUBE) ? 6 : 1;
    cpu_bits = new unsigned char[(size_t)cpu_pitch * (size_t)cpu_h * planes]();
}

void gxCanvas::sizeCPUStore(int w, int h) const {
    delete[] cpu_bits; cpu_bits = nullptr;
    cpu_w = w; cpu_h = h; cpu_pitch = 0;
    if (w <= 0 || h <= 0) return;
    int bpp = format.getPitch();
    if (bpp <= 0) return;
    cpu_pitch = w * bpp;
}

bool gxCanvas::syncFromGpu() const {
    if (!gpuNewer) return true;
    gpuNewer = false;
    if (graphics && graphics->runtime && graphics->runtime->sdlGpu)
        sdlgpu::DownloadCanvasTexture((SDL_GPUDevice*)graphics->runtime->sdlGpu, const_cast<gxCanvas*>(this));
    return true;
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

bool gxCanvas::pushRectD3D(const RECT& r) const {
    if (graphics && graphics->runtime && graphics->runtime->sdlGpu) return true;
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

gxCanvas::gxCanvas(gxGraphics* g, IDirect3DSurface9* s, int f) :
    graphics(g), plain_surf(s), tex(nullptr), cube_tex(nullptr), surf(s), z_surf(nullptr),
    flags(f), cube_mode(CUBEMODE_REFLECTION | CUBESPACE_WORLD), cube_face(2),
    cm_mask(nullptr), locked_cnt(0), mod_cnt(0), remip_cnt(0),
    lock_ro(false), effect2D(nullptr), has_mask(false), sdlDirtyValid(false), cpuTouched(false) {
    memset(cube_surfs, 0, sizeof(cube_surfs));

    D3DSURFACE_DESC desc;
    surf->GetDesc(&desc);
    format.setFormat(desc.Format);
    sizeCPUStore(desc.Width, desc.Height);
    gpuNewer = true;

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
    flags(f), cube_mode(CUBEMODE_REFLECTION | CUBESPACE_WORLD), cube_face(2),
    cm_mask(nullptr), locked_cnt(0), mod_cnt(0), remip_cnt(0),
    lock_ro(false), effect2D(nullptr), has_mask(false), sdlDirtyValid(false), cpuTouched(false) {
    memset(cube_surfs, 0, sizeof(cube_surfs));

    tex->GetSurfaceLevel(0, &surf);

    D3DSURFACE_DESC desc;
    surf->GetDesc(&desc);
    format.setFormat(desc.Format);
    sizeCPUStore(desc.Width, desc.Height);
    gpuNewer = true;

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
    flags(f), cube_mode(CUBEMODE_REFLECTION | CUBESPACE_WORLD), cube_face(2),
    cm_mask(nullptr), locked_cnt(0), mod_cnt(0), remip_cnt(0),
    lock_ro(false), effect2D(nullptr), has_mask(false), sdlDirtyValid(false), cpuTouched(false) {

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
    sizeCPUStore(desc.Width, desc.Height);
    gpuNewer = true;

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

gxCanvas::gxCanvas(gxGraphics* g, int w, int h, int f) :
    graphics(g), plain_surf(nullptr), tex(nullptr), cube_tex(nullptr), surf(nullptr), z_surf(nullptr),
    flags(f), cube_mode(CUBEMODE_REFLECTION | CUBESPACE_WORLD), cube_face(2),
    cm_mask(nullptr), locked_cnt(0), mod_cnt(0), remip_cnt(0),
    lock_ro(false), effect2D(nullptr), has_mask(false), sdlDirtyValid(false), cpuTouched(false) {
    memset(cube_surfs, 0, sizeof(cube_surfs));

    format.setFormat((f & (CANVAS_TEX_ALPHA | CANVAS_TEX_MASK)) ? D3DFMT_A8R8G8B8 : D3DFMT_X8R8G8B8);
    sizeCPUStore(w, h);
    gpuNewer = false;

    clip_rect.left = clip_rect.top = 0;
    clip_rect.right = w;
    clip_rect.bottom = h;
    logical_w = w;
    logical_h = h;
    mipmapNeeded = false;
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
    if (locked_cnt && surf) surf->UnlockRect();
    if (t_surf) t_surf->Release();
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
}

void gxCanvas::restore() {
}

void gxCanvas::restoreZBuffer() {
	if (z_surf) {
		releaseZBuffer();
		attachZBuffer();
	}
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
    if (!surf) return false;
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
        if (gpuNewer) syncFromGpu();
    }
    else {
        gpuNewer = true;
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
    return true;
}

void gxCanvas::damage(const RECT& r) const {
    damageImpl(r, true);
}

void gxCanvas::damageD3D(const RECT& r) const {
    damageImpl(r, false);
}

void gxCanvas::damageScene(const RECT& r) const {
    gpuNewer = false;
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
    ::gxCanvas* target = nullptr;
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
    out.target = nullptr;
    out.cw = cw;
    out.ch = ch;
    self->getViewport(&out.vx, &out.vy, &out.vw, &out.vh);
    return true;
}

static bool gpuDrawTarget(gxCanvas* self, GpuBack& out) {
    if (gpuBackbuffer(self, out)) return true;
    if (!self || !(self->getFlags() & gxCanvas::CANVAS_TEXTURE)) return false;
    if (!sdlgpu::IsActiveCanvasTarget(self)) return false;
    gxGraphics* gfx = self->graphics;
    if (!gfx || !gfx->runtime || !gfx->runtime->sdlGpu) return false;
    struct SDL_GPUDevice* dev = (struct SDL_GPUDevice*)gfx->runtime->sdlGpu;
    if (!sdlgpu::EnsureCanvasRenderTarget(dev, self)) return false;
    int cw = self->getWidth(), ch = self->getHeight();
    if (cw <= 0 || ch <= 0) return false;
    out.dev = dev;
    out.target = self;
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
    return sdlgpu::QueueRectFilled(b.dev, b.target, (unsigned)b.cw, (unsigned)b.ch,
        (float)x0, (float)y0, (float)(x1 - x0), (float)(y1 - y0), argb);
}

static bool tryGpuRect(gxCanvas* self, int x, int y, int w, int h, unsigned argb, bool solid) {
    if (!self || w <= 0 || h <= 0) return true;
    GpuBack b;
    if (!gpuDrawTarget(self, b)) return false;
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
    GpuBack b;
    if (gpuBackbuffer(this, b)) {
        if (((argb >> 24) & 0xff) == 255)
            sdlgpu::QueueBackbufferClear(b.dev, argb);
    }
    else if (graphics && graphics->runtime && graphics->runtime->sdlGpu && (flags & CANVAS_TEXTURE) && sdlgpu::IsActiveCanvasTarget(this)) {
        int w = getWidth(), h = getHeight();
        if (w > 0 && h > 0)
            sdlgpu::QueueRectFilled((SDL_GPUDevice*)graphics->runtime->sdlGpu, this, (unsigned)w, (unsigned)h, 0.0f, 0.0f, (float)w, (float)h, argb);
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

static bool tryGpuSprite(gxCanvas* self, const RECT& dest_r, gxCanvas* src, const RECT& src_r, unsigned tint, bool smooth, unsigned maskRGB = ~0u) {
    if (!self || !src || src == self) return false;
    if (dest_r.right <= dest_r.left || dest_r.bottom <= dest_r.top) return true;
    if (src_r.right <= src_r.left || src_r.bottom <= src_r.top) return true;
    gxGraphics* gfx = self->graphics;
    if (!gfx || !gfx->runtime || !gfx->runtime->sdlGpu) return false;
    if (self->get2DEffect()) return false;
    GpuBack b;
    if (!gpuDrawTarget(self, b)) return false;
    struct SDL_GPUDevice* dev = b.dev;
    bool masked = (maskRGB != ~0u);
    struct SDL_GPUTexture* tex = masked
        ? (struct SDL_GPUTexture*)sdlgpu::GetCanvasMaskedTexture(dev, src, maskRGB)
        : (struct SDL_GPUTexture*)sdlgpu::GetCanvasTexture(dev, src);
    if (!tex) return false;
    if (masked) smooth = false;
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
    return sdlgpu::QueueSpriteQuad(dev, b.target, tex, smooth, (unsigned)cw, (unsigned)ch, (unsigned)tw, (unsigned)th, &q);
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
}

static bool tryGpuOval(gxCanvas* self, int x1, int y1, int w, int h, unsigned argb, bool solid) {
    if (!self || w <= 0 || h <= 0) return true;
    GpuBack b;
    if (!gpuDrawTarget(self, b)) return false;
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

void gxCanvas::beginBlitBatch() const {
}

void gxCanvas::endBlitBatch() const {
}

void gxCanvas::cpuBlit(gxCanvas* dest, const RECT& dest_r, gxCanvas* src, const RECT& src_r, bool solid) {
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
    if (!solid && src->hasMask()) {
        unsigned maskRGB = src->format.toARGB(src->mask_surf) & 0x00ffffffu;
        if (tryGpuSprite(this, dest_r, src, src_r, 0xffffffff, false, maskRGB)) return;
    }

    cpuBlit(this, dest_r, src, src_r, solid);
    damage(dest_r);
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

    if (tryGpuSprite(this, dest_r, src, src_r, 0xffffffff, true,
        src->hasMask() ? (src->format.toARGB(src->mask_surf) & 0x00ffffffu) : ~0u)) return;

    cpuBlit(this, dest_r, src, src_r, solid);
    damage(dest_r);
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

    if (tryGpuSprite(this, dest_r, src, src_r, color_argb, filter,
        src->hasMask() ? (src->format.toARGB(src->mask_surf) & 0x00ffffffu) : ~0u)) return;

    cpuBlitAlpha(this, dest_r, src, src_r, color_argb);
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
        bool gpuAttempted = graphics && graphics->runtime && graphics->runtime->sdlGpu && font &&
            (this == graphics->getBackCanvas() || ((flags & CANVAS_TEXTURE) && sdlgpu::IsActiveCanvasTarget(this)));
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
    ir.top = r1.top > r2.top ? r1.top : r2.top;      ir.bottom = r1.bottom < r2.bottom ? r1.bottom : r2.bottom;
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

bool gxCanvas::ensureCPUBits() const {
    if (cpu_bits) return true;
    allocCPUStore(cpu_w, cpu_h);
    if (!cpu_bits) return false;
    if (graphics && graphics->runtime && graphics->runtime->sdlGpu) {
        if (sdlgpu::DownloadCanvasTexture((SDL_GPUDevice*)graphics->runtime->sdlGpu, const_cast<gxCanvas*>(this)))
            gpuNewer = false;
    }
    return true;
}

void gxCanvas::releaseCPUBitsIfUnused() const {
    if (!cpu_bits || locked_cnt != 0 || cpu_keep || cpuTouched) return;
    if (!(flags & CANVAS_TEXTURE)) return;
    delete[] cpu_bits;
    cpu_bits = nullptr;
    gpuNewer = true;
}

bool gxCanvas::lockImpl(bool ro) const {
    (void)ro;
    if (locked_cnt == 0) {
        if (!ensureCPUBits()) return false;
        if (gpuNewer && !syncFromGpu()) return false;
        locked_pitch = cpu_pitch;
        locked_surf = cpu_bits;
        if ((flags & CANVAS_TEX_CUBE) && graphics && graphics->runtime && graphics->runtime->sdlGpu)
            locked_surf += (size_t)cube_face * (size_t)cpu_pitch * (size_t)cpu_h;
        lock_mod_cnt = mod_cnt;
        cpu_keep = true;
        cpuTouched = true;
    }
    ++locked_cnt;
    return true;
}

void gxCanvas::unlock() const {
    if (locked_cnt == 0) return;
    if (locked_cnt == 1 && lock_mod_cnt != mod_cnt && cm_mask)
        updateBitMask(clip_rect);
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
    lockRO();
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

    {
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
}

void gxCanvas::setCubeMode(int mode) { cube_mode = mode; }

void gxCanvas::setCubeFace(int face) {
    if (face < 0 || face >= 6) return;
    cube_face = face;
    if (cube_surfs[face]) {
        surf = cube_surfs[face];
        gpuNewer = true;
        ++mod_cnt;
    }
}
