#include "std.h"
#include "brush.h"

#include "../sdlruntime/sdlgraphics.h"

struct Brush::Rep {
	union { int ref_cnt; Rep* next; };
	int blend, max_tex;
	bool blend_valid;
	sdlScene::RenderState rs;
	Texture texs[sdlScene::MAX_TEXTURES];
	int tex_frame[sdlScene::MAX_TEXTURES];
	sdlEffect* effect;
	float roughness = 0.0f, metallic = 0.0f;
	bool pinned = false;

	static Rep* pool;

	Rep() :
		ref_cnt(1), blend(0), max_tex(0), blend_valid(true), effect(nullptr) {
		memset(&rs, 0, sizeof(rs));
		memset(tex_frame, 0, sizeof(tex_frame));
		rs.blend = sdlScene::BLEND_REPLACE;
		rs.color[0] = rs.color[1] = rs.color[2] = rs.alpha = 1;
	}

	Rep(const Rep& t) :
		ref_cnt(1), blend(t.blend), max_tex(t.max_tex), rs(t.rs), blend_valid(t.blend_valid), effect(t.effect),
		roughness(t.roughness), metallic(t.metallic) {
		for (int k = 0; k < max_tex; ++k) texs[k] = t.texs[k];
		memcpy(tex_frame, t.tex_frame, sizeof(tex_frame));
	}

	void* operator new(size_t sz) {
		static const int GROW = 64;
		if (!pool) {
			pool = new Rep[GROW];
			for (int k = 0; k < GROW - 1; ++k) pool[k].next = &pool[k + 1];
			pool[GROW - 1].next = 0;
		}
		Rep* p = pool;
		pool = p->next;
		return p;
	}

	void operator delete(void* q) {
		Rep* p = (Rep*)q;
		p->next = pool;
		pool = p;
	}
};

Brush::Rep* Brush::Rep::pool;

Brush::Brush() :
	rep(new Rep()) {
}

Brush::Brush(const Brush& t) :
	rep(t.rep) {
	++rep->ref_cnt;
}

Brush::Brush(const Brush& a, const Brush& b) :
	rep(new Rep(*a.rep)) {

	*(Vector*)rep->rs.color *= *(Vector*)b.rep->rs.color;

	rep->rs.alpha *= b.rep->rs.alpha;
	rep->rs.shininess += b.rep->rs.shininess;

	if (b.rep->blend) rep->blend = b.rep->blend;

	rep->rs.fx |= b.rep->rs.fx;

	rep->rs.fx = (rep->rs.fx & ~(unsigned)sdlScene::FX_NOFOG) | (b.rep->rs.fx & sdlScene::FX_NOFOG);

	if (b.rep->max_tex > rep->max_tex) rep->max_tex = b.rep->max_tex;

	for (int k = 0; k < rep->max_tex; ++k) {
		if (b.rep->texs[k].valid()) {
			rep->texs[k] = b.rep->texs[k];
			rep->tex_frame[k] = b.rep->tex_frame[k];
		}
	}

	rep->blend_valid = false;
}

Brush::~Brush() {
	if (!--rep->ref_cnt) delete rep;
}

Brush& Brush::operator=(const Brush& t) {
	++t.rep->ref_cnt;
	if (!--rep->ref_cnt) delete rep;
	rep = t.rep; return *this;
}

Brush::Rep* Brush::write()const {
	if (rep->ref_cnt > 1) {
		--rep->ref_cnt;
		rep = new Rep(*rep);
	}
	return rep;
}

void Brush::setColor(const Vector& color) {
	*(Vector*)write()->rs.color = color;
}

void Brush::setAlpha(float alpha) {
	float a = rep->rs.alpha;
	write()->rs.alpha = alpha;
	if ((a < 1) != (alpha < 1)) rep->blend_valid = false;
}

void Brush::setShininess(float n) {
	write()->rs.shininess = n;
}

void Brush::setBlend(int blend) {
	write()->blend = blend;
	rep->blend_valid = false;
}

void Brush::setFX(int fx) {
	write()->rs.fx = fx;
	rep->blend_valid = false;
}

void Brush::setTexture(int index, const Texture& t, int n) {
	write();
	sdlScene::RenderState& rs = rep->rs;

	rep->texs[index] = t;
	rep->tex_frame[index] = n;

	rep->max_tex = 0;
	for (int k = 0; k < sdlScene::MAX_TEXTURES; ++k) {
		if (rep->texs[k].valid()) rep->max_tex = k + 1;
	}
	rep->blend_valid = false;
}

const Vector& Brush::getColor()const {
	return *(Vector*)rep->rs.color;
}

float Brush::getAlpha()const {
	return rep->rs.alpha;
}

float Brush::getShininess()const {
	return rep->rs.shininess;
}

void Brush::setMaterial(float roughness, float metallic) {
	write()->roughness = roughness;
	write()->metallic = metallic;
}

float Brush::getRoughness()const {
	return rep->roughness;
}

float Brush::getMetallic()const {
	return rep->metallic;
}

int Brush::getBlend()const {
	if (rep->blend_valid) return rep->rs.blend;

	rep->blend_valid = true;	//well, it will be...

	sdlScene::RenderState& rs = rep->rs;

	//alphatest
	if (rep->texs[0].getCanvasFlags() & sdlCanvas::CANVAS_TEX_MASK) {
		rs.fx |= sdlScene::FX_ALPHATEST;
	}
	else {
		rs.fx &= ~sdlScene::FX_ALPHATEST;
	}

	//0 = default/replace
	//1 = alpha
	//2 = multiply
	//3 = add
	if (rep->blend) {
		if (rep->blend != sdlScene::BLEND_ALPHA) {
			return rs.blend = rep->blend;
		}
		for (int k = 0; k < rep->max_tex; ++k) {
			if (rep->texs[k].isTransparent()) {
				return rs.blend = sdlScene::BLEND_ALPHA;
			}
		}
	}
	else if (rep->max_tex == 1 && rep->texs[0].isTransparent()) {
		//single transparent texture?
		return rs.blend = sdlScene::BLEND_ALPHA;
	}

	//vertex alpha or entityalpha?
	if ((rs.fx & sdlScene::FX_VERTEXALPHA) || rs.alpha < 1) {
		return rs.blend = sdlScene::BLEND_ALPHA;
	}

	return rs.blend = sdlScene::BLEND_REPLACE;
}

int Brush::getFX()const {
	return rep->rs.fx;
}

Texture Brush::getTexture(int index)const {
	return rep->texs[index];
}

const sdlScene::RenderState& Brush::getRenderState()const {
	getBlend();
	for (int k = 0; k < rep->max_tex; ++k) {
		sdlScene::RenderState::TexState* ts = &rep->rs.tex_states[k];
		ts->canvas = rep->texs[k].getCanvas(rep->tex_frame[k]);
		ts->matrix = rep->texs[k].getMatrix();
		ts->blend = rep->texs[k].getBlend();
		ts->flags = rep->texs[k].getFlags();
		ts->bumpEnvMat[0][0] = rep->texs[k].getBumpEnvMat(0, 0);
		ts->bumpEnvMat[1][0] = rep->texs[k].getBumpEnvMat(1, 0);
		ts->bumpEnvMat[0][1] = rep->texs[k].getBumpEnvMat(0, 1);
		ts->bumpEnvMat[1][1] = rep->texs[k].getBumpEnvMat(1, 1);
		ts->bumpEnvScale = rep->texs[k].getBumpEnvScale();
		ts->bumpEnvOffset = rep->texs[k].getBumpEnvOffset();
	}
	// Clear unused slots so stale shit never leaks through!
	for (int k = rep->max_tex; k < sdlScene::MAX_TEXTURES; ++k) {
		sdlScene::RenderState::TexState* ts = &rep->rs.tex_states[k];
		ts->canvas = nullptr;
		ts->matrix = nullptr;
		ts->blend = 0;
		ts->flags = 0;
		ts->bumpEnvMat[0][0] = ts->bumpEnvMat[1][0] = 0;
		ts->bumpEnvMat[0][1] = ts->bumpEnvMat[1][1] = 0;
		ts->bumpEnvScale = 0;
		ts->bumpEnvOffset = 0;
	}
	rep->rs.effect = rep->effect;
	return rep->rs;
}

bool Brush::operator<(const Brush& t)const {
	if (rep == t.rep) return false;
	int c = memcmp(rep->rs.color, t.rep->rs.color, 12);
	if (c) return c < 0;
	if (rep->rs.shininess != t.rep->rs.shininess) return rep->rs.shininess < t.rep->rs.shininess;
	if (rep->rs.alpha != t.rep->rs.alpha) return rep->rs.alpha < t.rep->rs.alpha;
	if (rep->rs.blend != t.rep->rs.blend) return rep->rs.blend < t.rep->rs.blend;
	if (rep->rs.fx != t.rep->rs.fx) return rep->rs.fx < t.rep->rs.fx;
	if (rep->max_tex != t.rep->max_tex) return rep->max_tex < t.rep->max_tex;
	if (rep->effect != t.rep->effect) return rep->effect < t.rep->effect;
	for (int k = 0; k < sdlScene::MAX_TEXTURES; ++k) {
		const Texture& ta = rep->texs[k];
		const Texture& tb = t.rep->texs[k];
		CachedTexture* ca = ta.getCachedTexture();
		CachedTexture* cb = tb.getCachedTexture();
		if (ca != cb) return ca < cb;
		if (rep->tex_frame[k] != t.rep->tex_frame[k]) return rep->tex_frame[k] < t.rep->tex_frame[k];
		if (ta.getBlend() != tb.getBlend()) return ta.getBlend() < tb.getBlend();
		if (ta.getFlags() != tb.getFlags()) return ta.getFlags() < tb.getFlags();
		if (ta.getBumpEnvMat(0, 0) != tb.getBumpEnvMat(0, 0)) return ta.getBumpEnvMat(0, 0) < tb.getBumpEnvMat(0, 0);
		if (ta.getBumpEnvMat(1, 0) != tb.getBumpEnvMat(1, 0)) return ta.getBumpEnvMat(1, 0) < tb.getBumpEnvMat(1, 0);
		if (ta.getBumpEnvMat(0, 1) != tb.getBumpEnvMat(0, 1)) return ta.getBumpEnvMat(0, 1) < tb.getBumpEnvMat(0, 1);
		if (ta.getBumpEnvMat(1, 1) != tb.getBumpEnvMat(1, 1)) return ta.getBumpEnvMat(1, 1) < tb.getBumpEnvMat(1, 1);
		if (ta.getBumpEnvScale() != tb.getBumpEnvScale()) return ta.getBumpEnvScale() < tb.getBumpEnvScale();
		if (ta.getBumpEnvOffset() != tb.getBumpEnvOffset()) return ta.getBumpEnvOffset() < tb.getBumpEnvOffset();
		const sdlScene::Matrix* ma = ta.getMatrix();
		const sdlScene::Matrix* mb = tb.getMatrix();
		if (ma != mb) return ma < mb;
	}
	return false;
}

void Brush::setEffect(sdlEffect* e) {
	write()->effect = e;
	rep->blend_valid = false;
}

sdlEffect* Brush::getEffect() const {
	return rep->effect;
}

void Brush::setPinned(bool pinned) {
	rep->pinned = pinned;
}

bool Brush::pinned() const {
	return rep->pinned;
}
