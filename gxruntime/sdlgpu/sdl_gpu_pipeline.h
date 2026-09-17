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
enum { MESH_BLEND_EXTRA_ADD = 4, MESH_BLEND_EXTRA_MUL = 5 };
enum { MESH_Z_NORMAL = 0, MESH_Z_DISABLE = 1, MESH_Z_CMPONLY = 2 };

struct MeshExtraStage {
	SDL_GPUTexture* tex = nullptr;
	int blend = MESH_BLEND_ALPHA;
	bool useUV1 = false;
	float matA[4] = {};
	float matB[4] = {};
	bool wrapU = true, wrapV = true, point = false;
};

	bool PresentBlit(SDL_GPUDevice* dev, SDL_Window* win, float r, float g, float b, unsigned w, unsigned h, const void* px);
	struct MeshDrawParams {
		SDL_GPUTexture* tex = nullptr;
		SDL_GPUTexture* tex1 = nullptr;
		float stage1[4] = {};
		float uvMat0A[4] = {};
		float uvMat0B[4] = {};
		float uvMat1A[4] = {};
		float uvMat1B[4] = {};
		float bumpMat[4] = {};
		SDL_GPUBuffer* boneBuf = nullptr;
		bool wrapU0 = true, wrapV0 = true, point0 = false;
		bool wrapU1 = true, wrapV1 = true, point1 = false;
		bool cube0 = false, cube1 = false;
		int blend = MESH_BLEND_REPLACE;
		int zMode = MESH_Z_NORMAL;
		SDL_GPUCullMode cull = SDL_GPU_CULLMODE_BACK;
		bool wireframe = false;
	};
	void DrawMesh(SDL_GPUDevice* dev, SDL_Window* win, SDL_GPUCommandBuffer* cmds, SDL_GPURenderPass* pass, GpuMesh* mesh, const float* uniforms, unsigned uniformBytes, unsigned indexCount, unsigned startIndex, int firstVertex, int colorFormat, int depthFormat, const MeshDrawParams& p);
	void DrawMeshExtraStage(SDL_GPUDevice* dev, SDL_GPUCommandBuffer* cmds, SDL_GPURenderPass* pass, GpuMesh* mesh, const float* uniforms, unsigned uniformBytes, unsigned indexCount, unsigned startIndex, int firstVertex, int colorFormat, int depthFormat, const MeshExtraStage& stage, const MeshDrawParams& base);
	void DrawCanvasOverlay(SDL_GPUDevice* dev, SDL_Window* win, SDL_GPURenderPass* pass, SDL_GPUTexture* tex);
	int MeshDepthFormat(SDL_GPUDevice* dev);
	int SceneColorFormat();
	SDL_GPUTransferBuffer* AcquireUploadTransferBuffer(SDL_GPUDevice* dev, Uint32 size);
	void ReleaseUploadTransferBuffer(SDL_GPUDevice* dev, SDL_GPUTransferBuffer* buf);
	SDL_GPUTransferBuffer* AcquireDownloadTransferBuffer(SDL_GPUDevice* dev, Uint32 size);
	void ReleaseDownloadTransferBuffer(SDL_GPUDevice* dev, SDL_GPUTransferBuffer* buf);
	void ClearTransferPool(SDL_GPUDevice* dev);

	void TeardownPipelines();

}

#endif
