#include "std.h"
#include <algorithm>
#include <queue>
#include <thread>
#include <utility>
#include <vector>
#include "world.h"

//0=tris compared for collision
//1=max proj err of terrain
float stats3d[10];

extern gxScene* gx_scene;
extern gxRuntime* gx_runtime;

static std::vector<Object*> _enabled, _visible;

static void enumEnabled() {
	_enabled.clear();
	for(Entity* e = Entity::orphans(); e; e = e->successor()) {
		e->enumEnabled(_enabled);
	}
}

static void enumVisible() {
	_visible.clear();
	for(Entity* e = Entity::orphans(); e; e = e->successor()) {
		e->enumVisible(_visible);
	}
}

/******************************* Update *******************************/

static std::unordered_map<int, std::vector<Object*>> _objsByType;

static std::vector<ObjCollision*> free_colls, used_colls;

static ObjCollision* allocObjColl(Object* with, const Vector& coords, const Collision& coll) {
	ObjCollision* c;
	if(free_colls.size()) {
		c = free_colls.back();
		free_colls.pop_back();
	}
	else {
		c = new ObjCollision();
	}
	used_colls.push_back(c);
	c->with = with;
	c->coords = coords;
	c->collision = coll;
	return c;
}

static void collided(Object* src, Object* dest, const Line& line,
	const Collision& coll, float y_scale) {
	const Vector& coords = line * coll.time - coll.normal * src->getCollisionRadii().x;

	ObjCollision* c = allocObjColl(dest, coords, coll);
	c->coords.y *= y_scale;
	src->addCollision(c);

	c = allocObjColl(src, coords, coll);
	c->coords.y *= y_scale;
	dest->addCollision(c);
}

void World::clearCollisions() {
	Scene* s = g_sceneManager.get(g_sceneManager.currentSceneId);
	if (!s) s = g_sceneManager.get(0);
	if (s) s->collisions.clear();
}

void World::addCollision(int src_type, int dst_type, int method, int response) {
	Scene* s = g_sceneManager.get(g_sceneManager.currentSceneId);
	if (!s) s = g_sceneManager.get(0);
	if (!s) return;
	auto& info = s->collisions[src_type];
	for (const auto& t : info) {
		if (dst_type == t.dst_type) return;
	}
	info.push_back({ dst_type, method, response });
}


bool World::hitTest(const Line& line, float radius, Object* obj, const Transform& tf, int method, Collision* curr_coll) {
switch(method) {
	case COLLISION_METHOD_SPHERE:
		return curr_coll->sphereCollide(line, radius, tf.v, obj->getCollisionRadii().x);
	case COLLISION_METHOD_POLYGON:
		return obj->collide(line, radius, curr_coll, tf);
	case COLLISION_METHOD_BOX:
		Transform t = tf;
		t.m.i.normalize(); t.m.j.normalize(); t.m.k.normalize();
		if(curr_coll->boxCollide(~t * line, radius, obj->getCollisionBox())) {
			curr_coll->normal = t.m * curr_coll->normal;
			return true;
		}
	}
	return false;
}

bool World::checkLOS(Object* src, Object* dest) {

	enumEnabled();

	Collision curr_coll;

	Line line(src->getWorldPosition(), dest->getWorldPosition() - src->getWorldPosition());

	for (Object* obj : _enabled) {
		if (obj == src || obj == dest || !obj->getPickGeometry() || !obj->getObscurer())
			continue;
		if (hitTest(line, 0, obj, obj->getWorldTform(), obj->getPickGeometry(), &curr_coll))
			return false;
	}
	return true;
}

Object* World::traceRay(const Line& line, float radius, ObjCollision* curr_coll) {

	enumEnabled();

	Object* coll_obj = nullptr;

	for (Object* obj : _enabled) {
		if (!obj->getPickGeometry()) continue;
		if (hitTest(line, radius, obj, obj->getWorldTform(),
			obj->getPickGeometry(), &curr_coll->collision)) {
			coll_obj = obj;
		}
	}
	if(curr_coll->with = coll_obj) {
		curr_coll->coords = line * curr_coll->collision.time - curr_coll->collision.normal * radius;
	}
	return coll_obj;
}

//
// NEW VERSION
//
void World::collide(Object* src, const std::vector<CollInfo>& collinfos, const std::unordered_map<int, std::vector<Object*>>& objsByType) {

	static const int MAX_HITS = 10;

	Vector dv = src->getWorldTform().v;
	Vector sv = src->getPrevWorldTform().v;

	if(sv == dv) {
		if(dv.x != sv.x || dv.y != sv.y || dv.z != sv.z) {
			src->setWorldPosition(sv);
		}
		return;
	}

	Vector panic = sv;

	static Transform y_tform;

	const Vector& radii = src->getCollisionRadii();

	float radius = radii.x, inv_y_scale;
	float y_scale = inv_y_scale = y_tform.m.j.y = 1;

	if(radii.x != radii.y) {
		y_scale = y_tform.m.j.y = radius / radii.y;
		inv_y_scale = 1 / y_scale;
		sv.y *= y_scale;
		dv.y *= y_scale;
	}

	int n_hit = 0;
	Plane planes[2];
	Line coll_line(sv, dv - sv);
	Vector dir = coll_line.d;

	float td = coll_line.d.length();
	float td_xz = Vector(coll_line.d.x, 0, coll_line.d.z).length();

	int hits = 0;
	for(;;) {

		Collision coll;
		Object* coll_obj = nullptr;
		const CollInfo* winning_info = nullptr;

		for (const auto& ci : collinfos) {
			auto dst_it = objsByType.find(ci.dst_type);
			if (dst_it == objsByType.end()) continue;

			for (Object* dst : dst_it->second) {
				if (src == dst) continue;
				const Transform& dst_tform = dst->getPrevWorldTform();
				bool hit = (y_scale == 1) ?
					hitTest(coll_line, radius, dst, dst_tform, ci.method, &coll) :
					hitTest(coll_line, radius, dst, y_tform * dst_tform, ci.method, &coll);
				if (hit) {
					coll_obj = dst;
					winning_info = &ci;
				}
			}
		}
		if(!coll_obj) break;

		//register collision
		if(++hits == MAX_HITS) {
			//			exit(0);
			break;
		}

		collided(src, coll_obj, coll_line, coll, inv_y_scale);

		Plane coll_plane(coll_line * coll.time, coll.normal);

		coll_plane.d -= COLLISION_EPSILON;
		coll.time = coll_plane.t_intersect(coll_line);

		if(coll.time > 0) {
			//update source position - ONLY IF AHEAD!
			sv = coll_line * coll.time;
			td *= 1 - coll.time;
			td_xz *= 1 - coll.time;
		}

		if(winning_info->response == COLLISION_RESPONSE_STOP) {
			dv = sv;
			break;
		}

		//find nearest point on plane to dest
		Vector nv = coll_plane.nearest(dv);

		if(n_hit == 0) {
			dv = nv;
		}
		else if(n_hit == 1) {
			if(planes[0].distance(nv) >= 0) {
				dv = nv; n_hit = 0;
			}
			else if(fabs(planes[0].n.dot(coll_plane.n)) < 1 - EPSILON) {
				dv = coll_plane.intersect(planes[0]).nearest(dv);
			}
			else {
				//SQUISHED!
				hits = MAX_HITS; break;
			}
		}
		else if(planes[0].distance(nv) >= 0 && planes[1].distance(nv) >= 0) {
			dv = nv; n_hit = 0;
		}
		else {
			dv = sv; break;
		}

		Vector dd(dv - sv);

		//going behind initial direction? really necessary?
		if(dd.dot(dir) <= 0) { dv = sv; break; }

		if(winning_info->response == COLLISION_RESPONSE_SLIDE) {
			float d = dd.length();
			if(d <= EPSILON) { dv = sv; break; }
			if(d > td) dd *= td / d;
		}
		else if (winning_info->response == COLLISION_RESPONSE_SLIDEXZ) {
			float d = Vector(dd.x, 0, dd.z).length();
			if(d <= EPSILON) { dv = sv; break; }
			if(d > td_xz) dd *= td_xz / d;
		}

		coll_line.o = sv;
		coll_line.d = dd; dv = sv + dd;
		planes[n_hit++] = coll_plane;
	}

	if(hits) {
		if(hits < MAX_HITS) {
			dv.y *= inv_y_scale;
			src->setWorldPosition(dv);
		}
		else {
			src->setWorldPosition(panic);
		}
	}
}

void World::update(float elapsed) {

	stats3d[0] = 0;

	for(; used_colls.size(); used_colls.pop_back()) {
		free_colls.push_back(used_colls.back());
	}

	enumEnabled();

	Scene* curr = g_sceneManager.get(g_sceneManager.currentSceneId);
	if (!curr) curr = g_sceneManager.get(0);
	if (!curr) return; // should never happen

	std::unordered_map<int, std::vector<Object*>> objsByType;
	for (Object* o : _enabled) {
		if (int n = o->getCollisionType())
			objsByType[n].push_back(o);
	}

	for (Object* o : _enabled) {
		o->beginUpdate(elapsed);
		if (o->getCollisionType()) {
			auto it = curr->collisions.find(o->getCollisionType());
			if (it != curr->collisions.end()) {
				collide(o, it->second, objsByType);
			}
		}
		o->endUpdate();
	}
}

/****************************** Render *********************************/

static Transform cam_tform;		//current camera transform

static std::vector<gxLight*> _lights;
static std::vector<Mirror*> _mirrors;
static std::vector<Listener*> _listeners;

struct OrderComp {
	bool operator()(Object* a, Object* b) {
		return a->getOrder() < b->getOrder();
	}
};

struct TransComp {
	bool operator()(Model* a, Model* b)const {
		return
			cam_tform.v.distance(a->getRenderTform().v) <
			cam_tform.v.distance(b->getRenderTform().v);
	}
};

namespace {

constexpr size_t kParThreshold = 256;

template<typename Fn>
void parallel_for(size_t n, Fn&& fn) {
	if(!n) return;
	unsigned hc = std::thread::hardware_concurrency();
	if(hc <= 1 || n < kParThreshold) {
		for(size_t i = 0; i < n; ++i) fn(i);
		return;
	}
	size_t num_threads = hc > n ? n : hc;
	size_t chunk = (n + num_threads - 1) / num_threads;
	std::vector<std::thread> workers;
	workers.reserve(num_threads - 1);
	for(size_t t = 1; t < num_threads; ++t) {
		size_t begin = t * chunk;
		size_t end = begin + chunk > n ? n : begin + chunk;
		if(begin >= end) break;
		workers.emplace_back([&, begin, end] {
			for(size_t i = begin; i < end; ++i) fn(i);
		});
	}
	size_t end0 = chunk > n ? n : chunk;
	for(size_t i = 0; i < end0; ++i) fn(i);
	for(auto& w : workers) w.join();
}

}

static std::vector<Model*> ord_mods, unord_mods;

static std::priority_queue<Model*, std::vector<Model*>, OrderComp> ord_que;

static std::priority_queue<Camera*, std::vector<Camera*>, OrderComp> cam_que;

static std::priority_queue<Model*, std::vector<Model*>, TransComp> transparents;

void World::capture() {

	enumVisible();

	for (Object* o : _visible) o->capture();
}

void World::render(float tween) {

	//set render tweens, and build ordered and unordered model lists...
	ord_mods.clear();
	unord_mods.clear();

	_visible.clear();

	Scene* curr = g_sceneManager.get(g_sceneManager.currentSceneId);
	if (!curr) curr = g_sceneManager.get(0);
	if (!curr) return; // should never happen
	curr->lights.clear();
	curr->mirrors.clear();
	curr->listeners.clear();

	enumVisible();

	for (Object* o : _visible) {
		if (!o->beginRender(tween)) continue;

		if (Light* t = o->getLight())    curr->lights.push_back(t->getGxLight());
		else if (Camera* t = o->getCamera())   cam_que.push(t);
		else if (Mirror* t = o->getMirror())   curr->mirrors.push_back(t);
		else if (Listener* t = o->getListener()) curr->listeners.push_back(t);
		else if (Model* t = o->getModel()) {
			if (t->getOrder()) ord_que.push(t);
			else               unord_mods.push_back(t);
		}
	}

	while (!ord_que.empty()) { ord_mods.push_back(ord_que.top()); ord_que.pop(); }

	if (!gx_scene->begin(curr->lights)) return;

	while (!cam_que.empty()) {
		Camera* cam = cam_que.top(); cam_que.pop();
		if (!cam->beginRenderFrame()) continue;

		for (Mirror* mir : curr->mirrors) render(cam, mir);
		render(cam, nullptr);
	}

	gx_scene->end();

	for (Listener* lis : curr->listeners) lis->renderListener();
}

void World::render(Camera* cam, Mirror* mirror) {

	if(mirror) {
		const Transform& t = mirror->getRenderTform();
		cam_tform = t * Transform(scaleMatrix(1, -1, 1)) * -t * cam->getRenderTform();
		gx_scene->setFlippedTris(true);
	}
	else {
		cam_tform = cam->getRenderTform();
		gx_scene->setFlippedTris(false);
	}

	//set camera matrix
	gx_scene->setViewMatrix((gxScene::Matrix*)&(-cam_tform));
	gx_scene->setEyePosition(&cam_tform.v.x);

	//initialize render context
	RenderContext rc(cam_tform, cam->getFrustum(), mirror != 0);

	//draw everything in order
	int ord = 0;
	gx_scene->setZMode(gxScene::ZMODE_DISABLE);
	while(ord < static_cast<int>(ord_mods.size()) && ord_mods[ord]->getOrder() > 0) {
		Model* mod = ord_mods[ord++];
		if(!mod->doAutoFade(cam_tform.v)) continue;
		render(mod, rc);
		flushTransparent();
	}

	gx_scene->setZMode(gxScene::ZMODE_NORMAL);
	std::vector<char> fadeOk(unord_mods.size());
	parallel_for(unord_mods.size(), [&](size_t i) {
		fadeOk[i] = unord_mods[i]->doAutoFade(cam_tform.v);
	});
	std::map<Brush, std::vector<Model*>> buckets;
	for (size_t i = 0; i < unord_mods.size(); ++i) {
		if(!fadeOk[i]) continue;
		buckets[unord_mods[i]->getBrush()].push_back(unord_mods[i]);
	}
	for (auto& bucket : buckets) {
		for (Model* mod : bucket.second) {
			render(mod, rc);
		}
	}
	gx_scene->setZMode(gxScene::ZMODE_CMPONLY);
	flushTransparent();

	gx_scene->setZMode(gxScene::ZMODE_DISABLE);
	while(ord < static_cast<int>(ord_mods.size())) {
		Model* mod = ord_mods[ord++];
		if(!mod->doAutoFade(cam_tform.v)) continue;
		render(mod, rc);
		flushTransparent();
	}
}

void World::render(Model* mod, const RenderContext& rc) {

	bool trans = mod->render(rc);

	if(mod->queueSize(Model::QUEUE_OPAQUE)) {
		if(mod->getRenderSpace() == Model::RENDER_SPACE_LOCAL) {
			gx_scene->setWorldMatrix((gxScene::Matrix*)&mod->getRenderTform());
		}
		else {
			gx_scene->setWorldMatrix(0);
		}
		mod->renderQueue(Model::QUEUE_OPAQUE);
	}

	if(trans || mod->queueSize(Model::QUEUE_TRANSPARENT)) {
		transparents.push_back(mod);
	}
}

void World::renderEntity(Camera* cam, float tween) {
	ord_mods.clear();
	unord_mods.clear();

	_visible.clear();
	_lights.clear();
	_mirrors.clear();
	_listeners.clear();

	enumVisible();

	for (Object* o : _visible) {
		if (!o->beginRender(tween)) continue;
		if (Light* t = o->getLight())    _lights.push_back(t->getGxLight());
		else if (Mirror* t = o->getMirror())   _mirrors.push_back(t);
		else if (Model* t = o->getModel()) {
			if (t->getOrder()) ord_que.push(t);
			else               unord_mods.push_back(t);
		}
	}

	while (!ord_que.empty()) { ord_mods.push_back(ord_que.top()); ord_que.pop(); }

	if (!gx_scene->begin(_lights)) return;

	if (cam->beginRenderFrame()) {
		for (Mirror* mir : _mirrors) render(cam, mir);
		render(cam, nullptr);
	}

	gx_scene->end();
}

void World::renderEntity(Camera* cam, Entity* target, float tween) {
	ord_mods.clear();
	unord_mods.clear();

	_visible.clear();
	_lights.clear();
	_mirrors.clear();
	_listeners.clear();

	if (target) target->enumVisible(_visible);

	for (Object* o : _visible) {
		if (!o->beginRender(tween)) continue;
		if (Light* t = o->getLight())    _lights.push_back(t->getGxLight());
		else if (Mirror* t = o->getMirror())   _mirrors.push_back(t);
		else if (Model* t = o->getModel()) {
			if (t->getOrder()) ord_que.push(t);
			else               unord_mods.push_back(t);
		}
	}

	while (!ord_que.empty()) { ord_mods.push_back(ord_que.top()); ord_que.pop(); }

	if (!gx_scene->begin(_lights)) return;

	if (cam->beginRenderFrame()) {
		for (Mirror* mir : _mirrors) render(cam, mir);
		render(cam, nullptr);
	}

	gx_scene->end();
}

void World::flushTransparent() {
	size_t n = transparents.size();
	if(!n) return;
	std::vector<std::pair<float, Model*>> items(n);
	parallel_for(n, [&](size_t i) {
		Model* mod = transparents[i];
		items[i] = { cam_tform.v.distance(mod->getRenderTform().v), mod };
	});
	std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
		return a.first > b.first;
	});
	bool local = true;
	for (auto& it : items) {
		Model* mod = it.second;
		if (mod->getRenderSpace() == Model::RENDER_SPACE_LOCAL) {
			gx_scene->setWorldMatrix((gxScene::Matrix*)&mod->getRenderTform());
			local = true;
		}
		else if(local) {
			gx_scene->setWorldMatrix(0);
			local = false;
		}
		mod->renderQueue(Model::QUEUE_TRANSPARENT);
	}
	transparents.clear();
}