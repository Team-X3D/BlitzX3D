#ifndef ANIMATOR_H
#define ANIMATOR_H

#include "animation.h"

class Object;

class Animator {
public:
	enum {
		ANIM_MODE_LOOP = 1,
		ANIM_MODE_PINGPONG = 2,
		ANIM_MODE_ONESHOT = 3
	};

	Animator(Animator* animator);

	Animator(Object* tree, int frames);

	Animator(const std::vector<Object*>& objs, int frames);

	void addSeq(int frames);

	void addSeqs(Animator* t);

	void extractSeq(int first, int last, int seq);

	void setAnimTime(float time, int seq);

	void animate(int mode, float speed, int seq, float trans);

	void update(float elapsed);

	int blend(int seq, float weight, int mode, float speed, float fade, bool additive = false, int ref = -1);
	void stopBlend(int seq);
	float blendWeight(int seq)const;
	int numBlends()const { return _blends.size(); }

	int animSeq()const { return _seq; }
	int animLen()const { return _seq_len; }
	float animTime()const { return _time; }
	bool animating()const { return !!_mode || !_blends.empty(); }

	int numSeqs()const { return _seqs.size(); }
	const std::vector<Object*>& getObjects()const { return _objs; }

private:
	struct Seq {
		int frames;
	};

	struct Anim {
		//anim keys
		std::vector<Animation> keys;
		//for transitions...
		bool pos, scl, rot;
		Vector src_pos, dest_pos;
		Vector src_scl, dest_scl;
		Quat src_rot, dest_rot;
		Anim() :pos(false), scl(false), rot(false) {}
	};

	struct Pose {
		Vector pos, scl;
		Quat rot;
		bool p, s, r;
		Pose() :p(false), s(false), r(false) {}
	};

	struct Blend {
		int seq, mode, len, ref;
		bool additive;
		float time, speed;
		float weight;
		float cur;
		float fade;
		Blend() :seq(-1), mode(0), len(0), ref(-1), additive(false), time(0), speed(1), weight(0), cur(0), fade(0) {}
	};

	std::vector<Seq> _seqs;

	std::vector<Anim> _anims;
	std::vector<Object*> _objs;
	std::vector<Blend> _blends;
	std::vector<Pose> _rest;
	std::vector<Pose> _base;
	bool _hasBase;

	int _seq, _mode, _seq_len;
	float _time, _speed, _trans_time, _trans_speed;

	void reset();
	void addObjs(Object* obj);
	void captureRest();
	void updateAnim();
	void updateBlend();
	void advanceBlends(float elapsed);
	void beginTrans();
	void updateTrans();
};

#endif