#include "std.h"
#include "model.h"
#include "sprite.h"
#include <algorithm>

extern sdlScene* sdl_scene;
extern sdlGraphics* sdl_graphics;

class Model::MeshQueue {
	union {
		sdlMesh* mesh;
		MeshQueue* next;
	};
	int fv, vc, ft, tc;
	Brush brush;
	sdlEffect* effect;
	int q_type;
	uint64_t stateKey;
	std::vector<float> bone_data;
	int bone_cnt;

	static MeshQueue* pool;

public:
	MeshQueue() {}

	MeshQueue(sdlMesh* m, int fv, int vc, int ft, int tc, const Brush& b, sdlEffect* e, uint64_t key) :
		mesh(m), fv(fv), vc(vc), ft(ft), tc(tc), brush(b), effect(e), stateKey(key), bone_cnt(0) {
		int n = brush.getBlend();
		q_type = (n == sdlScene::BLEND_REPLACE) ? QUEUE_OPAQUE : QUEUE_TRANSPARENT;
	}

	MeshQueue(sdlMesh* m, int fv, int vc, int ft, int tc, const Brush& b, sdlEffect* e, uint64_t key,
		const float* bones, int n_bones) :
		mesh(m), fv(fv), vc(vc), ft(ft), tc(tc), brush(b), effect(e), stateKey(key),
		bone_data(bones, bones + n_bones * 12), bone_cnt(n_bones) {
		int n = brush.getBlend();
		q_type = (n == sdlScene::BLEND_REPLACE) ? QUEUE_OPAQUE : QUEUE_TRANSPARENT;
	}

	int getQueueType()const {
		return q_type;
	}

	const Brush& getBrush() const { return brush; }
	uint64_t getStateKey() const { return stateKey; }

	void render() {
		sdl_scene->setRenderState(brush.getRenderState());
		if (bone_cnt > 0) {
			sdl_scene->setEffect(nullptr);
			sdl_scene->renderSkinned(mesh, fv, vc, ft, tc, bone_data.data(), bone_cnt);
			return;
		}
		Sprite::flushStage();
		sdl_scene->setEffect(sdl_graphics->verifyEffect(effect) ? effect : nullptr);
		sdl_scene->render(mesh, fv, vc, ft, tc);
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

static uint64_t computeStateKey(const Brush& b, sdlEffect* e) {
	const auto& rs = b.getRenderState();
	uint64_t key = 0;

	key ^= (uint64_t)rs.blend;
	key ^= (uint64_t)rs.fx << 8;
	key ^= (uint64_t)(rs.alpha * 255.0f) << 16;
	key ^= (uint64_t)(rs.shininess * 255.0f) << 24;

	for (int i = 0; i < sdlScene::MAX_TEXTURES; ++i) {
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
entityEffect(t.entityEffect), renderEffect(nullptr) {
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

void Model::enqueue(sdlMesh* mesh, int fv, int vc, int ft, int tc) {
	uint64_t key = computeStateKey(render_brush, renderEffect);
	enqueue(new MeshQueue(mesh, fv, vc, ft, tc, render_brush, renderEffect, key));
}

void Model::enqueue(sdlMesh* mesh, int fv, int vc, int ft, int tc, const Brush& brush) {
	uint64_t key = computeStateKey(brush, renderEffect);
	enqueue(new MeshQueue(mesh, fv, vc, ft, tc, brush, renderEffect, key));
}

void Model::enqueueSkinned(sdlMesh* mesh, int fv, int vc, int ft, int tc, const Brush& brush,
	const float* bone_data, int bone_cnt) {
	uint64_t key = computeStateKey(brush, renderEffect);
	enqueue(new MeshQueue(mesh, fv, vc, ft, tc, brush, renderEffect, key, bone_data, bone_cnt));
}

void Model::renderQueue(int type) {
	auto& que = queues[type];
	std::sort(que.begin(), que.end(), [](const MeshQueue* a, const MeshQueue* b) {
			return a->getStateKey() < b->getStateKey();
		});

	for (auto q : que) {
		q->render();
		delete q;
	}
	que.clear();
}

void Model::setEffect(sdlEffect* e) {
	entityEffect = e;
}

sdlEffect* Model::getEffect() const {
	return entityEffect;
}