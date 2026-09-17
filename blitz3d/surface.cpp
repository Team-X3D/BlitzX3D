#include "std.h"
#include "surface.h"

extern gxGraphics* gx_graphics;

static Surface::Monitor nop_mon;

Surface::Surface() :
	mesh(0), skin_mesh(0), mesh_vs(0), mesh_ts(0), skin_mesh_vs(0), skin_mesh_ts(0),
	valid_vs(0), valid_ts(0), skin_valid_vs(0), skin_valid_ts(0), mon(&nop_mon) {
}

Surface::Surface(Monitor* m) :
	mesh(0), skin_mesh(0), mesh_vs(0), mesh_ts(0), skin_mesh_vs(0), skin_mesh_ts(0),
	valid_vs(0), valid_ts(0), skin_valid_vs(0), skin_valid_ts(0), mon(m) {
}

Surface::~Surface() {
	if(mesh) gx_graphics->freeMesh(mesh);
	if(skin_mesh) gx_graphics->freeMesh(skin_mesh);
}

void Surface::setBrush(const Brush& b) {
	brush = b;
	++mon->brush_changes;
}

void Surface::setName(const std::string& n) {
	name = n;
}

void Surface::clear(bool verts, bool tris) {
	if(verts) { vertices.clear(); valid_vs = 0; skin_valid_vs = 0; }
	if(tris) { triangles.clear(); valid_ts = 0; skin_valid_ts = 0; }
	++mon->geom_changes;
}

void Surface::addVertices(const std::vector<Vertex>& verts) {
	vertices.insert(vertices.end(), verts.begin(), verts.end());
	++mon->geom_changes;
}

void Surface::setColor(int n, const Vector& v) {
	int r = floor(v.x * 255); if(r < 0)r = 0; else if(r > 255)r = 255;
	int g = floor(v.y * 255); if(g < 0)g = 0; else if(g > 255)g = 255;
	int b = floor(v.z * 255); if(b < 0)b = 0; else if(b > 255)b = 255;

	unsigned argb = 0xff000000 | (r << 16) | (g << 8) | b;

	vertices[n].color = argb;
	if(n < valid_vs) valid_vs = n;
	if(n < skin_valid_vs) skin_valid_vs = n;
}

Vector Surface::getColor(int n)const {
	float r = (vertices[n].color & 0x00ff0000) >> 16;
	float g = (vertices[n].color & 0x0000ff00) >> 8;
	float b = vertices[n].color & 0x000000ff;
	return Vector(r / 255.0f, g / 255.0f, b / 255.0f);
}

void Surface::addTriangles(const std::vector<Triangle>& tris) {
	triangles.insert(triangles.end(), tris.begin(), tris.end());
}

void Surface::updateNormals() {
	int k;
	std::map<Vector, Vector> norm_map;
	for(k = 0; k < triangles.size(); ++k) {
		const Triangle& t = triangles[k];
		const Vector& v0 = vertices[t.verts[0]].coords;
		const Vector& v1 = vertices[t.verts[1]].coords;
		const Vector& v2 = vertices[t.verts[2]].coords;
		Vector n = (v1 - v0).cross(v2 - v0);
		if(n.length() <= EPSILON) continue;
		n.normalize();
		norm_map[v0] += n;
		norm_map[v1] += n;
		norm_map[v2] += n;
	}
	for(k = 0; k < vertices.size(); ++k) {
		Vertex* v = &vertices[k];
		v->normal = norm_map[v->coords].normalized();
	}
}

gxMesh* Surface::getMesh() {
	if(mesh && mesh->dirty()) { valid_vs = 0; valid_ts = 0; }

	if(valid_vs == vertices.size() && valid_ts == triangles.size()) return mesh;

	int start_vs = valid_vs, start_ts = valid_ts;
	valid_vs = valid_ts = 0;
	if (start_vs > (int)vertices.size()) start_vs = (int)vertices.size();
	if (start_ts > (int)triangles.size()) start_ts = (int)triangles.size();

	if(mesh_vs < vertices.size() || mesh_ts < triangles.size()) {
		if(mesh) {
			gx_graphics->freeMesh(mesh);
			mesh_vs = vertices.size() + mesh_vs / 2;
			mesh_ts = triangles.size() + mesh_ts / 2;
		}
		else {
			mesh_vs = vertices.size();
			mesh_ts = triangles.size();
		}
		mesh = gx_graphics->createMesh(mesh_vs, mesh_ts, 0);
		start_vs = start_ts = 0;
	}

	if (!mesh) {
		valid_vs = start_vs;
		valid_ts = start_ts;
		return mesh;
	}

	mesh->uploadFrom(start_vs, vertices.data() + start_vs, (int)vertices.size() - start_vs, (int)sizeof(Vertex),
		start_ts, triangles.data() + start_ts, (int)triangles.size() - start_ts);

	valid_vs = (int)vertices.size();
	valid_ts = (int)triangles.size();
	return mesh;
}

gxMesh* Surface::getSkinMesh() {
	if (skin_mesh && skin_mesh->dirty()) skin_valid_vs = 0;

	if (skin_valid_vs == vertices.size() && skin_valid_ts == triangles.size()) return skin_mesh;

	int start_vs = skin_valid_vs, start_ts = skin_valid_ts;
	skin_valid_vs = skin_valid_ts = 0;

	if (skin_mesh_vs < vertices.size() || skin_mesh_ts < triangles.size()) {
		if (skin_mesh) {
			gx_graphics->freeMesh(skin_mesh);
			skin_mesh_vs = vertices.size() + skin_mesh_vs / 2;
			skin_mesh_ts = triangles.size() + skin_mesh_ts / 2;
		}
		else {
			skin_mesh_vs = vertices.size();
			skin_mesh_ts = triangles.size();
		}
		skin_mesh = gx_graphics->createMesh(skin_mesh_vs, skin_mesh_ts, gxMesh::MESH_SKINNED);
	}

	if (!skin_mesh || !skin_mesh->lock(true)) {
		skin_valid_vs = start_vs;
		skin_valid_ts = start_ts;
		return skin_mesh;
	}
	for (; skin_valid_vs < vertices.size(); ++skin_valid_vs) {
		const Vertex& v = vertices[skin_valid_vs];
		unsigned char bones[4] = { 0,0,0,0 };
		float weights[4] = { 0,0,0,0 };
		if (v.bone_bones[0] == 255) {
		}
		else {
			for (int n = 0; n < MAX_SURFACE_BONES; ++n) {
				if (v.bone_bones[n] == 255) break;
				bones[n] = v.bone_bones[n];
				weights[n] = v.bone_weights[n];
			}
		}
		Vector coords = v.coords, normal = v.normal;
		skin_mesh->setSkinVertex(skin_valid_vs, coords, normal, v.color, v.tex_coords, bones, weights);
	}
	for (; skin_valid_ts < triangles.size(); ++skin_valid_ts) {
		const Triangle& t = triangles[skin_valid_ts];
		skin_mesh->setTriangle(skin_valid_ts, t.verts[0], t.verts[1], t.verts[2]);
	}
	skin_mesh->unlock();
	return skin_mesh;
}

gxMesh* Surface::getMesh(const std::vector<Bone>& bones) {

	bool bones_changed = true;
	if (skin_valid && mesh && !mesh->dirty() && last_bones.size() == bones.size() && mesh_vs >= vertices.size() && mesh_ts >= triangles.size())
	{
		bones_changed = (memcmp(last_bones.data(), bones.data(), sizeof(Bone) * bones.size()) != 0);
	}

	if (!bones_changed && valid_vs == vertices.size() && valid_ts == triangles.size()) {
		return mesh;
	}

	valid_vs = valid_ts = 0;

	if(mesh_vs < vertices.size() || mesh_ts < triangles.size()) {
		if(mesh) gx_graphics->freeMesh(mesh);
		mesh_vs = vertices.size();
		mesh_ts = triangles.size();
		mesh = gx_graphics->createMesh(mesh_vs, mesh_ts, gxMesh::MESH_DYNAMIC);
		skin_valid = false;
	}

	if (!mesh || !mesh->lock(true)) return mesh;
	for(; valid_vs < vertices.size(); ++valid_vs) {
		const Vertex& v = vertices[valid_vs];
		if(v.bone_bones[0] == 255) {
			//no bone!
			const Bone& bone = bones[0];
			mesh->setVertex(valid_vs, bone.coord_tform * v.coords, bone.normal_tform * v.normal, v.color, v.tex_coords);
		}
		else if(v.bone_bones[1] == 255) {
			//one bone only
			const Bone& bone = bones[v.bone_bones[0]];
			mesh->setVertex(valid_vs, bone.coord_tform * v.coords, bone.normal_tform * v.normal, v.color, v.tex_coords);
		}
		else {
			const Vertex& v = vertices[valid_vs];
			//two or more bones
			Vector tv, tn;
			for(int n = 0; n < MAX_SURFACE_BONES; ++n) {
				if(v.bone_bones[n] == 255) break;
				const Bone& bone = bones[v.bone_bones[n]];
				tv += bone.coord_tform * v.coords * v.bone_weights[n];
				tn += bone.normal_tform * v.normal * v.bone_weights[n];
			}
			mesh->setVertex(valid_vs, tv, tn.normalized(), v.color, v.tex_coords);
		}
	}
	for(; valid_ts < triangles.size(); ++valid_ts) {
		const Triangle& t = triangles[valid_ts];
		mesh->setTriangle(valid_ts, t.verts[0], t.verts[1], t.verts[2]);
	}
	mesh->unlock();

	last_bones = bones;
	skin_valid = true;

	return mesh;
}