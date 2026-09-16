#ifndef SDL_GPU_TEXT_H
#define SDL_GPU_TEXT_H

struct SDL_GPUDevice;
struct SDL_Window;
struct SDL_GPURenderPass;
struct SDL_GPUCommandBuffer;
struct SDL_GPUTexture;

class gxCanvas;

namespace sdlgpu {

struct TextQuad {
	float destX = 0.0f;
	float destY = 0.0f;
	float destW = 0.0f;
	float destH = 0.0f;
	float srcX = 0.0f;
	float srcY = 0.0f;
	float srcW = 0.0f;
	float srcH = 0.0f;
	unsigned color = 0xffffffff;
};

bool QueueTextQuads(SDL_GPUDevice* dev, ::gxCanvas* target, ::gxCanvas* atlas, bool smooth, unsigned canvasW, unsigned canvasH, const TextQuad* quads, unsigned count);
bool QueueTextSolid(SDL_GPUDevice* dev, ::gxCanvas* target, unsigned canvasW, unsigned canvasH, float dx, float dy, float dw, float dh, unsigned color);
bool QueueRectFilled(SDL_GPUDevice* dev, ::gxCanvas* target, unsigned canvasW, unsigned canvasH, float x, float y, float w, float h, unsigned color);
bool QueueRectOutline(SDL_GPUDevice* dev, ::gxCanvas* target, unsigned canvasW, unsigned canvasH, float x, float y, float w, float h, unsigned color);
bool QueueSpriteQuad(SDL_GPUDevice* dev, ::gxCanvas* target, SDL_GPUTexture* tex, bool smooth, unsigned canvasW, unsigned canvasH, unsigned texW, unsigned texH, const TextQuad* quad);
bool HasPendingText();
bool PreparePendingText(SDL_GPUDevice* dev, SDL_GPUCommandBuffer* cmds);
void DrawPendingText(SDL_GPUDevice* dev, SDL_Window* win, SDL_GPURenderPass* pass);
bool FlushPendingTextToCanvas(SDL_GPUDevice* dev, ::gxCanvas* canvas);
bool FlushPendingTextTargets(SDL_GPUDevice* dev);
void SetActiveCanvasTarget(SDL_GPUDevice* dev, ::gxCanvas* canvas);
bool IsActiveCanvasTarget(::gxCanvas* canvas);
void ClearPendingText();
void InvalidateTextAtlas(::gxCanvas* atlas);
void InvalidatePendingTexture(SDL_GPUTexture* tex);
void TeardownText();

}

#endif
