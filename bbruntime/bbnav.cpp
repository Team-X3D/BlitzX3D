#include "std.h"
#include "bbsys.h"
#include "bbnav.h"
#include "bbblitz3d.h"
#include "../blitz3d/world.h"
#include "../blitz3d/model.h"
#include "../blitz3d/meshmodel.h"
#include "../blitz3d/surface.h"

#include "Recast.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "DetourNavMeshQuery.h"

#include <algorithm>
#include <cstdio>
#include <cfloat>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <utility>

struct bbBank;
extern int bbVerifyBank(bbBank* b);
extern int bbBankSize(bbBank* b);
extern int bbPeekInt(bbBank* b, int offset);
extern void bbPokeFloat(bbBank* b, int offset, float value);

namespace {

	const unsigned short NAV_FLAG_WALK = 0x0001;
	const unsigned short NAV_FLAG_LINK = 0x0002;
	const int NAV_MAX_PATH = 256;
	const int NAV_MAX_NODES = 65535;

	struct NavMesh {
		dtNavMesh* mesh = 0;
		dtNavMeshQuery* query = 0;
		const dtMeshTile* tile = 0;
		dtQueryFilter filter;

		int polyCount = 0;
		int vertCount = 0;
		int offMeshBase = 0;

		struct LinkMeta {
			int kind = 0;
			std::string name;
		};
		std::vector<LinkMeta> links;
		std::vector<dtPolyRef> linkRefs;
		std::vector<std::vector<dtPolyRef>> linkPolys;
		std::vector<int> linkBlockCount;
		std::vector<unsigned char> linkOn;
		std::vector<unsigned char> userBlocked;

		float agentRadius = 0.3f;
		float agentHeight = 1.8f;
		float stepHeight = 0.25f;
		std::vector<float> buildVerts;
		struct BuildPoly {
			std::vector<int> verts;
			unsigned char area = 0;
		};
		std::vector<BuildPoly> buildPolys;
		struct BuildLink {
			float a[3] = { 0, 0, 0 };
			float b[3] = { 0, 0, 0 };
			int kind = 0;
			bool bidir = false;
			float radius = 0.0f;
			std::string name;
		};
		std::vector<BuildLink> buildLinks;

		~NavMesh();
	};

	std::unordered_set<NavMesh*> nav_set;

	void navRelease(NavMesh* n) {
		if (n->query) {
			dtFreeNavMeshQuery(n->query);
			n->query = 0;
		}
		if (n->mesh) {
			dtFreeNavMesh(n->mesh);
			n->mesh = 0;
		}
		n->tile = 0;
		n->polyCount = n->vertCount = n->offMeshBase = 0;
		n->links.clear();
		n->linkRefs.clear();
		n->linkPolys.clear();
		n->linkBlockCount.clear();
		n->linkOn.clear();
		n->userBlocked.clear();
		n->filter = dtQueryFilter();
	}

	NavMesh::~NavMesh() {
		navRelease(this);
	}

	bool navValid(NavMesh* n) {
		return n && nav_set.count(n);
	}

	void navApplyPoly(NavMesh* n, int poly);

	bool navRefresh(NavMesh* n) {
		if (!n->mesh) return false;
		const dtMeshTile* tile = n->mesh->getTileAt(0, 0, 0);
		if (!tile || !tile->header) return false;
		n->tile = tile;
		const dtMeshHeader* h = tile->header;
		n->offMeshBase = h->offMeshBase;
		n->polyCount = h->polyCount - h->offMeshConCount;
		n->vertCount = h->vertCount - h->offMeshConCount * 2;
		n->agentRadius = h->walkableRadius;
		n->agentHeight = h->walkableHeight;
		n->stepHeight = h->walkableClimb;
		n->userBlocked.assign(n->polyCount, 0);
		n->linkBlockCount.assign(n->polyCount, 0);

		n->linkRefs.assign(n->links.size(), 0);
		n->linkPolys.assign(n->links.size(), {});
		n->linkOn.assign(n->links.size(), 1);
		dtPolyRef base = n->mesh->getPolyRefBase(tile);
		for (int i = 0; i < h->offMeshConCount; ++i) {
			const dtOffMeshConnection& con = tile->offMeshCons[i];
			if (con.userId < n->linkRefs.size()) n->linkRefs[con.userId] = base | (dtPolyRef)con.poly;
		}

		float r = 0.0f;
		for (int i = 0; i < h->offMeshConCount; ++i) {
			const dtOffMeshConnection& con = tile->offMeshCons[i];
			unsigned id = con.userId;
			if (id >= n->links.size()) continue;
			r = std::max(0.1f, con.rad);
			float minx = std::min(con.pos[0], con.pos[3]) - r, maxx = std::max(con.pos[0], con.pos[3]) + r;
			float miny = std::min(con.pos[1], con.pos[4]) - 1.0f, maxy = std::max(con.pos[1], con.pos[4]) + 1.0f;
			float minz = std::min(con.pos[2], con.pos[5]) - r, maxz = std::max(con.pos[2], con.pos[5]) + r;
			for (int p = 0; p < n->polyCount; ++p) {
				const dtPoly& dp = tile->polys[p];
				float pminx = FLT_MAX, pmaxx = -FLT_MAX;
				float pminy = FLT_MAX, pmaxy = -FLT_MAX;
				float pminz = FLT_MAX, pmaxz = -FLT_MAX;
				for (int j = 0; j < dp.vertCount; ++j) {
					const float* v = &tile->verts[dp.verts[j] * 3];
					pminx = std::min(pminx, v[0]); pmaxx = std::max(pmaxx, v[0]);
					pminy = std::min(pminy, v[1]); pmaxy = std::max(pmaxy, v[1]);
					pminz = std::min(pminz, v[2]); pmaxz = std::max(pmaxz, v[2]);
				}
				if (maxx < pminx || minx > pmaxx || maxy < pminy || miny > pmaxy || maxz < pminz || minz > pmaxz) continue;
				n->linkPolys[id].push_back(base | (dtPolyRef)p);
			}
		}
		return true;
	}

	bool navValidateData(const unsigned char* data, int size) {
		if (size < (int)sizeof(dtMeshHeader)) return false;
		const dtMeshHeader* h = (const dtMeshHeader*)data;
		if (h->magic != DT_NAVMESH_MAGIC || h->version != DT_NAVMESH_VERSION) return false;
		if (h->polyCount < 1 || h->polyCount > 0xffff) return false;
		if (h->vertCount < 0 || h->vertCount > 0xffff) return false;
		if (h->maxLinkCount < 1 || h->detailMeshCount < 0 || h->detailVertCount < 0 || h->detailTriCount < 0) return false;
		if (h->bvNodeCount < 0 || h->offMeshConCount < 0 || h->offMeshBase < 0 || h->offMeshBase > h->polyCount) return false;
		if (h->offMeshConCount > h->polyCount - h->offMeshBase) return false;

		auto align4 = [](unsigned long long n) { return (n + 3) & ~3ull; };
		unsigned long long need = align4(sizeof(dtMeshHeader));
		need += align4(sizeof(float) * 3ull * (unsigned)h->vertCount);
		need += align4(sizeof(dtPoly) * (unsigned)h->polyCount);
		need += align4(sizeof(dtLink) * (unsigned)h->maxLinkCount);
		need += align4(sizeof(dtPolyDetail) * (unsigned)h->detailMeshCount);
		need += align4(sizeof(float) * 3ull * (unsigned)h->detailVertCount);
		need += align4(4ull * (unsigned)h->detailTriCount);
		need += align4(sizeof(dtBVNode) * (unsigned)h->bvNodeCount);
		need += align4(sizeof(dtOffMeshConnection) * (unsigned)h->offMeshConCount);
		return need <= (unsigned long long)size;
	}

	bool navAdoptData(NavMesh* n, unsigned char* data, int size, std::vector<NavMesh::LinkMeta> meta) {
		if (!navValidateData(data, size)) {
			dtFree(data);
			return false;
		}
		dtNavMesh* mesh = dtAllocNavMesh();
		if (!mesh) {
			dtFree(data);
			return false;
		}
		if (dtStatusFailed(mesh->init(data, size, DT_TILE_FREE_DATA))) {
			dtFreeNavMesh(mesh);
			dtFree(data);
			return false;
		}
		dtNavMeshQuery* query = dtAllocNavMeshQuery();
		if (!query || dtStatusFailed(query->init(mesh, NAV_MAX_NODES))) {
			if (query) dtFreeNavMeshQuery(query);
			dtFreeNavMesh(mesh);
			return false;
		}
		const dtMeshTile* tile = mesh->getTileAt(0, 0, 0);
		if (!tile || !tile->header) {
			dtFreeNavMeshQuery(query);
			dtFreeNavMesh(mesh);
			return false;
		}

		navRelease(n);
		n->mesh = mesh;
		n->query = query;
		n->links = std::move(meta);
		n->filter = dtQueryFilter();
		if (!navRefresh(n)) {
			navRelease(n);
			return false;
		}
		return true;
	}

	dtQueryFilter navFilter(const NavMesh* n, unsigned short includeFlags) {
		dtQueryFilter f = n->filter;
		f.setIncludeFlags(includeFlags);
		f.setExcludeFlags(0);
		return f;
	}

	dtPolyRef navPolyRef(const NavMesh* n, int poly) {
		if (!n->tile || poly < 0 || poly >= n->offMeshBase) return 0;
		return n->mesh->getPolyRefBase(n->tile) | (dtPolyRef)poly;
	}

	void navApplyPoly(NavMesh* n, int poly) {
		dtPolyRef ref = navPolyRef(n, poly);
		if (!ref) return;
		bool blocked = n->userBlocked[poly] || n->linkBlockCount[poly] > 0;
		n->mesh->setPolyFlags(ref, blocked ? 0 : NAV_FLAG_WALK);
	}

	bool navNearest(NavMesh* n, const float* p, dtPolyRef* ref, float* nearest, bool* over) {
		dtQueryFilter f = navFilter(n, NAV_FLAG_WALK);
		float ext[3] = { 2.0f, 4.0f, 2.0f };
		*ref = 0;
		if (dtStatusFailed(n->query->findNearestPoly(p, ext, &f, ref, nearest, over)) || !*ref) return false;
		return true;
	}

	bool navReadFile(const std::string& path, std::vector<unsigned char>& out) {
		FILE* f = fopen(path.c_str(), "rb");
		if (!f) return false;
		fseek(f, 0, SEEK_END);
		long size = ftell(f);
		fseek(f, 0, SEEK_SET);
		if (size <= 0 || size > 64 * 1024 * 1024) {
			fclose(f);
			return false;
		}
		out.resize((size_t)size);
		bool ok = fread(out.data(), 1, out.size(), f) == out.size();
		fclose(f);
		return ok;
	}

	bool navWriteFile(const std::string& path, const void* data, size_t size) {
		FILE* f = fopen(path.c_str(), "wb");
		if (!f) return false;
		bool ok = fwrite(data, 1, size, f) == size;
		fclose(f);
		return ok;
	}

	unsigned putU32(std::vector<unsigned char>& b, unsigned v) {
		size_t at = b.size();
		b.resize(at + 4);
		b[at + 0] = (unsigned char)(v & 0xff);
		b[at + 1] = (unsigned char)((v >> 8) & 0xff);
		b[at + 2] = (unsigned char)((v >> 16) & 0xff);
		b[at + 3] = (unsigned char)((v >> 24) & 0xff);
		return (unsigned)at;
	}

	unsigned getU32(const unsigned char* p) {
		return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
	}

	std::vector<NavMesh::LinkMeta> navMetaFromBuild(const NavMesh* n) {
		std::vector<NavMesh::LinkMeta> meta(n->buildLinks.size());
		for (size_t i = 0; i < n->buildLinks.size(); ++i) {
			meta[i].kind = n->buildLinks[i].kind;
			meta[i].name = n->buildLinks[i].name;
		}
		return meta;
	}

	bool navBuildPolyMesh(NavMesh* n, unsigned char** outData, int* outSize) {
		const int nvp = 8;
		const int nverts = (int)n->buildVerts.size() / 3;
		const int npolys = (int)n->buildPolys.size();
		if (nverts <= 0 || npolys <= 0) return false;

		float bmin[3] = { n->buildVerts[0], n->buildVerts[1], n->buildVerts[2] };
		float bmax[3] = { bmin[0], bmin[1], bmin[2] };
		for (int i = 1; i < nverts; ++i) {
			for (int k = 0; k < 3; ++k) {
				float v = n->buildVerts[i * 3 + k];
				bmin[k] = std::min(bmin[k], v);
				bmax[k] = std::max(bmax[k], v);
			}
		}
		for (const auto& l : n->buildLinks) {
			for (int k = 0; k < 3; ++k) {
				bmin[k] = std::min(bmin[k], std::min(l.a[k], l.b[k]));
				bmax[k] = std::max(bmax[k], std::max(l.a[k], l.b[k]));
			}
		}
		bmin[0] -= 1.0f; bmin[1] -= 1.0f; bmin[2] -= 1.0f;
		bmax[0] += 1.0f; bmax[1] += 1.0f; bmax[2] += 1.0f;

		float span = std::max(std::max(bmax[0] - bmin[0], bmax[2] - bmin[2]), bmax[1] - bmin[1]);
		float cs = std::max(0.001f, span / 60000.0f);
		float ch = cs;

		std::vector<unsigned short> qverts((size_t)nverts * 3);
		for (int i = 0; i < nverts; ++i) {
			for (int k = 0; k < 3; ++k) {
				float q = (n->buildVerts[i * 3 + k] - bmin[k]) / (k == 1 ? ch : cs);
				q = std::max(0.0f, std::min(65535.0f, q + 0.5f));
				qverts[i * 3 + k] = (unsigned short)q;
			}
		}

		std::vector<unsigned short> polys((size_t)npolys * nvp * 2, 0xffff);
		std::vector<unsigned short> flags(npolys, NAV_FLAG_WALK);
		std::vector<unsigned char> areas(npolys, 0);

		std::unordered_map<unsigned long long, int> edges;
		edges.reserve((size_t)npolys * 8);
		for (int i = 0; i < npolys; ++i) {
			const auto& p = n->buildPolys[i];
			for (size_t j = 0; j < p.verts.size(); ++j) {
				unsigned long long key = ((unsigned long long)(unsigned)p.verts[j] << 32) | (unsigned)p.verts[(j + 1) % p.verts.size()];
				edges.emplace(key, i * nvp + (int)j);
			}
			areas[i] = p.area <= 63 ? p.area : 0;
		}

		for (int i = 0; i < npolys; ++i) {
			const auto& p = n->buildPolys[i];
			unsigned short* dst = &polys[(size_t)i * nvp * 2];
			for (size_t j = 0; j < p.verts.size(); ++j) {
				dst[j] = (unsigned short)p.verts[j];
				unsigned long long key = ((unsigned long long)(unsigned)p.verts[(j + 1) % p.verts.size()] << 32) | (unsigned)p.verts[j];
				auto it = edges.find(key);
				dst[nvp + j] = it == edges.end() ? 0xffff : (unsigned short)(it->second / nvp);
			}
		}

		std::vector<float> linkVerts;
		std::vector<float> linkRad;
		std::vector<unsigned short> linkFlags;
		std::vector<unsigned char> linkAreas;
		std::vector<unsigned char> linkDir;
		std::vector<unsigned int> linkIds;
		for (size_t i = 0; i < n->buildLinks.size(); ++i) {
			const auto& l = n->buildLinks[i];
			linkVerts.insert(linkVerts.end(), l.a, l.a + 3);
			linkVerts.insert(linkVerts.end(), l.b, l.b + 3);
			linkRad.push_back(l.radius > 0.0f ? l.radius : std::max(0.5f, n->agentRadius));
			linkFlags.push_back(NAV_FLAG_LINK);
			linkAreas.push_back(0);
			linkDir.push_back(l.bidir ? 1 : 0);
			linkIds.push_back((unsigned int)i);
		}

		dtNavMeshCreateParams params;
		memset(&params, 0, sizeof(params));
		params.verts = qverts.data();
		params.vertCount = nverts;
		params.polys = polys.data();
		params.polyFlags = flags.data();
		params.polyAreas = areas.data();
		params.polyCount = npolys;
		params.nvp = nvp;
		params.offMeshConVerts = linkVerts.empty() ? 0 : linkVerts.data();
		params.offMeshConRad = linkRad.empty() ? 0 : linkRad.data();
		params.offMeshConFlags = linkFlags.empty() ? 0 : linkFlags.data();
		params.offMeshConAreas = linkAreas.empty() ? 0 : linkAreas.data();
		params.offMeshConDir = linkDir.empty() ? 0 : linkDir.data();
		params.offMeshConUserID = linkIds.empty() ? 0 : linkIds.data();
		params.offMeshConCount = (int)linkIds.size();
		params.walkableHeight = n->agentHeight;
		params.walkableRadius = n->agentRadius;
		params.walkableClimb = n->stepHeight;
		params.cs = cs;
		params.ch = ch;
		params.buildBvTree = true;
		for (int k = 0; k < 3; ++k) {
			params.bmin[k] = bmin[k];
			params.bmax[k] = bmax[k];
		}
		return dtCreateNavMeshData(&params, outData, outSize);
	}

	bool navCollectWorld(std::vector<float>& verts, std::vector<int>& tris, int collisionType) {
		World* world = bbGetWorld();
		if (!world) return false;

		std::vector<Object*> objects;
		world->enabledObjects(objects);
		for (Object* obj : objects) {
			if (!obj->getPickGeometry()) continue;
			if (collisionType >= 0 && obj->getCollisionType() != collisionType) continue;
			Model* model = obj->getModel();
			if (!model) continue;
			MeshModel* mesh = model->getMeshModel();
			if (!mesh) continue;

			const Transform& t = obj->getWorldTform();
			for (Surface* s : mesh->getSurfaces()) {
				int nt = s->numTriangles();
				for (int i = 0; i < nt; ++i) {
					const Surface::Triangle& tri = s->getTriangle(i);
					Vector p[3];
					Vector n(0, 0, 0);
					for (int k = 0; k < 3; ++k) {
						const Surface::Vertex& v = s->getVertex(tri.verts[k]);
						p[k] = t * v.coords;
						n = n + t.m * v.normal;
					}

					float e1x = p[1].x - p[0].x, e1y = p[1].y - p[0].y, e1z = p[1].z - p[0].z;
					float e2x = p[2].x - p[0].x, e2y = p[2].y - p[0].y, e2z = p[2].z - p[0].z;
					float ny = e1z * e2x - e1x * e2z;
					if (ny * (n.y) < 0) std::swap(p[1], p[2]);

					int base = (int)verts.size() / 3;
					for (int k = 0; k < 3; ++k) {
						verts.push_back(p[k].x);
						verts.push_back(p[k].y);
						verts.push_back(p[k].z);
						tris.push_back(base + k);
					}
				}
			}
		}
		return !tris.empty();
	}

	bool navBake(NavMesh* n, float cellSize, float radius, float height, float maxSlope, float step, int collisionType) {
		std::vector<float> verts;
		std::vector<int> tris;
		if (!navCollectWorld(verts, tris, collisionType)) return false;

		const int nverts = (int)verts.size() / 3;
		const int ntris = (int)tris.size() / 3;

		float bmin[3], bmax[3];
		rcCalcBounds(verts.data(), nverts, bmin, bmax);
		for (int k = 0; k < 3; ++k) {
			bmin[k] -= 1.0f;
			bmax[k] += 1.0f;
		}

		rcConfig cfg;
		memset(&cfg, 0, sizeof(cfg));
		cfg.cs = cellSize;
		cfg.ch = cellSize * 0.5f;
		cfg.walkableSlopeAngle = maxSlope;
		cfg.walkableHeight = std::max(3, (int)ceilf(height / cfg.ch));
		cfg.walkableClimb = std::max(0, (int)floorf(step / cfg.ch));
		cfg.walkableRadius = std::max(0, (int)ceilf(radius / cfg.cs));
		cfg.maxEdgeLen = std::max(1, (int)(2.0f / cfg.cs));
		cfg.maxSimplificationError = 1.3f;
		cfg.minRegionArea = std::max(1, (int)((0.5f / cfg.cs) * (0.5f / cfg.cs)));
		cfg.mergeRegionArea = cfg.minRegionArea * 4;
		cfg.maxVertsPerPoly = 6;
		cfg.detailSampleDist = cfg.cs < 0.9f ? 0.9f : cfg.cs * 6.0f;
		cfg.detailSampleMaxError = cfg.ch;
		cfg.borderSize = 0;
		rcCalcGridSize(bmin, bmax, cfg.cs, &cfg.width, &cfg.height);
		for (int k = 0; k < 3; ++k) {
			cfg.bmin[k] = bmin[k];
			cfg.bmax[k] = bmax[k];
		}
		if (cfg.width <= 0 || cfg.height <= 0) return false;
		if ((long long)cfg.width * cfg.height > 16000000LL) return false;

		rcContext ctx(false);
		rcHeightfield* hf = rcAllocHeightfield();
		rcCompactHeightfield* chf = 0;
		rcContourSet* cset = 0;
		rcPolyMesh* pmesh = 0;
		rcPolyMeshDetail* dmesh = 0;
		bool ok = false;

		if (hf && rcCreateHeightfield(ctx, *hf, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch)) {
			std::vector<unsigned char> triAreas(ntris, RC_NULL_AREA);
			rcMarkWalkableTriangles(ctx, cfg.walkableSlopeAngle, verts.data(), nverts, tris.data(), ntris, triAreas.data());
			if (rcRasterizeTriangles(ctx, verts.data(), nverts, tris.data(), triAreas.data(), ntris, *hf, cfg.walkableClimb)) {
				rcFilterLowHangingWalkableObstacles(ctx, cfg.walkableClimb, *hf);
				rcFilterLedgeSpans(ctx, cfg.walkableHeight, cfg.walkableClimb, *hf);
				rcFilterWalkableLowHeightSpans(ctx, cfg.walkableHeight, *hf);

				chf = rcAllocCompactHeightfield();
				cset = rcAllocContourSet();
				pmesh = rcAllocPolyMesh();
				dmesh = rcAllocPolyMeshDetail();
				unsigned char* data = 0;
				int dataSize = 0;

				if (chf && cset && pmesh && dmesh
					&& rcBuildCompactHeightfield(ctx, cfg.walkableHeight, cfg.walkableClimb, *hf, *chf)
					&& rcErodeWalkableArea(ctx, cfg.walkableRadius, *chf)
					&& rcBuildDistanceField(ctx, *chf)
					&& rcBuildRegions(ctx, *chf, 0, cfg.minRegionArea, cfg.mergeRegionArea)
					&& rcBuildContours(ctx, *chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *cset)
					&& rcBuildPolyMesh(ctx, *cset, cfg.maxVertsPerPoly, *pmesh)
					&& rcBuildPolyMeshDetail(ctx, *pmesh, *chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *dmesh)) {
					std::vector<unsigned short> flags(pmesh->npolys, NAV_FLAG_WALK);
					std::vector<float> linkVerts;
					std::vector<float> linkRad;
					std::vector<unsigned short> linkFlags;
					std::vector<unsigned char> linkAreas;
					std::vector<unsigned char> linkDir;
					std::vector<unsigned int> linkIds;
					for (size_t i = 0; i < n->buildLinks.size(); ++i) {
						const auto& l = n->buildLinks[i];
						linkVerts.insert(linkVerts.end(), l.a, l.a + 3);
						linkVerts.insert(linkVerts.end(), l.b, l.b + 3);
						linkRad.push_back(l.radius > 0.0f ? l.radius : std::max(0.5f, radius));
						linkFlags.push_back(NAV_FLAG_LINK);
						linkAreas.push_back(0);
						linkDir.push_back(l.bidir ? 1 : 0);
						linkIds.push_back((unsigned int)i);
					}

					dtNavMeshCreateParams params;
					memset(&params, 0, sizeof(params));
					params.verts = pmesh->verts;
					params.vertCount = pmesh->nverts;
					params.polys = pmesh->polys;
					params.polyFlags = flags.data();
					params.polyAreas = pmesh->areas;
					params.polyCount = pmesh->npolys;
					params.nvp = pmesh->nvp;
					params.detailMeshes = dmesh->meshes;
					params.detailVerts = dmesh->verts;
					params.detailVertsCount = dmesh->nverts;
					params.detailTris = dmesh->tris;
					params.detailTriCount = dmesh->ntris;
					params.offMeshConVerts = linkVerts.empty() ? 0 : linkVerts.data();
					params.offMeshConRad = linkRad.empty() ? 0 : linkRad.data();
					params.offMeshConFlags = linkFlags.empty() ? 0 : linkFlags.data();
					params.offMeshConAreas = linkAreas.empty() ? 0 : linkAreas.data();
					params.offMeshConDir = linkDir.empty() ? 0 : linkDir.data();
					params.offMeshConUserID = linkIds.empty() ? 0 : linkIds.data();
					params.offMeshConCount = (int)linkIds.size();
					params.walkableHeight = height;
					params.walkableRadius = radius;
					params.walkableClimb = step;
					params.cs = pmesh->cs;
					params.ch = pmesh->ch;
					params.buildBvTree = true;
					for (int k = 0; k < 3; ++k) {
						params.bmin[k] = pmesh->bmin[k];
						params.bmax[k] = pmesh->bmax[k];
					}
					ok = dtCreateNavMeshData(&params, &data, &dataSize) != 0;
				}

				if (ok) {
					std::vector<NavMesh::LinkMeta> meta = navMetaFromBuild(n);
					ok = navAdoptData(n, data, dataSize, std::move(meta));
				}
			}
		}

		rcFreeHeightField(hf);
		rcFreeCompactHeightfield(chf);
		rcFreeContourSet(cset);
		rcFreePolyMesh(pmesh);
		rcFreePolyMeshDetail(dmesh);
		return ok;
	}

	int navWritePoints(bbBank* bank, const float* points, int count) {
		if (!bank || !bbVerifyBank(bank)) return 0;
		int cap = bbBankSize(bank) / 12;
		if (count > cap) count = cap;
		for (int i = 0; i < count; ++i) {
			bbPokeFloat(bank, i * 12 + 0, points[i * 3 + 0]);
			bbPokeFloat(bank, i * 12 + 4, points[i * 3 + 1]);
			bbPokeFloat(bank, i * 12 + 8, points[i * 3 + 2]);
		}
		return count;
	}

	NavMesh* bbNavCreate(float radius, float height, float step) {
		NavMesh* n = new NavMesh();
		n->agentRadius = std::max(0.01f, radius);
		n->agentHeight = std::max(0.1f, height);
		n->stepHeight = std::max(0.0f, step);
		nav_set.insert(n);
		return n;
	}

	void bbNavFree(NavMesh* n) {
		if (!navValid(n)) return;
		nav_set.erase(n);
		delete n;
	}

	int bbNavPolyCount(NavMesh* n) {
		return navValid(n) ? n->polyCount : 0;
	}

	int bbNavVertCount(NavMesh* n) {
		return navValid(n) ? n->vertCount : 0;
	}

	int bbNavNearestPoly(NavMesh* n, float x, float y, float z) {
		if (!navValid(n) || !n->mesh) return -1;
		dtPolyRef ref = 0;
		float p[3] = { x, y, z }, nearest[3];
		bool over = false;
		if (!navNearest(n, p, &ref, nearest, &over)) return -1;
		int poly = (int)n->mesh->decodePolyIdPoly(ref);
		return poly < n->offMeshBase ? poly : -1;
	}

	float bbNavHeightAt(NavMesh* n, float x, float z) {
		if (!navValid(n) || !n->mesh) return std::numeric_limits<float>::infinity();
		dtQueryFilter f = navFilter(n, NAV_FLAG_WALK);
		dtPolyRef ref = 0;
		float p[3] = { x, 0, z }, nearest[3];
		float ext[3] = { 2.0f, 10000.0f, 2.0f };
		bool over = false;
		if (dtStatusFailed(n->query->findNearestPoly(p, ext, &f, &ref, nearest, &over)) || !ref || !over) return std::numeric_limits<float>::infinity();
		float closest[3];
		if (dtStatusFailed(n->query->closestPointOnPoly(ref, p, closest, 0))) return std::numeric_limits<float>::infinity();
		return closest[1];
	}

	int bbNavFindPath(NavMesh* n, bbBank* bank, float sx, float sy, float sz, float ex, float ey, float ez, int maxPoints) {
		if (!navValid(n) || !n->mesh || !n->query || maxPoints <= 0) return 0;
		dtQueryFilter f = navFilter(n, NAV_FLAG_WALK | NAV_FLAG_LINK);
		float sp[3] = { sx, sy, sz }, ep[3] = { ex, ey, ez };
		float sn[3], en[3];
		bool over = false;
		dtPolyRef sref = 0, eref = 0;
		if (!navNearest(n, sp, &sref, sn, &over)) return 0;
		if (!navNearest(n, ep, &eref, en, &over)) return 0;

		dtPolyRef polys[NAV_MAX_PATH];
		int npolys = 0;
		if (dtStatusFailed(n->query->findPath(sref, eref, sn, en, &f, polys, &npolys, NAV_MAX_PATH)) || !npolys) return 0;

		float straight[NAV_MAX_PATH * 3];
		unsigned char flags[NAV_MAX_PATH];
		int nstraight = 0;
		if (dtStatusFailed(n->query->findStraightPath(sn, en, polys, npolys, straight, flags, 0, &nstraight, NAV_MAX_PATH)) || nstraight <= 0) return 0;

		nstraight = std::min(nstraight, maxPoints);
		return navWritePoints(bank, straight, nstraight);
	}

	int bbNavRaycast(NavMesh* n, float ax, float ay, float az, float bx, float by, float bz) {
		if (!navValid(n) || !n->mesh) return 0;
		dtQueryFilter f = navFilter(n, NAV_FLAG_WALK);
		float sp[3] = { ax, ay, az }, ep[3] = { bx, by, bz }, nearest[3];
		bool over = false;
		dtPolyRef sref = 0;
		if (!navNearest(n, sp, &sref, nearest, &over)) return 0;

		dtRaycastHit hit;
		memset(&hit, 0, sizeof(hit));
		if (dtStatusFailed(n->query->raycast(sref, nearest, ep, &f, 0, &hit))) return 0;
		return hit.t >= 1.0f;
	}

	bool navSamplePoly(const NavMesh* n, int poly, float* p) {
		const dtPoly& dp = n->tile->polys[poly];
		int nv = dp.vertCount;
		if (nv < 3) return false;
		int i = rand() % (nv - 2);
		const float* a = &n->tile->verts[dp.verts[0] * 3];
		const float* b = &n->tile->verts[dp.verts[i + 1] * 3];
		const float* c = &n->tile->verts[dp.verts[i + 2] * 3];
		float r1 = (rand() & 0x7fff) / 32767.0f;
		float r2 = (rand() & 0x7fff) / 32767.0f;
		if (r1 + r2 > 1.0f) { r1 = 1.0f - r1; r2 = 1.0f - r2; }
		for (int k = 0; k < 3; ++k) p[k] = a[k] + (b[k] - a[k]) * r1 + (c[k] - a[k]) * r2;
		return true;
	}

	int bbNavRandomPoint(NavMesh* n, bbBank* bank, int poly) {
		if (!navValid(n) || !n->mesh || !n->tile || n->polyCount <= 0) return 0;
		float p[3];
		if (poly < 0) poly = rand() % n->polyCount;
		if (!navPolyRef(n, poly) || !navSamplePoly(n, poly, p)) return 0;
		return navWritePoints(bank, p, 1);
	}

	int bbNavPolyArea(NavMesh* n, int poly) {
		if (!navValid(n)) return 0;
		dtPolyRef ref = navPolyRef(n, poly);
		unsigned char area = 0;
		if (!ref || dtStatusFailed(n->mesh->getPolyArea(ref, &area))) return 0;
		return area;
	}

	void bbNavSetAreaCost(NavMesh* n, int area, float cost) {
		if (!navValid(n) || area < 0 || area > 63) return;
		n->filter.setAreaCost(area, std::max(0.0f, cost));
	}

	void bbNavSetPolyBlocked(NavMesh* n, int poly, int state) {
		if (!navValid(n) || poly < 0 || poly >= n->polyCount) return;
		n->userBlocked[poly] = state ? 1 : 0;
		navApplyPoly(n, poly);
	}

	int bbNavLinkCount(NavMesh* n) {
		return navValid(n) ? (int)n->links.size() : 0;
	}

	int bbNavLinkEnabled(NavMesh* n, int link) {
		if (!navValid(n) || link < 0 || link >= (int)n->linkRefs.size()) return 0;
		unsigned short flags = 0;
		dtPolyRef ref = n->linkRefs[link];
		if (!ref || dtStatusFailed(n->mesh->getPolyFlags(ref, &flags))) return 0;
		return (flags & NAV_FLAG_LINK) != 0;
	}

	void bbNavSetLinkEnabled(NavMesh* n, int link, int state) {
		if (!navValid(n) || link < 0 || link >= (int)n->linkRefs.size()) return;
		bool on = state != 0;
		if ((n->linkOn[link] != 0) == on) return;
		n->linkOn[link] = on ? 1 : 0;

		for (dtPolyRef ref : n->linkPolys[link]) {
			int poly = (int)n->mesh->decodePolyIdPoly(ref);
			if (on) {
				if (n->linkBlockCount[poly] > 0) --n->linkBlockCount[poly];
			}
			else ++n->linkBlockCount[poly];
			navApplyPoly(n, poly);
		}

		dtPolyRef ref = n->linkRefs[link];
		if (ref) n->mesh->setPolyFlags(ref, on ? NAV_FLAG_LINK : 0);
	}

	int bbNavLinkKind(NavMesh* n, int link) {
		if (!navValid(n) || link < 0 || link >= (int)n->links.size()) return 0;
		return n->links[link].kind;
	}

	BBStr* bbNavLinkName(NavMesh* n, int link) {
		if (!navValid(n) || link < 0 || link >= (int)n->links.size()) return new BBStr("");
		return new BBStr(n->links[link].name);
	}

	int bbNavAddVertex(NavMesh* n, float x, float y, float z) {
		if (!navValid(n) || n->mesh) return -1;
		n->buildVerts.push_back(x);
		n->buildVerts.push_back(y);
		n->buildVerts.push_back(z);
		return (int)n->buildVerts.size() / 3 - 1;
	}

	int bbNavAddPoly(NavMesh* n, bbBank* bank, int area) {
		if (!navValid(n) || n->mesh || !bank || !bbVerifyBank(bank)) return 0;
		int count = bbBankSize(bank) / 4;
		if (count < 3 || count > 8) return 0;
		NavMesh::BuildPoly p;
		int nverts = (int)n->buildVerts.size() / 3;
		for (int i = 0; i < count; ++i) {
			int v = bbPeekInt(bank, i * 4);
			if (v < 0 || v >= nverts) return 0;
			p.verts.push_back(v);
		}
		p.area = (unsigned char)(area < 0 ? 0 : (area > 63 ? 63 : area));
		n->buildPolys.push_back(std::move(p));
		return 1;
	}

	void bbNavAddLink(NavMesh* n, float ax, float ay, float az, float bx, float by, float bz, int kind, int bidir, float radius, BBStr* name) {
		if (!navValid(n)) {
			delete name;
			return;
		}
		NavMesh::BuildLink l;
		l.a[0] = ax; l.a[1] = ay; l.a[2] = az;
		l.b[0] = bx; l.b[1] = by; l.b[2] = bz;
		l.kind = kind;
		l.bidir = bidir != 0;
		l.radius = radius;
		l.name = *name;
		delete name;
		n->buildLinks.push_back(std::move(l));
	}

	int bbNavFinalize(NavMesh* n) {
		if (!navValid(n)) return 0;
		unsigned char* data = 0;
		int size = 0;
		if (!navBuildPolyMesh(n, &data, &size)) return 0;
		if (!navAdoptData(n, data, size, navMetaFromBuild(n))) return 0;
		n->buildVerts.clear();
		n->buildPolys.clear();
		n->buildLinks.clear();
		return 1;
	}

	int bbNavBakeWorld(NavMesh* n, float cellSize, float radius, float height, float maxSlope, float step, int collisionType) {
		if (!navValid(n)) return 0;
		cellSize = std::max(0.05f, std::min(2.0f, cellSize));
		radius = std::max(0.01f, std::min(2.0f, radius));
		height = std::max(0.5f, std::min(5.0f, height));
		maxSlope = std::max(1.0f, std::min(89.0f, maxSlope));
		step = std::max(0.0f, std::min(2.0f, step));
		if (!navBake(n, cellSize, radius, height, maxSlope, step, collisionType)) {
			gx_runtime->debugLog("NavBakeWorld: bake failed (no walkable collidable geometry?)");
			return 0;
		}
		n->buildVerts.clear();
		n->buildPolys.clear();
		n->buildLinks.clear();
		return 1;
	}

	int bbNavSave(NavMesh* n, BBStr* path) {
		std::string file = *path;
		delete path;
		if (!navValid(n) || !n->mesh || !n->tile || !n->tile->data) return 0;

		std::vector<unsigned char> out;
		const char magic[8] = { 'B', 'N', 'A', 'V', 0, 0, 0, 0 };
		out.insert(out.end(), magic, magic + 8);
		putU32(out, 0x00010000);
		putU32(out, 0);
		putU32(out, (unsigned)n->tile->dataSize);
		putU32(out, (unsigned)n->links.size());

		for (int p = 0; p < n->polyCount; ++p) n->mesh->setPolyFlags(n->mesh->getPolyRefBase(n->tile) | (dtPolyRef)p, NAV_FLAG_WALK);
		for (dtPolyRef ref : n->linkRefs) if (ref) n->mesh->setPolyFlags(ref, NAV_FLAG_LINK);

		out.insert(out.end(), n->tile->data, n->tile->data + n->tile->dataSize);

		for (int p = 0; p < n->polyCount; ++p) navApplyPoly(n, p);
		for (size_t i = 0; i < n->linkRefs.size(); ++i) {
			if (n->linkRefs[i]) n->mesh->setPolyFlags(n->linkRefs[i], n->linkOn[i] ? NAV_FLAG_LINK : 0);
		}
		for (const auto& l : n->links) {
			putU32(out, (unsigned)l.kind);
			putU32(out, (unsigned)l.name.size());
			out.insert(out.end(), l.name.begin(), l.name.end());
		}
		return navWriteFile(file, out.data(), out.size()) ? 1 : 0;
	}

	bool navLoadFile(NavMesh* n, const std::string& file) {
		std::vector<unsigned char> in;
		if (!navReadFile(file, in) || in.size() < 24) return 0;
		if (memcmp(in.data(), "BNAV", 4) != 0) return 0;
		unsigned version = getU32(&in[8]);
		if ((version >> 16) != 1) return 0;
		unsigned dataSize = getU32(&in[16]);
		unsigned linkCount = getU32(&in[20]);
		if (linkCount > 65535) return 0;
		if (dataSize == 0 || (unsigned long long)24 + dataSize + (unsigned long long)linkCount * 8 > in.size()) return 0;

		std::vector<NavMesh::LinkMeta> meta;
		meta.reserve(linkCount);
		size_t at = 24 + (size_t)dataSize;
		for (unsigned i = 0; i < linkCount; ++i) {
			if (at > in.size() || in.size() - at < 8) return 0;
			NavMesh::LinkMeta m;
			m.kind = (int)getU32(&in[at]);
			unsigned len = getU32(&in[at + 4]);
			at += 8;
			if (len > in.size() - at) return 0;
			m.name.assign((const char*)&in[at], len);
			at += len;
			meta.push_back(std::move(m));
		}

		unsigned char* data = (unsigned char*)dtAlloc((int)dataSize, DT_ALLOC_PERM);
		if (!data) return 0;
		memcpy(data, &in[24], dataSize);
		if (!navAdoptData(n, data, (int)dataSize, std::move(meta))) return false;
		n->buildVerts.clear();
		n->buildPolys.clear();
		n->buildLinks.clear();
		return true;
	}

	NavMesh* bbNavLoad(BBStr* path) {
		std::string file = *path;
		delete path;
		NavMesh* n = bbNavCreate(0.3f, 1.8f, 0.25f);
		if (!navLoadFile(n, file)) {
			bbNavFree(n);
			return 0;
		}
		return n;
	}

	int bbNavLoadInto(NavMesh* n, BBStr* path) {
		std::string file = *path;
		delete path;
		if (!navValid(n)) return 0;
		return navLoadFile(n, file) ? 1 : 0;
	}

}

bool nav_create() {
	return true;
}

bool nav_destroy() {
	while (!nav_set.empty()) bbNavFree(*nav_set.begin());
	return true;
}

void nav_link(void (*rtSym)(const char* sym, void* pc)) {
	rtSym("%NavCreate#agentRadius=0.3#agentHeight=1.8#stepHeight=0.25", bbNavCreate);
	rtSym("NavFree%nav", bbNavFree);
	rtSym("%NavLoad$file", bbNavLoad);
	rtSym("%NavLoadInto%nav$file", bbNavLoadInto);
	rtSym("%NavSave%nav$file", bbNavSave);
	rtSym("%NavPolyCount%nav", bbNavPolyCount);
	rtSym("%NavVertCount%nav", bbNavVertCount);
	rtSym("%NavNearestPoly%nav#x#y#z", bbNavNearestPoly);
	rtSym("#NavHeightAt%nav#x#z", bbNavHeightAt);
	rtSym("%NavFindPath%nav%bank#sx#sy#sz#ex#ey#ez%maxPoints", bbNavFindPath);
	rtSym("%NavRaycast%nav#ax#ay#az#bx#by#bz", bbNavRaycast);
	rtSym("%NavRandomPoint%nav%bank%poly=-1", bbNavRandomPoint);
	rtSym("%NavPolyArea%nav%poly", bbNavPolyArea);
	rtSym("NavSetAreaCost%nav%area#cost", bbNavSetAreaCost);
	rtSym("NavSetPolyBlocked%nav%poly%state", bbNavSetPolyBlocked);
	rtSym("%NavLinkCount%nav", bbNavLinkCount);
	rtSym("%NavLinkEnabled%nav%link", bbNavLinkEnabled);
	rtSym("NavSetLinkEnabled%nav%link%state", bbNavSetLinkEnabled);
	rtSym("%NavLinkKind%nav%link", bbNavLinkKind);
	rtSym("$NavLinkName%nav%link", bbNavLinkName);
	rtSym("%NavAddVertex%nav#x#y#z", bbNavAddVertex);
	rtSym("%NavAddPoly%nav%bank%area=0", bbNavAddPoly);
	rtSym("NavAddLink%nav#ax#ay#az#bx#by#bz%kind=0%bidir=0#radius=0$name=\"\"", bbNavAddLink);
	rtSym("%NavFinalize%nav", bbNavFinalize);
	rtSym("%NavBakeWorld%nav#cellSize=0.3#agentRadius=0.3#agentHeight=1.8#maxSlope=45#stepHeight=0.25%collisionType=-1", bbNavBakeWorld);
}
