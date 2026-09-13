#ifndef SDL_GPU_MESH_H
#define SDL_GPU_MESH_H

struct SDL_GPUDevice;
struct SDL_GPUBuffer;

namespace sdlgpu {

struct GpuMesh {
	SDL_GPUBuffer* verts = nullptr;
	SDL_GPUBuffer* indices = nullptr;
	unsigned vertStride = 0;
	unsigned maxVerts = 0;
	unsigned maxTris = 0;
};

GpuMesh* CreateMesh(SDL_GPUDevice* dev, unsigned vertStride, unsigned maxVerts, unsigned maxTris);
bool UploadMesh(SDL_GPUDevice* dev, GpuMesh* mesh, const void* vertData, unsigned vertBytes, const void* idxData, unsigned idxBytes);
bool UploadMeshRange(SDL_GPUDevice* dev, GpuMesh* mesh, const void* vertBase, unsigned vertDstOff, unsigned vertBytes, const void* idxBase, unsigned idxDstOff, unsigned idxBytes);
void ReleaseMesh(SDL_GPUDevice* dev, GpuMesh* mesh);

static constexpr unsigned kMaxBones = 64; // don't really think we need these anymore...
static constexpr unsigned kBoneFloat4s = kMaxBones * 3;
SDL_GPUBuffer* EnsureBoneBuffer(SDL_GPUDevice* dev);
bool UploadBones(SDL_GPUDevice* dev, const float* boneData, unsigned boneCount);
void ReleaseBones(SDL_GPUDevice* dev);

}

#endif
