#ifndef GXCANVAS_H
#define GXCANVAS_H

#include "ddutil.h"

class gxFont;
class gxGraphics;
class gxEffect;


class gxCanvas {
public:
	gxCanvas(gxGraphics* g, IDirect3DSurface9* surf, int flags);
	gxCanvas(gxGraphics* g, IDirect3DTexture9* tex, int flags);
	gxCanvas(gxGraphics* g, IDirect3DCubeTexture9* cube_tex, int flags);
	gxCanvas(gxGraphics* g, int w, int h, int flags);
	~gxCanvas();
	bool isCpuCanvas()const { return !surf && !tex && !cube_tex && !plain_surf; }

	gxGraphics* graphics;

	void backup();
	void restore();

	IDirect3DSurface9* getSurface()  const;
	IDirect3DBaseTexture9* getTexture() const;

	mutable int mod_cnt;
	mutable bool mipmapNeeded;

	mutable IDirect3DTexture9* blit_tex;
	mutable int blit_tex_mod_cnt;
	mutable unsigned blit_tex_mask;

	mutable int locked_pitch, locked_cnt, lock_mod_cnt, remip_cnt;
	mutable unsigned char* locked_surf;
	mutable bool lock_is_rt;
	mutable bool lock_ro;
	mutable bool lock_d3d = false;
	mutable unsigned char* cpu_bits = nullptr;
	mutable int cpu_pitch = 0, cpu_w = 0, cpu_h = 0;
	mutable bool d3d_dirty = false;
	mutable RECT sdlDirtyRect;
	mutable bool sdlDirtyValid;
	mutable bool cpu_keep = false;

	PixelFormat format;

	RECT clip_rect;

	unsigned mask_surf, color_surf, color_argb, clsColor_surf;
	bool has_mask;

	void setModify(int n);
	int  getModify() const;

	bool attachZBuffer();
	void releaseZBuffer();

	void restoreZBuffer();

	bool clip(RECT* d)          const;
	bool clip(RECT* d, RECT* s) const;
	void damage(const RECT& r)  const;
	void damageD3D(const RECT& r) const;
	void damageScene(const RECT& r) const;
	bool pushAllD3D() const;

	void set2DEffect(gxEffect* effect);
	gxEffect* get2DEffect() const;

	IDirect3DSurface9* surf;             // the "active" surf
	IDirect3DSurface9* z_surf;           // depth/stencil surf

private:
	int   flags, cube_mode, cube_face;

	IDirect3DSurface9* plain_surf;   // non text offscreen surf
	IDirect3DTexture9* tex;
	IDirect3DCubeTexture9* cube_tex;

	IDirect3DSurface9* cube_surfs[6];

	mutable IDirect3DSurface9* t_surf;

	mutable int cm_pitch;
	mutable unsigned* cm_mask;

	gxEffect* effect2D;
	gxFont* font;
	RECT viewport;
	int origin_x, origin_y, handle_x, handle_y;

	void updateBitMask(const RECT& r) const;
	bool lockImpl(bool ro)const;
	bool lockD3DRO() const;
	void allocCPUStore(int w, int h) const;
	void sizeCPUStore(int w, int h) const;
	bool ensureTemp(int w, int h, int fmt) const;
	bool pullD3D() const;
	bool pushRectD3D(const RECT& r) const;
	void damageImpl(const RECT& r, bool cpuSource) const;

	mutable int blit_batch_depth;
	mutable bool blit_batch_active;
	mutable void* blit_batch_saved;

	/***** GX INTERFACE *****/
public:
	enum {
		CANVAS_TEX_RGB = 0x0001,
		CANVAS_TEX_ALPHA = 0x0002,
		CANVAS_TEX_MASK = 0x0004,
		CANVAS_TEX_MIPMAP = 0x0008,
		CANVAS_TEX_CLAMPU = 0x0010,
		CANVAS_TEX_CLAMPV = 0x0020,
		CANVAS_TEX_SPHERE = 0x0040,
		CANVAS_TEX_CUBE = 0x0080,
		CANVAS_TEX_VIDMEM = 0x0100,
		CANVAS_TEX_HICOLOR = 0x0200,
		CANVAS_TEX_POINT = 0x0400,
		CANVAS_TEX_NOFILTER = 0x0800,
		CANVAS_TEX_BILINEAR = 0x1000,
		CANVAS_TEX_ANISOTROPIC = 0x2000,

		CANVAS_TEXTURE = 0x10000,
		CANVAS_NONDISPLAY = 0x20000,
		CANVAS_HIGHCOLOR = 0x40000
	};

	enum {
		CUBEMODE_REFLECTION = 1,
		CUBEMODE_NORMAL = 2,
		CUBEMODE_POSITION = 3,

		CUBESPACE_WORLD = 0,
		CUBESPACE_CAMERA = 4
	};

	void fillRect(const RECT& r, unsigned argb);

	//MANIPULATORS
	void setFont(gxFont* font);
	void setMask(unsigned argb);
	void setColor(unsigned argb);
	void setClsColor(unsigned argb);
	void setOrigin(int x, int y);
	void setHandle(int x, int y);
	void setViewport(int x, int y, int w, int h);

	void cls();
	void plot(int x, int y);
	void line(int x, int y, int x2, int y2);
	void rect(int x, int y, int w, int h, bool solid);
	void rectBlend(int x, int y, int w, int h, unsigned argb);
	void oval(int x, int y, int w, int h, bool solid);
	void text(int x, int y, const std::string& t);
	void blit(int x, int y, gxCanvas* src, int src_x, int src_y, int src_w, int src_h, bool solid);

	void blitstretch(int x, int y, int w, int h, gxCanvas* src, int src_x, int src_y, int src_w, int src_h, bool solid);//for CopyRectStretch

	void blitAlpha(int x, int y, gxCanvas* src, int src_x, int src_y, int src_w, int src_h, unsigned color_argb, bool filter = false);//for anti-aliased fonts

	void blitTForm(int x, int y, gxCanvas* src, int src_x, int src_y, int src_w, int src_h, float mat[2][2], bool filter);

	bool collide(int x, int y, const gxCanvas* src, int src_x, int src_y, bool solid)const;
	bool rect_collide(int x, int y, int rect_x, int rect_y, int rect_w, int rect_h, bool solid)const;

	void beginBlitBatch() const;
	void endBlitBatch() const;

	bool lock()const;
	bool lockRO()const;
	bool ensureCPUBits()const;
	void releaseCPUBitsIfUnused()const;
	bool isLocked()const { return locked_cnt > 0; }
	bool getSDLDirtyRect(RECT& out)const {
		if (!sdlDirtyValid) return false;
		out = sdlDirtyRect;
		return true;
	}
	void clearSDLDirty()const { sdlDirtyValid = false; }
	unsigned char* getLockedSurf()const { return locked_surf; }
	int getLockedPitch()const { return locked_pitch; }
	void setPixel(int x, int y, unsigned argb);
	void setPixelFast(int x, int y, unsigned argb) {
		format.setPixel(locked_surf + y * locked_pitch + x * format.getPitch(), argb);
		++mod_cnt;
	}
	void copyPixel(int x, int y, gxCanvas* src, int src_x, int src_y);
	void copyPixelFast(int x, int y, gxCanvas* src, int src_x, int src_y);
	unsigned getPixel(int x, int y)const;
	unsigned getPixelFast(int x, int y)const {
		return format.getPixel(locked_surf + y * locked_pitch + x * format.getPitch());
	};
	void unlock()const;

	void setCubeMode(int mode);
	void setCubeFace(int face);

	int logical_w, logical_h;
	void setLogicalSize(int w, int h) { logical_w = w; logical_h = h; }

	//ACCESSORS
	int getWidth()const;
	int getHeight()const;
	int getDepth()const;
	int getFlags()const { return flags; }
	int cubeMode()const { return cube_mode; }
	int getCubeFace()const { return cube_face; }
	void getOrigin(int* x, int* y)const;
	void getHandle(int* x, int* y)const;
	void getViewport(int* x, int* y, int* w, int* h)const;
	unsigned getMask()const;
	bool hasMask()const { return has_mask; }
	void copyMaskFrom(const gxCanvas* src) { mask_surf = src->mask_surf; has_mask = src->has_mask; }
	unsigned getColor()const;
	unsigned getClsColor()const;
	IDirect3DBaseTexture9* getTexSurface() const;
	void setMipmapNeeded(bool needed) const { mipmapNeeded = needed; }
};

#endif