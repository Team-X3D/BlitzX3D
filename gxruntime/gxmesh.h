#ifndef GXMESH_H
#define GXMESH_H

#include <d3d9.h>
#include <d3dx9.h>

class gxGraphics;

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

    void render(int first_vert, int vert_cnt, int first_tri, int tri_cnt);
    void renderSkinned(int first_vert, int vert_cnt, int first_tri, int tri_cnt, const float* bone_data, int bone_cnt);
    bool renderInstanced(int first_vert, int vert_cnt, int first_tri, int tri_cnt, const float* instance_data, int instance_count);

    void backup();
    void restore();

    static const DWORD VTXFMT = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX2 | D3DFVF_TEXCOORDSIZE2(0) | D3DFVF_TEXCOORDSIZE2(1);

private:
    gxGraphics* graphics;
    IDirect3DVertexBuffer9* vertex_buff;
    IDirect3DIndexBuffer9* index_buff;
    IDirect3DVertexDeclaration9* vertex_decl;
    IDirect3DVertexBuffer9* instance_buff;
    int instance_capacity;

    int  max_verts, max_tris;
    bool mesh_dirty;
    bool skinned;
    dxVertex* locked_verts;
    dxSkinVertex* locked_skin_verts;
    WORD* locked_indices;

    bool ensureInstanceCapacity(int count);

    /***** GX INTERFACE *****/
public:
    bool lock(bool all);
    void unlock();

    void setVertex(int n, const void* v) {
        memcpy(locked_verts + n, v, sizeof(dxVertex));
    }
    void setVertex(int n, const float coords[3], const float normal[3], const float tex_coords[2][2]) {
        dxVertex* t = locked_verts + n;
        memcpy(t->coords, coords, 12);
        memcpy(t->normal, normal, 12);
        t->argb = 0xffffffff;
        memcpy(t->tex_coords, tex_coords, 16);
    }
    void setVertex(int n, const float coords[3], const float normal[3], unsigned argb, const float tex_coords[2][2]) {
        dxVertex* t = locked_verts + n;
        memcpy(t->coords, coords, 12);
        memcpy(t->normal, normal, 12);
        t->argb = argb;
        memcpy(t->tex_coords, tex_coords, 16);
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
    }
    void setTriangle(int n, int v0, int v1, int v2) {
        locked_indices[n * 3] = (WORD)v0;
        locked_indices[n * 3 + 1] = (WORD)v1;
        locked_indices[n * 3 + 2] = (WORD)v2;
    }
};

#endif