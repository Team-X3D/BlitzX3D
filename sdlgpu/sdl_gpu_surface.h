#ifndef SDL_GPU_SURFACE_H
#define SDL_GPU_SURFACE_H

#include "sdl_gpu_common.h"
#include "sdl_gpu_pixels.h"

struct SDL_GPUDevice;

namespace sdlgpu {

class Surface {
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

	Surface(int w, int h, int flags);
	~Surface();

	SDL_GPUDevice* device = nullptr;

	int getWidth() const { return width; }
	int getHeight() const { return height; }
	int getFlags() const { return flags; }
	int cubeMode() const { return cube_mode; }
	int getCubeFace() const { return cube_face; }
	int getModify() const { return mod_cnt; }
	unsigned getClsColor() const { return cls_color; }

	void setModify(int n) const { mod_cnt = n; }
	void setClsColor(unsigned argb) { cls_color = argb; }
	void setCubeFace(int face) { cube_face = face; }
	void setCubeMode(int mode) { cube_mode = mode; }
	void setLogicalSize(int w, int h) { logical_w = w; logical_h = h; }

	bool ensureCPUBits() const;
	void releaseCPUBitsIfUnused() const;
	bool lockRO() const;
	bool lock() const { return lockRO(); }
	void unlock() const;
	bool isLocked() const { return locked_cnt > 0; }

	unsigned char* getLockedSurf() const { return locked_surf; }
	int getLockedPitch() const { return locked_pitch; }

	bool getSDLDirtyRect(RECT& out) const;
	void clearSDLDirty() const;
	void damage(const RECT& r) const;
	void damageAll() const;

	mutable unsigned char* cpu_bits = nullptr;
	mutable int cpu_pitch = 0, cpu_w = 0, cpu_h = 0;
	PixelFormat format;

	int logical_w = 0, logical_h = 0;

private:
	int width = 0, height = 0;
	int flags = 0, cube_mode = 0, cube_face = 2;
	unsigned cls_color = 0;

	mutable int mod_cnt = 0;
	mutable int locked_cnt = 0;
	mutable unsigned char* locked_surf = nullptr;
	mutable int locked_pitch = 0;

	mutable RECT sdlDirtyRect = {};
	mutable bool sdlDirtyValid = false;

	void sizeCPUStore(int w, int h) const;
	void allocCPUStore(int w, int h) const;
};

}

#endif
