#include "sdl_gpu_surface.h"

namespace sdlgpu {

Surface::Surface(int w, int h, int f) {
	width = w;
	height = h;
	flags = f;
	cube_mode = CUBEMODE_REFLECTION | CUBESPACE_WORLD;
	cube_face = 2;
	format.alpha = (f & (CANVAS_TEX_ALPHA | CANVAS_TEX_MASK)) != 0;
	logical_w = w;
	logical_h = h;
	sizeCPUStore(w, h);
}

Surface::~Surface() {
	delete[] cpu_bits;
	cpu_bits = nullptr;
}

void Surface::sizeCPUStore(int w, int h) const {
	delete[] cpu_bits;
	cpu_bits = nullptr;
	cpu_w = w;
	cpu_h = h;
	cpu_pitch = 0;
	if (w <= 0 || h <= 0) return;
	int bpp = format.getPitch();
	if (bpp <= 0) return;
	cpu_pitch = w * bpp;
}

void Surface::allocCPUStore(int w, int h) const {
	sizeCPUStore(w, h);
	if (cpu_pitch <= 0 || cpu_h <= 0) return;
	size_t planes = (flags & CANVAS_TEX_CUBE) ? 6 : 1;
	cpu_bits = new unsigned char[(size_t)cpu_pitch * (size_t)cpu_h * planes]();
}

bool Surface::ensureCPUBits() const {
	if (cpu_bits) return true;
	allocCPUStore(cpu_w, cpu_h);
	return cpu_bits != nullptr;
}

void Surface::releaseCPUBitsIfUnused() const {
	if (!cpu_bits || locked_cnt != 0) return;
	if (!(flags & CANVAS_TEXTURE)) return;
	delete[] cpu_bits;
	cpu_bits = nullptr;
}

bool Surface::lockRO() const {
	if (locked_cnt == 0) {
		if (!cpu_bits) {
			if (!ensureCPUBits()) return false;
		}
		locked_pitch = cpu_pitch;
		locked_surf = cpu_bits;
		if (flags & CANVAS_TEX_CUBE)
			locked_surf += (size_t)cube_face * (size_t)cpu_pitch * (size_t)cpu_h;
	}
	++locked_cnt;
	return true;
}

void Surface::unlock() const {
	if (locked_cnt == 0) return;
	--locked_cnt;
	if (locked_cnt == 0) {
		damageAll();
	}
}

bool Surface::getSDLDirtyRect(RECT& out) const {
	if (!sdlDirtyValid) return false;
	out = sdlDirtyRect;
	return true;
}

void Surface::clearSDLDirty() const {
	sdlDirtyValid = false;
}

void Surface::damage(const RECT& r) const {
	++mod_cnt;
	if (!sdlDirtyValid) {
		sdlDirtyRect = r;
		sdlDirtyValid = true;
		return;
	}
	if (r.left < sdlDirtyRect.left) sdlDirtyRect.left = r.left;
	if (r.top < sdlDirtyRect.top) sdlDirtyRect.top = r.top;
	if (r.right > sdlDirtyRect.right) sdlDirtyRect.right = r.right;
	if (r.bottom > sdlDirtyRect.bottom) sdlDirtyRect.bottom = r.bottom;
}

void Surface::damageAll() const {
	RECT full = { 0, 0, cpu_w, cpu_h };
	damage(full);
}

}
