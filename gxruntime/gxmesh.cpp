#include "std.h"
#include "gxmesh.h"
#include "gxgraphics.h"

#include "gxruntime.h"
#include "sdlgpu/sdl_gpu_mesh.h"
#include "sdlgpu/sdl_gpu_upload.h"

#include <vector>

extern gxRuntime* gx_runtime;

gxMesh::gxMesh(gxGraphics* g, int max_vs, int max_ts, int flags) :
    graphics(g),
    max_verts(max_vs > 0 ? max_vs : 1),
    max_tris(max_ts > 0 ? max_ts : 1),
    mesh_dirty(false),
    skinned((flags & MESH_SKINNED) != 0),
    keep_staging((flags & MESH_DYNAMIC) != 0),
    staging_full(false),
    locked_verts(nullptr), locked_skin_verts(nullptr), locked_indices(nullptr),
    gpu_dirty_vmin(-1), gpu_dirty_vmax(-1), gpu_dirty_tmin(-1), gpu_dirty_tmax(-1),
    gpu_uploaded(false) {
    if (g && g->runtime && g->runtime->sdlGpu) {
        gpuMirror = sdlgpu::CreateMesh(g->runtime->sdlGpu,
            skinned ? sizeof(dxSkinVertex) : sizeof(dxVertex), max_verts, max_tris);
    }
}

gxMesh::~gxMesh() {
    unlock();
    syncGpuUpload();
    if (graphics && graphics->runtime && gpuMirror) {
        sdlgpu::ReleaseMesh(graphics->runtime->sdlGpu, gpuMirror);
        gpuMirror = nullptr;
    }
}

struct MeshUploadJob {
    SDL_GPUDevice* dev = nullptr;
    sdlgpu::GpuMesh* mirror = nullptr;
    std::vector<char> verts;
    std::vector<char> indices;
    unsigned vOff = 0;
    unsigned iOff = 0;
    bool full = false;
};

static bool RecordMeshUpload(void* ctx) {
    MeshUploadJob* job = (MeshUploadJob*)ctx;
    bool ok;
    if (job->full) {
        ok = sdlgpu::UploadMesh(job->dev, job->mirror,
            job->verts.data(), (unsigned)job->verts.size(),
            job->indices.data(), (unsigned)job->indices.size());
    } else {
        ok = sdlgpu::UploadMeshRange(job->dev, job->mirror,
            job->verts.empty() ? nullptr : job->verts.data(), job->vOff, (unsigned)job->verts.size(),
            job->indices.empty() ? nullptr : job->indices.data(), job->iOff, (unsigned)job->indices.size());
    }
    delete job;
    return ok;
}

void gxMesh::syncGpuUpload() {
    if (!gpuUpload) return;
    sdlgpu::UploadHandle* h = gpuUpload;
    gpuUpload = nullptr;
    if (sdlgpu::WaitUpload(h)) {
        gpu_dirty_vmin = gpu_dirty_vmax = gpu_dirty_tmin = gpu_dirty_tmax = -1;
        gpu_uploaded = true;
    } else {
        markGpuFullDirty();
    }
}

sdlgpu::GpuMesh* gxMesh::getGpuMirror() {
    syncGpuUpload();
    return gpuMirror;
}

bool gxMesh::lock(bool all) {
    if (locked_verts || locked_skin_verts || locked_indices) return true;

    size_t vbytes = (size_t)(skinned ? sizeof(dxSkinVertex) : sizeof(dxVertex)) * (size_t)max_verts;
    size_t ibytes = (size_t)max_tris * 3 * sizeof(WORD);
    if (staging_v.size() != vbytes) staging_v.assign(vbytes, 0);
    if (staging_i.size() != ibytes) staging_i.assign(ibytes, 0);

    if (skinned) locked_skin_verts = (dxSkinVertex*)staging_v.data();
    else locked_verts = (dxVertex*)staging_v.data();
    locked_indices = (WORD*)staging_i.data();

    staging_full = all;
    if (all) markGpuFullDirty();
    mesh_dirty = false;
    return true;
}

void gxMesh::unlock() {
    syncGpuUpload();

    const void* verts = skinned ? (const void*)locked_skin_verts : (const void*)locked_verts;
    if (verts && locked_indices && gpuMirror && graphics && graphics->runtime && graphics->runtime->sdlGpu) {
        SDL_GPUDevice* dev = (SDL_GPUDevice*)graphics->runtime->sdlGpu;
        unsigned stride = skinned ? sizeof(dxSkinVertex) : sizeof(dxVertex);
        MeshUploadJob* job = nullptr;
        if (!gpu_uploaded) {
            job = new MeshUploadJob;
            job->dev = dev;
            job->mirror = gpuMirror;
            job->full = true;
            job->verts.assign((const char*)verts, (const char*)verts + (size_t)stride * max_verts);
            job->indices.assign((const char*)locked_indices, (const char*)locked_indices + (size_t)sizeof(WORD) * 3 * max_tris);
        } else if (gpu_dirty_vmin >= 0 || gpu_dirty_tmin >= 0) {
            job = new MeshUploadJob;
            job->dev = dev;
            job->mirror = gpuMirror;
            job->full = false;
            if (gpu_dirty_vmin >= 0) {
                job->vOff = (unsigned)gpu_dirty_vmin * stride;
                job->verts.assign((const char*)verts + job->vOff,
                    (const char*)verts + job->vOff + (size_t)(gpu_dirty_vmax - gpu_dirty_vmin + 1) * stride);
            }
            if (gpu_dirty_tmin >= 0) {
                job->iOff = (unsigned)gpu_dirty_tmin * 3 * sizeof(WORD);
                job->indices.assign((const char*)locked_indices + job->iOff,
                    (const char*)locked_indices + job->iOff + (size_t)(gpu_dirty_tmax - gpu_dirty_tmin + 1) * 3 * sizeof(WORD));
            }
        }
        if (job) {
            gpuUpload = sdlgpu::EnqueueUpload(RecordMeshUpload, job);
            gpu_uploaded = true;
        }
    }

    locked_verts = nullptr;
    locked_skin_verts = nullptr;
    locked_indices = nullptr;

    if (staging_full && !keep_staging) {
        staging_v.clear(); staging_v.shrink_to_fit();
        staging_i.clear(); staging_i.shrink_to_fit();
    }
    staging_full = false;
}

void gxMesh::uploadFrom(int firstVert, const void* verts, int vertCount, int srcStride,
                        int firstTri, const void* tris, int triCount) {
    if (!gpuMirror || !graphics || !graphics->runtime || !graphics->runtime->sdlGpu) return;

    syncGpuUpload();

    unsigned stride = skinned ? sizeof(dxSkinVertex) : sizeof(dxVertex);

    if (firstVert < 0) vertCount = 0;
    if (firstVert + vertCount > max_verts) vertCount = max_verts - firstVert;
    if (vertCount < 0) vertCount = 0;
    if (firstTri < 0) triCount = 0;
    if (firstTri + triCount > max_tris) triCount = max_tris - firstTri;
    if (triCount < 0) triCount = 0;

    MeshUploadJob* job = new MeshUploadJob;
    job->dev = (SDL_GPUDevice*)graphics->runtime->sdlGpu;
    job->mirror = gpuMirror;
    job->full = false;

    if (verts && vertCount > 0) {
        job->vOff = (unsigned)firstVert * stride;
        job->verts.resize((size_t)stride * vertCount);
        for (int i = 0; i < vertCount; ++i)
            memcpy(job->verts.data() + (size_t)i * stride, (const char*)verts + (size_t)i * srcStride, stride);
        if (gpu_dirty_vmin < 0) { gpu_dirty_vmin = firstVert; gpu_dirty_vmax = firstVert + vertCount - 1; }
        else {
            if (firstVert < gpu_dirty_vmin) gpu_dirty_vmin = firstVert;
            if (firstVert + vertCount - 1 > gpu_dirty_vmax) gpu_dirty_vmax = firstVert + vertCount - 1;
        }
    }
    if (tris && triCount > 0) {
        job->iOff = (unsigned)firstTri * 3 * sizeof(WORD);
        job->indices.assign((const char*)tris, (const char*)tris + (size_t)triCount * 3 * sizeof(WORD));
        if (gpu_dirty_tmin < 0) { gpu_dirty_tmin = firstTri; gpu_dirty_tmax = firstTri + triCount - 1; }
        else {
            if (firstTri < gpu_dirty_tmin) gpu_dirty_tmin = firstTri;
            if (firstTri + triCount - 1 > gpu_dirty_tmax) gpu_dirty_tmax = firstTri + triCount - 1;
        }
    }

    if (job->verts.empty() && job->indices.empty()) { delete job; return; }
    gpuUpload = sdlgpu::EnqueueUpload(RecordMeshUpload, job);
    gpu_uploaded = true;
    mesh_dirty = false;
}

void gxMesh::backup() {
    unlock();
}

void gxMesh::restore() {
    mesh_dirty = true;
}
