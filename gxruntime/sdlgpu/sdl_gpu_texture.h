#ifndef SDL_GPU_TEXTURE_H
#define SDL_GPU_TEXTURE_H

struct SDL_GPUDevice;
struct SDL_GPUTexture;
struct SDL_GPUCommandBuffer;

class gxCanvas;

namespace sdlgpu {

	SDL_GPUTexture* CreateTexture2D(SDL_GPUDevice* dev, unsigned w, unsigned h, bool mipmaps);
	bool UploadTextureRGBA(SDL_GPUDevice* dev, SDL_GPUTexture* tex, unsigned w, unsigned h, const void* px);
	bool UploadTextureRGBA(SDL_GPUDevice* dev, SDL_GPUTexture* tex, unsigned w, unsigned h, const void* px, bool cycle);
	void ReleaseTexture(SDL_GPUDevice* dev, SDL_GPUTexture* tex);
	void InvalidateCanvasTextures(::gxCanvas* canvas);

	SDL_GPUTexture* CreateColorTarget(SDL_GPUDevice* dev, unsigned w, unsigned h);
	SDL_GPUTexture* CreateColorTarget(SDL_GPUDevice* dev, unsigned w, unsigned h, float r, float g, float b, float a);
	SDL_GPUTexture* CreateColorTargetMS(SDL_GPUDevice* dev, unsigned w, unsigned h, int sampleCount, float r, float g, float b);
	SDL_GPUTexture* CreateDepthTarget(SDL_GPUDevice* dev, unsigned w, unsigned h, int formatValue);
	SDL_GPUTexture* CreateDepthTarget(SDL_GPUDevice* dev, unsigned w, unsigned h, int formatValue, float depth, unsigned char stencil);
	SDL_GPUTexture* CreateDepthTarget(SDL_GPUDevice* dev, unsigned w, unsigned h, int formatValue, float depth, unsigned char stencil, int sampleCount);

	SDL_GPUTexture* GetCanvasTexture(SDL_GPUDevice* dev, ::gxCanvas* canvas);
	SDL_GPUTexture* GetCanvasMaskedTexture(SDL_GPUDevice* dev, ::gxCanvas* canvas, unsigned maskRGB);
	SDL_GPUTexture* EnsureCanvasRenderTarget(SDL_GPUDevice* dev, ::gxCanvas* canvas);
	SDL_GPUTexture* EnsureCanvasCubeTexture(SDL_GPUDevice* dev, ::gxCanvas* canvas);
	SDL_GPUTexture* EnsureCanvasDepthTarget(SDL_GPUDevice* dev, ::gxCanvas* canvas, unsigned w, unsigned h);
	bool DownloadCanvasTexture(SDL_GPUDevice* dev, ::gxCanvas* canvas);
	bool SeedCanvasTexture(SDL_GPUDevice* dev, ::gxCanvas* canvas, unsigned w, unsigned h, const void* rgba);
	SDL_GPUTexture* GetCanvasOverlayTexture(SDL_GPUDevice* dev, ::gxCanvas* canvas, bool keyCls = true);
	SDL_GPUTexture* GetCanvasOverlayTextureBatched(SDL_GPUDevice* dev, ::gxCanvas* canvas, SDL_GPUCommandBuffer* cmds, bool* didUpload = nullptr, bool keyCls = true);

	void TeardownTexturePools(SDL_GPUDevice* dev);
}

#endif
