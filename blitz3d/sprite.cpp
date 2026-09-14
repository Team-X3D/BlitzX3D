#include "std.h"
#include "sprite.h"

extern float stats3d[];

static float null[] = { 0,0,0 };

static float tex_coords0[2][2] = { {0,0},{0,0} };
static float tex_coords1[2][2] = { {1,0},{1,0} };
static float tex_coords2[2][2] = { {1,1},{1,1} };
static float tex_coords3[2][2] = { {0,1},{0,1} };

extern gxRuntime* gx_runtime;
extern gxGraphics* gx_graphics;

static gxMesh* mesh;
static int mesh_size;
static std::vector<int> mesh_indices;

static std::vector<Vector> stage_verts;
static std::vector<char> stage_slots;
static bool stage_dirty;
static bool stage_reflected;

static int allocIndex() {
	if(!mesh_indices.size()) {
		if(mesh_size) gx_graphics->freeMesh(mesh);
		for(int k = 0; k < 256; ++k) {
			mesh_indices.push_back(mesh_size++);
		}
		mesh = gx_graphics->createMesh(mesh_size * 4, mesh_size * 2, 0);
		stage_verts.resize(mesh_size * 4);
		stage_slots.assign(mesh_size, 0);
	}
	int n = mesh_indices.back();
	mesh_indices.pop_back();
	return n;
}

static void freeIndex(int n) {
	mesh_indices.push_back(n);
	if(mesh_indices.size() != mesh_size) return;
	gx_graphics->freeMesh(mesh);
	mesh_indices.clear();
	stage_verts.clear();
	stage_slots.clear();
	stage_dirty = false;
	mesh_size = 0;
}

Sprite::Sprite() :
	view_mode(VIEW_MODE_FREE),
	xhandle(0), yhandle(0),
	rot(0), xscale(1), yscale(1), captured(false) {
	setRenderSpace(RENDER_SPACE_WORLD);
	mesh_index = allocIndex();
}

Sprite::Sprite(const Sprite& t) :
	Model(t),
	view_mode(t.view_mode),
	xhandle(t.xhandle), yhandle(t.yhandle),
	rot(t.rot), xscale(t.xscale), yscale(t.yscale), captured(false) {
	mesh_index = allocIndex();
}

Sprite::~Sprite() {
	freeIndex(mesh_index);
}

void Sprite::setRotation(float angle) {
	rot = angle;
}

void Sprite::setScale(float x, float y) {
	xscale = x; yscale = y;
}

void Sprite::setHandle(float x, float y) {
	xhandle = x; yhandle = y;
}

void Sprite::setViewmode(int mode) {
	view_mode = mode;
}

void Sprite::capture() {
	Model::capture();
	r_rot = rot;
	r_xscale = xscale;
	r_yscale = yscale;
	captured = true;
}

bool Sprite::beginRender(float tween) {
	Model::beginRender(tween);
	if(tween == 1 || !captured) {
		r_rot = rot;
		r_xscale = xscale;
		r_yscale = yscale;
	}
	else {
		r_rot = (rot - r_rot) * tween + r_rot;
		r_xscale = (xscale - r_xscale) * tween + r_xscale;
		r_yscale = (yscale - r_yscale) * tween + r_yscale;
	}
	return true;
}

bool Sprite::render(const RenderContext& rc) {

	Transform t = getRenderTform();

	if(view_mode == VIEW_MODE_FREE) {
		t.m = rc.getCameraTform().m;
	}
	else if(view_mode == VIEW_MODE_UPRIGHT) {
		t.m.k = rc.getCameraTform().m.k; t.m.orthogonalize();
	}
	else if(view_mode == VIEW_MODE_UPRIGHT2) {
		t.m = yawMatrix(matrixYaw(rc.getCameraTform().m)) * t.m;
	}

	t.m = t.m * rollMatrix(r_rot) * scaleMatrix(r_xscale, r_yscale, 1);

	Vector verts[4];
	verts[0] = t * Vector(-1 - xhandle, 1 - yhandle, 0);
	verts[1] = t * Vector(1 - xhandle, 1 - yhandle, 0);
	verts[2] = t * Vector(1 - xhandle, -1 - yhandle, 0);
	verts[3] = t * Vector(-1 - xhandle, -1 - yhandle, 0);

	if(!rc.getWorldFrustum().cull(verts, 4)) return false;

	int base = mesh_index * 4;
	if(base + 3 < (int)stage_verts.size()) {
		for(int k = 0; k < 4; ++k) stage_verts[base + k] = verts[k];
		if(base / 4 < (int)stage_slots.size()) stage_slots[base / 4] = 1;
		stage_reflected = rc.isReflected();
		stage_dirty = true;
	}

	enqueue(mesh, base, 4, mesh_index * 2, 2);
	return false;
}

void Sprite::flushStage() {
	if(!stage_dirty) return;
	stage_dirty = false;
	if(!mesh || !stage_verts.size() || (int)stage_slots.size() < mesh_size) return;

	mesh->lock(false);

	for(int s = 0; s < mesh_size; ++s) {
		if(!stage_slots[s]) continue;
		stage_slots[s] = 0;

		int fv = s * 4, ft = s * 2;
		const Vector* v = &stage_verts[fv];
		mesh->setVertex(fv + 0, &v[0].x, null, tex_coords0);
		mesh->setVertex(fv + 1, &v[1].x, null, tex_coords1);
		mesh->setVertex(fv + 2, &v[2].x, null, tex_coords2);
		mesh->setVertex(fv + 3, &v[3].x, null, tex_coords3);
		if(stage_reflected) {
			mesh->setTriangle(ft + 0, 0, 2, 1);
			mesh->setTriangle(ft + 1, 0, 3, 2);
		}
		else {
			mesh->setTriangle(ft + 0, 0, 1, 2);
			mesh->setTriangle(ft + 1, 0, 2, 3);
		}
	}

	mesh->unlock();
}
