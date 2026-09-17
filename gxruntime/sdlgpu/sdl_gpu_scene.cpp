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
	if (frame.colorTarget) { SDL_ReleaseGPUTexture(relDev, frame.colorTarget); frame.colorTarget = nullptr; }
	if (frame.msaaColor) { SDL_ReleaseGPUTexture(relDev, frame.msaaColor); frame.msaaColor = nullptr; }
	if (frame.ownedDepth) { SDL_ReleaseGPUTexture(relDev, frame.ownedDepth); frame.ownedDepth = nullptr; }
	frame.depthTarget = nullptr;
	frame.colorW = frame.colorH = 0;
	frame.colorFormat = 0;
	frame.colorSamples = 1;
	frame.depthW = frame.depthH = 0;
	frame.depthFormat = 0;
	frame.depthSamples = 1;
	frame.sampleCount = 1;
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
	frame.targetW = frame.targetH = 0;
	frame.skipped = false;
	frame.drew3D = false;
}

bool BeginSceneFrame(GpuSceneFrame& frame, SDL_GPUDevice* dev, SDL_Window* win, unsigned targetW, unsigned targetH, bool antialias) {
	if (!dev || !win) return false;

	if (frame.cmds && frame.dev != dev) {
		EndSceneFrame(frame);
		SDL_GPUCommandBuffer* stale = frame.cmds;
		frame.cmds = nullptr;
		SDL_CancelGPUCommandBuffer(stale);
	}

	frame.dev = dev;
	frame.antialias = antialias;
	frame.skipped = false;
	if (frame.cmds) {
		EndSceneFrame(frame);
		if (targetW > frame.targetW) frame.targetW = targetW;
		if (targetH > frame.targetH) frame.targetH = targetH;
		return true;
	}

	frame.drew3D = false;
	frame.targetW = targetW ? targetW : 1;
	frame.targetH = targetH ? targetH : 1;
	frame.cmds = SDL_AcquireGPUCommandBuffer(dev);
	if (!frame.cmds) {
		frame.skipped = true;
		return false;
	}
	return true;
}

static bool EnsureTargets(GpuSceneFrame& frame) {
	if (!frame.dev || !frame.targetW || !frame.targetH) return false;
	int colorFmt = SceneColorFormat();
	int fmt = MeshDepthFormat(frame.dev);
	bool extDepth = frame.externalDepth && frame.externalDepthW == frame.targetW && frame.externalDepthH == frame.targetH;
	int want = 1;
	if (frame.antialias && !extDepth) {
		if (SDL_GPUTextureSupportsSampleCount(frame.dev, (SDL_GPUTextureFormat)colorFmt, SDL_GPU_SAMPLECOUNT_4)) want = 4;
		else if (SDL_GPUTextureSupportsSampleCount(frame.dev, (SDL_GPUTextureFormat)colorFmt, SDL_GPU_SAMPLECOUNT_2)) want = 2;
	}
	if (!frame.colorTarget || frame.colorW != frame.targetW || frame.colorH != frame.targetH || frame.colorFormat != colorFmt || frame.colorSamples != want) {
		if (frame.colorTarget) { SDL_ReleaseGPUTexture(frame.dev, frame.colorTarget); frame.colorTarget = nullptr; }
		if (frame.msaaColor) { SDL_ReleaseGPUTexture(frame.dev, frame.msaaColor); frame.msaaColor = nullptr; }
		if (frame.ownedDepth) { SDL_ReleaseGPUTexture(frame.dev, frame.ownedDepth); frame.ownedDepth = nullptr; }
		frame.depthTarget = nullptr;
		frame.depthW = frame.depthH = 0;
		frame.depthFormat = 0;
		frame.depthSamples = 1;
		frame.colorTarget = CreateColorTarget(frame.dev, frame.targetW, frame.targetH,
			frame.colorClearR, frame.colorClearG, frame.colorClearB, 1.0f);
		if (!frame.colorTarget) return false;
		frame.msaaColor = (want > 1) ? CreateColorTargetMS(frame.dev, frame.targetW, frame.targetH, want) : nullptr;
		frame.colorW = frame.targetW;
		frame.colorH = frame.targetH;
		frame.colorFormat = colorFmt;
		frame.colorSamples = frame.msaaColor ? want : 1;
	}
	frame.sampleCount = frame.colorSamples;
	if (frame.externalDepth && frame.externalDepthW == frame.colorW && frame.externalDepthH == frame.colorH) {
		frame.depthTarget = frame.externalDepth;
	}
	else {
		if (!frame.ownedDepth || frame.depthW != frame.colorW || frame.depthH != frame.colorH || frame.depthFormat != fmt || frame.depthSamples != frame.colorSamples) {
			if (frame.ownedDepth) { SDL_ReleaseGPUTexture(frame.dev, frame.ownedDepth); frame.ownedDepth = nullptr; }
			int ds = frame.colorSamples;
			SDL_GPUTexture* depth = (ds > 1) ? CreateDepthTarget(frame.dev, frame.colorW, frame.colorH, fmt, 1.0f, 0, ds) : nullptr;
			if (!depth) {
				depth = CreateDepthTarget(frame.dev, frame.colorW, frame.colorH, fmt, 1.0f, 0, 1);
				if (!depth) return false;
				if (frame.msaaColor) { SDL_ReleaseGPUTexture(frame.dev, frame.msaaColor); frame.msaaColor = nullptr; }
				frame.colorSamples = 1;
				frame.sampleCount = 1;
			}
			frame.ownedDepth = depth;
			frame.depthFormat = fmt;
			frame.depthSamples = frame.colorSamples;
		}
		frame.depthTarget = frame.ownedDepth;
	}
	frame.depthW = frame.colorW;
	frame.depthH = frame.colorH;
	return true;
}

void SetSceneViewport(GpuSceneFrame& frame, int vpX, int vpY, int vpW, int vpH) {
	if (!frame.pass) return;
	if (vpW <= 0 || vpH <= 0) return;
	frame.vpX = vpX; frame.vpY = vpY; frame.vpW = vpW; frame.vpH = vpH;
	SDL_GPUViewport vp{};
	vp.x = (float)vpX; vp.y = (float)vpY; vp.w = (float)vpW; vp.h = (float)vpH;
	vp.min_depth = 0.0f; vp.max_depth = 1.0f;
	SDL_SetGPUViewport(frame.pass, &vp);
	SDL_Rect sc{};
	sc.x = vpX; sc.y = vpY; sc.w = vpW; sc.h = vpH;
	if (sc.x < 0) sc.x = 0;
	if (sc.y < 0) sc.y = 0;
	if ((Uint32)sc.w > frame.colorW) sc.w = (int)frame.colorW;
	if ((Uint32)sc.h > frame.colorH) sc.h = (int)frame.colorH;
	SDL_SetGPUScissor(frame.pass, &sc);
}

bool BeginScenePass(GpuSceneFrame& frame, int vpX, int vpY, int vpW, int vpH,
	float clearR, float clearG, float clearB, bool clearColor, bool clearDepth) {
	if (!frame.ready()) return false;
	EndSceneFrame(frame);

	bool firstPass = !frame.drew3D;

	if (!EnsureTargets(frame)) return false;

	SDL_GPUColorTargetInfo colorInfo{};
	colorInfo.texture = frame.msaaColor ? frame.msaaColor : frame.colorTarget;
	colorInfo.load_op = (clearColor || firstPass) ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
	colorInfo.clear_color = SDL_FColor{ clearR, clearG, clearB, 1.0f };
	colorInfo.cycle = firstPass;
	if (frame.msaaColor) {
		colorInfo.store_op = SDL_GPU_STOREOP_RESOLVE_AND_STORE;
		colorInfo.resolve_texture = frame.colorTarget;
		colorInfo.resolve_mip_level = 0;
		colorInfo.resolve_layer = 0;
	}
	else {
		colorInfo.store_op = SDL_GPU_STOREOP_STORE;
	}

	SDL_GPUDepthStencilTargetInfo depthInfo{};
	depthInfo.texture = frame.depthTarget;
	depthInfo.load_op = (clearDepth || firstPass) ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
	depthInfo.store_op = SDL_GPU_STOREOP_STORE;
	depthInfo.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
	depthInfo.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
	depthInfo.clear_depth = 1.0f;
	depthInfo.clear_stencil = 0;
	depthInfo.cycle = firstPass;

	frame.pass = SDL_BeginGPURenderPass(frame.cmds, &colorInfo, 1, &depthInfo);
	if (!frame.pass) return false;
	SetSceneViewport(frame, vpX, vpY, vpW, vpH);
	frame.drew3D = true;
	return true;
}

void RenderSceneMesh(GpuSceneFrame& frame, GpuMesh* mesh, const MeshUniforms& uniforms, int first_vert, int vert_cnt, int first_tri, int tri_cnt, const MeshDrawParams& p) {
	if (!frame.active() || !frame.cmds || !frame.dev || !mesh || tri_cnt <= 0 || vert_cnt <= 0) return;
	if (first_vert < 0 || first_tri < 0) return;
	if ((unsigned)first_vert + (unsigned)vert_cnt > mesh->maxVerts) return;
	if ((unsigned)first_tri + (unsigned)tri_cnt > mesh->maxTris) return;

	unsigned indexCount = (unsigned)tri_cnt * 3;
	unsigned startIndex = (unsigned)first_tri * 3;
	DrawMesh(frame.dev, nullptr, frame.cmds, frame.pass, mesh, (const float*)&uniforms, (unsigned)sizeof(uniforms), indexCount, startIndex, first_vert, SceneColorFormat(), MeshDepthFormat(frame.dev), p, frame.sampleCount);
}

void RenderSceneMeshExtra(GpuSceneFrame& frame, GpuMesh* mesh, const MeshUniforms& uniforms, int first_vert, int vert_cnt, int first_tri, int tri_cnt, const MeshDrawParams& base, const MeshExtraStage* extras, int extraCount) {
	if (!extras || extraCount <= 0 || !mesh || tri_cnt <= 0 || vert_cnt <= 0) return;
	if (first_vert < 0 || first_tri < 0) return;
	if ((unsigned)first_vert + (unsigned)vert_cnt > mesh->maxVerts) return;
	if ((unsigned)first_tri + (unsigned)tri_cnt > mesh->maxTris) return;
	if (!frame.cmds || !frame.dev) return;

	unsigned indexCount = (unsigned)tri_cnt * 3;
	unsigned startIndex = (unsigned)first_tri * 3;
	if (!frame.active()) {
		if (!BeginScenePass(frame, frame.vpX, frame.vpY, frame.vpW, frame.vpH, 0, 0, 0, false, false)) return;
	}
	for (int k = 0; k < extraCount; ++k) {
		if (!extras[k].tex) continue;
		DrawMeshExtraStage(frame.dev, frame.cmds, frame.pass, mesh, (const float*)&uniforms, (unsigned)sizeof(uniforms), indexCount, startIndex, first_vert, SceneColorFormat(), MeshDepthFormat(frame.dev), extras[k], base, frame.sampleCount);
	}
}

void EndSceneFrame(GpuSceneFrame& frame) {
	if (frame.pass) {
		SDL_EndGPURenderPass(frame.pass);
		frame.pass = nullptr;
	}
}

static void SceneSourceRect(const GpuSceneFrame& frame, int& x, int& y, unsigned& w, unsigned& h) {
	x = 0; y = 0; w = frame.colorW; h = frame.colorH;
	if (frame.vpW > 0 && frame.vpH > 0) { x = frame.vpX; y = frame.vpY; w = (unsigned)frame.vpW; h = (unsigned)frame.vpH; }
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if ((unsigned)x >= frame.colorW || (unsigned)y >= frame.colorH) { x = 0; y = 0; w = frame.colorW; h = frame.colorH; return; }
	if ((unsigned)x + w > frame.colorW) w = frame.colorW - x;
	if ((unsigned)y + h > frame.colorH) h = frame.colorH - y;
}

static void BlitSceneToSwap(SDL_GPUCommandBuffer* cmds, SDL_GPUTexture* scene, int sceneX, int sceneY, unsigned sceneW, unsigned sceneH, SDL_GPUTexture* swap, Uint32 swapW, Uint32 swapH) {
	SDL_GPUBlitInfo blit{};
	blit.source.texture = scene;
	blit.source.x = (Uint32)sceneX;
	blit.source.y = (Uint32)sceneY;
	blit.source.w = sceneW;
	blit.source.h = sceneH;
	blit.destination.texture = swap;
	blit.destination.w = swapW;
	blit.destination.h = swapH;
	blit.load_op = SDL_GPU_LOADOP_CLEAR;
	blit.clear_color = SDL_FColor{ 0, 0, 0, 0 };
	blit.flip_mode = SDL_FLIP_NONE;
	blit.filter = SDL_GPU_FILTER_LINEAR;
	blit.cycle = false;
	SDL_BlitGPUTexture(cmds, &blit);
}

static bool AcquireSwap(SDL_GPUDevice* dev, SDL_Window* win, SDL_GPUCommandBuffer* cmds, SDL_GPUTexture** outTex, Uint32* outW, Uint32* outH) {
	SDL_GPUTexture* swap = nullptr;
	Uint32 sw = 0, sh = 0;
	if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmds, win, &swap, &sw, &sh)) {
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "SDL_WaitAndAcquireGPUSwapchainTexture failed: %s", SDL_GetError());
		return false;
	}
	if (outTex) *outTex = swap;
	if (outW) *outW = sw;
	if (outH) *outH = sh;
	return true;
}

bool PresentSceneFrame(SDL_GPUDevice* dev, SDL_Window* win, GpuSceneFrame& frame) {
	if (!dev || !win) return false;
	FlushPendingTextTargets(dev);
	if (!frame.drew3D || !frame.cmds) {
		ClearPendingText();
		return false;
	}
	EndSceneFrame(frame);

	SDL_GPUCommandBuffer* cmds = frame.cmds;
	frame.cmds = nullptr;

	SDL_GPUTexture* swap = nullptr;
	Uint32 sw = 0, sh = 0;
	if (!AcquireSwap(dev, win, cmds, &swap, &sw, &sh)) {
		SDL_CancelGPUCommandBuffer(cmds);
		ClearPendingText();
		return false;
	}
	if (!swap) {
		if (!SDL_SubmitGPUCommandBuffer(cmds)) SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Submit minimized frame failed: %s", SDL_GetError());
		ClearPendingText();
		return true;
	}
	SDL_GPUTextureFormat swapFmt = SDL_GetGPUSwapchainTextureFormat(dev, win);
	bool gamma = GammaActive(dev);
	SDL_GPUTexture* target = swap;
	if (gamma) {
		target = AcquireGammaComposite(dev, swapFmt, sw, sh);
		if (!target) gamma = false;
	}
	{ int sxx, syy; unsigned sww, shh; SceneSourceRect(frame, sxx, syy, sww, shh); BlitSceneToSwap(cmds, frame.colorTarget, sxx, syy, sww, shh, target, sw, sh); }

	bool textReady = HasPendingText() && PreparePendingText(dev, cmds);
	if (textReady) {
		SDL_GPUColorTargetInfo ci{};
		ci.texture = target;
		ci.load_op = SDL_GPU_LOADOP_LOAD;
		ci.store_op = SDL_GPU_STOREOP_STORE;
		ci.clear_color = SDL_FColor{ 0, 0, 0, 1 };
		ci.cycle = false;
		if (SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmds, &ci, 1, nullptr)) {
			SDL_GPUViewport vp{};
			vp.x = 0; vp.y = 0; vp.w = (float)sw; vp.h = (float)sh;
			vp.min_depth = 0.0f; vp.max_depth = 1.0f;
			SDL_SetGPUViewport(pass, &vp);
			SDL_Rect sc{};
			sc.x = 0; sc.y = 0; sc.w = (int)sw; sc.h = (int)sh;
			SDL_SetGPUScissor(pass, &sc);
			DrawPendingText(dev, win, pass);
			SDL_EndGPURenderPass(pass);
		}
	}
	ClearPendingText();

	if (gamma && !GammaBlit(dev, cmds, target, swap, sw, sh, swapFmt)) {
		SDL_GPUBlitInfo info{};
		info.source.texture = target;
		info.source.w = sw; info.source.h = sh;
		info.destination.texture = swap;
		info.destination.w = sw; info.destination.h = sh;
		info.load_op = SDL_GPU_LOADOP_DONT_CARE;
		info.flip_mode = SDL_FLIP_NONE;
		info.filter = SDL_GPU_FILTER_NEAREST;
		info.cycle = false;
		SDL_BlitGPUTexture(cmds, &info);
	}

	return SDL_SubmitGPUCommandBuffer(cmds);
}

bool PresentSceneWithCanvas(SDL_GPUDevice* dev, SDL_Window* win, GpuSceneFrame& frame, ::gxCanvas* canvas) {
	if (!dev || !win) return false;
	FlushPendingTextTargets(dev);
	bool has3D = frame.drew3D && frame.cmds;
	if (!has3D && !canvas) {
		ClearPendingText();
		return false;
	}
	if (frame.pass) {
		EndSceneFrame(frame);
	}

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
	SDL_GPUTexture* swap = nullptr;
	Uint32 sw = 0, sh = 0;
	if (!AcquireSwap(dev, win, cmds, &swap, &sw, &sh)) {
		SDL_CancelGPUCommandBuffer(cmds);
		ClearPendingText();
		return false;
	}
	if (!swap) {
		if (!SDL_SubmitGPUCommandBuffer(cmds)) SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Submit minimized frame failed: %s", SDL_GetError());
		ClearPendingText();
		return true;
	}

	SDL_GPUTextureFormat swapFmt = SDL_GetGPUSwapchainTextureFormat(dev, win);
	bool gamma = GammaActive(dev);
	SDL_GPUTexture* target = swap;
	if (gamma) {
		target = AcquireGammaComposite(dev, swapFmt, sw, sh);
		if (!target) gamma = false;
	}

	if (has3D) { int sxx, syy; unsigned sww, shh; SceneSourceRect(frame, sxx, syy, sww, shh); BlitSceneToSwap(cmds, frame.colorTarget, sxx, syy, sww, shh, target, sw, sh); }

	SDL_GPUTexture* canvasTex = canvas ? GetCanvasOverlayTextureBatched(dev, canvas, cmds) : nullptr;
	bool haveText = HasPendingText();
	bool textReady = haveText && PreparePendingText(dev, cmds);
	if (canvasTex || textReady || !has3D) {
		SDL_GPUColorTargetInfo ci{};
		ci.texture = target;
		ci.load_op = has3D ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
		ci.store_op = SDL_GPU_STOREOP_STORE;
		ci.clear_color = SDL_FColor{ 0, 0, 0, 0 };
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

	if (gamma && !GammaBlit(dev, cmds, target, swap, sw, sh, swapFmt)) {
		SDL_GPUBlitInfo info{};
		info.source.texture = target;
		info.source.w = sw; info.source.h = sh;
		info.destination.texture = swap;
		info.destination.w = sw; info.destination.h = sh;
		info.load_op = SDL_GPU_LOADOP_DONT_CARE;
		info.flip_mode = SDL_FLIP_NONE;
		info.filter = SDL_GPU_FILTER_NEAREST;
		info.cycle = false;
		SDL_BlitGPUTexture(cmds, &info);
	}

	return SDL_SubmitGPUCommandBuffer(cmds);
}

bool BlitFrameToCanvas(SDL_GPUDevice* dev, GpuSceneFrame& frame, ::gxCanvas* dest, int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh) {
	if (!dev || !dest || !frame.cmds || !frame.colorTarget) return false;
	if (dx < 0) { sx -= dx; dw += dx; dx = 0; }
	if (dy < 0) { sy -= dy; dh += dy; dy = 0; }
	if (sx < 0) { dx -= sx; dw += sx; sx = 0; }
	if (sy < 0) { dy -= sy; dh += sy; sy = 0; }
	if (sx >= (int)frame.colorW) return false;
	if (sy >= (int)frame.colorH) return false;
	if (sx + sw > (int)frame.colorW) sw = (int)frame.colorW - sx;
	if (sy + sh > (int)frame.colorH) sh = (int)frame.colorH - sy;
	if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return false;
	EndSceneFrame(frame);
	bool cube = (dest->getFlags() & ::gxCanvas::CANVAS_TEX_CUBE) != 0;
	SDL_GPUTexture* destTex = cube ? EnsureCanvasCubeTexture(dev, dest) : EnsureCanvasRenderTarget(dev, dest);
	if (!destTex) return false;
	static const Uint32 kCubeLayer[6] = {
		SDL_GPU_CUBEMAPFACE_NEGATIVEX,
		SDL_GPU_CUBEMAPFACE_POSITIVEZ,
		SDL_GPU_CUBEMAPFACE_POSITIVEX,
		SDL_GPU_CUBEMAPFACE_NEGATIVEZ,
		SDL_GPU_CUBEMAPFACE_POSITIVEY,
		SDL_GPU_CUBEMAPFACE_NEGATIVEY
	};
	int dstFace = dest->getCubeFace();
	if (dstFace < 0 || dstFace > 5) dstFace = 0;
	SDL_GPUBlitInfo info{};
	info.source.texture = frame.colorTarget;
	info.source.mip_level = 0;
	info.source.layer_or_depth_plane = 0;
	info.source.x = (Uint32)sx;
	info.source.y = (Uint32)sy;
	info.source.w = (Uint32)sw;
	info.source.h = (Uint32)sh;
	info.destination.texture = destTex;
	info.destination.mip_level = 0;
	info.destination.layer_or_depth_plane = cube ? kCubeLayer[dstFace] : 0;
	info.destination.x = (Uint32)dx;
	info.destination.y = (Uint32)dy;
	info.destination.w = (Uint32)dw;
	info.destination.h = (Uint32)dh;
	info.load_op = SDL_GPU_LOADOP_LOAD;
	info.clear_color = SDL_FColor{ 0, 0, 0, 1 };
	info.flip_mode = SDL_FLIP_NONE;
	info.filter = SDL_GPU_FILTER_LINEAR;
	info.cycle = false;
	SDL_BlitGPUTexture(frame.cmds, &info);
	if (cube && (dest->getFlags() & ::gxCanvas::CANVAS_TEX_MIPMAP) && dest->getWidth() > 1) SDL_GenerateMipmapsForGPUTexture(frame.cmds, destTex);
	dest->releaseCPUBitsIfUnused();
	return true;
}

}
