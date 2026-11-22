#include "std.h"
#include "model.h"
#include "sprite.h"
#include <algorithm>

extern gxScene* gx_scene;
extern gxGraphics* gx_graphics;

class Model::MeshQueue {
	friend class Model;
	union {
		gxMesh* mesh;
		MeshQueue* next;
	};
	int fv, vc, ft, tc;
	Brush brush;
	gxEffect* effect;
	int q_type;
	uint64_t stateKey;
	std::vector<float> bone_data;
	int bone_cnt;
	Model* owner;
	bool instance_ok;

	static MeshQueue* pool;

public:
	MeshQueue() {}

	MeshQueue(Model* o, gxMesh* m, int fv, int vc, int ft, int tc, const Brush& b, gxEffect* e, uint64_t key, bool inst) :
		mesh(m), fv(fv), vc(vc), ft(ft), tc(tc), brush(b), effect(e), stateKey(key), bone_cnt(0), owner(o), instance_ok(inst) {
		int n = brush.getBlend();
		q_type = (n == gxScene::BLEND_REPLACE) ? QUEUE_OPAQUE : QUEUE_TRANSPARENT;
	}

	MeshQueue(Model* o, gxMesh* m, int fv, int vc, int ft, int tc, const Brush& b, gxEffect* e, uint64_t key,
		const float* bones, int n_bones) :
		mesh(m), fv(fv), vc(vc), ft(ft), tc(tc), brush(b), effect(e), stateKey(key),
		bone_data(bones, bones + n_bones * 12), bone_cnt(n_bones), owner(o), instance_ok(false) {
		int n = brush.getBlend();
		q_type = (n == gxScene::BLEND_REPLACE) ? QUEUE_OPAQUE : QUEUE_TRANSPARENT;
	}

	int getQueueType()const {
		return q_type;
	}

	const Brush& getBrush() const { return brush; }
	uint64_t getStateKey() const { return stateKey; }

	void render() {
		gx_scene->setRenderState(brush.getRenderState());
		if (bone_cnt > 0) {
			gx_scene->setEffect(nullptr);
			gx_scene->renderSkinned(mesh, fv, vc, ft, tc, bone_data.data(), bone_cnt);
			return;
		}
		Sprite::flushStage();
		gx_scene->setEffect(gx_graphics->verifyEffect(effect) ? effect : nullptr);
		gx_scene->render(mesh, fv, vc, ft, tc);
	}
	void* operator new(size_t sz) {
		static const int GROW = 2048;
		if(!pool) {
			pool = new MeshQueue[GROW];
			for(int k = 0; k < GROW - 1; ++k) pool[k].next = &pool[k + 1];
			pool[GROW - 1].next = 0;
		}
		MeshQueue* t = pool;
		pool = t->next;
		return t;
	}
	void operator delete(void* q) {
		MeshQueue* t = (MeshQueue*)q;
		t->next = pool;
		pool = t;
	}
};

Model::MeshQueue* Model::MeshQueue::pool;

bool Model::batch_opaque = false;
bool Model::instancing_enabled = false;
bool Model::any_instancing_opt_in = false;
std::vector<Model::MeshQueue*> Model::pending_opaque;

struct InstanceKey {
	gxMesh* mesh;
	int fv, vc, ft, tc;
	Brush brush;

	bool operator<(const InstanceKey& o)const {
		if(mesh != o.mesh) return mesh < o.mesh;
		if(fv != o.fv) return fv < o.fv;
		if(vc != o.vc) return vc < o.vc;
		if(ft != o.ft) return ft < o.ft;
		if(tc != o.tc) return tc < o.tc;
		return brush < o.brush;
	}
};

static bool canInstanceBrush(const Brush& brush) {
	const gxScene::RenderState& rs = brush.getRenderState();
	if(rs.effect) return false;
	if(rs.fx & (gxScene::FX_FLATSHADED | gxScene::FX_EMISSIVE)) return false;

	for(int k = 0; k < gxScene::MAX_TEXTURES; ++k) {
		const gxScene::RenderState::TexState& ts = rs.tex_states[k];
		if(!ts.canvas) continue;
		if(ts.matrix) return false;
		if(ts.flags & gxScene::TEX_COORDS2) return false;
		if(ts.canvas->getFlags() & (gxCanvas::CANVAS_TEX_SPHERE | gxCanvas::CANVAS_TEX_CUBE)) return false;
	}
	return true;
}

bool Model::brushSupportsInstancing(const Brush& brush) {
	return canInstanceBrush(brush);
}

static uint64_t computeStateKey(const Brush& b, gxEffect* e) {
	const auto& rs = b.getRenderState();
	uint64_t key = 0;

	key ^= (uint64_t)rs.blend;
	key ^= (uint64_t)rs.fx << 8;
	key ^= (uint64_t)(rs.alpha * 255.0f) << 16;
	key ^= (uint64_t)(rs.shininess * 255.0f) << 24;

	for (int i = 0; i < gxScene::MAX_TEXTURES; ++i) {
		if (rs.tex_states[i].canvas) {
			uint64_t ptr = (uint64_t)(uintptr_t)rs.tex_states[i].canvas;
			key ^= (ptr << (i * 8)) ^ (ptr >> (64 - i * 8));
		}
	}

	if (e) key ^= (uint64_t)(uintptr_t)e << 32;
	return key;
}

Model::Model() :
	space(RENDER_SPACE_LOCAL),
	auto_fade(false),
	auto_fade_nr(0), auto_fade_fr(0),
	captured_alpha(1), tweened_alpha(1), w_brush(true),
	entityEffect(nullptr), renderEffect(nullptr) {
}

Model::Model(const Model& t) :Object(t),
space(t.space), brush(t.brush),
auto_fade(t.auto_fade), auto_fade_nr(t.auto_fade_nr), auto_fade_fr(t.auto_fade_fr),
captured_alpha(t.captured_alpha), tweened_alpha(t.tweened_alpha), w_brush(true),
entityEffect(t.entityEffect), renderEffect(nullptr), instancing_opt_in(t.instancing_opt_in) {
}

void Model::capture() {
	Object::capture();
	captured_alpha = brush.getAlpha();
}

bool Model::beginRender(float t) {
	Object::beginRender(t);
	tweened_alpha = brush.getAlpha();
	if(t != 1 && tweened_alpha != captured_alpha) {
		//
		//render tweening of alpha
		//
		tweened_alpha = (tweened_alpha - captured_alpha) * t + captured_alpha;
	}
	renderEffect = entityEffect ? entityEffect : brush.getEffect();
	return tweened_alpha > 0;
}

bool Model::doAutoFade(const Vector& eye) {
	float alpha = tweened_alpha;
	if(auto_fade) {
		//
		//autofading of alpha
		//
		float d = eye.distance(getRenderTform().v);
		if(d >= auto_fade_fr) return false;
		if(d >= auto_fade_nr) {
			float t = 1 - (d - auto_fade_nr) / (auto_fade_fr - auto_fade_nr);
			alpha *= t; if(alpha <= 0) return false;
		}
	}
	if(w_brush) render_brush = brush;

	if(alpha != render_brush.getAlpha()) {
		render_brush.setAlpha(alpha);
	}
	else if(!w_brush) {
		return true;
	}

	setRenderBrush(render_brush);
	w_brush = false;
	return true;
}

void Model::enqueue(MeshQueue* q) {
	queues[q->getQueueType()].push_back(q);
}

void Model::enqueue(gxMesh* mesh, int fv, int vc, int ft, int tc) {
	uint64_t key = computeStateKey(render_brush, renderEffect);
	enqueue(new MeshQueue(this, mesh, fv, vc, ft, tc, render_brush, renderEffect, key, true));
}

void Model::enqueue(gxMesh* mesh, int fv, int vc, int ft, int tc, const Brush& brush, bool allow_instance) {
	uint64_t key = computeStateKey(brush, renderEffect);
	enqueue(new MeshQueue(this, mesh, fv, vc, ft, tc, brush, renderEffect, key, allow_instance));
}

void Model::enqueueSkinned(gxMesh* mesh, int fv, int vc, int ft, int tc, const Brush& brush,
	const float* bone_data, int bone_cnt) {
	uint64_t key = computeStateKey(brush, renderEffect);
	enqueue(new MeshQueue(this, mesh, fv, vc, ft, tc, brush, renderEffect, key, bone_data, bone_cnt));
}

void Model::renderQueue(int type) {
	auto& que = queues[type];
	std::sort(que.begin(), que.end(), [](const MeshQueue* a, const MeshQueue* b) {
			return a->getStateKey() < b->getStateKey();
		});

	if(batch_opaque && type == QUEUE_OPAQUE) {
		for(auto q : que) {
			bool batchable = (instancing_enabled || q->owner->getInstancingOptIn()) && q->instance_ok && q->bone_cnt == 0 && !q->effect && q->owner->getMeshModel() != 0 && q->owner->getRenderSpace() == RENDER_SPACE_LOCAL && brushSupportsInstancing(q->brush);
			if(batchable) {
				pending_opaque.push_back(q);
			}
			else {
				q->render();
				delete q;
			}
		}
		que.clear();
		return;
	}

	for (auto q : que) {
		q->render();
		delete q;
	}
	que.clear();
}

void Model::beginOpaqueBatch() {
	pending_opaque.clear();
	batch_opaque = gx_graphics && gx_graphics->instancingSupported() && (instancing_enabled || any_instancing_opt_in);
}

void Model::flushOpaqueBatch() {
	batch_opaque = false;
	if(pending_opaque.empty()) return;

	std::map<InstanceKey, std::vector<MeshQueue*>> groups;
	for(auto q : pending_opaque) {
		InstanceKey key = { q->mesh, q->fv, q->vc, q->ft, q->tc, q->brush };
		groups[key].push_back(q);
	}
	pending_opaque.clear();

	for(auto& group : groups) {
		std::vector<MeshQueue*>& items = group.second;
		MeshQueue* first = items[0];

		if(items.size() < 2) {
			gx_scene->setWorldMatrix((gxScene::Matrix*)&first->owner->getRenderTform());
			first->render();
			delete first;
			continue;
		}

		std::vector<float> instance_data;
		instance_data.reserve(items.size() * 12);
		for(auto q : items) {
			const Transform& t = q->owner->getRenderTform();
			instance_data.push_back(t.m.i.x); instance_data.push_back(t.m.j.x); instance_data.push_back(t.m.k.x); instance_data.push_back(t.v.x);
			instance_data.push_back(t.m.i.y); instance_data.push_back(t.m.j.y); instance_data.push_back(t.m.k.y); instance_data.push_back(t.v.y);
			instance_data.push_back(t.m.i.z); instance_data.push_back(t.m.j.z); instance_data.push_back(t.m.k.z); instance_data.push_back(t.v.z);
		}

		gx_scene->setRenderState(first->brush.getRenderState());
		Sprite::flushStage();

		if(gx_scene->renderInstanced(first->mesh, first->fv, first->vc, first->ft, first->tc,
			instance_data.data(), (int)items.size())) {
			for(auto q : items) delete q;
			continue;
		}

		for(auto q : items) {
			gx_scene->setWorldMatrix((gxScene::Matrix*)&q->owner->getRenderTform());
			q->render();
			delete q;
		}
	}
}

void Model::setEffect(gxEffect* e) {
	entityEffect = e;
}

gxEffect* Model::getEffect() const {
	return entityEffect;
}