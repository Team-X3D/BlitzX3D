#include "sdl_gpu_scene.h"
#include "sdl_gpu_mesh.h"
#include "sdl_gpu_pipeline.h"
#include "sdl_gpu_text.h"
#include "sdl_gpu_texture.h"

#include "../std.h"
#include "../gxcanvas.h"

#include <cstdio>
#include <SDL3/SDL_log.h>

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_video.h>

namespace sdlgpu {

static void ReleaseTargetsLocked(SDL_GPUDevice* dev, GpuSceneFrame& frame) {
	SDL_GPUDevice* relDev = frame.dev ? frame.dev : dev;
	if (!relDev) return;
	if (frame.depthTarget) { SDL_ReleaseGPUTexture(relDev, frame.depthTarget); frame.depthTarget = nullptr; }
	frame.depthW = frame.depthH = 0;
	frame.depthFormat = 0;
}

void ReleaseSceneTargets(SDL_GPUDevice* dev, GpuSceneFrame& frame) {
	EndSceneFrame(frame);
	if (frame.cmds) {
		SDL_GPUCommandBuffer* stale = frame.cmds;
		frame.cmds = nullptr;
		SDL_CancelGPUCommandBuffer(stale);
	}
	SDL_GPUDevice* relDev = frame.dev ? frame.dev : dev;
	if (!relDev) return;
	ReleaseTargetsLocked(relDev, frame);
	frame.dev = nullptr;
	frame.swap = nullptr;
	frame.swapW = frame.swapH = 0;
	frame.skipped = false;
	frame.drew3D = false;
}

bool BeginSceneFrame(GpuSceneFrame& frame, SDL_GPUDevice* dev, SDL_Window* win) {
	if (!dev || !win) return false;

	if (frame.cmds) {
		EndSceneFrame(frame);
		SDL_GPUCommandBuffer* stale = frame.cmds;
		frame.cmds = nullptr;
		frame.swap = nullptr;
		SDL_CancelGPUCommandBuffer(stale);
	}

	frame.dev = dev;
	frame.skipped = false;
	frame.drew3D = false;
	frame.swap = nullptr;
	frame.swapW = frame.swapH = 0;

	frame.cmds = SDL_AcquireGPUCommandBuffer(dev);
	if (!frame.cmds) return false;

	SDL_GPUTexture* swap = nullptr;
	Uint32 sw = 0, sh = 0;
	if (!SDL_WaitAndAcquireGPUSwapchainTexture(frame.cmds, win, &swap, &sw, &sh)) {
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "SDL_WaitAndAcquireGPUSwapchainTexture failed: %s", SDL_GetError());
		SDL_CancelGPUCommandBuffer(frame.cmds);
		frame.cmds = nullptr;
		return false;
	}
	if (!swap) {
		if (!SDL_SubmitGPUCommandBuffer(frame.cmds)) SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Submit minimized frame failed: %s", SDL_GetError());
		frame.cmds = nullptr;
		frame.skipped = true;
		return false;
	}
	frame.swap = swap;
	frame.swapW = sw;
	frame.swapH = sh;
	return true;
}

static bool EnsureDepth(GpuSceneFrame& frame) {
	if (!frame.dev || !frame.swapW || !frame.swapH) return false;
	int fmt = MeshDepthFormat(frame.dev);
	if (frame.depthTarget && frame.depthW == frame.swapW && frame.depthH == frame.swapH && frame.depthFormat == fmt)
		return true;
	ReleaseTargetsLocked(frame.dev, frame);
	frame.depthTarget = CreateDepthTarget(frame.dev, frame.swapW, frame.swapH, fmt, 1.0f, 0);
	if (!frame.depthTarget) return false;
	frame.dev = frame.dev;
	frame.depthW = frame.swapW;
	frame.depthH = frame.swapH;
	frame.depthFormat = fmt;
	return true;
}

void SetSceneViewport(GpuSceneFrame& frame, int vpX, int vpY, int vpW, int vpH) {
	if (!frame.pass) return;
	if (vpW <= 0 || vpH <= 0) return;
	SDL_GPUViewport vp{};
	vp.x = (float)vpX; vp.y = (float)vpY; vp.w = (float)vpW; vp.h = (float)vpH;
	vp.min_depth = 0.0f; vp.max_depth = 1.0f;
	SDL_SetGPUViewport(frame.pass, &vp);
	SDL_Rect sc{};
	sc.x = vpX; sc.y = vpY; sc.w = vpW; sc.h = vpH;
	if (sc.x < 0) sc.x = 0;
	if (sc.y < 0) sc.y = 0;
	if ((Uint32)sc.w > frame.swapW) sc.w = (int)frame.swapW;
	if ((Uint32)sc.h > frame.swapH) sc.h = (int)frame.swapH;
	SDL_SetGPUScissor(frame.pass, &sc);
}

bool BeginScenePass(GpuSceneFrame& frame, int vpX, int vpY, int vpW, int vpH,
	float clearR, float clearG, float clearB, bool clearColor, bool clearDepth) {
	if (!frame.ready()) return false;
	EndSceneFrame(frame);
	if (!EnsureDepth(frame)) return false;

	bool firstPass = !frame.drew3D;
	SDL_GPUColorTargetInfo colorInfo{};
	colorInfo.texture = frame.swap;
	colorInfo.load_op = (clearColor || firstPass) ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
	colorInfo.store_op = SDL_GPU_STOREOP_STORE;
	colorInfo.clear_color = SDL_FColor{ clearR, clearG, clearB, 1.0f };
	colorInfo.cycle = false;

	SDL_GPUDepthStencilTargetInfo depthInfo{};
	depthInfo.texture = frame.depthTarget;
	depthInfo.load_op = (clearDepth || firstPass) ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
	depthInfo.store_op = SDL_GPU_STOREOP_STORE;
	depthInfo.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
	depthInfo.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
	depthInfo.clear_depth = 1.0f;
	depthInfo.clear_stencil = 0;
	depthInfo.cycle = false;

	frame.pass = SDL_BeginGPURenderPass(frame.cmds, &colorInfo, 1, &depthInfo);
	if (!frame.pass) return false;
	SetSceneViewport(frame, vpX, vpY, vpW, vpH);
	frame.drew3D = true;
	return true;
}

void RenderSceneMesh(GpuSceneFrame& frame, GpuMesh* mesh, const MeshUniforms& uniforms, SDL_GPUTexture* tex, int first_vert, int vert_cnt, int first_tri, int tri_cnt, int blendMode, int zMode, int cullMode) {
	if (!frame.active() || !frame.cmds || !frame.dev || !mesh || tri_cnt <= 0 || vert_cnt <= 0) return;
	if (first_vert < 0 || first_tri < 0) return;
	if ((unsigned)first_vert + (unsigned)vert_cnt > mesh->maxVerts) return;
	if ((unsigned)first_tri + (unsigned)tri_cnt > mesh->maxTris) return;

	unsigned indexCount = (unsigned)tri_cnt * 3;
	unsigned startIndex = (unsigned)first_tri * 3;
	DrawMesh(frame.dev, nullptr, frame.cmds, frame.pass, mesh, (const float*)&uniforms, (unsigned)sizeof(uniforms), tex, indexCount, startIndex, first_vert, SceneColorFormat(), MeshDepthFormat(frame.dev), blendMode, zMode, (SDL_GPUCullMode)cullMode);
}

void EndSceneFrame(GpuSceneFrame& frame) {
	if (frame.pass) {
		SDL_EndGPURenderPass(frame.pass);
		frame.pass = nullptr;
	}
}

static bool SubmitFrame(GpuSceneFrame& frame) {
	if (frame.pass) return false;
	SDL_GPUCommandBuffer* cmds = frame.cmds;
	frame.cmds = nullptr;
	frame.swap = nullptr;
	if (!cmds) return false;
	return SDL_SubmitGPUCommandBuffer(cmds);
}

bool PresentSceneFrame(SDL_GPUDevice* dev, SDL_Window* win, GpuSceneFrame& frame) {
	if (!dev || !win) return false;
	if (!frame.drew3D || !frame.cmds) {
		ClearPendingText();
		return false;
	}
	(void)win;
	return SubmitFrame(frame);
}

bool PresentSceneWithCanvas(SDL_GPUDevice* dev, SDL_Window* win, GpuSceneFrame& frame, ::gxCanvas* canvas) {
	if (!dev || !win) return false;
	bool has3D = frame.drew3D && frame.cmds;
	if (!has3D && !canvas) {
		ClearPendingText();
		return false;
	}
	if (frame.pass) return false;

	SDL_GPUCommandBuffer* cmds = frame.cmds;
	frame.cmds = nullptr;
	if (!cmds) {
		cmds = SDL_AcquireGPUCommandBuffer(dev);
		if (!cmds) {
			SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "SDL_AcquireGPUCommandBuffer failed: %s", SDL_GetError());
			ClearPendingText();
			return false;
		}
	}
	SDL_GPUTexture* swap = frame.swap;
	frame.swap = nullptr;
	Uint32 sw = frame.swapW, sh = frame.swapH;
	if (!swap) {
		if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmds, win, &swap, &sw, &sh)) {
			SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "SDL_WaitAndAcquireGPUSwapchainTexture failed: %s", SDL_GetError());
			SDL_CancelGPUCommandBuffer(cmds);
			ClearPendingText();
			return false;
		}
		if (!swap) {
			if (!SDL_SubmitGPUCommandBuffer(cmds)) SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Submit minimized frame failed: %s", SDL_GetError());
			ClearPendingText();
			return true;
		}
	}

	SDL_GPUTexture* canvasTex = canvas ? GetCanvasOverlayTextureBatched(dev, canvas, cmds) : nullptr;
	bool haveText = HasPendingText();
	bool textReady = haveText && PreparePendingText(dev, cmds);
	if (canvasTex || textReady || !has3D) {
		SDL_GPUColorTargetInfo ci{};
		ci.texture = swap;
		ci.load_op = has3D ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
		ci.store_op = SDL_GPU_STOREOP_STORE;
		ci.clear_color = SDL_FColor{0,0,0,1};
		ci.cycle = false;
		SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmds, &ci, 1, nullptr);
		if (pass) {
			SDL_GPUViewport vp{};
			vp.x = 0; vp.y = 0; vp.w = (float)sw; vp.h = (float)sh;
			vp.min_depth = 0.0f; vp.max_depth = 1.0f;
			SDL_SetGPUViewport(pass, &vp);
			SDL_Rect sc{};
			sc.x = 0; sc.y = 0; sc.w = (int)sw; sc.h = (int)sh;
			SDL_SetGPUScissor(pass, &sc);
			if (canvasTex) DrawCanvasOverlay(dev, win, pass, canvasTex);
			if (textReady) DrawPendingText(dev, win, pass);
			SDL_EndGPURenderPass(pass);
		}
	}
	if (haveText) ClearPendingText();

	return SDL_SubmitGPUCommandBuffer(cmds);
}

}
