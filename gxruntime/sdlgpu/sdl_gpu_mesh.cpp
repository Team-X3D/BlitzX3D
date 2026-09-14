#include "sdl_gpu_mesh.h"
#include "sdl_gpu_lock.h"
#include "sdl_gpu_pipeline.h"

#include "../std.h"

#include <cstdio>
#include <cstring>

#include <SDL3/SDL_gpu.h>

namespace sdlgpu {

GpuMesh* CreateMesh(SDL_GPUDevice* dev, unsigned vertStride, unsigned maxVerts, unsigned maxTris) {
	if (!dev || !vertStride || !maxVerts || !maxTris) return nullptr;
	GpuMesh* mesh = new GpuMesh;
	mesh->vertStride = vertStride;
	mesh->maxVerts = maxVerts;
	mesh->maxTris = maxTris;

	SDL_GPUBufferCreateInfo vertInfo{};
	vertInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
	vertInfo.size = vertStride * maxVerts;
	mesh->verts = SDL_CreateGPUBuffer(dev, &vertInfo);

	SDL_GPUBufferCreateInfo idxInfo{};
	idxInfo.usage = SDL_GPU_BUFFERUSAGE_INDEX;
	idxInfo.size = (unsigned)sizeof(unsigned short) * maxTris * 3;
	mesh->indices = SDL_CreateGPUBuffer(dev, &idxInfo);

	if (!mesh->verts || !mesh->indices) {
		ReleaseMesh(dev, mesh);
		return nullptr;
	}
	return mesh;
}

static bool UploadToBuffer(SDL_GPUDevice* dev, SDL_GPUBuffer* dst, unsigned dstOff, const void* data, unsigned bytes) {
	SDL_GPUTransferBuffer* buf = AcquireUploadTransferBuffer(dev, bytes);
	if (!buf) return false;
	void* mapped = SDL_MapGPUTransferBuffer(dev, buf, true);
	if (!mapped) { ReleaseUploadTransferBuffer(dev, buf); return false; }
	memcpy(mapped, data, bytes);
	SDL_UnmapGPUTransferBuffer(dev, buf);

	SDL_GPUCommandBuffer* cmds = SDL_AcquireGPUCommandBuffer(dev);
	if (!cmds) { ReleaseUploadTransferBuffer(dev, buf); return false; }
	SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(cmds);
	SDL_GPUTransferBufferLocation src{};
	src.transfer_buffer = buf;
	SDL_GPUBufferRegion region{};
	region.buffer = dst;
	region.offset = dstOff;
	region.size = bytes;
	SDL_UploadToGPUBuffer(pass, &src, &region, true);
	SDL_EndGPUCopyPass(pass);
	bool ok = SDL_SubmitGPUCommandBuffer(cmds);
	ReleaseUploadTransferBuffer(dev, buf);
	return ok;
}

bool UploadMeshRange(SDL_GPUDevice* dev, GpuMesh* mesh, const void* vertBase, unsigned vertDstOff, unsigned vertBytes, const void* idxBase, unsigned idxDstOff, unsigned idxBytes) {
	if (!dev || !mesh || !mesh->verts || !mesh->indices) return false;
	if (!vertBytes && !idxBytes) return true;
	if (vertBytes) {
		if (!vertBase) return false;
		if (vertDstOff + vertBytes > mesh->vertStride * mesh->maxVerts) return false;
	}
	if (idxBytes) {
		if (!idxBase) return false;
		if (idxDstOff + idxBytes > (unsigned)sizeof(unsigned short) * mesh->maxTris * 3) return false;
	}
	if (!vertBytes) return UploadToBuffer(dev, mesh->indices, idxDstOff, idxBase, idxBytes);
	if (!idxBytes) return UploadToBuffer(dev, mesh->verts, vertDstOff, vertBase, vertBytes);
	unsigned alignedVertBytes = (vertBytes + 511u) & ~511u;
	unsigned total = alignedVertBytes + idxBytes;
	SDL_GPUTransferBuffer* buf = AcquireUploadTransferBuffer(dev, total);
	if (!buf) return UploadToBuffer(dev, mesh->verts, vertDstOff, vertBase, vertBytes) && UploadToBuffer(dev, mesh->indices, idxDstOff, idxBase, idxBytes);
	void* mapped = SDL_MapGPUTransferBuffer(dev, buf, true);
	if (!mapped) { ReleaseUploadTransferBuffer(dev, buf); return false; }
	memcpy(mapped, vertBase, vertBytes);
	memcpy((char*)mapped + alignedVertBytes, idxBase, idxBytes);
	SDL_UnmapGPUTransferBuffer(dev, buf);
	SDL_GPUCommandBuffer* cmds = SDL_AcquireGPUCommandBuffer(dev);
	if (!cmds) { ReleaseUploadTransferBuffer(dev, buf); return false; }
	SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(cmds);
	SDL_GPUTransferBufferLocation src{};
	src.transfer_buffer = buf;
	SDL_GPUBufferRegion vr{}; vr.buffer = mesh->verts; vr.offset = vertDstOff; vr.size = vertBytes;
	src.offset = 0;
	SDL_UploadToGPUBuffer(pass, &src, &vr, true);
	SDL_GPUBufferRegion ir{}; ir.buffer = mesh->indices; ir.offset = idxDstOff; ir.size = idxBytes;
	src.offset = alignedVertBytes;
	SDL_UploadToGPUBuffer(pass, &src, &ir, true);
	SDL_EndGPUCopyPass(pass);
	bool ok = SDL_SubmitGPUCommandBuffer(cmds);
	ReleaseUploadTransferBuffer(dev, buf);
	return ok;
}

bool UploadMesh(SDL_GPUDevice* dev, GpuMesh* mesh, const void* vertData, unsigned vertBytes, const void* idxData, unsigned idxBytes) {
	return UploadMeshRange(dev, mesh, vertData, 0, vertBytes, idxData, 0, idxBytes);
}

void ReleaseMesh(SDL_GPUDevice* dev, GpuMesh* mesh) {
	if (!mesh) return;
	if (dev) {
		if (mesh->verts) SDL_ReleaseGPUBuffer(dev, mesh->verts);
		if (mesh->indices) SDL_ReleaseGPUBuffer(dev, mesh->indices);
	}
	delete mesh;
}

namespace {
	SDL_GPUDevice* g_boneDev = nullptr;
	SDL_GPUBuffer* g_boneBuf = nullptr;
}

SDL_GPUBuffer* EnsureBoneBuffer(SDL_GPUDevice* dev) {
	GpuLock lock;
	if (g_boneBuf && g_boneDev == dev) return g_boneBuf;
	ReleaseBones(g_boneDev);
	if (!dev) return nullptr;
	SDL_GPUBufferCreateInfo info{};
	info.usage = (SDL_GPUBufferUsageFlags)SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
	info.size = kBoneFloat4s * (unsigned)sizeof(float) * 4u;
	SDL_GPUBuffer* buf = SDL_CreateGPUBuffer(dev, &info);
	if (!buf) return nullptr;
	g_boneDev = dev;
	g_boneBuf = buf;
	return g_boneBuf;
}

bool UploadBonesBatched(SDL_GPUDevice* dev, SDL_GPUCommandBuffer* cmds, const float* boneData, unsigned boneCount) {
	GpuLock lock;
	if (!dev || !cmds || !boneData || !boneCount) return false;
	if (boneCount > kMaxBones) boneCount = kMaxBones;
	SDL_GPUBuffer* dst = EnsureBoneBuffer(dev);
	if (!dst) return false;
	unsigned bytes = boneCount * 3u * 4u * (unsigned)sizeof(float);
	SDL_GPUTransferBuffer* buf = AcquireUploadTransferBuffer(dev, bytes);
	if (!buf) return false;
	void* mapped = SDL_MapGPUTransferBuffer(dev, buf, true);
	if (!mapped) { ReleaseUploadTransferBuffer(dev, buf); return false; }
	memcpy(mapped, boneData, bytes);
	SDL_UnmapGPUTransferBuffer(dev, buf);
	SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(cmds);
	if (!pass) { ReleaseUploadTransferBuffer(dev, buf); return false; }
	SDL_GPUTransferBufferLocation src{};
	src.transfer_buffer = buf;
	SDL_GPUBufferRegion region{};
	region.buffer = dst;
	region.offset = 0;
	region.size = bytes;
	SDL_UploadToGPUBuffer(pass, &src, &region, true);
	SDL_EndGPUCopyPass(pass);
	ReleaseUploadTransferBuffer(dev, buf);
	return true;
}

bool UploadBones(SDL_GPUDevice* dev, const float* boneData, unsigned boneCount) {
	GpuLock lock;
	if (!dev || !boneData || !boneCount) return false;
	if (boneCount > kMaxBones) boneCount = kMaxBones;
	if (!EnsureBoneBuffer(dev)) return false;
	unsigned bytes = boneCount * 3u * 4u * (unsigned)sizeof(float);
	SDL_GPUTransferBuffer* buf = AcquireUploadTransferBuffer(dev, bytes);
	if (!buf) return false;
	void* mapped = SDL_MapGPUTransferBuffer(dev, buf, true);
	if (!mapped) { ReleaseUploadTransferBuffer(dev, buf); return false; }
	memcpy(mapped, boneData, bytes);
	SDL_UnmapGPUTransferBuffer(dev, buf);
	SDL_GPUCommandBuffer* cmds = SDL_AcquireGPUCommandBuffer(dev);
	if (!cmds) { ReleaseUploadTransferBuffer(dev, buf); return false; }
	SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(cmds);
	SDL_GPUTransferBufferLocation src{};
	src.transfer_buffer = buf;
	SDL_GPUBufferRegion region{};
	region.buffer = EnsureBoneBuffer(dev);
	region.offset = 0;
	region.size = bytes;
	SDL_UploadToGPUBuffer(pass, &src, &region, true);
	SDL_EndGPUCopyPass(pass);
	bool ok = SDL_SubmitGPUCommandBuffer(cmds);
	ReleaseUploadTransferBuffer(dev, buf);
	return ok;
}

void ReleaseBones(SDL_GPUDevice* dev) {
	GpuLock lock;
	if (g_boneBuf && (!dev || g_boneDev == dev)) {
		SDL_ReleaseGPUBuffer(g_boneDev, g_boneBuf);
		g_boneBuf = nullptr;
		g_boneDev = nullptr;
	}
}

}
