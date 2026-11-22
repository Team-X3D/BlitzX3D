#include "std.h"
#include "gxmesh.h"
#include "gxgraphics.h"

#include "gxruntime.h"

extern gxRuntime* gx_runtime;

gxMesh::gxMesh(gxGraphics* g, IDirect3DVertexBuffer9* vs, IDirect3DIndexBuffer9* is,
    int max_vs, int max_ts) :
    graphics(g), vertex_buff(vs), index_buff(is), vertex_decl(nullptr),
    instance_buff(nullptr), instance_capacity(0),
    locked_verts(nullptr), locked_skin_verts(nullptr), locked_indices(nullptr),
    max_verts(max_vs), max_tris(max_ts), mesh_dirty(false), skinned(false) {
}

gxMesh::gxMesh(gxGraphics* g, IDirect3DVertexBuffer9* vs, IDirect3DIndexBuffer9* is,
    IDirect3DVertexDeclaration9* decl, int max_vs, int max_ts) :
    graphics(g), vertex_buff(vs), index_buff(is), vertex_decl(decl),
    instance_buff(nullptr), instance_capacity(0),
    locked_verts(nullptr), locked_skin_verts(nullptr), locked_indices(nullptr),
    max_verts(max_vs), max_tris(max_ts), mesh_dirty(false), skinned(true) {
}

gxMesh::~gxMesh() {
    unlock();
    if (vertex_buff) { vertex_buff->Release(); vertex_buff = nullptr; }
    if (index_buff) { index_buff->Release();  index_buff = nullptr; }
    if (instance_buff) { instance_buff->Release(); instance_buff = nullptr; }
}

bool gxMesh::lock(bool all) {
    if ((locked_verts || locked_skin_verts) && locked_indices) return true;

    // lock vert buffer
    if (skinned) {
        if (!locked_skin_verts) {
            DWORD vflags = D3DLOCK_NOSYSLOCK | (all ? D3DLOCK_DISCARD : D3DLOCK_NOOVERWRITE);
            void* ptr = nullptr;
            if (FAILED(vertex_buff->Lock(0, 0, &ptr, vflags))) {
                return false;
            }
            locked_skin_verts = reinterpret_cast<dxSkinVertex*>(ptr);
        }
    }
    else if (!locked_verts) {
        DWORD vflags = D3DLOCK_NOSYSLOCK | (all ? D3DLOCK_DISCARD : D3DLOCK_NOOVERWRITE);
        void* ptr = nullptr;
        if (FAILED(vertex_buff->Lock(0, 0, &ptr, vflags))) {
            return false;
        }
        locked_verts = reinterpret_cast<dxVertex*>(ptr);
    }

    // lock index buffer
    if (!locked_indices) {
        DWORD iflags = D3DLOCK_NOSYSLOCK | (all ? D3DLOCK_DISCARD : D3DLOCK_NOOVERWRITE);
        void* ptr = nullptr;
        if (FAILED(index_buff->Lock(0, 0, &ptr, iflags))) {
            if (locked_verts) { vertex_buff->Unlock(); locked_verts = nullptr; }
            if (locked_skin_verts) { vertex_buff->Unlock(); locked_skin_verts = nullptr; }
            return false;
        }
        locked_indices = reinterpret_cast<WORD*>(ptr);
    }

    mesh_dirty = false;
    return true;
}

void gxMesh::unlock() {
    if (locked_verts) {
        vertex_buff->Unlock();
        locked_verts = nullptr;
    }
    if (locked_skin_verts) {
        vertex_buff->Unlock();
        locked_skin_verts = nullptr;
    }
    if (locked_indices) {
        index_buff->Unlock();
        locked_indices = nullptr;
    }
}

void gxMesh::backup() {
	unlock();
}

void gxMesh::restore() {
	if (instance_buff) { instance_buff->Release(); instance_buff = nullptr; instance_capacity = 0; }
	mesh_dirty = true;
}

void gxMesh::render(int first_vert, int vert_cnt, int first_tri, int tri_cnt) {
    unlock();

    IDirect3DDevice9* dev = graphics->dir3dDev;

    if (skinned) {
        dev->SetVertexDeclaration(vertex_decl);
        dev->SetStreamSource(0, vertex_buff, 0, sizeof(dxSkinVertex));
        dev->SetIndices(index_buff);
        dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, first_vert, 0, vert_cnt, first_tri * 3, tri_cnt);
        return;
    }

    dev->SetStreamSource(0, vertex_buff, 0, sizeof(dxVertex));
    dev->SetFVF(VTXFMT);
    dev->SetIndices(index_buff);
    dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, first_vert, 0, vert_cnt, first_tri * 3, tri_cnt);
}

void gxMesh::renderSkinned(int first_vert, int vert_cnt, int first_tri, int tri_cnt,
    const float* bone_data, int bone_cnt) {
    unlock();

    IDirect3DDevice9* dev = graphics->dir3dDev;
    IDirect3DVertexShader9* shader = graphics->getSkinningShader();
    if (!shader || !vertex_decl) return;

    if (bone_cnt > MAX_SKIN_BONES) bone_cnt = MAX_SKIN_BONES;

    dev->SetVertexShaderConstantF(0, bone_data, bone_cnt * 3);

    dev->SetVertexDeclaration(vertex_decl);
    dev->SetVertexShader(shader);
    dev->SetStreamSource(0, vertex_buff, 0, sizeof(dxSkinVertex));
    dev->SetIndices(index_buff);
    dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, first_vert, 0, vert_cnt, first_tri * 3, tri_cnt);

    dev->SetVertexShader(nullptr);
    dev->SetVertexDeclaration(nullptr);
}

bool gxMesh::ensureInstanceCapacity(int count) {
    if (instance_capacity >= count) return true;

    if (instance_buff) { instance_buff->Release(); instance_buff = nullptr; instance_capacity = 0; }

    int cap = 16;
    while (cap < count) cap *= 2;

    DWORD usage = D3DUSAGE_WRITEONLY | D3DUSAGE_DYNAMIC;
    if (FAILED(graphics->dir3dDev->CreateVertexBuffer(cap * 12 * sizeof(float), usage, 0, D3DPOOL_DEFAULT, &instance_buff, nullptr)))
        return false;

    instance_capacity = cap;
    return true;
}

bool gxMesh::renderInstanced(int first_vert, int vert_cnt, int first_tri, int tri_cnt,
    const float* instance_data, int instance_count) {
    unlock();

    if (skinned || !instance_data || instance_count <= 0) return false;
    if (!graphics->ensureInstancingShader()) return false;

    IDirect3DVertexShader9* shader = graphics->getInstancingShader();
    IDirect3DVertexDeclaration9* decl = graphics->getInstancingDecl();
    if (!shader || !decl || !ensureInstanceCapacity(instance_count)) return false;

    void* ptr = nullptr;
    if (FAILED(instance_buff->Lock(0, 0, &ptr, D3DLOCK_DISCARD | D3DLOCK_NOSYSLOCK))) return false;
    memcpy(ptr, instance_data, instance_count * 12 * sizeof(float));
    instance_buff->Unlock();

    IDirect3DDevice9* dev = graphics->dir3dDev;

    dev->SetVertexDeclaration(decl);
    dev->SetVertexShader(shader);
    dev->SetStreamSource(0, vertex_buff, 0, sizeof(dxVertex));
    dev->SetStreamSource(1, instance_buff, 0, 12 * sizeof(float));
    HRESULT hr0 = dev->SetStreamSourceFreq(0, D3DSTREAMSOURCE_INDEXEDDATA | (DWORD)instance_count);
    HRESULT hr1 = dev->SetStreamSourceFreq(1, D3DSTREAMSOURCE_INSTANCEDATA | 1);
    HRESULT hr = FAILED(hr0) || FAILED(hr1) ? E_FAIL : S_OK;
    if (SUCCEEDED(hr)) {
        dev->SetIndices(index_buff);
        hr = dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, first_vert, 0, vert_cnt, first_tri * 3, tri_cnt);
    }

    dev->SetStreamSourceFreq(0, 1);
    dev->SetStreamSourceFreq(1, 1);
    dev->SetStreamSource(1, nullptr, 0, 0);
    dev->SetVertexShader(nullptr);
    dev->SetVertexDeclaration(nullptr);
    return SUCCEEDED(hr);
}
