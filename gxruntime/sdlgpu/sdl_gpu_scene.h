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
struct MeshDrawParams;
struct MeshExtraStage;

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
	float texGen[4];
	float viewX[4];
	float viewY[4];
	float viewZ[4];
	float cubeParams[4];
};

struct GpuSceneFrame {
	SDL_GPUDevice* dev = nullptr;
	SDL_GPUCommandBuffer* cmds = nullptr;
	SDL_GPURenderPass* pass = nullptr;

	unsigned targetW = 0, targetH = 0;
	unsigned displayW = 0, displayH = 0;

	SDL_GPUTexture* colorTarget = nullptr;
	SDL_GPUTexture* msaaColor = nullptr;
	unsigned colorW = 0, colorH = 0;
	int colorFormat = 0;
	int colorSamples = 1;
	float colorClearR = 0.0f, colorClearG = 0.0f, colorClearB = 0.0f;
	float createdClearR = 0.0f, createdClearG = 0.0f, createdClearB = 0.0f;
	bool createdClearValid = false;

	SDL_GPUTexture* depthTarget = nullptr;
	SDL_GPUTexture* ownedDepth = nullptr;
	unsigned depthW = 0, depthH = 0;
	int depthFormat = 0;
	int depthSamples = 1;

	bool antialias = false;
	int sampleCount = 1;

	SDL_GPUTexture* externalDepth = nullptr;
	unsigned externalDepthW = 0, externalDepthH = 0;

	int vpX = 0, vpY = 0, vpW = 0, vpH = 0;

	bool skipped = false;
	bool drew3D = false;

	bool active() const { return pass != nullptr; }
	bool ready() const { return cmds != nullptr && !skipped; }
};

bool BeginSceneFrame(GpuSceneFrame& frame, SDL_GPUDevice* dev, SDL_Window* win, unsigned targetW, unsigned targetH, unsigned displayW, unsigned displayH, bool antialias);
bool BeginScenePass(GpuSceneFrame& frame, int vpX, int vpY, int vpW, int vpH,
	float clearR, float clearG, float clearB, bool clearColor, bool clearDepth);
void SetSceneViewport(GpuSceneFrame& frame, int vpX, int vpY, int vpW, int vpH);
void RenderSceneMesh(GpuSceneFrame& frame, GpuMesh* mesh, const MeshUniforms& uniforms, int first_vert, int vert_cnt, int first_tri, int tri_cnt, const struct MeshDrawParams& p);
void RenderSceneMeshExtra(GpuSceneFrame& frame, GpuMesh* mesh, const MeshUniforms& uniforms, int first_vert, int vert_cnt, int first_tri, int tri_cnt, const struct MeshDrawParams& base, const MeshExtraStage* extras, int extraCount);
void EndSceneFrame(GpuSceneFrame& frame);
bool PresentSceneFrame(SDL_GPUDevice* dev, SDL_Window* win, GpuSceneFrame& frame);
bool PresentSceneWithCanvas(SDL_GPUDevice* dev, SDL_Window* win, GpuSceneFrame& frame, ::gxCanvas* canvas);
bool BlitFrameToCanvas(SDL_GPUDevice* dev, GpuSceneFrame& frame, ::gxCanvas* dest, int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh);
void ReleaseSceneTargets(SDL_GPUDevice* dev, GpuSceneFrame& frame);

}

#endif
