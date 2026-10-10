#include "std.h"
#include "cachedtexture.h"
#include "../sdlruntime/sdlgraphics.h"

int active_texs;

extern sdlRuntime* sdl_runtime;
extern sdlGraphics* sdl_graphics;

std::set<CachedTexture::Rep*> CachedTexture::rep_set;

static std::string path;

std::vector<CachedTexture::Rep*> CachedTexture::pending_reps;

CachedTexture::PathMutator CachedTexture::pathMutator = nullptr;
void CachedTexture::setPathMutator(PathMutator m) {
	pathMutator = m;
}

static bool fileExists(const std::string& f) {
	DWORD attrs = GetFileAttributesA(f.c_str());
	return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

struct CachedTexture::Rep {
	int ref_cnt;
	std::string file;
	int flags, w, h, first;
	int requested_cnt;
	std::vector<sdlCanvas*> frames;
	std::shared_ptr<AsyncImageLoader::Job> job;
	bool materialized;
	bool failed;

	Rep(int w, int h, int flags, int cnt) :
		ref_cnt(1), flags(flags), w(w), h(h), first(0), requested_cnt(cnt),
		job(nullptr), materialized(true), failed(false) {
		++active_texs;
		while (cnt-- > 0) {
			if (sdlCanvas* t = sdl_graphics->createCanvas(w, h, flags)) {
				frames.push_back(t);
			}
			else break;
		}
	}

	Rep(const std::string& f, int flags, int w, int h, int first, int cnt) :
		ref_cnt(1), file(f), flags(flags), w(w), h(h), first(first), requested_cnt(cnt),
		job(nullptr), materialized(false), failed(false) {
		++active_texs;
		if (!fileExists(f)) {
			failed = true;
			materialized = true;
			return;
		}
		job = AsyncImageLoader::instance().load(f);
		pending_reps.push_back(this);
	}

	~Rep() {
		--active_texs;
		for (int k = 0; k < frames.size(); ++k) sdl_graphics->freeCanvas(frames[k]);
		cancelJob();
	}

	void cancelJob() {
		for (auto it = pending_reps.begin(); it != pending_reps.end(); ++it) {
			if (*it == this) {
				pending_reps.erase(it);
				break;
			}
		}
		if (job) {
			AsyncImageLoader::instance().cancel(job);
			job.reset();
		}
	}

	void materialize(bool blocking) {
		if (materialized) return;

		if (job) {
			if (blocking) {
				AsyncImageLoader::instance().wait(job);
			}
			else {
				int s = job->state.load();
				if (s == AsyncImageLoader::STATE_QUEUED || s == AsyncImageLoader::STATE_DECODING) return;
			}
			if (job->state.load() == AsyncImageLoader::STATE_FAILED) {
				failed = true;
				cancelJob();
				materialized = true;
				return;
			}
			if (job->state.load() == AsyncImageLoader::STATE_CANCELLED) {
				failed = true;
				cancelJob();
				materialized = true;
				return;
			}
		}

		const DecodedImage* img = (job && job->image) ? job->image.get() : nullptr;

		if (!img) {
			failed = true;
			cancelJob();
			materialized = true;
			return;
		}

		int t_flags = (flags & (
			sdlCanvas::CANVAS_TEX_RGB |
			sdlCanvas::CANVAS_TEX_ALPHA |
			sdlCanvas::CANVAS_TEX_MASK |
			sdlCanvas::CANVAS_TEX_HICOLOR)) | sdlCanvas::CANVAS_NONDISPLAY | sdlCanvas::CANVAS_TEXTURE;

		int frame_flags = flags;
		if ((flags & sdlCanvas::CANVAS_TEX_MASK) && !(flags & sdlCanvas::CANVAS_TEX_ALPHA)) {
			frame_flags |= sdlCanvas::CANVAS_TEX_ALPHA;
		}

		if (!(flags & sdlCanvas::CANVAS_TEX_CUBE)) {
			if (w <= 0 || h <= 0 || first < 0 || requested_cnt <= 0) {
				if (img) {
					if (sdlCanvas* t = sdl_graphics->createCanvasFromImage(img, flags)) {
						frames.push_back(t);
					}
				}
				if (frames.empty()) failed = true;
				cancelJob();
				materialized = true;
				return;
			}
		}

		sdlCanvas* t = sdl_graphics->createCanvasFromImage(img, t_flags);
		if (!t) {
			failed = true;
			cancelJob();
			materialized = true;
			return;
		}
		if (!t->getDepth()) {
			sdl_graphics->freeCanvas(t);
			failed = true;
			cancelJob();
			materialized = true;
			return;
		}

		if (flags & sdlCanvas::CANVAS_TEX_CUBE) {
			int cw = t->getWidth() / 6;
			if (cw * 6 != t->getWidth()) {
				sdl_graphics->freeCanvas(t);
				failed = true;
				cancelJob();
				materialized = true;
				return;
			}
			int ch = t->getHeight();

			sdlCanvas* tex = sdl_graphics->createCanvas(cw, ch, frame_flags);
			if (tex) {
				frames.push_back(tex);

				for (int face = 0; face < 6; ++face) {
					tex->setCubeFace(face);
					sdl_graphics->copy(tex, 0, 0, cw, ch, t, face * cw, 0, cw, ch);
				}
				tex->setCubeFace(1);
			}
		}
		else {
			int x_tiles = t->getWidth() / w;
			int y_tiles = t->getHeight() / h;
			if (first + requested_cnt > x_tiles * y_tiles) {
				sdl_graphics->freeCanvas(t);
				failed = true;
				cancelJob();
				materialized = true;
				return;
			}
			int x = (first % x_tiles) * w;
			int y = (first / x_tiles) * h;
			int cnt = requested_cnt;
			while (cnt--) {
				sdlCanvas* p = sdl_graphics->createCanvas(w, h, frame_flags);
				if (p) {
					sdl_graphics->copy(p, 0, 0, w, h, t, x, y, w, h);
					p->setLogicalSize(w, h);
					frames.push_back(p);
				}
				x = x + w; if (x + w > t->getWidth()) { x = 0; y = y + h; }
			}
		}
		sdl_graphics->freeCanvas(t);

		cancelJob();
		materialized = true;
	}
};

CachedTexture::Rep* CachedTexture::findRep(const std::string& f, int flags, int w, int h, int first, int cnt) {
	for (Rep* rep : rep_set) {
		if (rep->file == f && rep->flags == flags && rep->w == w && rep->h == h && rep->first == first) {
			int have = rep->materialized ? (int)rep->frames.size() : rep->requested_cnt;
			if (have != cnt) continue;
			++rep->ref_cnt; return rep;
		}
	}
	return 0;
}

CachedTexture::CachedTexture(int w, int h, int flags, int cnt) :
	rep(new Rep(w, h, flags, cnt)) {
}

CachedTexture::CachedTexture(const std::string& f_, int flags, int w, int h, int first, int cnt) {
	std::string f = f_;
	if (f.substr(0, 2) == ".\\") f = f.substr(2);
	if (path.size()) {
		std::string t = path + tolower(filenamefile(f));
		if (rep = findRep(t, flags, w, h, first, cnt)) return;
		rep = new Rep(t, flags, w, h, first, cnt);
		rep_set.insert(rep);
		return;
	}

	if (pathMutator) {
		std::string mutated = pathMutator(f);
		if (!mutated.empty()) {
			std::string t = tolower(fullfilename(mutated));
			if (rep = findRep(t, flags, w, h, first, cnt)) return;
			rep = new Rep(t, flags, w, h, first, cnt);
			rep_set.insert(rep);
			return;
		}
	}

	std::string t = tolower(fullfilename(f));
	if (rep = findRep(t, flags, w, h, first, cnt)) return;
	rep = new Rep(t, flags, w, h, first, cnt);
	rep_set.insert(rep);
}

CachedTexture::CachedTexture(const CachedTexture& t) :
	rep(t.rep) {
	++rep->ref_cnt;
}

CachedTexture::~CachedTexture() {
	if (!--rep->ref_cnt) {
		rep_set.erase(rep);
		delete rep;
	}
}

CachedTexture& CachedTexture::operator=(const CachedTexture& t) {
	++t.rep->ref_cnt;
	if (!--rep->ref_cnt) {
		rep_set.erase(rep);
		delete rep;
	}
	rep = t.rep;
	return *this;
}

std::string CachedTexture::getName()const {
	return rep->file;
}

const std::vector<sdlCanvas*>& CachedTexture::getFrames()const {
	rep->materialize(true);
	return rep->frames;
}

bool CachedTexture::valid()const {
	if (!rep || rep->failed) return false;
	return true;
}

void CachedTexture::flushAll() {
	for (size_t idx = 0; idx < pending_reps.size(); ) {
		Rep* r = pending_reps[idx];
		r->materialize(false);
		if (r->materialized) {
		}
		else {
			++idx;
		}
	}
}

void CachedTexture::setPath(const std::string& t) {
	path = tolower(t);
	if (int sz = path.size()) {
		if (path[sz - 1] != '/' && path[sz - 1] != '\\') path += '\\';
	}
}