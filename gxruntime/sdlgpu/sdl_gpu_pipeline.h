#ifndef SDL_GPU_PIPELINE_H
#define SDL_GPU_PIPELINE_H

#include <SDL3/SDL_gpu.h>

struct SDL_GPUDevice;
struct SDL_Window;
struct SDL_GPURenderPass;
struct SDL_GPUCommandBuffer;
struct SDL_GPUTexture;
struct SDL_GPUTransferBuffer;

namespace sdlgpu {

struct GpuMesh;

enum { MESH_BLEND_REPLACE = 0, MESH_BLEND_ALPHA = 1, MESH_BLEND_MULTIPLY = 2, MESH_BLEND_ADD = 3 };
enum { MESH_Z_NORMAL = 0, MESH_Z_DISABLE = 1, MESH_Z_CMPONLY = 2 };

	bool PresentBlit(SDL_GPUDevice* dev, SDL_Window* win, float r, float g, float b, unsigned w, unsigned h, const void* px);
	void DrawMesh(SDL_GPUDevice* dev, SDL_Window* win, SDL_GPUCommandBuffer* cmds, SDL_GPURenderPass* pass, GpuMesh* mesh, const float* uniforms, unsigned uniformBytes, SDL_GPUTexture* tex, unsigned indexCount, unsigned startIndex, int firstVertex, int colorFormat, int depthFormat, int blendMode, int zMode, SDL_GPUCullMode cullMode);
	void DrawCanvasOverlay(SDL_GPUDevice* dev, SDL_Window* win, SDL_GPURenderPass* pass, SDL_GPUTexture* tex);
	int MeshDepthFormat(SDL_GPUDevice* dev);
	int SceneColorFormat();
	SDL_GPUTransferBuffer* AcquireUploadTransferBuffer(SDL_GPUDevice* dev, Uint32 size);
	void ReleaseUploadTransferBuffer(SDL_GPUDevice* dev, SDL_GPUTransferBuffer* buf);
	void ClearTransferPool(SDL_GPUDevice* dev);

	void TeardownPipelines();

}

#endif
