#ifndef DDUTIL_H
#define DDUTIL_H

#include <d3d9.h>

class gxGraphics;
struct DecodedImage;

struct ddUtil {

	static void buildMipMaps(IDirect3DTexture9* tex);
    static void copy(IDirect3DDevice9* dev, IDirect3DSurface9* dest, int dx, int dy, int dw, int dh, IDirect3DSurface9* src, int sx, int sy, int sw, int sh);

    // Display canvases
    static IDirect3DSurface9* createDisplaySurface(int w, int h, int flags, gxGraphics* gfx);
    static IDirect3DSurface9* loadDisplaySurface(const std::string& file, int flags, gxGraphics* gfx);

    // Texture canvases
    static IDirect3DTexture9* createTextureSurface(int w, int h, int flags, gxGraphics* gfx);
    static IDirect3DTexture9* createTextureSurface(int w, int h, int flags, gxGraphics* gfx, bool renderTarget);

    static IDirect3DCubeTexture9* createCubeTextureSurface(int size, int flags, gxGraphics* gfx);

    static IDirect3DTexture9* loadTextureSurface(const std::string& file, int flags, gxGraphics* gfx);
    static IDirect3DTexture9* loadTextureSurface(const std::string& file, int flags, gxGraphics* gfx, bool renderTarget);
    static IDirect3DTexture9* loadTextureSurface(const std::string& file, int flags, gxGraphics* gfx, bool renderTarget, int* outW, int* outH);

    static IDirect3DTexture9* textureFromDecoded(const DecodedImage* img, int flags, gxGraphics* gfx, bool renderTarget, int* outW, int* outH);

    static bool hasActualAlpha(const std::string& file);
    static const std::string& getLastImageError();
};

class PixelFormat {
    int depth, pitch;
    unsigned amask, rmask, gmask, bmask, argbfill;
    unsigned char ashr, ashl, rshr, rshl, gshr, gshl, bshr, bshl;
    typedef void(_fastcall* Plot)(void* pix, unsigned argb);
    typedef unsigned(_fastcall* Point)(void* pix);
    Plot plot;
    Point point;

    char* plot_code, * point_code;

    void calcShifts(unsigned mask, unsigned char* shr, unsigned char* shl) {
        if (mask) {
            for (*shl = 0; !(mask & 1); ++*shl, mask >>= 1) {}
            for (*shr = 8; mask & 1; --*shr, mask >>= 1) {}
        }
        else *shr = *shl = 0;
    }

public:
    PixelFormat() :plot_code(0) {}

    PixelFormat(D3DFORMAT fmt) :plot_code(0) {
        setFormat(fmt);
    }

    ~PixelFormat();

    void setFormat(D3DFORMAT fmt);

    int getDepth() const { return depth; }
    int getPitch() const { return pitch; }

    bool is8888() const {
        return pitch == 4 && rmask == 0x00ff0000 && gmask == 0x0000ff00 && bmask == 0x000000ff && (amask == 0 || amask == 0xff000000);
    }
    bool hasAlphaMask() const { return amask != 0; }

    unsigned fromARGB(unsigned n) const {
        return ((n >> ashr << ashl) & amask) |
            ((n >> rshr << rshl) & rmask) |
            ((n >> gshr << gshl) & gmask) |
            ((n >> bshr << bshl) & bmask);
    }
    unsigned toARGB(unsigned n) const {
        return ((n & amask) >> ashl << ashr) |
            ((n & rmask) >> rshl << rshr) |
            ((n & gmask) >> gshl << gshr) |
            ((n & bmask) >> bshl << bshr) | argbfill;
    }
    void setPixel(void* p, unsigned n) const { plot(p, n); }
    unsigned getPixel(void* p) const { return point(p); }
};

inline void ConvertPixelsToRGBA(const PixelFormat& fmt, const unsigned char* src, unsigned char* dst, unsigned w) {
    if (fmt.is8888()) {
        unsigned fill = fmt.hasAlphaMask() ? 0u : 0xff000000u;
        for (unsigned x = 0; x < w; ++x) {
            unsigned argb = *(const unsigned*)src | fill;
            dst[0] = (unsigned char)(argb >> 16);
            dst[1] = (unsigned char)(argb >> 8);
            dst[2] = (unsigned char)argb;
            dst[3] = (unsigned char)(argb >> 24);
            src += 4;
            dst += 4;
        }
        return;
    }
    int pitch = fmt.getPitch();
    for (unsigned x = 0; x < w; ++x) {
        unsigned argb = fmt.getPixel((void*)src);
        dst[0] = (unsigned char)(argb >> 16);
        dst[1] = (unsigned char)(argb >> 8);
        dst[2] = (unsigned char)argb;
        dst[3] = (unsigned char)(argb >> 24);
        src += pitch;
        dst += 4;
    }
}

#endif