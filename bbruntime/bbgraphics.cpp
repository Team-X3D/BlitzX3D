#include "std.h"
#include "bbgraphics.h"
#include "bbinput.h"
#include "../sdlruntime/sdlutf8.h"
#include "../MultiLang/MultiLang.h"
#include <algorithm>
#include <cmath>
#include <vector>
#include "../blitz3d/texture.h"
#include "../blitz3d/cachedtexture.h"
#include "../sdlruntime/sdlruntime.h"
#include "../sdlruntime/asyncimage.h"

sdlGraphics* sdl_graphics;
sdlCanvas* sdl_canvas;
sdlCanvas* sdl_depth_canvas;

struct ScrollRectState
{
    RECT viewport;
    int origin_x, origin_y;
};

static std::vector<ScrollRectState> scroll_rect_stack;

struct GfxMode
{
    int w, h, d, caps;
};

class bbImage
{
public:
    int origWidth, origHeight;
    std::string name;
    float drawScaleX = 1.0f;
    float drawScaleY = 1.0f;
    float tform[2][2] = { {1.0f, 0.0f},{0.0f, 1.0f} };
    bbImage(const std::vector<sdlCanvas*>& f, int origW = -1, int origH = -1) : frames(f) {
        if (origW == -1) {
            origWidth = frames[0]->getWidth();
            origHeight = frames[0]->getHeight();
        }
        else {
            origWidth = origW;
            origHeight = origH;
        }
    }
    ~bbImage()
    {
        for (int k = 0; k < frames.size(); ++k)
            sdl_graphics->freeCanvas(frames[k]);
    }
    const std::vector<sdlCanvas*>& getFrames()const
    {
        return frames;
    }

    bool isTFormIdentity() const {
        const float eps = 1e-6f;
        return fabsf(tform[0][0] - 1.0f) < eps && fabsf(tform[1][1] - 1.0f) < eps && fabsf(tform[0][1]) < eps && fabsf(tform[1][0]) < eps;
    }

    bool isIdentity() const {
        return isTFormIdentity() && drawScaleX == 1.0f && drawScaleY == 1.0f;
    }

    void getCombinedMat(float out[2][2]) const {
        out[0][0] = drawScaleX * tform[0][0];
        out[0][1] = drawScaleX * tform[0][1];
        out[1][0] = drawScaleY * tform[1][0];
        out[1][1] = drawScaleY * tform[1][1];
    }

    void resetTForm() {
        tform[0][0] = 1.0f; tform[0][1] = 0.0f;
        tform[1][0] = 0.0f; tform[1][1] = 1.0f;
    }

    void mulTForm(float a, float b, float c, float d) {
        float curA = tform[0][0], curC = tform[0][1];
        float curB = tform[1][0], curD = tform[1][1];
        float na = a * curA + c * curB;
        float nb = b * curA + d * curB;
        float nc = a * curC + c * curD;
        float nd = b * curC + d * curD;
        tform[0][0] = na; tform[0][1] = nc;
        tform[1][0] = nb; tform[1][1] = nd;
    }

    void replaceFrame(int n, sdlCanvas* c)
    {
        sdl_graphics->freeCanvas(frames[n]);
        frames[n] = c;
        drawScaleX = 1.0f;
        drawScaleY = 1.0f;
        resetTForm();
    }
    void savePixels()
    {
        pixelData.resize(frames.size());
        widths.resize(frames.size());
        heights.resize(frames.size());
        for (int k = 0; k < (int)frames.size(); ++k)
        {
            sdlCanvas* c = frames[k];
            int w = c->getWidth(), h = c->getHeight();
            widths[k] = w; heights[k] = h;
            pixelData[k].resize(w * h);
            c->lock();
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    pixelData[k][y * w + x] = c->getPixelFast(x, y);
            c->unlock();
        }
    }
    void saveOrigPixels()
    {
        origPixelData.resize(frames.size());
        for (int k = 0; k < (int)frames.size(); ++k)
        {
            sdlCanvas* c = frames[k];
            int w = c->getWidth(), h = c->getHeight();
            origPixelData[k].resize(w * h);
            c->lock();
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    origPixelData[k][y * w + x] = c->getPixelFast(x, y);
            c->unlock();
        }
    }
    const std::vector<uint32_t>& getOrigPixels(int frame)
    {
        static const std::vector<uint32_t> empty;
        if (frames.empty()) return empty;
        if ((int)origPixelData.size() != (int)frames.size()) origPixelData.resize(frames.size());
        if (frame < 0 || frame >= (int)frames.size()) frame = 0;
        std::vector<uint32_t>& px = origPixelData[frame];
        if (px.empty()) {
            sdlCanvas* c = frames[frame];
            int w = c->getWidth(), h = c->getHeight();
            px.resize((size_t)w * h);
            c->lock();
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    px[(size_t)y * w + x] = c->getPixelFast(x, y);
            c->unlock();
        }
        return px;
    }
    void restoreToDevice()
    {
        for (int k = 0; k < (int)frames.size(); ++k)
        {
            int w = widths[k], h = heights[k];
            sdlCanvas* c = sdl_graphics->createCanvas(w, h, sdlCanvas::CANVAS_TEXTURE);
            if (!c) continue;
            c->lock();
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    c->setPixelFast(x, y, pixelData[k][y * w + x]);
            c->unlock();
            // preserve handle and mask from old canvas
            int hx, hy;
            frames[k]->getHandle(&hx, &hy);
            c->setHandle(hx, hy);
            c->copyMaskFrom(frames[k]);
            frames[k] = c;
            sdl_graphics->adoptCanvas(c);
        }
    }
    void releaseSavedPixels()
    {
        pixelData.clear();
        pixelData.shrink_to_fit();
        widths.clear();
        heights.clear();
    }
private:
    std::vector<sdlCanvas*> frames;
    std::vector<std::vector<uint32_t>> pixelData;
    std::vector<std::vector<uint32_t>> origPixelData;
    std::vector<int> widths, heights;
};

static int sdl_driver;	//Current graphics driver index.

static bool filter;
static int tform_method = 2; // 0 = nearest, 1 = bilinear, 2 = bicubic
static bool auto_dirty;
static bool auto_midhandle;
static std::unordered_set<bbImage*> image_set;
static int curs_x, curs_y;
static sdlCanvas* p_canvas;

static int fps_cap_ms = 0;
static int last_flip_ms = 0;

static int fps_frame_count = 0;
static int fps_window_start = 0;
static int fps_value = 0;

static sdlFont* curr_font;
static unsigned curr_color;
static unsigned curr_clsColor;

static std::vector<GfxMode> gfx_modes;

extern std::unordered_set<Texture*> texture_set;

static inline void debugImage(bbImage* i, const char* function, int frame = 0)
{
    if (!image_set.count(i)) RTEX(MultiLang::image_not_exist);
    if (frame < 0 || frame >= (int)i->getFrames().size()) RTEX(MultiLang::image_frame_out_of_range);
}

static inline void debugFont(sdlFont* f, const char* function)
{
    if (!sdl_graphics->verifyFont(f)) ErrorLog(function, MultiLang::font_not_exist);
}

static inline void debugCanvas(sdlCanvas* c, const char* function)
{
    if (!sdl_graphics->verifyCanvas(c)) ErrorLog(function, MultiLang::buffer_not_exist);
}

static inline void debugDriver(int n, const char* function)
{
    if (n < 1 || n > sdl_runtime->numGraphicsDrivers()) ErrorLog(function, MultiLang::illegal_graphics_driver_index);
}

static inline void debugMode(int n, const char* function)
{
    if (n<1 || n>gfx_modes.size()) ErrorLog(function, MultiLang::illegal_graphics_mode_index);
}

void bbFreeImage(bbImage* i);

static void freeGraphics(bool freeImages = true)
{
    extern void blitz3d_close();
    if (sdl_graphics) blitz3d_close();
    if (freeImages)
    {
        while (image_set.size()) bbFreeImage(*image_set.begin());
    }
    if (p_canvas)
    {
        sdl_graphics->freeCanvas(p_canvas);
        p_canvas = 0;
    }
}

#define RED(_X_) ( ((_X_)>>16) & 0xff )
#define GRN(_X_) ( ((_X_)>>8) & 0xff )
#define BLU(_X_) ( (_X_) & 0xff )

static int getPixel(sdlCanvas* c, float x, float y)
{
    debugCanvas(c, "getPixel");

    x -= .5f; y -= .5f;
    float fx = floor(x), fy = floor(y);
    int ix = fx, iy = fy; fx = x - fx; fy = y - fy;

    int tl = c->getPixel(ix, iy);
    int tr = c->getPixel(ix + 1, iy);
    int br = c->getPixel(ix + 1, iy + 1);
    int bl = c->getPixel(ix, iy + 1);

    float w1 = (1 - fx) * (1 - fy), w2 = fx * (1 - fy), w3 = (1 - fx) * fy, w4 = fx * fy;

    float r = RED(tl) * w1 + RED(tr) * w2 + RED(bl) * w3 + RED(br) * w4;
    float g = GRN(tl) * w1 + GRN(tr) * w2 + GRN(bl) * w3 + GRN(br) * w4;
    float b = BLU(tl) * w1 + BLU(tr) * w2 + BLU(bl) * w3 + BLU(br) * w4;

    return (int(r + .5f) << 16) | (int(g + .5f) << 8) | int(b + .5f);
}

struct vec2 { float x, y; };

static vec2 vrot(float m[2][2], const vec2& v)
{
    vec2 t; t.x = m[0][0] * v.x + m[0][1] * v.y; t.y = m[1][0] * v.x + m[1][1] * v.y;
    return t;
}

static float vmin(float a, float b, float c, float d)
{
    float t = a; if (b < t) t = b; if (c < t) t = c; if (d < t) t = d; return t;
}

static float vmax(float a, float b, float c, float d)
{
    float t = a; if (b > t) t = b; if (c > t) t = c; if (d > t) t = d; return t;
}

static float cubic_weight(float t, float a = -0.5f) {
    t = fabsf(t);
    if (t < 1.0f) {
        return ((a + 2.0f) * t - (a + 3.0f)) * t * t + 1.0f;
    }
	else if (t < 2.0f) {
		return ((a * t - 5.0f * a) * t + 8.0f * a) * t - 4.0f * a;
	}
	return 0.0f;
}

static const int CUBIC_LUT_SIZE = 1024;
static const int CUBIC_LUT_MASK = CUBIC_LUT_SIZE - 1;
struct CubicLUT {
	float w[CUBIC_LUT_SIZE][4];
	CubicLUT() {
		for (int i = 0; i < CUBIC_LUT_SIZE; ++i) {
			float f = (i + 0.5f) / CUBIC_LUT_SIZE;
			float w0 = cubic_weight(f + 1.0f);
			float w1 = cubic_weight(f);
			float w2 = cubic_weight(f - 1.0f);
			float w3 = cubic_weight(f - 2.0f);
			float s = w0 + w1 + w2 + w3;
			if (s != 0.0f) { w0 /= s; w1 /= s; w2 /= s; w3 /= s; }
			w[i][0] = w0; w[i][1] = w1; w[i][2] = w2; w[i][3] = w3;
		}
	}
	const float* taps(float frac) const {
		return w[(int)(frac * CUBIC_LUT_SIZE) & CUBIC_LUT_MASK];
	}
};
static const CubicLUT cubic_lut;

static unsigned box4(unsigned c00, unsigned c10, unsigned c01, unsigned c11) {
    int a = ((c00 >> 24) & 0xFF) + ((c10 >> 24) & 0xFF) + ((c01 >> 24) & 0xFF) + ((c11 >> 24) & 0xFF);
    int r = ((c00 >> 16) & 0xFF) + ((c10 >> 16) & 0xFF) + ((c01 >> 16) & 0xFF) + ((c11 >> 16) & 0xFF);
    int g = ((c00 >> 8) & 0xFF) + ((c10 >> 8) & 0xFF) + ((c01 >> 8) & 0xFF) + ((c11 >> 8) & 0xFF);
    int b = (c00 & 0xFF) + (c10 & 0xFF) + (c01 & 0xFF) + (c11 & 0xFF);
    return ((((a + 2) >> 2) & 0xFF) << 24) | ((((r + 2) >> 2) & 0xFF) << 16) | ((((g + 2) >> 2) & 0xFF) << 8) | (((b + 2) >> 2) & 0xFF);
}

static std::vector<uint32_t> progressiveMinifyPixels(const std::vector<uint32_t>& src, int srcW, int srcH, int targetW, int targetH, int* outW, int* outH) {
    int w = srcW, h = srcH;
    std::vector<uint32_t> cur = src;
    while ((w > targetW * 2 + 1 || h > targetH * 2 + 1) && w >= 2 && h >= 2) {
        int nw = w / 2, nh = h / 2;
        if (nw < 1) nw = 1;
        if (nh < 1) nh = 1;
        if (nw == w && nh == h) break;
        std::vector<uint32_t> nxt((size_t)nw * nh);
        for (int y = 0; y < nh; ++y) {
            int sy0 = y * 2, sy1 = y * 2 + 1;
            if (sy1 >= h) sy1 = h - 1;
            for (int x = 0; x < nw; ++x) {
                int sx0 = x * 2, sx1 = x * 2 + 1;
                if (sx1 >= w) sx1 = w - 1;
                nxt[(size_t)y * nw + x] = box4(
                    cur[(size_t)sy0 * w + sx0], cur[(size_t)sy0 * w + sx1],
                    cur[(size_t)sy1 * w + sx0], cur[(size_t)sy1 * w + sx1]);
            }
        }
        cur = std::move(nxt);
        w = nw; h = nh;
    }
    *outW = w;
    *outH = h;
    return cur;
}

static sdlCanvas* progressiveMinifyCanvas(sdlCanvas* c, int targetW, int targetH, float* outDiv) {
    int w = c->getWidth(), h = c->getHeight();
    if (targetW <= 0 || targetH <= 0 || (w <= targetW * 2 && h <= targetH * 2)) {
        *outDiv = 1.0f;
        return c;
    }
    sdlCanvas* cur = c;
    float div = 1.0f;
    while ((w > targetW * 2 + 1 || h > targetH * 2 + 1) && w >= 2 && h >= 2) {
        int nw = w / 2, nh = h / 2;
        if (nw < 1) nw = 1;
        if (nh < 1) nh = 1;
        if (nw == w && nh == h) break;
        sdlCanvas* nxt = sdl_graphics->createCanvas(nw, nh, cur->getFlags());
        cur->lock();
        nxt->lock();
        for (int y = 0; y < nh; ++y) {
            int sy0 = y * 2, sy1 = y * 2 + 1;
            if (sy1 >= h) sy1 = h - 1;
            for (int x = 0; x < nw; ++x) {
                int sx0 = x * 2, sx1 = x * 2 + 1;
                if (sx1 >= w) sx1 = w - 1;
                nxt->setPixelFast(x, y, box4(
                    cur->getPixelFast(sx0, sy0), cur->getPixelFast(sx1, sy0),
                    cur->getPixelFast(sx0, sy1), cur->getPixelFast(sx1, sy1)));
            }
        }
        cur->unlock();
        nxt->unlock();
        if (cur != c) sdl_graphics->freeCanvas(cur);
        cur = nxt;
        w = nw; h = nh;
        div *= 2.0f;
    }
    *outDiv = div;
    return cur;
}

static sdlCanvas* tformCanvas(sdlCanvas* c, float m[2][2], int x_handle, int y_handle)
{
    c->backup();

    vec2 v, v0, v1, v2, v3;
    float i[2][2];
    float dt = 1.0f / (m[0][0] * m[1][1] - m[1][0] * m[0][1]);
    i[0][0] = dt * m[1][1]; i[1][0] = -dt * m[1][0];
    i[0][1] = -dt * m[0][1]; i[1][1] = dt * m[0][0];

    float ox = x_handle, oy = y_handle;
    v0.x = -ox; v0.y = -oy;	//tl
    v1.x = c->getWidth() - ox; v1.y = -oy;	//tr
    v2.x = c->getWidth() - ox; v2.y = c->getHeight() - oy;	//br
    v3.x = -ox; v3.y = c->getHeight() - oy;	//bl
    v0 = vrot(m, v0); v1 = vrot(m, v1); v2 = vrot(m, v2); v3 = vrot(m, v3);
    float minx = floor(vmin(v0.x, v1.x, v2.x, v3.x));
    float miny = floor(vmin(v0.y, v1.y, v2.y, v3.y));
    float maxx = ceil(vmax(v0.x, v1.x, v2.x, v3.x));
    float maxy = ceil(vmax(v0.y, v1.y, v2.y, v3.y));
    int iw = maxx - minx, ih = maxy - miny;

    sdlCanvas* t = sdl_graphics->createCanvas(iw, ih, c->getFlags());
    t->setHandle(-minx, -miny);
    t->copyMaskFrom(c);

    if (fabs(m[0][0] - 1.0f) < 0.001f && fabs(m[1][1] - 1.0f) < 0.001f &&
        fabs(m[0][1]) < 0.001f && fabs(m[1][0]) < 0.001f &&
        fabs(minx - (int)minx) < 0.001f && fabs(miny - (int)miny) < 0.001f) {
        t->blit(0, 0, c, -(int)minx, -(int)miny, c->getWidth(), c->getHeight(), true);
        t->backup();
        return t;
    }

    sdlCanvas* mid = nullptr;
    float srcDiv = 1.0f;
    bool pureScale = fabs(m[0][1]) < 0.0001f && fabs(m[1][0]) < 0.0001f && m[0][0] > 0.0f && m[1][1] > 0.0f;
    if (pureScale && (m[0][0] < 0.5f || m[1][1] < 0.5f)) {
        int tw = (int)ceilf(c->getWidth() * m[0][0]);
        int th = (int)ceilf(c->getHeight() * m[1][1]);
        mid = progressiveMinifyCanvas(c, tw, th, &srcDiv);
    }
    sdlCanvas* src = mid ? mid : c;

    src->lock();
    t->lock();

    int srcW = src->getWidth(), srcH = src->getHeight();
    int dstW = t->getWidth(), dstH = t->getHeight();

    const int SHIFT = 16;
    const int ONE = 1 << SHIFT;

    for (int y = 0; y < dstH; ++y) {
        float fy = miny + y + 0.5f;
        for (int x = 0; x < dstW; ++x) {
            float fx = minx + x + 0.5f;
            float sx = (i[0][0] * (fx - ox) + i[0][1] * (fy - oy)) / srcDiv;
            float sy = (i[1][0] * (fx - ox) + i[1][1] * (fy - oy)) / srcDiv;

            int ix = (int)floor(sx);
            int iy = (int)floor(sy);
            float fxfrac = sx - ix;
            float fyfrac = sy - iy;

            unsigned color;
            if (!filter) {
                if (ix < 0) ix = 0; else if (ix >= srcW) ix = srcW - 1;
                if (iy < 0) iy = 0; else if (iy >= srcH) iy = srcH - 1;
                color = src->getPixelFast(ix, iy);
            }
            else if (tform_method == 1) {
                int w1 = (int)((1.0f - fxfrac) * (1.0f - fyfrac) * ONE);
                int w2 = (int)(fxfrac * (1.0f - fyfrac) * ONE);
                int w3 = (int)((1.0f - fxfrac) * fyfrac * ONE);
                int w4 = (int)(fxfrac * fyfrac * ONE);
                unsigned c00 = src->getPixelFast(ix, iy);
                unsigned c10 = src->getPixelFast(ix + 1, iy);
                unsigned c01 = src->getPixelFast(ix, iy + 1);
                unsigned c11 = src->getPixelFast(ix + 1, iy + 1);
                int a = ((c00 >> 24) & 0xFF) * w1 + ((c10 >> 24) & 0xFF) * w2 + ((c01 >> 24) & 0xFF) * w3 + ((c11 >> 24) & 0xFF) * w4;
                int r = ((c00 >> 16) & 0xFF) * w1 + ((c10 >> 16) & 0xFF) * w2 + ((c01 >> 16) & 0xFF) * w3 + ((c11 >> 16) & 0xFF) * w4;
                int g = ((c00 >> 8) & 0xFF) * w1 + ((c10 >> 8) & 0xFF) * w2 + ((c01 >> 8) & 0xFF) * w3 + ((c11 >> 8) & 0xFF) * w4;
                int b = (c00 & 0xFF) * w1 + (c10 & 0xFF) * w2 + (c01 & 0xFF) * w3 + (c11 & 0xFF) * w4;
                a = (a >> SHIFT) & 0xFF;
                r = (r >> SHIFT) & 0xFF;
                g = (g >> SHIFT) & 0xFF;
                b = (b >> SHIFT) & 0xFF;
                color = ((unsigned)a << 24) | (r << 16) | (g << 8) | b;
            }
            else if (tform_method == 2) {
                const float* wx = cubic_lut.taps(fxfrac);
                const float* wy = cubic_lut.taps(fyfrac);
                float a = 0.0f, r = 0.0f, g = 0.0f, b = 0.0f, total_w = 0.0f;
                for (int dy = 0; dy < 4; ++dy) {
                    float wyv = wy[dy];
                    if (wyv == 0.0f) continue;
                    int yi = iy + dy - 1;
                    if (yi < 0) yi = 0;
                    if (yi >= srcH) yi = srcH - 1;
                    for (int dx = 0; dx < 4; ++dx) {
                        float w = wx[dx] * wyv;
                        if (w == 0.0f) continue;
                        int xi = ix + dx - 1;
                        if (xi < 0) xi = 0;
                        if (xi >= srcW) xi = srcW - 1;
                        unsigned pix = src->getPixelFast(xi, yi);
                        a += ((pix >> 24) & 0xFF) * w;
                        r += ((pix >> 16) & 0xFF) * w;
                        g += ((pix >> 8) & 0xFF) * w;
                        b += (pix & 0xFF) * w;
                        total_w += w;
                    }
                }
                if (total_w > 0.0f) {
                    int aa = (int)(a / total_w + 0.5f);
                    int rr = (int)(r / total_w + 0.5f);
                    int gg = (int)(g / total_w + 0.5f);
                    int bb = (int)(b / total_w + 0.5f);
                    if (aa < 0) aa = 0; else if (aa > 255) aa = 255;
                    if (rr < 0) rr = 0; else if (rr > 255) rr = 255;
                    if (gg < 0) gg = 0; else if (gg > 255) gg = 255;
                    if (bb < 0) bb = 0; else if (bb > 255) bb = 255;
                    color = ((unsigned)aa << 24) | (rr << 16) | (gg << 8) | bb;
                }
                else {
                    color = 0; // this still should never happen and if it does the world will explode
                }
            }
            else {
                if (ix < 0) ix = 0; else if (ix >= srcW) ix = srcW - 1;
                if (iy < 0) iy = 0; else if (iy >= srcH) iy = srcH - 1;
                color = src->getPixelFast(ix, iy);
            }
            t->setPixelFast(x, y, color);
        }
    }

    t->unlock();
    src->unlock();
    if (mid) sdl_graphics->freeCanvas(mid);
    t->backup();

    return t;
}

static bool saveCanvas(sdlCanvas* c, const std::string& f)
{
    std::ofstream out(f.c_str(), std::ios::binary);
    if (!out.good()) return false;

    int tempsize = (c->getWidth() * 3 + 3) & ~3;

    BITMAPFILEHEADER bf;
    memset(&bf, 0, sizeof(bf));
    bf.bfType = 'B' | ('M' << 8);
    bf.bfSize = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + tempsize * c->getHeight();
    bf.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    BITMAPINFOHEADER bi; memset(&bi, 0, sizeof(bi));
    bi.biSize = sizeof(bi);
    bi.biWidth = c->getWidth();
    bi.biHeight = c->getHeight();
    bi.biPlanes = 1;
    bi.biBitCount = 24;
    out.write((char*)&bf, sizeof(bf));
    out.write((char*)&bi, sizeof(bi));

    unsigned char* temp = new unsigned char[tempsize];
    memset(temp, 0, tempsize);

    c->lock();
    for (int y = c->getHeight() - 1; y >= 0; --y)
    {
        unsigned char* dest = temp;
        for (int x = 0; x < c->getWidth(); ++x)
        {
            unsigned rgb = c->getPixelFast(x, y);
            *dest++ = rgb & 0xff;
            *dest++ = (rgb >> 8) & 0xff;
            *dest++ = (rgb >> 16) & 0xff;
        }
        out.write((char*)temp, tempsize);
    }
    c->unlock();

    delete[] temp;

    return out.good();
}

int bbCountGfxDrivers()
{
    return sdl_runtime->numGraphicsDrivers();
}

BBStr* bbGfxDriverName(int n)
{
    debugDriver(n, "GfxDriverName");
    std::string t; int caps;
    sdl_runtime->graphicsDriverInfo(n - 1, &t, &caps);
    return new BBStr(t);
}

void  bbSetGfxDriver(int n)
{
    debugDriver(n, "SetGfxDriver");
    gfx_modes.clear();
    sdl_driver = n - 1;
}

int  bbCountGfxModes()
{
    gfx_modes.clear();
    int n = sdl_runtime->numGraphicsModes(sdl_driver);
    for (int k = 0; k < n; ++k)
    {
        GfxMode m;
        sdl_runtime->graphicsModeInfo(sdl_driver, k, &m.w, &m.h, &m.d, &m.caps);
        gfx_modes.push_back(m);
    }
    return gfx_modes.size();
}

int  bbGfxModeWidth(int n)
{
    debugMode(n, "GfxModeWidth");
    return gfx_modes[n - 1].w;
}

int  bbGfxModeHeight(int n)
{
    debugMode(n, "GfxModeHeight");
    return gfx_modes[n - 1].h;
}

int  bbGfxModeDepth(int n)
{
    debugMode(n, "GfxModeDepth");
    return gfx_modes[n - 1].d;
}

static int modeExists(int w, int h, int d, bool bb3d)
{
    int cnt = sdl_runtime->numGraphicsModes(sdl_driver);
    for (int k = 0; k < cnt; ++k)
    {
        int tw, th, td, tc;
        sdl_runtime->graphicsModeInfo(sdl_driver, k, &tw, &th, &td, &tc);
        if (bb3d && !(tc & sdlRuntime::GFXMODECAPS_3D)) continue;
        if (w == tw && h == th && d == td) return 1;
    }
    return 0;
}

int  bbGfxModeExists(int w, int h, int d)
{
    return modeExists(w, h, d, false);
}

int  bbGfxDriver3D(int n)
{
    debugDriver(n, "GfxDriver3D");
    std::string t; int caps;
    sdl_runtime->graphicsDriverInfo(n - 1, &t, &caps);
    return (caps & sdlRuntime::GFXMODECAPS_3D) ? 1 : 0;
}

int  bbCountGfxModes3D()
{
    gfx_modes.clear();
    int n = sdl_runtime->numGraphicsModes(sdl_driver);
    for (int k = 0; k < n; ++k)
    {
        GfxMode m;
        sdl_runtime->graphicsModeInfo(sdl_driver, k, &m.w, &m.h, &m.d, &m.caps);
        if (m.caps & sdlRuntime::GFXMODECAPS_3D) gfx_modes.push_back(m);
    }
    return gfx_modes.size();
}

int  bbGfxMode3DExists(int w, int h, int d)
{
    return modeExists(w, h, d, true);
}

int  bbGfxMode3D(int n)
{
    debugMode(n, "GfxMode3D");
    return gfx_modes[n - 1].caps & sdlRuntime::GFXMODECAPS_3D ? 1 : 0;
}

int  bbWindowed3D()
{
    int tc;
    sdl_runtime->windowedModeInfo(&tc);
    return (tc & sdlRuntime::GFXMODECAPS_3D) ? 1 : 0;
}

float bbDPIScaleX() {
    sdl_runtime->calculateDPI();
    return sdl_runtime->scale_x;
}

float bbDPIScaleY() {
    sdl_runtime->calculateDPI();
    return sdl_runtime->scale_y;
}

int  bbTotalVidMem()
{
    return sdl_graphics->getTotalVidmem();
}

int  bbAvailVidMem()
{
    return sdl_graphics->getAvailVidmem();
}

static void applyCanvasBuffer(sdlCanvas* buff)
{
    if (!buff || !sdl_graphics->verifyCanvas(buff)) {
        RTEX(MultiLang::buffer_not_exist);
    }
    if (sdl_graphics) sdl_graphics->setActiveCanvas(buff);
    sdl_canvas = buff;
    scroll_rect_stack.clear();
    curs_x = curs_y = 0;
    sdl_canvas->setOrigin(0, 0);
    sdl_canvas->setViewport(0, 0, sdl_canvas->getWidth(), sdl_canvas->getHeight());
    sdl_canvas->setColor(curr_color);
    sdl_canvas->setClsColor(curr_clsColor);
    sdl_canvas->setFont(curr_font);
    if (sdl_scene) sdl_scene->setDepthTarget(nullptr);
}

void bbSetBuffer(sdlCanvas* buff)
{
    debugCanvas(buff, "SetBuffer");
    applyCanvasBuffer(buff);
}

void bbSetBufferDepth(sdlCanvas* buff, sdlCanvas* depthBuff)
{
    debugCanvas(buff, "SetBufferDepth");
    applyCanvasBuffer(buff);
    if (sdl_scene) sdl_scene->setDepthTarget(depthBuff);
}

sdlCanvas* bbGraphicsBuffer()
{
    return sdl_canvas;
}

int bbLoadBuffer(sdlCanvas* c, BBStr* str)
{
    debugCanvas(c, "LoadBuffer");
    std::string s = *str; delete str;
    sdlCanvas* t = sdl_graphics->loadCanvas(s, 0);
    if (!t) return 0;
    float m[2][2];
    m[0][0] = (float)c->getWidth() / (float)t->getWidth();
    m[1][1] = (float)c->getHeight() / (float)t->getHeight();
    m[1][0] = m[0][1] = 0;
    sdlCanvas* p = tformCanvas(t, m, 0, 0);
    sdl_graphics->freeCanvas(t);
    int ox, oy;
    c->getOrigin(&ox, &oy); c->setOrigin(0, 0);
    c->blit(0, 0, p, 0, 0, p->getWidth(), p->getHeight(), true);
    sdl_graphics->freeCanvas(p);
    return 1;
}

int bbSaveBuffer(sdlCanvas* c, BBStr* str)
{
    debugCanvas(c, "SaveBuffer");
    std::string t = *str; delete str;
    return saveCanvas(c, t) ? 1 : 0;
}

void bbBufferDirty(sdlCanvas* c)
{
    debugCanvas(c, "BufferDirty");
    c->backup();
}

static void graphics(int w, int h, int d, int flags) {
    for (bbImage* img : image_set) img->savePixels();
    // MessageBoxA(NULL, "graphics(): entered", "Debug", MB_OK);
    freeGraphics(false);
    // MessageBoxA(NULL, "graphics(): after freeGraphics", "Debug", MB_OK);
    sdl_runtime->closeGraphics(sdl_graphics);
    // MessageBoxA(NULL, "graphics(): after closeGraphics", "Debug", MB_OK);
    sdl_graphics = sdl_runtime->openGraphics(w, h, d, sdl_driver, flags);
    // MessageBoxA(NULL, "graphics(): after openGraphics", "Debug", MB_OK);
    if (!sdl_runtime->idle()) RTEX(0);
    // MessageBoxA(NULL, "graphics(): after idle", "Debug", MB_OK);
    if (!sdl_graphics) RTEX(MultiLang::unable_create_gxgraphics_instance);
    // MessageBoxA(NULL, "graphics(): sdl_graphics valid", "Debug", MB_OK);

    for (bbImage* img : image_set) {
        img->restoreToDevice();
    }
    for (bbImage* img : image_set) img->releaseSavedPixels();

    curr_clsColor = 0;
    curr_color = 0xffffffff;
    curr_font = sdl_graphics->getDefaultFont();

    // MessageBoxA(NULL, "graphics(): after getDefaultFont", "Debug", MB_OK);
    sdlCanvas* buff = (flags & sdlGraphics::GRAPHICS_3D) ? sdl_graphics->getBackCanvas() : sdl_graphics->getFrontCanvas();
    // MessageBoxA(NULL, "graphics(): before bbSetBuffer", "Debug", MB_OK);
    bbSetBuffer(buff);
    // MessageBoxA(NULL, "graphics(): after bbSetBuffer", "Debug", MB_OK);
}

void bbGraphics(int w, int h, int d, int mode)
{
    int flags = 0;
    switch (mode)
    {
    case 0:flags |= debug ? sdlGraphics::GRAPHICS_WINDOWED : 0; break;
    case 1:break;
    case 2:flags |= sdlGraphics::GRAPHICS_WINDOWED; break;
    case 3:flags |= sdlGraphics::GRAPHICS_WINDOWED | sdlGraphics::GRAPHICS_SCALED; break;
    case 4:flags |= sdlGraphics::GRAPHICS_WINDOWED | sdlGraphics::GRAPHICS_BORDERLESS; break;
    case 6:flags |= sdlGraphics::GRAPHICS_WINDOWED | sdlGraphics::GRAPHICS_AUTOSUSPEND; break;
    case 7:flags |= sdlGraphics::GRAPHICS_WINDOWED | sdlGraphics::GRAPHICS_SCALED | sdlGraphics::GRAPHICS_AUTOSUSPEND; break;
    default:RTEX(MultiLang::illegal_graphics_mode);
    }
    graphics(w, h, d, flags);
}

void bbGraphics3D(int w, int h, int d, int mode)
{
    int flags = sdlGraphics::GRAPHICS_3D;
    switch (mode)
    {
    case 0:flags |= (debug && bbWindowed3D()) ? sdlGraphics::GRAPHICS_WINDOWED : 0; break;
    case 1:break;
    case 2:flags |= sdlGraphics::GRAPHICS_WINDOWED; break;
    case 3:flags |= sdlGraphics::GRAPHICS_WINDOWED | sdlGraphics::GRAPHICS_SCALED; break;
    case 4:flags |= sdlGraphics::GRAPHICS_WINDOWED | sdlGraphics::GRAPHICS_BORDERLESS; break;
    case 5:flags |= sdlGraphics::GRAPHICS_WINDOWED | sdlGraphics::GRAPHICS_BORDERLESS | sdlGraphics::GRAPHICS_SCALED; break;
    case 6:flags |= sdlGraphics::GRAPHICS_WINDOWED | sdlGraphics::GRAPHICS_AUTOSUSPEND; break;
    case 7:flags |= sdlGraphics::GRAPHICS_WINDOWED | sdlGraphics::GRAPHICS_SCALED | sdlGraphics::GRAPHICS_AUTOSUSPEND; break;
    default:RTEX(MultiLang::illegal_graphics3d_mode);
    }
    graphics(w, h, d, flags);
    extern void blitz3d_open();
    blitz3d_open();
}

void bbEndGraphics()
{
    freeGraphics();
    sdl_runtime->closeGraphics(sdl_graphics);
    sdl_graphics = sdl_runtime->openGraphics(400, 300, 0, 0, sdlGraphics::GRAPHICS_WINDOWED | 4);  // 4 = GRAPHICS_3D
    if (!sdl_runtime->idle()) RTEX(0);
    if (sdl_graphics)
    {
        curr_clsColor = 0;
        curr_color = 0xffffffff;
        curr_font = sdl_graphics->getDefaultFont();
        bbSetBuffer(sdl_graphics->getFrontCanvas());
        return;
    }
    RTEX(MultiLang::unable_close_gxgraphics_instance);
}

void bbSetGraphicsMode(int width, int height, int fullscreen, int borderless = 0) {
    if (!sdl_graphics) {
        ErrorLog("SetGraphicsMode", MultiLang::graphics_not_set);
        return;
    }
    bool fs = (fullscreen != 0);
    bool bl = (borderless != 0);
    if (!sdl_graphics->changeDisplayMode(width, height, fs, bl)) {
        ErrorLog("SetGraphicsMode", "Failed to change display mode");
    }
}

int bbGraphicsLost()
{
    return sdl_runtime->graphicsLost();
}

int bbInFocus()
{
    return sdl_runtime->focus();
}

void bbSetDarkMode(int dark_mode) {
    if (!sdl_graphics) {
        ErrorLog("SetDarkMode", MultiLang::graphics_not_set);
        return;
    }
    if (!sdl_graphics->setDarkMode(dark_mode)) {
        ErrorLog("SetDarkMode", "Failed to set dark mode");
    }
}

int bbDesktopWidth()
{
    return sdl_runtime->desktopWidth();
}

int bbDesktopHeight()
{
    return sdl_runtime->desktopHeight();
}

void  bbSetGamma(int r, int g, int b, float dr, float dg, float db)
{
    if (dr < 0) dr = 0;
    else if (dr > 255.0f) dr = 255.0f;
    if (dg < 0) dg = 0;
    else if (dg > 255.0f) dg = 255.0f;
    if (db < 0) db = 0;
    else if (db > 255.0f) db = 255.0f;
    sdl_graphics->setGamma(r, g, b, dr, dg, db);
}

void  bbUpdateGamma(int calibrate)
{
    sdl_graphics->updateGamma(!!calibrate);
}

float  bbGammaRed(int n)
{
    float dr, dg, db;
    sdl_graphics->getGamma(n, n, n, &dr, &dg, &db);
    return dr;
}

float  bbGammaGreen(int n)
{
    float dr, dg, db;
    sdl_graphics->getGamma(n, n, n, &dr, &dg, &db);
    return dg;
}

float  bbGammaBlue(int n)
{
    float dr, dg, db;
    sdl_graphics->getGamma(n, n, n, &dr, &dg, &db);
    return db;
}

sdlCanvas* bbFrontBuffer()
{
    return sdl_graphics->getFrontCanvas();
}

sdlCanvas* bbBackBuffer()
{
    return sdl_graphics->getBackCanvas();
}

void bbLockBuffer(sdlCanvas* buff)
{
    if (buff) debugCanvas(buff, "LockBuffer");
    (buff ? buff : sdl_canvas)->lock();
}

void bbUnlockBuffer(sdlCanvas* buff)
{
    if (buff) debugCanvas(buff, "UnlockBuffer");
    (buff ? buff : sdl_canvas)->unlock();
}

int bbBufferWidth(sdlCanvas* buff)
{
    if (buff) debugCanvas(buff, "BufferWidth");
    return (buff ? buff : sdl_canvas)->getWidth();
}

int bbBufferHeight(sdlCanvas* buff)
{
    if (buff) debugCanvas(buff, "BufferHeight");
    return (buff ? buff : sdl_canvas)->getHeight();
}

int bbBufferDepth(sdlCanvas* buff) {
    if (buff) debugCanvas(buff, "BufferDepth");
    return (buff ? buff : sdl_canvas)->getDepth();
}

sdlCanvas* bbDepthBuffer() {
    return sdl_scene ? sdl_scene->getDepthTarget() : 0;
}

void bbDrawBuffer(sdlCanvas* buff, int x, int y, int width, int height, int blending) {
    debugCanvas(buff, "DrawBuffer");
    if (!sdl_canvas) return;
    sdl_canvas->blitstretch(x, y, width, height, buff, 0, 0, buff->getWidth(), buff->getHeight(), blending == 0);
}

bbImage* bbGetImage(int id) {
    if (id < 0) return 0;
    for (bbImage* i : image_set) {
        if (id-- == 0) return i;
    }
    return 0;
}

int bbGetImagesCount() {
    return (int)image_set.size();
}

BBStr* bbImageName(bbImage* i) {
    debugImage(i, "ImageName");
    return new BBStr(i->name);
}

int bbReadPixel(int x, int y, sdlCanvas* buff)
{
    if (buff) debugCanvas(buff, "ReadPixel");
    return (buff ? buff : sdl_canvas)->getPixel(x, y);
}

void bbWritePixel(int x, int y, int argb, sdlCanvas* buff)
{
    if (buff) debugCanvas(buff, "WritePixel");
    (buff ? buff : sdl_canvas)->setPixel(x, y, argb);
}

int bbReadPixelFast(int x, int y, sdlCanvas* buff)
{
    return (buff ? buff : sdl_canvas)->getPixelFast(x, y);
}

void bbWritePixelFast(int x, int y, int argb, sdlCanvas* buff)
{
    (buff ? buff : sdl_canvas)->setPixelFast(x, y, argb);
}

void bbCopyPixel(int src_x, int src_y, sdlCanvas* src, int dest_x, int dest_y, sdlCanvas* buff)
{
    (buff ? buff : sdl_canvas)->copyPixel(dest_x, dest_y, src ? src : sdl_canvas, src_x, src_y);
}

void bbCopyPixelFast(int src_x, int src_y, sdlCanvas* src, int dest_x, int dest_y, sdlCanvas* buff)
{
    (buff ? buff : sdl_canvas)->copyPixelFast(dest_x, dest_y, src ? src : sdl_canvas, src_x, src_y);
}

int bbScanLine()
{
    return sdl_graphics->getScanLine();
}

void bbVWait(int n)
{
    sdl_graphics->vwait();
    if (!sdl_runtime->idle()) RTEX(0);
}

void bbFlip(int vwait)
{
    if (fps_cap_ms > 0) {
        int now = sdl_runtime->getMilliSecs();
        int elapsed = now - last_flip_ms;
        int remaining = fps_cap_ms - elapsed;
        if (remaining > 0) {
            if (!sdl_runtime->delay(remaining)) RTEX(0);
        }
    }
    sdl_graphics->flip(vwait ? true : false);
    if (!sdl_runtime->idle()) RTEX(0);
    int now_ms = sdl_runtime->getMilliSecs();
    last_flip_ms = now_ms;
    if (fps_window_start == 0) {
        fps_window_start = now_ms;
        fps_frame_count = 0;
        fps_value = 0;
    }
    ++fps_frame_count;
    int window_ms = now_ms - fps_window_start;
    if (window_ms >= 500 && window_ms > 0) {
        fps_value = (int)(fps_frame_count * 1000 / window_ms);
        fps_frame_count = 0;
        fps_window_start = now_ms;
    }
}

int bbGetFPS()
{
    if (!sdl_runtime || !sdl_graphics || fps_window_start == 0) return 0;
    if (sdl_runtime->getMilliSecs() - last_flip_ms > 1000) return 0;
    return fps_value;
}

void bbCapFPS(int fps)
{
    if (fps <= 0) {
        fps_cap_ms = 0;
        return;
    }
    fps_cap_ms = 1000 / fps;
    if (fps_cap_ms < 1) fps_cap_ms = 1;
    last_flip_ms = sdl_runtime->getMilliSecs();
}

void bbUncapFPS()
{
    fps_cap_ms = 0;
}

int bbGraphicsWidth()
{
    return sdl_graphics->getWidth();
}

int bbGraphicsHeight()
{
    return sdl_graphics->getHeight();
}

int bbGraphicsDepth()
{
    return sdl_graphics->getDepth();
}

void bbOrigin(int x, int y)
{
    sdl_canvas->setOrigin(x, y);
}

void bbViewport(int x, int y, int w, int h)
{
    sdl_canvas->setViewport(x, y, w, h);
}

static RECT intersectRect(const RECT& a, const RECT& b)
{
    RECT r;
    r.left = a.left > b.left ? a.left : b.left;
    r.top = a.top > b.top ? a.top : b.top;
    r.right = a.right < b.right ? a.right : b.right;
    r.bottom = a.bottom < b.bottom ? a.bottom : b.bottom;
    if (r.right < r.left) r.right = r.left;
    if (r.bottom < r.top) r.bottom = r.top;
    return r;
}

void bbBeginScrollRect(int x, int y, int w, int h, int scroll_x, int scroll_y)
{
    int ox, oy, vx, vy, vw, vh;
    sdl_canvas->getOrigin(&ox, &oy);
    sdl_canvas->getViewport(&vx, &vy, &vw, &vh);

    ScrollRectState st;
    st.viewport.left = vx; st.viewport.top = vy;
    st.viewport.right = vx + vw; st.viewport.bottom = vy + vh;
    st.origin_x = ox; st.origin_y = oy;
    scroll_rect_stack.push_back(st);

    RECT want;
    want.left = ox + x; want.top = oy + y;
    want.right = want.left + w; want.bottom = want.top + h;
    RECT clip = intersectRect(st.viewport, want);

    sdl_canvas->setViewport(clip.left, clip.top, clip.right - clip.left, clip.bottom - clip.top);
    sdl_canvas->setOrigin(ox + x - scroll_x, oy + y - scroll_y);
}

void bbEndScrollRect()
{
    if (scroll_rect_stack.empty()) return;
    ScrollRectState st = scroll_rect_stack.back();
    scroll_rect_stack.pop_back();
    sdl_canvas->setViewport(st.viewport.left, st.viewport.top,
        st.viewport.right - st.viewport.left, st.viewport.bottom - st.viewport.top);
    sdl_canvas->setOrigin(st.origin_x, st.origin_y);
}

void bbColor(int r, int g, int b, int a)
{
    sdl_canvas->setColor(curr_color = (a << 24) | (r << 16) | (g << 8) | b);
}

void bbGetColor(int x, int y)
{
    sdl_canvas->setColor(curr_color = sdl_canvas->getPixel(x, y));
}

int bbColorRed()
{
    return (sdl_canvas->getColor() >> 16) & 0xff;
}

int bbColorGreen()
{
    return (sdl_canvas->getColor() >> 8) & 0xff;
}

int bbColorBlue()
{
    return sdl_canvas->getColor() & 0xff;
}

int bbColorAlpha()
{
    return (curr_color >> 24) & 0xff;
}

void bbSet2DEffect(sdlEffect* effect) {
    if (!sdl_canvas) return;
    sdl_canvas->set2DEffect(effect);
}

void bbClear2DEffect() {
    if (!sdl_canvas) return;
    sdl_canvas->set2DEffect(nullptr);
}

sdlEffect* bbGet2DEffect() {
    return sdl_canvas ? sdl_canvas->get2DEffect() : nullptr;
}

void bbClsColor(int r, int g, int b, int a)
{
    sdl_canvas->setClsColor(curr_clsColor = (a << 24) | (r << 16) | (g << 8) | b);
}

void bbSetFont(sdlFont* f)
{
    debugFont(f, "SetFont");
    sdl_canvas->setFont(curr_font = f);
}

void bbCls()
{
    sdl_canvas->cls();
}

void bbPlot(int x, int y)
{
    sdl_canvas->plot(x, y);
}

void bbLine(int x1, int y1, int x2, int y2)
{
    sdl_canvas->line(x1, y1, x2, y2);
}

void bbRect(int x, int y, int w, int h, int solid)
{
    sdl_canvas->rect(x, y, w, h, solid);
}

void bbOval(int x, int y, int w, int h, int solid)
{
    sdl_canvas->oval(x, y, w, h, solid);
}

/*
* xPos: 0 = align left, 1 = align center, 2 = align right
* yPos: 0 = align top, 1 = align middle, 2 = align bottom
*/
void bbText(int x, int y, BBStr* str, int xPos, int yPos)
{
    if (xPos == 2) x -= curr_font->getWidth(*str);
    if (xPos == 1) x -= curr_font->getWidth(*str) / 2;
    if (yPos == 2) y -= curr_font->getHeight();
    if (yPos == 1) y -= curr_font->getHeight() / 2;
    sdl_canvas->text(x, y, *str);
    delete str;
}

void bbSetFontSmooth(int enable) {
    if (curr_font) {
        curr_font->setSmooth(enable != 0);
    }
}

BBStr* bbConvertToANSI(BBStr* str)
{
    BBStr* ret = new BBStr(UTF8::convertToAnsi(str->c_str()));
    delete str;
    return ret;
}

BBStr* bbConvertToUTF8(BBStr* str)
{
    BBStr* ret = new BBStr(UTF8::convertToUtf8(str->c_str()));
    delete str;
    return ret;
}

BBStr* bbGetTextureLoadError() {
    return new BBStr(ddUtil::getLastImageError());
}

void bbCopyRect(int sx, int sy, int w, int h, int dx, int dy, sdlCanvas* src, sdlCanvas* dest)
{
    if (src) debugCanvas(src, "CopyRect");
    else src = sdl_canvas;
    if (dest) debugCanvas(dest, "CopyRect");
    else dest = sdl_canvas;

    if (dest->getFlags() & sdlCanvas::CANVAS_TEXTURE) {
        sdl_graphics->copy(dest, dx, dy, w, h, src, sx, sy, w, h);
    }
    else {
        dest->blit(dx, dy, src, sx, sy, w, h, true);
    }
}

void bbCopyRectStretch(int sx, int sy, int w, int h, int dx, int dy, int dw, int dh, sdlCanvas* src, sdlCanvas* dest)
{
    if (src) debugCanvas(src, "CopyRectStretch");
    else src = sdl_canvas;
    if (dest) debugCanvas(dest, "CopyRectStretch");
    else dest = sdl_canvas;
    dest->blitstretch(dx, dy, dw, dh, src, sx, sy, w, h, true);
}

void bbDrawBufferRect(sdlCanvas* src, int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh)
{
    debugCanvas(src, "DrawBufferRect");
    sdl_canvas->blitstretch(dx, dy, dw, dh, src, sx, sy, sw, sh, !src->hasMask());
}

sdlFont* bbLoadFont(BBStr* name, int height, bool bold, bool italic, bool underlined) {
    if (!sdl_graphics) {
        delete name;
        return nullptr;
    }
    sdlFont* font = sdl_graphics->loadFont(*name, height, bold, italic, underlined);
    delete name;
    return font;
}

void bbFreeFont(sdlFont* f)
{
    debugFont(f, "FreeFont");
    if (f == curr_font) bbSetFont(sdl_graphics->getDefaultFont());
    sdl_graphics->freeFont(f);
}

int bbFontWidth()
{
    return curr_font->getWidth();
}

int bbFontHeight()
{
    return curr_font->getHeight();
}

sdlFont* bbGetFont() {
    return curr_font;
}

int bbStringWidth(BBStr* str)
{
    std::string t = *str; delete str;
    return curr_font->getWidth(t);
}

int bbStringHeight(BBStr* str)
{
    delete str;
    return curr_font->getHeight() + curr_font->getRenderOffset();
}

BBStr* bbFontPath(BBStr* facename) {
    return new BBStr(UTF8::getSystemFontFile(facename->c_str()).c_str());
}

sdlMovie* bbOpenMovie(BBStr* s)
{
    sdlMovie* movie = sdl_graphics->openMovie(*s, 0); delete s;
    return movie;
}

int bbDrawMovie(sdlMovie* movie, int x, int y, int w, int h)
{
    if (w < 0) w = movie->getWidth();
    if (h < 0) h = movie->getHeight();
    int playing = movie->draw(sdl_canvas, x, y, w, h);
    if (!sdl_runtime->idle()) RTEX(0);
    return playing;
}

int bbMovieWidth(sdlMovie* movie)
{
    return movie->getWidth();
}

int bbMovieHeight(sdlMovie* movie)
{
    return movie->getHeight();
}

int bbMoviePlaying(sdlMovie* movie)
{
    return movie->isPlaying();
}

int bbMovieTime(sdlMovie* movie)
{
    return (int)(movie->getTime() * 1000.0);
}

int bbMovieLength(sdlMovie* movie)
{
    return (int)(movie->getLength() * 1000.0);
}

void bbSeekMovie(sdlMovie* movie, int time)
{
    movie->setTime(time / 1000.0);
}

void bbCloseMovie(sdlMovie* movie)
{
    sdl_graphics->closeMovie(movie);
}

bbImage* bbLoadImage(BBStr* s)
{
    std::string path = *s;
    delete s;
    sdlCanvas* c = sdl_graphics->loadCanvas(path, 0);
    if (!c) {
        std::string errMsg = "Failed to load image: " + path;
        const std::string& libErr = ddUtil::getLastImageError();
        if (!libErr.empty()) errMsg += " (" + libErr + ")";
        RTEX(errMsg.c_str());
    }
    c->backup();
    if (auto_midhandle) c->setHandle(c->getWidth() / 2, c->getHeight() / 2);
    std::vector<sdlCanvas*> frames;
    frames.push_back(c);
    bbImage* i = new bbImage(frames);
    i->name = path;
    image_set.insert(i);
    return i;
}

bbImage* bbLoadImageFlag(BBStr* s, int flags)
{
    std::string path = *s;
    delete s;
    sdlCanvas* c = sdl_graphics->loadCanvas(path, flags);
    if (!c) {
        std::string errMsg = "Failed to load image: " + path;
        const std::string& libErr = ddUtil::getLastImageError();
        if (!libErr.empty()) errMsg += " (" + libErr + ")";
        RTEX(errMsg.c_str());
    }
    c->backup();
    if (auto_midhandle) c->setHandle(c->getWidth() / 2, c->getHeight() / 2);
    std::vector<sdlCanvas*> frames;
    frames.push_back(c);
    bbImage* i = new bbImage(frames);
    i->name = path;
    image_set.insert(i);
    return i;
}

bbImage* bbLoadAnimImage(BBStr* s, int w, int h, int first, int cnt) {
    std::string path = *s;
    delete s;

    if (sdl_graphics->runtime && sdl_graphics->runtime->sdlGpu) {
        if (w <= 0 || h <= 0 || first < 0 || cnt <= 0) return 0;
        sdlCanvas* pic = sdl_graphics->loadCanvas(path, 0);
        if (!pic) return 0;
        int srcFlags = pic->getFlags() & sdlCanvas::CANVAS_TEX_ALPHA;
        int fpr = pic->getWidth() / w;
        int fpp = pic->getHeight() / h * fpr;
        if (fpr <= 0 || first + cnt > fpp) {
            sdl_graphics->freeCanvas(pic);
            return 0;
        }
        int src_x = first % fpr * w;
        int src_y = first / fpr * h;
        std::vector<sdlCanvas*> frames;
        for (int k = 0; k < cnt; ++k) {
            sdlCanvas* c = sdl_graphics->createCanvas(w, h, sdlCanvas::CANVAS_TEXTURE | srcFlags);
            if (!c) {
                for (int i = 0; i < k; ++i) sdl_graphics->freeCanvas(frames[i]);
                sdl_graphics->freeCanvas(pic);
                return 0;
            }
            c->setLogicalSize(w, h);
            sdl_graphics->copy(c, 0, 0, w, h, pic, src_x, src_y, w, h);
            c->backup();
            if (auto_midhandle) c->setHandle(w / 2, h / 2);
            frames.push_back(c);
            src_x += w;
            if (src_x + w > pic->getWidth()) { src_x = 0; src_y += h; }
        }
        sdl_graphics->freeCanvas(pic);
        bbImage* image = new bbImage(frames);
        image_set.insert(image);
        return image;
    }

    int srcFlags = ddUtil::hasActualAlpha(path) ? sdlCanvas::CANVAS_TEX_ALPHA : 0;

    IDirect3DTexture9* picTex = ddUtil::loadTextureSurface(path, srcFlags, sdl_graphics, false);
    if (!picTex) return 0;
    sdlCanvas* pic = new sdlCanvas(sdl_graphics, picTex, sdlCanvas::CANVAS_TEXTURE | srcFlags);
    sdl_graphics->adoptCanvas(pic);

    int fpr = pic->getWidth() / w;
    int fpp = pic->getHeight() / h * fpr;
    if (first + cnt > fpp) {
        sdl_graphics->freeCanvas(pic);
        return 0;
    }

    int src_x = first % fpr * w;
    int src_y = first / fpr * h;

    std::vector<sdlCanvas*> frames;
    for (int k = 0; k < cnt; ++k) {
        IDirect3DTexture9* tex = ddUtil::createTextureSurface(w, h, sdlCanvas::CANVAS_TEXTURE | srcFlags, sdl_graphics, false);
        if (!tex) {
            for (int i = 0; i < k; ++i) sdl_graphics->freeCanvas(frames[i]);
            sdl_graphics->freeCanvas(pic);
            return 0;
        }
        sdlCanvas* c = new sdlCanvas(sdl_graphics, tex, sdlCanvas::CANVAS_TEXTURE | srcFlags);
        sdl_graphics->adoptCanvas(c);

        c->setLogicalSize(w, h);

        c->blit(0, 0, pic, src_x, src_y, w, h, true);
        c->backup();
        if (auto_midhandle) c->setHandle(w / 2, h / 2);
        frames.push_back(c);
        src_x += w;
        if (src_x + w > pic->getWidth()) { src_x = 0; src_y += h; }
    }
    sdl_graphics->freeCanvas(pic);

    bbImage* image = new bbImage(frames);
    image_set.insert(image);
    return image;
}

Texture* bbLoadAnimTextureGrid(BBStr* file, int flags, int fw, int fh, int first, int cnt) {
    std::string path = *file;
    delete file;

    if (fw <= 0 || fh <= 0) {
        ErrorLog("LoadAnimTextureGrid", "Frame width and height must be positive");
        return nullptr;
    }
    if (first < 0 || cnt <= 0) {
        ErrorLog("LoadAnimTextureGrid", "Frame range out of bounds");
        return nullptr;
    }

    int imgW = 0, imgH = 0;
    if (sdl_graphics->runtime && sdl_graphics->runtime->sdlGpu) {
        auto probe = DecodeImageFile(path);
        if (!probe) {
            ErrorLog("LoadAnimTextureGrid", "Failed to load image");
            return nullptr;
        }
        imgW = probe->w;
        imgH = probe->h;
    }
    else {
        IDirect3DTexture9* picTex = ddUtil::loadTextureSurface(path, flags, sdl_graphics, false, &imgW, &imgH);
        if (!picTex) {
            ErrorLog("LoadAnimTextureGrid", "Failed to load image");
            return nullptr;
        }
        picTex->Release();
    }

    int frameW = fw;
    int frameH = fh;
    int gridCols = imgW / frameW;
    int gridRows = imgH / frameH;
    if (gridCols <= 0 || gridRows <= 0) {
        ErrorLog("LoadAnimTextureGrid", "Frame size larger than image");
        return nullptr;
    }
    if (first + cnt > gridCols * gridRows) {
        ErrorLog("LoadAnimTextureGrid", "Frame range out of bounds");
        return nullptr;
    }

    Texture* tex = new Texture(path, flags, frameW, frameH, first, cnt);
    if (!tex->getCanvas(0)) {
        delete tex;
        ErrorLog("LoadAnimTextureGrid", "Failed to create Texture from image");
        return nullptr;
    }
    texture_set.insert(tex);
    return tex;
}

bbImage* bbCopyImage(bbImage* i)
{
    debugImage(i, "CopyImage");
    std::vector<sdlCanvas*> frames;
    const std::vector<sdlCanvas*>& f = i->getFrames();
    for (int k = 0; k < f.size(); ++k)
    {
        sdlCanvas* t = f[k];
        sdlCanvas* c = sdl_graphics->createCanvas(t->getWidth(), t->getHeight(), 0);
        if (!c)
        {
            for (--k; k >= 0; --k) sdl_graphics->freeCanvas(frames[k]);
            return 0;
        }
        int x, y;
        t->getHandle(&x, &y);
        t->setHandle(0, 0);
        c->blit(0, 0, t, 0, 0, t->getWidth(), t->getHeight(), true);
        if (auto_dirty) c->backup();
        t->setHandle(x, y);
        c->setHandle(x, y);
        c->copyMaskFrom(t);
        frames.push_back(c);
    }
    bbImage* t = new bbImage(frames, i->origWidth, i->origHeight);
    t->drawScaleX = i->drawScaleX;
    t->drawScaleY = i->drawScaleY;
    t->tform[0][0] = i->tform[0][0]; t->tform[0][1] = i->tform[0][1];
    t->tform[1][0] = i->tform[1][0]; t->tform[1][1] = i->tform[1][1];
    image_set.insert(t);
    return t;
}

bbImage* bbCreateImage(int w, int h, int n)
{
    std::vector<sdlCanvas*> frames;
    for (int k = 0; k < n; ++k)
    {
        sdlCanvas* c = sdl_graphics->createCanvas(w, h, 0);
        if (!c)
        {
            for (--k; k >= 0; --k) sdl_graphics->freeCanvas(frames[k]);
            return 0;
        }
        if (auto_dirty) c->backup();
        if (auto_midhandle) c->setHandle(c->getWidth() / 2, c->getHeight() / 2);
        frames.push_back(c);
    }
    bbImage* i = new bbImage(frames);
    image_set.insert(i);
    return i;
}

bbImage* bbCreateImageFlag(int w, int h, int n, int flags)
{
    std::vector<sdlCanvas*> frames;
    for (int k = 0; k < n; ++k)
    {
        sdlCanvas* c = sdl_graphics->createCanvas(w, h, flags);
        if (!c)
        {
            for (--k; k >= 0; --k) sdl_graphics->freeCanvas(frames[k]);
            return 0;
        }
        if (auto_dirty) c->backup();
        if (auto_midhandle) c->setHandle(c->getWidth() / 2, c->getHeight() / 2);
        frames.push_back(c);
    }
    bbImage* i = new bbImage(frames);
    image_set.insert(i);
    return i;
}

void bbFreeImage(bbImage* i)
{
    if (!image_set.erase(i)) return;
    const std::vector<sdlCanvas*>& f = i->getFrames();
    for (int k = 0; k < f.size(); ++k)
    {
        if (f[k] == sdl_canvas)
        {
            bbSetBuffer(sdl_graphics->getFrontCanvas());
            break;
        }
    }
    delete i;
}

int bbSaveImage(bbImage* i, BBStr* str, int n)
{
    debugImage(i, "SaveImage", n);
    std::string t = *str; delete str;
    sdlCanvas* c = i->getFrames()[n];
    return saveCanvas(c, t) ? 1 : 0;
}

void bbGrabImage(bbImage* i, int x, int y, int n)
{
    debugImage(i, "GrabImage", n);
    sdlCanvas* c = i->getFrames()[n];
    int src_ox, src_oy, dst_hx, dst_hy;
    sdl_canvas->getOrigin(&src_ox, &src_oy);
    c->getHandle(&dst_hx, &dst_hy);
    x += src_ox - dst_hx; y += src_oy - dst_hy;
    c->setViewport(0, 0, c->getWidth(), c->getHeight());
    c->blit(0, 0, sdl_canvas, x, y, c->getWidth(), c->getHeight(), true);
    i->saveOrigPixels();
    if (auto_dirty) c->backup();
}

sdlCanvas* bbImageBuffer(bbImage* i, int n)
{
    debugImage(i, "ImageBuffer", n);
    return i->getFrames()[n];
}

void bbDrawImage(bbImage* i, int x, int y, int frame)
{
    debugImage(i, "DrawImage", frame);
    sdlCanvas* c = i->getFrames()[frame];
    int w = c->getWidth(), h = c->getHeight();
    if (!i->isIdentity()) {
        float m[2][2]; i->getCombinedMat(m);
        bool isScaleOnly = fabsf(m[0][1]) < 1e-6f && fabsf(m[1][0]) < 1e-6f && m[0][0] > 0.0f && m[1][1] > 0.0f;
        if (isScaleOnly) {
            float sx = m[0][0], sy = m[1][1];
            int hx, hy; c->getHandle(&hx, &hy);
            int dw = (int)(w * sx + 0.5f);
            int dh = (int)(h * sy + 0.5f);
            int shx = (int)(hx * sx + 0.5f);
            int shy = (int)(hy * sy + 0.5f);
            bool solid = !c->hasMask() && !((c->getFlags() & sdlCanvas::CANVAS_TEX_ALPHA) || c->format.hasAlphaMask());
            sdl_canvas->blitstretch(x + hx - shx, y + hy - shy, dw, dh, c, 0, 0, w, h, solid);
            return;
        }
        sdl_canvas->blitTForm(x, y, c, 0, 0, w, h, m, filter);
        return;
    }
    if (c->hasMask()) {
        sdl_canvas->blit(x, y, c, 0, 0, w, h, false);
    }
    else if ((c->getFlags() & sdlCanvas::CANVAS_TEX_ALPHA) || c->format.hasAlphaMask()) {
        sdl_canvas->blitAlpha(x, y, c, 0, 0, w, h, 0xffffffff, false);
    }
    else {
        sdl_canvas->blit(x, y, c, 0, 0, w, h, true);
    }
}

void bbDrawBlock(bbImage* i, int x, int y, int frame)
{
    debugImage(i, "DrawBlock", frame);
    sdlCanvas* c = i->getFrames()[frame];
    if (!i->isIdentity()) {
        float m[2][2]; i->getCombinedMat(m);
        bool isScaleOnly = fabsf(m[0][1]) < 1e-6f && fabsf(m[1][0]) < 1e-6f && m[0][0] > 0.0f && m[1][1] > 0.0f;
        if (isScaleOnly) {
            float sx = m[0][0], sy = m[1][1];
            int w = c->getWidth(), h = c->getHeight();
            int hx, hy; c->getHandle(&hx, &hy);
            int dw = (int)(w * sx + 0.5f);
            int dh = (int)(h * sy + 0.5f);
            int shx = (int)(hx * sx + 0.5f);
            int shy = (int)(hy * sy + 0.5f);
            sdl_canvas->blitstretch(x + hx - shx, y + hy - shy, dw, dh, c, 0, 0, w, h, true);
            return;
        }
        sdl_canvas->blitTForm(x, y, c, 0, 0, c->getWidth(), c->getHeight(), m, filter);
        return;
    }
    sdl_canvas->blit(x, y, c, 0, 0, c->getWidth(), c->getHeight(), true);
}

static void tile(bbImage* i, int x, int y, int frame, bool solid)
{
    sdlCanvas* c = i->getFrames()[frame];

    int hx, hy;
    c->getHandle(&hx, &hy);
    int w = c->getWidth(), h = c->getHeight();

    int ox, oy, vp_x, vp_y, vp_w, vp_h;
    sdl_canvas->getOrigin(&ox, &oy);
    sdl_canvas->getViewport(&vp_x, &vp_y, &vp_w, &vp_h);
    int dx = vp_x - ox + hx;
    int dy = vp_y - oy + hy;
    x -= dx;
    y -= dy;
    dx += (x >= 0 ? x % w : w - (-x % w));
    dy += (y >= 0 ? y % h : h - (-y % h));

    for (y = -h; y < vp_h; y += h)
    {
        for (x = -w; x < vp_w; x += w)
        {
            sdl_canvas->blit(x + dx, y + dy, c, 0, 0, w, h, solid);
        }
    }
}

void bbTileImage(bbImage* i, int x, int y, int frame)
{
    debugImage(i, "TileImage", frame);
    tile(i, x, y, frame, false);
}

void bbTileBlock(bbImage* i, int x, int y, int frame)
{
    debugImage(i, "TileBlock", frame);
    tile(i, x, y, frame, true);
}

void bbDrawImageRect(bbImage* i, int x, int y, int r_x, int r_y, int r_w, int r_h, int frame)
{
    debugImage(i, "DrawImageRect", frame);
    sdlCanvas* c = i->getFrames()[frame];
    if (!i->isIdentity()) {
        float m[2][2]; i->getCombinedMat(m);
        bool isScaleOnly = fabsf(m[0][1]) < 1e-6f && fabsf(m[1][0]) < 1e-6f && m[0][0] > 0.0f && m[1][1] > 0.0f;
        if (isScaleOnly) {
            float sx = m[0][0], sy = m[1][1];
            int hx, hy; c->getHandle(&hx, &hy);
            int dw = (int)(r_w * sx + 0.5f);
            int dh = (int)(r_h * sy + 0.5f);
            int shx = (int)(hx * sx + 0.5f);
            int shy = (int)(hy * sy + 0.5f);
            bool solid = !c->hasMask() && !((c->getFlags() & sdlCanvas::CANVAS_TEX_ALPHA) || c->format.hasAlphaMask());
            sdl_canvas->blitstretch(x + hx - shx, y + hy - shy, dw, dh, c, r_x, r_y, r_w, r_h, solid);
            return;
        }
        sdl_canvas->blitTForm(x, y, c, r_x, r_y, r_w, r_h, m, filter);
        return;
    }
    if (c->hasMask()) {
        sdl_canvas->blit(x, y, c, r_x, r_y, r_w, r_h, false);
    }
    else if ((c->getFlags() & sdlCanvas::CANVAS_TEX_ALPHA) || c->format.hasAlphaMask()) {
        sdl_canvas->blitAlpha(x, y, c, r_x, r_y, r_w, r_h, 0xffffffff, false);
    }
    else {
        sdl_canvas->blit(x, y, c, r_x, r_y, r_w, r_h, true);
    }
}

void bbDrawImageRectStretch(bbImage* i, int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh)
{
    debugImage(i, "DrawImageRectStretch", 0);
    sdlCanvas* c = i->getFrames()[0];
    sdl_canvas->blitstretch(dx, dy, dw, dh, c, sx, sy, sw, sh, true);
}

void bbDrawBlockRect(bbImage* i, int x, int y, int r_x, int r_y, int r_w, int r_h, int frame)
{
    debugImage(i, "DrawBlockRect", frame);
    sdlCanvas* c = i->getFrames()[frame];
    if (!i->isIdentity()) {
        float m[2][2]; i->getCombinedMat(m);
        bool isScaleOnly = fabsf(m[0][1]) < 1e-6f && fabsf(m[1][0]) < 1e-6f && m[0][0] > 0.0f && m[1][1] > 0.0f;
        if (isScaleOnly) {
            float sx = m[0][0], sy = m[1][1];
            int hx, hy; c->getHandle(&hx, &hy);
            int dw = (int)(r_w * sx + 0.5f);
            int dh = (int)(r_h * sy + 0.5f);
            int shx = (int)(hx * sx + 0.5f);
            int shy = (int)(hy * sy + 0.5f);
            sdl_canvas->blitstretch(x + hx - shx, y + hy - shy, dw, dh, c, r_x, r_y, r_w, r_h, true);
            return;
        }
        sdl_canvas->blitTForm(x, y, c, r_x, r_y, r_w, r_h, m, filter);
        return;
    }
    sdl_canvas->blit(x, y, c, r_x, r_y, r_w, r_h, true);
}

void bbMaskImage(bbImage* i, int r, int g, int b)
{
    debugImage(i, "MaskImage");
    unsigned argb = (r << 16) | (g << 8) | b;
    const std::vector<sdlCanvas*>& f = i->getFrames();
    for (int k = 0; k < f.size(); ++k) f[k]->setMask(argb);
}

void bbHandleImage(bbImage* i, int x, int y)
{
    debugImage(i, "HandleImage");
    const std::vector<sdlCanvas*>& f = i->getFrames();
    for (int k = 0; k < f.size(); ++k) f[k]->setHandle(x, y);
}

void bbMidHandle(bbImage* i)
{
    debugImage(i, "MidHandle");
    const std::vector<sdlCanvas*>& f = i->getFrames();
    for (int k = 0; k < f.size(); ++k) f[k]->setHandle(f[k]->getWidth() / 2, f[k]->getHeight() / 2);
}

void bbAutoMidHandle(int enable)
{
    auto_midhandle = enable ? true : false;
}

int bbImageWidth(bbImage* i)
{
    debugImage(i, "ImageWidth");
    sdlCanvas* c = i->getFrames()[0];
    int hx, hy; c->getHandle(&hx, &hy);
    int w = c->getWidth(), h = c->getHeight();
    if (i->isIdentity()) return (int)((w - hx) + 0.5f);
    float m[2][2]; i->getCombinedMat(m);
    float xs[4], ys[4];
    float px[4] = { (float)-hx, (float)(w - hx), (float)(w - hx), (float)-hx };
    float py[4] = { (float)-hy, (float)-hy, (float)(h - hy), (float)(h - hy) };
    float maxx = -1e30f;
    for (int k = 0; k < 4; ++k) {
        float tx = m[0][0] * px[k] + m[0][1] * py[k];
        if (tx > maxx) maxx = tx;
    }
    return (int)(maxx + 0.5f);
}

int bbImageHeight(bbImage* i)
{
    debugImage(i, "ImageHeight");
    sdlCanvas* c = i->getFrames()[0];
    int hx, hy; c->getHandle(&hx, &hy);
    int w = c->getWidth(), h = c->getHeight();
    if (i->isIdentity()) return (int)((h - hy) + 0.5f);
    float m[2][2]; i->getCombinedMat(m);
    float px[4] = { (float)-hx, (float)(w - hx), (float)(w - hx), (float)-hx };
    float py[4] = { (float)-hy, (float)-hy, (float)(h - hy), (float)(h - hy) };
    float maxy = -1e30f;
    for (int k = 0; k < 4; ++k) {
        float ty = m[1][0] * px[k] + m[1][1] * py[k];
        if (ty > maxy) maxy = ty;
    }
    return (int)(maxy + 0.5f);
}

int bbImageXHandle(bbImage* i)
{
    debugImage(i, "ImageXHandle");
    int x, y;
    i->getFrames()[0]->getHandle(&x, &y);
    return x;
}

int bbImageYHandle(bbImage* i)
{
    debugImage(i, "ImageYHandle");
    int x, y;
    i->getFrames()[0]->getHandle(&x, &y);
    return y;
}

static void getImageQuad(bbImage* i, sdlCanvas* c, int x, int y, vec2 out[4]) {
    int hx, hy; c->getHandle(&hx, &hy);
    int w = c->getWidth(), h = c->getHeight();
    float m[2][2]; i->getCombinedMat(m);
    float px[4] = { (float)-hx, (float)(w - hx), (float)(w - hx), (float)-hx };
    float py[4] = { (float)-hy, (float)-hy, (float)(h - hy), (float)(h - hy) };
    for (int k = 0; k < 4; ++k) {
        float tx = m[0][0] * px[k] + m[0][1] * py[k];
        float ty = m[1][0] * px[k] + m[1][1] * py[k];
        out[k].x = x + tx;
        out[k].y = y + ty;
    }
}

static bool quadsOverlap(const vec2 a[4], const vec2 b[4]) {
    auto axisTest = [&](float ax, float ay) -> bool {
        float minA = 1e30f, maxA = -1e30f, minB = 1e30f, maxB = -1e30f;
        for (int k = 0; k < 4; ++k) {
            float p = a[k].x * ax + a[k].y * ay;
            if (p < minA) minA = p; if (p > maxA) maxA = p;
            float q = b[k].x * ax + b[k].y * ay;
            if (q < minB) minB = q; if (q > maxB) maxB = q;
        }
        return !(maxA < minB || maxB < minA);
    };
    for (int e = 0; e < 4; ++e) {
        int n = (e + 1) % 4;
        float ex = a[n].x - a[e].x, ey = a[n].y - a[e].y;
        float ax = -ey, ay = ex;
        if (!axisTest(ax, ay)) return false;
    }
    for (int e = 0; e < 4; ++e) {
        int n = (e + 1) % 4;
        float ex = b[n].x - b[e].x, ey = b[n].y - b[e].y;
        float ax = -ey, ay = ex;
        if (!axisTest(ax, ay)) return false;
    }
    return true;
}

int bbImagesOverlap(bbImage* i1, int x1, int y1, bbImage* i2, int x2, int y2)
{
    debugImage(i1, "ImagesOverlap");
    debugImage(i2, "ImagesOverlap");
    sdlCanvas* c1 = i1->getFrames()[0];
    sdlCanvas* c2 = i2->getFrames()[0];
    if (!i1->isIdentity() || !i2->isIdentity()) {
        vec2 q1[4], q2[4];
        getImageQuad(i1, c1, x1, y1, q1);
        getImageQuad(i2, c2, x2, y2, q2);
        return quadsOverlap(q1, q2) ? 1 : 0;
    }
    return c1->collide(x1, y1, c2, x2, y2, true);
}

int bbImagesCollide(bbImage* i1, int x1, int y1, int f1, bbImage* i2, int x2, int y2, int f2)
{
    debugImage(i1, "ImagesCollide", f1);
    debugImage(i2, "ImagesCollide", f2);
    sdlCanvas* c1 = i1->getFrames()[f1];
    sdlCanvas* c2 = i2->getFrames()[f2];
    if (!i1->isIdentity() || !i2->isIdentity()) {
        vec2 q1[4], q2[4];
        getImageQuad(i1, c1, x1, y1, q1);
        getImageQuad(i2, c2, x2, y2, q2);
        return quadsOverlap(q1, q2) ? 1 : 0;
    }
    return c1->collide(x1, y1, c2, x2, y2, false);
}

int bbRectsOverlap(int x1, int y1, int w1, int h1, int x2, int y2, int w2, int h2)
{
    if (x1 + w1 <= x2 || x1 >= x2 + w2 || y1 + h1 <= y2 || y1 >= y2 + h2) return 0;
    return 1;
}

int bbImageRectOverlap(bbImage* i, int x, int y, int x2, int y2, int w2, int h2)
{
    debugImage(i, "ImageRectOverlap");
    sdlCanvas* c = i->getFrames()[0];
    if (!i->isIdentity()) {
        vec2 q[4]; getImageQuad(i, c, x, y, q);
        vec2 r[4] = { {(float)x2,(float)y2}, {(float)(x2+w2),(float)y2}, {(float)(x2+w2),(float)(y2+h2)}, {(float)x2,(float)(y2+h2)} };
        return quadsOverlap(q, r) ? 1 : 0;
    }
    return c->rect_collide(x, y, x2, y2, w2, h2, true);
}

int bbImageRectCollide(bbImage* i, int x, int y, int f, int x2, int y2, int w2, int h2)
{
    debugImage(i, "ImageRectCollide", f);
    sdlCanvas* c = i->getFrames()[f];
    if (!i->isIdentity()) {
        vec2 q[4]; getImageQuad(i, c, x, y, q);
        vec2 r[4] = { {(float)x2,(float)y2}, {(float)(x2+w2),(float)y2}, {(float)(x2+w2),(float)(y2+h2)}, {(float)x2,(float)(y2+h2)} };
        return quadsOverlap(q, r) ? 1 : 0;
    }
    return c->rect_collide(x, y, x2, y2, w2, h2, false);
}

int bbImageWidthUnscaled(bbImage* i) {
    debugImage(i, "ImageWidthUnscaled");
    return i->origWidth;
}

int bbImageHeightUnscaled(bbImage* i) {
    debugImage(i, "ImageHeightUnscaled");
    return i->origHeight;
}

void bbTFormImage(bbImage* i, float a, float b, float c, float d)
{
    debugImage(i, "TFormImage");
    i->mulTForm(a, b, c, d);
}

void bbSetTFormMethod(int method) {
    if (method < 0 || method > 2) method = 1;
    tform_method = method;
}

void bbScaleImage(bbImage* i, float w, float h)
{
    debugImage(i, "ScaleImage");
    bbTFormImage(i, w, 0, 0, h);
}

void bbScaleImageFast(bbImage* i, float xscale, float yscale)
{
    debugImage(i, "ScaleImageFast");
    i->drawScaleX = xscale;
    i->drawScaleY = yscale;
}

void bbResizeImageFast(bbImage* i, float w, float h)
{
    debugImage(i, "ResizeImageFast");
    sdlCanvas* c = i->getFrames()[0];
    int cw = c->getWidth(), ch = c->getHeight();
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;
    int iw = (int)w, ih = (int)h;
    if (iw < 1) iw = 1;
    if (ih < 1) ih = 1;
    i->drawScaleX = (float)iw / (float)cw;
    i->drawScaleY = (float)ih / (float)ch;
    i->resetTForm();
}

static unsigned sampleOrigPixel(const std::vector<uint32_t>& src, int srcW, int srcH, float sx, float sy)
{
    int ix = (int)floor(sx);
    int iy = (int)floor(sy);
    float fxfrac = sx - ix;
    float fyfrac = sy - iy;

    auto clampedPixel = [&](int xi, int yi) -> unsigned {
        if (xi < 0) xi = 0; else if (xi >= srcW) xi = srcW - 1;
        if (yi < 0) yi = 0; else if (yi >= srcH) yi = srcH - 1;
        return src[yi * srcW + xi];
        };

    if (!filter) {
        return clampedPixel(ix, iy);
    }
    else if (tform_method == 1) {
        const int SHIFT = 16;
        const int ONE = 1 << SHIFT;
        int w1 = (int)((1.0f - fxfrac) * (1.0f - fyfrac) * ONE);
        int w2 = (int)(fxfrac * (1.0f - fyfrac) * ONE);
        int w3 = (int)((1.0f - fxfrac) * fyfrac * ONE);
        int w4 = (int)(fxfrac * fyfrac * ONE);
        unsigned c00 = clampedPixel(ix, iy);
        unsigned c10 = clampedPixel(ix + 1, iy);
        unsigned c01 = clampedPixel(ix, iy + 1);
        unsigned c11 = clampedPixel(ix + 1, iy + 1);
        int a = ((c00 >> 24) & 0xFF) * w1 + ((c10 >> 24) & 0xFF) * w2 + ((c01 >> 24) & 0xFF) * w3 + ((c11 >> 24) & 0xFF) * w4;
        int r = ((c00 >> 16) & 0xFF) * w1 + ((c10 >> 16) & 0xFF) * w2 + ((c01 >> 16) & 0xFF) * w3 + ((c11 >> 16) & 0xFF) * w4;
        int g = ((c00 >> 8) & 0xFF) * w1 + ((c10 >> 8) & 0xFF) * w2 + ((c01 >> 8) & 0xFF) * w3 + ((c11 >> 8) & 0xFF) * w4;
        int b = (c00 & 0xFF) * w1 + (c10 & 0xFF) * w2 + (c01 & 0xFF) * w3 + (c11 & 0xFF) * w4;
        a = (a >> SHIFT) & 0xFF;
        r = (r >> SHIFT) & 0xFF;
        g = (g >> SHIFT) & 0xFF;
        b = (b >> SHIFT) & 0xFF;
        return ((unsigned)a << 24) | (r << 16) | (g << 8) | b;
    }
    else {
        const float* wx = cubic_lut.taps(fxfrac);
        const float* wy = cubic_lut.taps(fyfrac);
        float a = 0.0f, r = 0.0f, g = 0.0f, b = 0.0f, total_w = 0.0f;
        for (int dy = 0; dy < 4; ++dy) {
            float wyv = wy[dy];
            if (wyv == 0.0f) continue;
            for (int dx = 0; dx < 4; ++dx) {
                float w = wx[dx] * wyv;
                if (w == 0.0f) continue;
                unsigned pix = clampedPixel(ix + dx - 1, iy + dy - 1);
                a += ((pix >> 24) & 0xFF) * w;
                r += ((pix >> 16) & 0xFF) * w;
                g += ((pix >> 8) & 0xFF) * w;
                b += (pix & 0xFF) * w;
                total_w += w;
            }
        }
        if (total_w > 0.0f) {
            int aa = (int)(a / total_w + 0.5f);
            int rr = (int)(r / total_w + 0.5f);
            int gg = (int)(g / total_w + 0.5f);
            int bb = (int)(b / total_w + 0.5f);
            if (aa < 0) aa = 0; else if (aa > 255) aa = 255;
            if (rr < 0) rr = 0; else if (rr > 255) rr = 255;
            if (gg < 0) gg = 0; else if (gg > 255) gg = 255;
            if (bb < 0) bb = 0; else if (bb > 255) bb = 255;
            return ((unsigned)aa << 24) | (rr << 16) | (gg << 8) | bb;
        }
        return 0;
    }
}

void bbResizeImage(bbImage* i, float w, float h)
{
    debugImage(i, "ResizeImage");
    int iw = (int)w, ih = (int)h;
    if (iw < 1) iw = 1;
    if (ih < 1) ih = 1;

    i->saveOrigPixels();

    const std::vector<sdlCanvas*>& f = i->getFrames();
    for (int k = 0; k < (int)f.size(); ++k)
    {
        sdlCanvas* c = f[k];
        int hx, hy; c->getHandle(&hx, &hy);

        const std::vector<uint32_t>& src = i->getOrigPixels(k);
        int srcW = i->origWidth, srcH = i->origHeight;

        std::vector<uint32_t> mipData;
        int sampleW = srcW, sampleH = srcH;
        if (iw * 2 < srcW || ih * 2 < srcH) {
            mipData = progressiveMinifyPixels(src, srcW, srcH, iw, ih, &sampleW, &sampleH);
        }
        const std::vector<uint32_t>& sampleSrc = mipData.empty() ? src : mipData;

        int srcFlags = c->getFlags() & (sdlCanvas::CANVAS_TEXTURE | sdlCanvas::CANVAS_TEX_ALPHA);
        sdlCanvas* t = sdl_graphics->createCanvas(iw, ih, srcFlags);
        t->setHandle(hx, hy);
        t->copyMaskFrom(c);

        t->lock();
        for (int y = 0; y < ih; ++y) {
            float sy = (y + 0.5f) * sampleH / (float)ih;
            for (int x = 0; x < iw; ++x) {
                float sx = (x + 0.5f) * sampleW / (float)iw;
                unsigned color = sampleOrigPixel(sampleSrc, sampleW, sampleH, sx, sy);
                t->setPixelFast(x, y, color);
            }
        }
        t->unlock();
        // i have fourth degree burns all over my entire face
        i->replaceFrame(k, t);
        t->backup();
    }

    i->origWidth = iw;
    i->origHeight = ih;
}

void bbRotateImage(bbImage* i, float d)
{
    debugImage(i, "RotateImage");
    d *= -dtor;
    bbTFormImage(i, cos(d), -sin(d), sin(d), cos(d));
}

void bbTFormFilter(int enable)
{
    filter = enable ? true : false;
}

BBStr* bbGetEffectError() {
    if (sdl_graphics) {
        return new BBStr(sdl_graphics->getLastEffectError());
    }
    return new BBStr("");
}

static int p_ox, p_oy, p_hx, p_hy, p_vpx, p_vpy, p_vpw, p_vph;

static sdlCanvas* startPrinting()
{

    sdlCanvas* c = sdl_graphics->getFrontCanvas();

    c->lock();
    c->unlock();

    c->getOrigin(&p_ox, &p_oy);
    c->getHandle(&p_hx, &p_hy);
    c->getViewport(&p_vpx, &p_vpy, &p_vpw, &p_vph);

    c->setOrigin(0, 0);
    c->setHandle(0, 0);
    c->setViewport(0, 0, c->getWidth(), c->getHeight());
    if (c != sdl_canvas)
    {
        c->setFont(curr_font);
        c->setColor(curr_color);
    }

    int dy = curs_y + curr_font->getHeight() - c->getHeight();
    if (dy > 0)
    {
        curs_y = c->getHeight() - curr_font->getHeight();
        c->blit(0, 0, c, 0, dy, c->getWidth(), c->getHeight() - dy, true);
        c->setColor(curr_clsColor);
        c->rect(0, c->getHeight() - dy, c->getWidth(), dy, true);
        c->setColor(curr_color);
    }
    return c;
}

static void endPrinting(sdlCanvas* c)
{
    c->setViewport(p_vpx, p_vpy, p_vpw, p_vph);
    c->setHandle(p_hx, p_hy);
    c->setOrigin(p_ox, p_oy);
    if (c == sdl_canvas) c->setColor(curr_color);
    if (!sdl_runtime->idle()) RTEX(0);
}

void bbWrite(BBStr* str)
{
    sdlCanvas* c = startPrinting();
    c->text(curs_x, curs_y, *str);
    curs_x += curr_font->getWidth(*str);
    endPrinting(c);
    delete str;
}

void bbPrint(BBStr* str)
{
    sdlCanvas* c = startPrinting();
    c->text(curs_x, curs_y, *str);
    curs_x = 0;
    curs_y += curr_font->getHeight() + 3; //avoid multiline overlapping by adding 3 to the font height
    endPrinting(c);
    delete str;
}

BBStr* bbInput(BBStr* prompt)
{
    sdlCanvas* c = startPrinting();
    std::string t = *prompt; delete prompt;

    //get temp canvas
    if (!p_canvas || p_canvas->getWidth() < c->getWidth() || p_canvas->getHeight() < curr_font->getHeight() * 2)
    {
        if (p_canvas) sdl_graphics->freeCanvas(p_canvas);
        p_canvas = sdl_graphics->createCanvas(c->getWidth(), curr_font->getHeight() * 2, 0);
        if (!p_canvas)
        {
            endPrinting(c);
            return new BBStr();
        }
    }
    //draw prompt
    c->text(curs_x, curs_y, t);
    curs_x += curr_font->getWidth(t);

    p_canvas->setFont(curr_font);
    p_canvas->setColor(curr_color);
    p_canvas->blit(0, 0, c, 0, curs_y, c->getWidth(), curr_font->getHeight(), true);

    std::string str;
    bool go = true;
    int curs = 0, last_key = 0, last_time, rep_delay;

    while (go)
    {
        //render all text
        //calc curs x and width
        int cx = curs_x + curr_font->getWidth(str.substr(0, curs));
        int cw = curr_font->getWidth(curs < str.size() ? str.substr(curs, 1) : "X");

        //wait for a key
        int key = 0, st = sdl_runtime->getMilliSecs(), tc = -1;

        while (sdl_runtime->idle())
        {
            int t = sdl_runtime->getMilliSecs();
            int n = (t - st) / 320;
            if (n != tc)
            {
                tc = n;
                c->blit(0, curs_y, p_canvas, 0, 0, c->getWidth(), curr_font->getHeight(), true);
                c->text(curs_x, curs_y, str);
                if (!(tc & 1))
                {	//cursor ON
                    c->setColor(curr_clsColor ^ 0xffffff);
                    c->rect(cx, curs_y, cw, curr_font->getHeight(), true);
                    c->setColor(curr_color);
                    c->text(cx, curs_y, str.substr(curs, 1));
                }
                sdl_graphics->flip(false);
            }
            if (key = sdl_keyboard->getKey())
            {
                if (int asc = sdl_input->toUnicode(key))
                {
                    rep_delay = 280;
                    last_key = key;
                    last_time = t;
                    key = asc;
                    break;
                }
            }
            if (last_key && sdl_keyboard->keyDown(last_key))
            {
                if (t - last_time > rep_delay)
                {
                    if (key = sdl_input->toUnicode(last_key))
                    {
                        last_time += rep_delay;
                        rep_delay = 40;
                        break;
                    }
                }
            }
            else last_key = 0;
            sdl_runtime->delay(20);
        }

        //check the key
        switch (key)
        {
        case 0:
            go = false;
            str = "";
            break;
        case 8:
            if (curs)
            {
                str = str.substr(0, curs - 1) + str.substr(curs);
                --curs;
            }
            break;
        case 27:
            curs = 0; str = "";
            break;
        case sdlInput::ASC_DELETE:
            if (curs < str.size()) str = str.substr(0, curs) + str.substr(curs + 1);
            break;
        case sdlInput::ASC_HOME:
            curs = 0;
            break;
        case sdlInput::ASC_END:
            curs = str.size();
            break;
        case sdlInput::ASC_LEFT:
            if (curs) --curs;
            break;
        case sdlInput::ASC_RIGHT:
            if (curs < str.size()) ++curs;
            break;
        case '\r':
            go = false;
            break;
        default:
            if (curr_font->isPrintable(key))
            {
                str = str.substr(0, curs) + char(key) + str.substr(curs);
                ++curs;
            }
        }

        //render text
        p_canvas->blit(0, curr_font->getHeight(), p_canvas, 0, 0, c->getWidth(), curr_font->getHeight(), true);
        p_canvas->text(curs_x, curr_font->getHeight(), str);
        c->blit(0, curs_y, p_canvas, 0, curr_font->getHeight(), c->getWidth(), curr_font->getHeight(), true);
    }

    curs_x = 0;
    curs_y += curr_font->getHeight() + 3;
    endPrinting(c);
    return new BBStr(str);
}

void bbLocate(int x, int y)
{
    sdlCanvas* c = sdl_graphics->getFrontCanvas();
    curs_x = x < 0 ? 0 : (x > c->getWidth() ? c->getWidth() : x);
    curs_y = y < 0 ? 0 : (y > c->getHeight() ? c->getHeight() : y);
}

void bbShowPointer()
{
    sdl_runtime->setPointerVisible(true);
}

void bbHidePointer()
{
    sdl_runtime->setPointerVisible(false);
}

bool graphics_create() {
    p_canvas = 0;
    filter = true;
    sdl_driver = 0;
    freeGraphics();
    auto_dirty = true;
    auto_midhandle = false;
    sdl_graphics = sdl_runtime->openGraphics(400, 300, 0, 0, sdlGraphics::GRAPHICS_WINDOWED | 4); // do this so fonts arent null at launch like how DX7 managed it
    if (sdl_graphics)
    {
        curr_clsColor = 0;
        curr_color = 0xffffffff;
        curr_font = sdl_graphics->getDefaultFont();
        bbSetBuffer(bbFrontBuffer());
        return true;
    }
    return false;
}

bool graphics_destroy()
{
    freeGraphics();
    gfx_modes.clear();
    if (sdl_graphics)
    {
        sdl_runtime->closeGraphics(sdl_graphics);
        sdl_graphics = 0;
    }
    return true;
}

void graphics_link(void (*rtSym)(const char* sym, void* pc))
{

    //gfx driver info
    rtSym("%CountGfxDrivers", bbCountGfxDrivers);
    rtSym("$GfxDriverName%driver", bbGfxDriverName);
    rtSym("SetGfxDriver%driver", bbSetGfxDriver);

    //gfx mode info
    rtSym("%CountGfxModes", bbCountGfxModes);
    rtSym("%GfxModeExists%width%height%depth", bbGfxModeExists);

    rtSym("%GfxModeWidth%mode", bbGfxModeWidth);
    rtSym("%GfxModeHeight%mode", bbGfxModeHeight);
    rtSym("%GfxModeDepth%mode", bbGfxModeDepth);
    rtSym("%AvailVidMem", bbAvailVidMem);
    rtSym("%TotalVidMem", bbTotalVidMem);

    rtSym("#DPIScaleX", bbDPIScaleX);
    rtSym("#DPIScaleY", bbDPIScaleY);

    rtSym("%GfxDriver3D%driver", bbGfxDriver3D);
    rtSym("%CountGfxModes3D", bbCountGfxModes3D);
    rtSym("%GfxMode3DExists%width%height%depth", bbGfxMode3DExists);
    rtSym("%GfxMode3D%mode", bbGfxMode3D);
    rtSym("%Windowed3D", bbWindowed3D);

    //display mode
    rtSym("Graphics%width%height%depth=0%mode=0", bbGraphics);
    rtSym("Graphics3D%width%height%depth=0%mode=0", bbGraphics3D);
    rtSym("EndGraphics", bbEndGraphics);
    rtSym("SetGraphicsMode%width%height%fullscreen", bbSetGraphicsMode);
    rtSym("%GraphicsLost", bbGraphicsLost);
    rtSym("%InFocus", bbInFocus);
    rtSym("SetDarkMode%dark_mode", bbSetDarkMode);

    rtSym("SetGamma%src_red%src_green%src_blue#dest_red#dest_green#dest_blue", bbSetGamma);
    rtSym("UpdateGamma%calibrate=0", bbUpdateGamma);
    rtSym("#GammaRed%red", bbGammaRed);
    rtSym("#GammaGreen%green", bbGammaGreen);
    rtSym("#GammaBlue%blue", bbGammaBlue);

    rtSym("%FrontBuffer", bbFrontBuffer);
    rtSym("%BackBuffer", bbBackBuffer);
    rtSym("%ScanLine", bbScanLine);
    rtSym("VWait%frames=1", bbVWait);
    rtSym("Flip%vwait=1", bbFlip);
    rtSym("%GetFPS", bbGetFPS);
    rtSym("CapFPS%fps", bbCapFPS);
    rtSym("UncapFPS", bbUncapFPS);
    rtSym("%GraphicsWidth", bbGraphicsWidth);
    rtSym("%GraphicsHeight", bbGraphicsHeight);
    rtSym("%GraphicsDepth", bbGraphicsDepth);

    //buffer management
    rtSym("SetBuffer%buffer", bbSetBuffer);
    rtSym("SetBufferDepth%buffer%depthbuffer", bbSetBufferDepth);
    rtSym("%GraphicsBuffer", bbGraphicsBuffer);
    rtSym("%LoadBuffer%buffer$bmpfile", bbLoadBuffer);
    rtSym("%SaveBuffer%buffer$bmpfile", bbSaveBuffer);
    rtSym("BufferDirty%buffer", bbBufferDirty);

    //fast pixel reads/write
    rtSym("LockBuffer%buffer=0", bbLockBuffer);
    rtSym("UnlockBuffer%buffer=0", bbUnlockBuffer);
    rtSym("%BufferWidth%buffer=0", bbBufferWidth);
    rtSym("%BufferHeight%buffer=0", bbBufferHeight);
    rtSym("%BufferDepth%buffer=0", bbBufferDepth);
    rtSym("%DepthBuffer", bbDepthBuffer);
    rtSym("DrawBuffer%buffer%x%y%width%height%blending=1", bbDrawBuffer);
    rtSym("%GetImage%id", bbGetImage);
    rtSym("%GetImagesCount", bbGetImagesCount);
    rtSym("$ImageName%image", bbImageName);
    rtSym("%ReadPixel%x%y%buffer=0", bbReadPixel);
    rtSym("WritePixel%x%y%argb%buffer=0", bbWritePixel);
    rtSym("%ReadPixelFast%x%y%buffer=0", bbReadPixelFast);
    rtSym("WritePixelFast%x%y%argb%buffer=0", bbWritePixelFast);
    rtSym("CopyPixel%src_x%src_y%src_buffer%dest_x%dest_y%dest_buffer=0", bbCopyPixel);
    rtSym("CopyPixelFast%src_x%src_y%src_buffer%dest_x%dest_y%dest_buffer=0", bbCopyPixelFast);

    //rendering
    rtSym("Origin%x%y", bbOrigin);
    rtSym("Viewport%x%y%width%height", bbViewport);
    rtSym("BeginScrollRect%x%y%width%height%scrollX=0%scrollY=0", bbBeginScrollRect);
    rtSym("EndScrollRect", bbEndScrollRect);
    rtSym("Color%red%green%blue%alpha=255", bbColor);
    rtSym("GetColor%x%y", bbGetColor);
    rtSym("%ColorRed", bbColorRed);
    rtSym("%ColorGreen", bbColorGreen);
    rtSym("%ColorBlue", bbColorBlue);
    rtSym("%ColorAlpha", bbColorAlpha);
    rtSym("ClsColor%red%green%blue%alpha=255", bbClsColor);
    rtSym("SetFont%font", bbSetFont);
    rtSym("Cls", bbCls);
    rtSym("Plot%x%y", bbPlot);
    rtSym("Rect%x%y%width%height%solid=1", bbRect);
    rtSym("Oval%x%y%width%height%solid=1", bbOval);
    rtSym("Line%x1%y1%x2%y2", bbLine);
    rtSym("Text%x%y$text%xPos=0%yPos=0", bbText);
    rtSym("$ConvertToANSI$str", bbConvertToANSI);
    rtSym("$ConvertToUTF8$str", bbConvertToUTF8);
    rtSym("CopyRect%source_x%source_y%width%height%dest_x%dest_y%src_buffer=0%dest_buffer=0", bbCopyRect);
    rtSym("CopyRectStretch%source_x%source_y%width%height%dest_x%dest_y%dest_w%dest_h%src_buffer=0%dest_buffer=0", bbCopyRectStretch);
    rtSym("DrawBufferRect%buffer%dest_x%dest_y%dest_w%dest_h%source_x%source_y%source_w%source_h", bbDrawBufferRect);
    rtSym("Set2DEffect%effect", bbSet2DEffect);
    rtSym("Clear2DEffect", bbClear2DEffect);
    rtSym("%Get2DEffect", bbGet2DEffect);

    //fonts
    rtSym("%LoadFont$fontname%height=12%bold=0%italic=0%underlined=0", bbLoadFont);
    rtSym("%CurrentFont", bbGetFont);
    rtSym("FreeFont%font", bbFreeFont);
    rtSym("%FontWidth", bbFontWidth);
    rtSym("%FontHeight", bbFontHeight);
    rtSym("%StringWidth$string", bbStringWidth);
    rtSym("%StringHeight$string", bbStringHeight);
    rtSym("$FontPath$facename", bbFontPath);
    rtSym("SetFontSmooth%enable", bbSetFontSmooth);

    //movies
    rtSym("%OpenMovie$file", bbOpenMovie);
    rtSym("%DrawMovie%movie%x=0%y=0%w=-1%h=-1", bbDrawMovie);
    rtSym("%MovieWidth%movie", bbMovieWidth);
    rtSym("%MovieHeight%movie", bbMovieHeight);
    rtSym("%MoviePlaying%movie", bbMoviePlaying);
    rtSym("%MovieTime%movie", bbMovieTime);
    rtSym("%MovieLength%movie", bbMovieLength);
    rtSym("SeekMovie%movie%time", bbSeekMovie);
    rtSym("CloseMovie%movie", bbCloseMovie);

    rtSym("%LoadImage$bmpfile", bbLoadImage);
    rtSym("%LoadImageFlag$bmpfile%flags", bbLoadImageFlag);
    rtSym("%LoadAnimImage$bmpfile%cellwidth%cellheight%first%count", bbLoadAnimImage);
    rtSym("%LoadAnimTextureGrid$file%flags%columns%rows%first%count", bbLoadAnimTextureGrid);
    rtSym("%CopyImage%image", bbCopyImage);
    rtSym("%CreateImage%width%height%frames=1", bbCreateImage);
    rtSym("%CreateImageFlag%width%height%frames=1%flags", bbCreateImageFlag);
    rtSym("FreeImage%image", bbFreeImage);
    rtSym("%SaveImage%image$bmpfile%frame=0", bbSaveImage);
    rtSym("%ImageWidthUnscaled%image", bbImageWidthUnscaled);
    rtSym("%ImageHeightUnscaled%image", bbImageHeightUnscaled);

    rtSym("GrabImage%image%x%y%frame=0", bbGrabImage);
    rtSym("%ImageBuffer%image%frame=0", bbImageBuffer);
    rtSym("DrawImage%image%x%y%frame=0", bbDrawImage);
    rtSym("DrawBlock%image%x%y%frame=0", bbDrawBlock);
    rtSym("TileImage%image%x=0%y=0%frame=0", bbTileImage);
    rtSym("TileBlock%image%x=0%y=0%frame=0", bbTileBlock);
    rtSym("DrawImageRect%image%x%y%rect_x%rect_y%rect_width%rect_height%frame=0", bbDrawImageRect);
    rtSym("DrawImageRectStretch%image%dest_x%dest_y%dest_w%dest_h%src_x%src_y%src_w%src_h", bbDrawImageRectStretch);
    rtSym("DrawBlockRect%image%x%y%rect_x%rect_y%rect_width%rect_height%frame=0", bbDrawBlockRect);
    rtSym("MaskImage%image%red%green%blue", bbMaskImage);
    rtSym("HandleImage%image%x%y", bbHandleImage);
    rtSym("MidHandle%image", bbMidHandle);
    rtSym("AutoMidHandle%enable", bbAutoMidHandle);
    rtSym("%ImageWidth%image", bbImageWidth);
    rtSym("%ImageHeight%image", bbImageHeight);
    rtSym("%ImageXHandle%image", bbImageXHandle);
    rtSym("%ImageYHandle%image", bbImageYHandle);

    rtSym("ScaleImage%image#xscale#yscale", bbScaleImage);
    rtSym("ScaleImageFast%image#xscale#yscale", bbScaleImageFast);
    rtSym("ResizeImage%image#width#height", bbResizeImage);
    rtSym("ResizeImageFast%image#width#height", bbResizeImageFast);
    rtSym("RotateImage%image#angle", bbRotateImage);
    rtSym("TFormImage%image#a#b#c#d", bbTFormImage);
    rtSym("SetTFormMethod%method", bbSetTFormMethod);
    rtSym("TFormFilter%enable", bbTFormFilter);
    rtSym("$GetEffectError", bbGetEffectError);
    rtSym("$GetShaderError", bbGetEffectError);

    rtSym("%ImagesOverlap%image1%x1%y1%image2%x2%y2", bbImagesOverlap);
    rtSym("%ImagesCollide%image1%x1%y1%frame1%image2%x2%y2%frame2", bbImagesCollide);
    rtSym("%RectsOverlap%x1%y1%width1%height1%x2%y2%width2%height2", bbRectsOverlap);
    rtSym("%ImageRectOverlap%image%x%y%rect_x%rect_y%rect_width%rect_height", bbImageRectOverlap);
    rtSym("%ImageRectCollide%image%x%y%frame%rect_x%rect_y%rect_width%rect_height", bbImageRectCollide);

    rtSym("Write$string", bbWrite);
    rtSym("Print$string=\"\"", bbPrint);
    rtSym("$Input$prompt=\"\"", bbInput);
    rtSym("Locate%x%y", bbLocate);

    rtSym("ShowPointer", bbShowPointer);
    rtSym("HidePointer", bbHidePointer);

    rtSym("%DesktopWidth", bbDesktopWidth);
    rtSym("%DesktopHeight", bbDesktopHeight);
}

extern "C" {
    _declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
    _declspec(dllexport) DWORD AmdPowerXpressRequestHighPerformance = 0x00000001;
}