#include "std.h"
#include "geom.h"
#include "texture.h"
#include "cachedtexture.h"

#include "../sdlruntime/sdlgraphics.h"

extern sdlScene* sdl_scene;
extern sdlGraphics* sdl_graphics;

struct Filter {
	std::string t;
	int flags;
	Filter(const std::string& t, int flags) :t(t), flags(flags) {
	}
};

static std::vector<Filter> filters;

static int filterFile(const std::string& t, int flags) {
	//check filters...
	std::string l = tolower(t);
	for(int k = 0; k < filters.size(); ++k) {
		if(l.find(filters[k].t) != std::string::npos) {
			flags |= filters[k].flags;
		}
	}
	return flags;
}

struct Texture::Rep {

	int ref_cnt;
	CachedTexture cached_tex;

	int tex_blend, tex_flags;
	bool transparent;
	DWORD bumpEnvMat[2][2];
	DWORD bumpEnvScale;
	DWORD bumpEnvOffset;

	float sx, sy, tx, ty, rot;
	bool mat_used, mat_valid;
	sdlScene::Matrix matrix;
	bool pinned = false;

	Rep(int w, int h, int flags, int cnt) :
		ref_cnt(1), cached_tex(w, h, flags, cnt),
		tex_blend(sdlScene::BLEND_MULTIPLY), tex_flags(0),
		bumpEnvMat{ {0, 0}, {0, 0} }, bumpEnvScale(0), bumpEnvOffset(0),
		sx(1), sy(1), tx(0), ty(0), rot(0), mat_used(false), mat_valid(false) {
		transparent =
			(flags & sdlCanvas::CANVAS_TEX_ALPHA) &&
			!(flags & sdlCanvas::CANVAS_TEX_MASK);
		memset(&matrix, 0, sizeof(matrix));
	}

	Rep(const std::string& f, int flags, int w, int h, int first, int cnt) :
		ref_cnt(1), cached_tex(f, flags, w, h, first, cnt),
		tex_blend(sdlScene::BLEND_MULTIPLY), tex_flags(0),
		bumpEnvMat{ {0, 0}, {0, 0} }, bumpEnvScale(0), bumpEnvOffset(0),
		sx(1), sy(1), tx(0), ty(0), rot(0), mat_used(false), mat_valid(false) {
		transparent =
			(flags & sdlCanvas::CANVAS_TEX_ALPHA) &&
			!(flags & sdlCanvas::CANVAS_TEX_MASK);
		memset(&matrix, 0, sizeof(matrix));
	}

	Rep(const Rep& t) :
		ref_cnt(1), cached_tex(t.cached_tex),
		tex_blend(t.tex_blend), tex_flags(t.tex_flags),
		bumpEnvMat{ {t.bumpEnvMat[0][0], t.bumpEnvMat[0][1]}, {t.bumpEnvMat[1][0], t.bumpEnvMat[1][1]} },
		bumpEnvScale(t.bumpEnvScale), bumpEnvOffset(t.bumpEnvOffset),
		sx(t.sx), sy(t.sy), tx(t.tx), ty(t.ty), rot(t.rot),
		mat_used(t.mat_used), mat_valid(t.mat_valid), matrix(t.matrix),
		transparent(t.transparent), pinned(t.pinned) {
	}
};

Texture::Texture() :rep(0) {
}

Texture::Texture(const std::string& f, int flags) {
	flags = filterFile(f, flags) | sdlCanvas::CANVAS_TEXTURE;
	if(flags & sdlCanvas::CANVAS_TEX_MASK) flags |= sdlCanvas::CANVAS_TEX_RGB | sdlCanvas::CANVAS_TEX_ALPHA;
	rep = new Rep(f, flags, 0, 0, 0, 1);
}

Texture::Texture(const std::string& f, int flags, int w, int h, int first, int cnt) {
	flags = filterFile(f, flags) | sdlCanvas::CANVAS_TEXTURE;
	if(flags & sdlCanvas::CANVAS_TEX_MASK) flags |= sdlCanvas::CANVAS_TEX_RGB | sdlCanvas::CANVAS_TEX_ALPHA;
	rep = new Rep(f, flags, w, h, first, cnt);
}

Texture::Texture(int w, int h, int flags, int cnt) {
	flags |= sdlCanvas::CANVAS_TEXTURE;
	if(flags & sdlCanvas::CANVAS_TEX_MASK) flags |= sdlCanvas::CANVAS_TEX_RGB | sdlCanvas::CANVAS_TEX_ALPHA;
	rep = new Rep(w, h, flags, cnt);
}

Texture::Texture(const Texture& t) :
	rep(t.rep) {
	if(rep) ++rep->ref_cnt;
}

Texture::~Texture() {
	if(rep && !--rep->ref_cnt) delete rep;
}

Texture& Texture::operator=(const Texture& t) {
	if(t.rep) ++t.rep->ref_cnt;
	if(rep && !--rep->ref_cnt) delete rep;
	rep = t.rep;
	return *this;
}

void Texture::setScale(float u_scale, float v_scale) {
	if(!rep) return;
	rep->sx = u_scale; rep->sy = v_scale;
	rep->mat_valid = false;
	rep->mat_used = true;
}

void Texture::setRotation(float angle) {
	if(!rep) return;
	rep->rot = angle;
	rep->mat_valid = false;
	rep->mat_used = true;
}

void Texture::setPosition(float u_pos, float v_pos) {
	if(!rep) return;
	rep->tx = u_pos;
	rep->ty = v_pos;
	rep->mat_valid = false;
	rep->mat_used = true;
}

void Texture::setBlend(int blend) {
	if(!rep) return;
	rep->tex_blend = blend;
}

void Texture::setFlags(int flags) {
	if(!rep) return;
	rep->tex_flags = flags;
}

void Texture::setBumpEnvMat(int x, int y, float envmat) {
	if(!rep) return;
	rep->bumpEnvMat[x][y] = *((DWORD*)&envmat);
}

void Texture::setBumpEnvScale(float envscale) {
	if(!rep) return;
	rep->bumpEnvScale = *((DWORD*)&envscale);
}

void Texture::setBumpEnvOffset(float envoffset) {
	if(!rep) return;
	rep->bumpEnvOffset = *((DWORD*)&envoffset);
}

bool Texture::isTransparent()const {
	return rep ? rep->transparent : false;
}

sdlCanvas* Texture::getCanvas(int n)const {
	if (!rep) return 0;
	const std::vector<sdlCanvas*>& frames = rep->cached_tex.getFrames();
	return n >= 0 && n < (int)frames.size() ? frames[n] : 0;
}

int Texture::getCanvasFlags()const {
	if (!rep) return 0;
	const std::vector<sdlCanvas*>& frames = rep->cached_tex.getFrames();
	return frames.size() ? frames[0]->getFlags() : 0;
}

bool Texture::valid()const {
	return rep && rep->cached_tex.valid();
}

CachedTexture* Texture::getCachedTexture()const {
	return rep ? &rep->cached_tex : 0;
}

int Texture::getBlend()const {
	return rep ? rep->tex_blend : 0;
}

int Texture::getFlags()const {
	return rep ? rep->tex_flags : 0;
}

DWORD Texture::getBumpEnvMat(int x, int y)const {
	return rep ? rep->bumpEnvMat[x][y] : 0;
}

DWORD Texture::getBumpEnvScale()const {
	return rep ? rep->bumpEnvScale : 0;
}

DWORD Texture::getBumpEnvOffset()const {
	return rep ? rep->bumpEnvOffset : 0;
}

const sdlScene::Matrix* Texture::getMatrix()const {
	if(!rep || !rep->mat_used) return 0;
	if(!rep->mat_valid) {
		float c = cos(rep->rot), s = sin(rep->rot);
		rep->matrix.elements[0][0] = c * rep->sx;
		rep->matrix.elements[1][0] = s * rep->sx;
		rep->matrix.elements[0][1] = -s * rep->sy;
		rep->matrix.elements[1][1] = c * rep->sy;
		rep->matrix.elements[2][0] = rep->tx;
		rep->matrix.elements[2][1] = rep->ty;
		rep->mat_valid = true;
	}
	return &rep->matrix;
}

bool Texture::operator<(const Texture& t)const {
	if(rep && t.rep) return rep->cached_tex < t.rep->cached_tex;
	return rep < t.rep;
}

void Texture::setPinned(bool pinned) {
	if (rep) rep->pinned = pinned;
}

bool Texture::pinned()const {
	return rep ? rep->pinned : false;
}

void Texture::clearFilters() {
	filters.clear();
}

void Texture::addFilter(const std::string& t, int flags) {
	filters.push_back(Filter(tolower(t), flags));
}