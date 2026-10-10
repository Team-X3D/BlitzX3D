#ifndef SDL_GPU_PIXELS_H
#define SDL_GPU_PIXELS_H

namespace sdlgpu {

struct PixelFormat {
	bool alpha = true;

	int getDepth() const { return 32; }
	int getPitch() const { return 4; }
	bool is8888() const { return true; }
	bool hasAlphaMask() const { return alpha; }

	unsigned fromARGB(unsigned n) const { return n; }
	unsigned toARGB(unsigned n) const { return n; }

	void setPixel(void* p, unsigned n) const { *(unsigned*)p = n; }
	unsigned getPixel(void* p) const { return *(const unsigned*)p; }
};

inline void ConvertPixelsToRGBA(const PixelFormat& fmt, const unsigned char* src, unsigned char* dst, unsigned w) {
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
}

}

#endif
