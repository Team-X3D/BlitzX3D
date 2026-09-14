#ifndef GXMESH_H
#define GXMESH_H

#include <d3d9.h>
#include <d3dx9.h>

class gxGraphics;

namespace sdlgpu { struct GpuMesh; struct UploadHandle; }

class gxMesh {
public:
    static const int MESH_DYNAMIC = 1;
    static const int MESH_SKINNED = 2;
    static const int MAX_SKIN_BONES = 64;
    static const int MAX_VERTEX_BONES = 4;

    struct dxVertex {
        float coords[3];
        float normal[3];
        unsigned argb;
        float tex_coords[4];   // 2 sets x 2 floats
    };

    struct dxSkinVertex {
        float coords[3];
        float normal[3];
        unsigned argb;
        float tex_coords[4];   // 2 sets x 2 floats again
        float blend_indices[4];
        float blend_weights[4];
    };

    gxMesh(gxGraphics* graphics, IDirect3DVertexBuffer9* verts, IDirect3DIndexBuffer9* indices, int max_verts, int max_tris);
    gxMesh(gxGraphics* graphics, IDirect3DVertexBuffer9* verts, IDirect3DIndexBuffer9* indices, IDirect3DVertexDeclaration9* decl, int max_verts, int max_tris);
    ~gxMesh();

    int maxVerts() const { return max_verts; }
    int maxTris()  const { return max_tris; }

    bool dirty() const { return mesh_dirty; }
    bool isSkinned() const { return skinned; }

    void render(int first_vert, int vert_cnt, int first_tri, int tri_cnt, bool skipDxDraw = false);
    void renderSkinned(int first_vert, int vert_cnt, int first_tri, int tri_cnt, const float* bone_data, int bone_cnt);

    void backup();
    void restore();

    static const DWORD VTXFMT = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX2 | D3DFVF_TEXCOORDSIZE2(0) | D3DFVF_TEXCOORDSIZE2(1);

private:
    gxGraphics* graphics;
    IDirect3DVertexBuffer9* vertex_buff;
    IDirect3DIndexBuffer9* index_buff;
    IDirect3DVertexDeclaration9* vertex_decl;

    int  max_verts, max_tris;
    bool mesh_dirty;
    bool skinned;
    dxVertex* locked_verts;
    dxSkinVertex* locked_skin_verts;
    WORD* locked_indices;
    dxVertex* cpu_verts = nullptr;
    dxSkinVertex* cpu_skin_verts = nullptr;
    WORD* cpu_indices = nullptr;

    int gpu_dirty_vmin, gpu_dirty_vmax;
    int gpu_dirty_tmin, gpu_dirty_tmax;
    bool gpu_uploaded;

    void markVertDirty(int n) {
        if (n < 0 || n >= max_verts) return;
        if (gpu_dirty_vmin < 0) gpu_dirty_vmin = gpu_dirty_vmax = n;
        else {
            if (n < gpu_dirty_vmin) gpu_dirty_vmin = n;
            if (n > gpu_dirty_vmax) gpu_dirty_vmax = n;
        }
    }
    void markTriDirty(int n) {
        if (n < 0 || n >= max_tris) return;
        if (gpu_dirty_tmin < 0) gpu_dirty_tmin = gpu_dirty_tmax = n;
        else {
            if (n < gpu_dirty_tmin) gpu_dirty_tmin = n;
            if (n > gpu_dirty_tmax) gpu_dirty_tmax = n;
        }
    }
    void markGpuFullDirty() {
        gpu_dirty_vmin = 0;
        gpu_dirty_vmax = max_verts - 1;
        gpu_dirty_tmin = 0;
        gpu_dirty_tmax = max_tris - 1;
    }

    sdlgpu::GpuMesh* gpuMirror = nullptr;
    sdlgpu::UploadHandle* gpuUpload = nullptr;

    void syncGpuUpload();

    /***** GX INTERFACE *****/
public:
    bool lock(bool all);
    void unlock();

    sdlgpu::GpuMesh* getGpuMirror();

    void setVertex(int n, const void* v) {
        memcpy(locked_verts + n, v, sizeof(dxVertex));
        markVertDirty(n);
    }
    void setVertex(int n, const float coords[3], const float normal[3], const float tex_coords[2][2]) {
        dxVertex* t = locked_verts + n;
        memcpy(t->coords, coords, 12);
        memcpy(t->normal, normal, 12);
        t->argb = 0xffffffff;
        memcpy(t->tex_coords, tex_coords, 16);
        markVertDirty(n);
    }
    void setVertex(int n, const float coords[3], const float normal[3], unsigned argb, const float tex_coords[2][2]) {
        dxVertex* t = locked_verts + n;
        memcpy(t->coords, coords, 12);
        memcpy(t->normal, normal, 12);
        t->argb = argb;
        memcpy(t->tex_coords, tex_coords, 16);
        markVertDirty(n);
    }
    void setSkinVertex(int n, const float coords[3], const float normal[3], unsigned argb, const float tex_coords[2][2], const unsigned char bone_indices[4], const float bone_weights[4]) {
        dxSkinVertex* t = locked_skin_verts + n;
        memcpy(t->coords, coords, 12);
        memcpy(t->normal, normal, 12);
        t->argb = argb;
        memcpy(t->tex_coords, tex_coords, 16);
        for(int i = 0; i < 4; ++i) {
            t->blend_indices[i] = (float)bone_indices[i];
            t->blend_weights[i] = bone_weights[i];
        }
        markVertDirty(n);
    }
    void setTriangle(int n, int v0, int v1, int v2) {
        locked_indices[n * 3] = (WORD)v0;
        locked_indices[n * 3 + 1] = (WORD)v1;
        locked_indices[n * 3 + 2] = (WORD)v2;
        markTriDirty(n);
    }
};

#endif