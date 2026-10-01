#include "std.h"
#include "meshutil.h"

MeshModel* MeshUtil::createCube(const Brush& b) {
	static Vector norms[] = {
		Vector(0,0,-1),Vector(1,0,0),Vector(0,0,1),
		Vector(-1,0,0),Vector(0,1,0),Vector(0,-1,0)
	};
	static Vector tex_coords[] = {
		Vector(0,0,1),Vector(1,0,1),Vector(1,1,1),Vector(0,1,1)
	};
	static int verts[] = {
		2,3,1,0,3,7,5,1,7,6,4,5,6,2,0,4,6,7,3,2,0,1,5,4
	};
	static Box box(Vector(-1, -1, -1), Vector(1, 1, 1));

	MeshModel* m = new MeshModel();
	Surface* s = m->createSurface(b);
	Surface::Vertex v;
	Surface::Triangle t;
	for(int k = 0; k < 24; k += 4) {
		const Vector& normal = norms[k / 4];
		for(int j = 0; j < 4; ++j) {
			v.coords = box.corner(verts[k + j]);
			v.normal = normal;
			v.tex_coords[0][0] = v.tex_coords[1][0] = tex_coords[j].x;
			v.tex_coords[0][1] = v.tex_coords[1][1] = tex_coords[j].y;
			s->addVertex(v);
		}
		t.verts[0] = k; t.verts[1] = k + 1; t.verts[2] = k + 2; s->addTriangle(t);
		t.verts[1] = k + 2; t.verts[2] = k + 3; s->addTriangle(t);
	}
	return m;
}

MeshModel* MeshUtil::createSphere(const Brush& b, int segs) {

	int h_segs = segs * 2, v_segs = segs;

	MeshModel* m = new MeshModel();
	Surface* s = m->createSurface(b);

	Surface::Vertex v;
	Surface::Triangle t;

	v.coords = v.normal = Vector(0, 1, 0);
	int k;
	for(k = 0; k < h_segs; ++k) {
		v.tex_coords[0][0] = v.tex_coords[1][0] = (k + .5f) / h_segs;
		v.tex_coords[0][1] = v.tex_coords[1][1] = 0;
		s->addVertex(v);
	}
	for(k = 1; k < v_segs; ++k) {
		float pitch = k * PI / v_segs - HALFPI;
		for(int j = 0; j <= h_segs; ++j) {
			float yaw = (j % h_segs) * TWOPI / h_segs;
			v.coords = v.normal = rotationMatrix(pitch, yaw, 0).k;
			v.tex_coords[0][0] = v.tex_coords[1][0] = float(j) / float(h_segs);
			v.tex_coords[0][1] = v.tex_coords[1][1] = float(k) / float(v_segs);
			s->addVertex(v);
		}
	}
	v.coords = v.normal = Vector(0, -1, 0);
	for(k = 0; k < h_segs; ++k) {
		v.tex_coords[0][0] = v.tex_coords[1][0] = (k + .5f) / h_segs;
		v.tex_coords[0][1] = v.tex_coords[1][1] = 1;
		s->addVertex(v);
	}
	for(k = 0; k < h_segs; ++k) {
		t.verts[0] = k;
		t.verts[1] = t.verts[0] + h_segs + 1;
		t.verts[2] = t.verts[1] - 1;
		s->addTriangle(t);
	}
	for(k = 1; k < v_segs - 1; ++k) {
		for(int j = 0; j < h_segs; ++j) {
			t.verts[0] = k * (h_segs + 1) + j - 1;
			t.verts[1] = t.verts[0] + 1;
			t.verts[2] = t.verts[1] + h_segs + 1;
			s->addTriangle(t);
			t.verts[1] = t.verts[2];
			t.verts[2] = t.verts[1] - 1;
			s->addTriangle(t);
		}
	}
	for(k = 0; k < h_segs; ++k) {
		t.verts[0] = (h_segs + 1) * (v_segs - 1) + k - 1;
		t.verts[1] = t.verts[0] + 1;
		t.verts[2] = t.verts[1] + h_segs;
		s->addTriangle(t);
	}

	return m;
}

MeshModel* MeshUtil::createCylinder(const Brush& b, int segs, bool solid) {

	MeshModel* m = new MeshModel();
	Surface::Vertex v;
	Surface::Triangle t;

	Surface* s = m->createSurface(b);
	int k;
	for(k = 0; k <= segs; ++k) {
		float yaw = (k % segs) * TWOPI / segs;
		v.coords = rotationMatrix(0, yaw, 0).k;
		v.coords.y = 1;
		v.normal = Vector(v.coords.x, 0, v.coords.z);
		v.tex_coords[0][0] = v.tex_coords[1][0] = float(k) / segs;
		v.tex_coords[0][1] = v.tex_coords[1][1] = 0;
		s->addVertex(v);
		v.coords.y = -1;
		v.tex_coords[0][0] = v.tex_coords[1][0] = float(k) / segs;
		v.tex_coords[0][1] = v.tex_coords[1][1] = 1;
		s->addVertex(v);
	}
	for(k = 0; k < segs; ++k) {
		t.verts[0] = k * 2;
		t.verts[1] = t.verts[0] + 2;
		t.verts[2] = t.verts[1] + 1;
		s->addTriangle(t);
		t.verts[1] = t.verts[2];
		t.verts[2] = t.verts[1] - 2;
		s->addTriangle(t);
	}

	if(!solid) return m;

	s = m->createSurface(b);

	for(k = 0; k < segs; ++k) {
		float yaw = k * TWOPI / segs;
		v.coords = rotationMatrix(0, yaw, 0).k;
		v.coords.y = 1; v.normal = Vector(0, 1, 0);
		v.tex_coords[0][0] = v.tex_coords[1][0] = v.coords.x * .5f + .5f;
		v.tex_coords[0][1] = v.tex_coords[1][1] = v.coords.z * .5f + .5f;
		s->addVertex(v);
		v.coords.y = -1; v.normal = Vector(0, -1, 0);
		s->addVertex(v);
	}
	for(k = 2; k < segs; ++k) {
		t.verts[0] = 0;
		t.verts[1] = k * 2;
		t.verts[2] = (k - 1) * 2;
		s->addTriangle(t);
		t.verts[0] = 1;
		t.verts[1] = (k - 1) * 2 + 1;
		t.verts[2] = k * 2 + 1;
		s->addTriangle(t);
	}

	return m;
}

MeshModel* MeshUtil::createCone(const Brush& b, int segs, bool solid) {
	MeshModel* m = new MeshModel();
	Surface::Vertex v;
	Surface::Triangle t;

	Surface* s;
	s = m->createSurface(b);
	int k;
	v.coords = v.normal = Vector(0, 1, 0);
	for(k = 0; k < segs; ++k) {
		v.tex_coords[0][0] = v.tex_coords[1][0] = (k + .5f) / segs;
		v.tex_coords[0][1] = v.tex_coords[1][1] = 0;
		s->addVertex(v);
	}
	for(k = 0; k <= segs; ++k) {
		float yaw = (k % segs) * TWOPI / segs;
		v.coords = yawMatrix(yaw).k; v.coords.y = -1;
		v.normal = Vector(v.coords.x, 0, v.coords.z);
		v.tex_coords[0][0] = v.tex_coords[1][0] = float(k) / segs;
		v.tex_coords[0][1] = v.tex_coords[1][1] = 1;
		s->addVertex(v);
	}
	for(k = 0; k < segs; ++k) {
		t.verts[0] = k;
		t.verts[1] = k + segs + 1;
		t.verts[2] = k + segs;
		s->addTriangle(t);
	}
	if(!solid) return m;
	s = m->createSurface(b);
	for(k = 0; k < segs; ++k) {
		float yaw = k * TWOPI / segs;
		v.coords = yawMatrix(yaw).k; v.coords.y = -1;
		v.normal = Vector(v.coords.x, 0, v.coords.z);
		v.tex_coords[0][0] = v.tex_coords[1][0] = v.coords.x * .5f + .5f;
		v.tex_coords[0][1] = v.tex_coords[1][1] = v.coords.z * .5f + .5f;
		s->addVertex(v);
	}
	t.verts[0] = 0;
	for(k = 2; k < segs; ++k) {
		t.verts[1] = k - 1;
		t.verts[2] = k;
		s->addTriangle(t);
	}
	return m;
}

namespace {

struct DecalPoint {
	Vector p;	// box local pos
	Vector n;	// world space normal
};

static float boxPlaneDist(const Vector& p, int plane) {
	switch (plane) {
	case 0: return p.x + .5f;
	case 1: return .5f - p.x;
	case 2: return p.y + .5f;
	case 3: return .5f - p.y;
	case 4: return p.z + .5f;
	default: return .5f - p.z;
	}
}

static std::vector<DecalPoint> clipAgainstBox(const std::vector<DecalPoint>& poly, int plane) {
	std::vector<DecalPoint> out;
	int n = (int)poly.size();
	for (int i = 0; i < n; ++i) {
		const DecalPoint& a = poly[i];
		const DecalPoint& b = poly[(i + 1) % n];
		float da = boxPlaneDist(a.p, plane);
		float db = boxPlaneDist(b.p, plane);
		if (da >= 0) out.push_back(a);
		if ((da >= 0) != (db >= 0)) {
			float t = da / (da - db);
			DecalPoint c;
			c.p = a.p + (b.p - a.p) * t;
			c.n = (a.n + (b.n - a.n) * t).normalized();
			out.push_back(c);
		}
	}
	return out;
}

}

int MeshUtil::projectDecal(MeshModel* dest, const Brush& b, MeshModel* source,
	const Transform& source_world, const Transform& box_world) {
	Transform box_inv = -box_world;
	Transform src_to_local = box_inv * source_world;
	Matrix normal_src = source_world.m.cofactor();
	Matrix normal_to_local = box_inv.m;
	//nudge to stop z-fighting, a proper fix is a depth bias render state but i don't really care!
	const float eps = .001f;

	Surface* surf = dest->findSurface(b);
	if (!surf) surf = dest->createSurface(b);

	int added = 0;
	const MeshModel::SurfaceList& surfaces = source->getSurfaces();
	for (size_t s = 0; s < surfaces.size(); ++s) {
		Surface* src = surfaces[s];
		int ntri = src->numTriangles();
		for (int t = 0; t < ntri; ++t) {
			const Surface::Triangle& tri = src->getTriangle(t);
			const Surface::Vertex* sv[3] = {
				&src->getVertex(tri.verts[0]),
				&src->getVertex(tri.verts[1]),
				&src->getVertex(tri.verts[2])
			};

			std::vector<DecalPoint> poly(3);
			for (int i = 0; i < 3; ++i) {
				poly[i].p = src_to_local * sv[i]->coords;
				poly[i].n = (normal_to_local * (normal_src * sv[i]->normal)).normalized();
			}

			for (int plane = 0; plane < 6 && poly.size() >= 3; ++plane) {
				poly = clipAgainstBox(poly, plane);
			}
			if (poly.size() < 3) continue;

			for (size_t i = 1; i + 1 < poly.size(); ++i) {
				if (surf->numVertices() + 3 > 65535) return added;
				Surface::Triangle out;
				for (int k = 0; k < 3; ++k) {
					const DecalPoint& d = poly[k == 0 ? 0 : (k == 1 ? i : i + 1)];
					Surface::Vertex v;
					v.coords = d.p + d.n * eps;
					v.normal = d.n;
					v.tex_coords[0][0] = v.tex_coords[1][0] = d.p.x + .5f;
					v.tex_coords[0][1] = v.tex_coords[1][1] = .5f - d.p.y;
					out.verts[k] = surf->numVertices();
					surf->addVertex(v);
				}
				surf->addTriangle(out);
				++added;
			}
		}
	}
	return added;
}

void MeshUtil::lightMesh(MeshModel* m, const Vector& pos, const Vector& rgb, float range) {
	if(range) {
		float att = 1.0f / range;
		const MeshModel::SurfaceList& surfs = m->getSurfaces();
		for(int k = 0; k < surfs.size(); ++k) {
			Surface* s = surfs[k];
			for(int j = 0; j < s->numVertices(); ++j) {
				const Surface::Vertex& v = s->getVertex(j);
				Vector lv = pos - v.coords;
				float dp = v.normal.normalized().dot(lv);
				if(dp <= 0) continue;
				float d = lv.length();
				float i = 1 / (d * att) * (dp / d);
				s->setColor(j, s->getColor(j) + rgb * i);
			}
		}
	}
	else {
		const MeshModel::SurfaceList& surfs = m->getSurfaces();
		for(int k = 0; k < surfs.size(); ++k) {
			Surface* s = surfs[k];
			for(int j = 0; j < s->numVertices(); ++j) {
				const Surface::Vertex& v = s->getVertex(j);
				s->setColor(j, s->getColor(j) + rgb);
			}
		}
	}
}