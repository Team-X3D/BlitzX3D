#ifndef SDL_GPU_SCENE_H
#define SDL_GPU_SCENE_H

struct SDL_GPUDevice;
struct SDL_GPUCommandBuffer;
struct SDL_GPURenderPass;
struct SDL_GPUTexture;
struct SDL_Window;

#include <stdint.h>

class gxCanvas;

namespace sdlgpu {

struct GpuMesh;

static constexpr int kGpuMaxLights = 8;

struct MeshUniforms {
	float mvp[16];
	float world[16];
	float ambient[4];
	float matDiffuse[4];
	float matAmbient[4];
	float matEmissive[4];
	float matSpec[4];
	float matSrc[4];
	float fogColor[4];
	float fogParams[4];
	float eyePos[4];
	float flags[4];
	int lightCount;
	float lightPad[3];
	float lightPos[kGpuMaxLights][4];
	float lightColor[kGpuMaxLights][4];
	float lightSpec[kGpuMaxLights][4];
	float lightAmb[kGpuMaxLights][4];
	float lightAtten[kGpuMaxLights][4];
	float lightSpotDir[kGpuMaxLights][4];
	float lightSpotPrm[kGpuMaxLights][4];
};

struct GpuSceneFrame {
	SDL_GPUDevice* dev = nullptr;
	SDL_GPUCommandBuffer* cmds = nullptr;
	SDL_GPURenderPass* pass = nullptr;

	SDL_GPUTexture* swap = nullptr;
	uint32_t swapW = 0, swapH = 0;

	SDL_GPUTexture* depthTarget = nullptr;
	unsigned depthW = 0, depthH = 0;
	int depthFormat = 0;

	bool skipped = false;
	bool drew3D = false;

	bool active() const { return pass != nullptr; }
	bool ready() const { return cmds != nullptr && swap != nullptr && !skipped; }
};

bool BeginSceneFrame(GpuSceneFrame& frame, SDL_GPUDevice* dev, SDL_Window* win);
bool BeginScenePass(GpuSceneFrame& frame, int vpX, int vpY, int vpW, int vpH,
	float clearR, float clearG, float clearB, bool clearColor, bool clearDepth);
void SetSceneViewport(GpuSceneFrame& frame, int vpX, int vpY, int vpW, int vpH);
void RenderSceneMesh(GpuSceneFrame& frame, GpuMesh* mesh, const MeshUniforms& uniforms, SDL_GPUTexture* tex, int first_vert, int vert_cnt, int first_tri, int tri_cnt, bool alphaBlend, int cullMode);
void EndSceneFrame(GpuSceneFrame& frame);
bool PresentSceneFrame(SDL_GPUDevice* dev, SDL_Window* win, GpuSceneFrame& frame);
bool PresentSceneWithCanvas(SDL_GPUDevice* dev, SDL_Window* win, GpuSceneFrame& frame, ::gxCanvas* canvas);
void ReleaseSceneTargets(SDL_GPUDevice* dev, GpuSceneFrame& frame);

}

#endif
