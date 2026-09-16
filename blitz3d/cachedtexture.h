#ifndef CACHEDTEXTURE_H
#define CACHEDTEXTURE_H

#include <memory>

#include "../gxruntime/gxcanvas.h"
#include "../gxruntime/asyncimage.h"

class CachedTexture {
public:
	CachedTexture(int w, int h, int flags, int cnt);
	CachedTexture(const std::string& f, int flags, int w, int h, int first, int cnt);
	CachedTexture(const CachedTexture& t);
	~CachedTexture();

	CachedTexture& operator=(const CachedTexture& t);

	std::string getName()const;

	const std::vector<gxCanvas*>& getFrames()const;

	bool valid()const;

	bool operator<(const CachedTexture& t)const { return rep < t.rep; }

	typedef std::string(*PathMutator)(const std::string&);
	static void setPathMutator(PathMutator m);
	static void setPath(const std::string& t);

	static void flushAll();
	static size_t pendingCount();

private:
	struct Rep;
	Rep* rep;
	static PathMutator pathMutator;

	Rep* findRep(const std::string& f, int flags, int w, int h, int first, int cnt);

	static std::set<Rep*> rep_set;
	static std::vector<Rep*> pending_reps;
};

#endif