#include "std.h"
#include "animator.h"
#include "object.h"

Animator::Animator(Animator* t) :_seqs(t->_seqs) {

	_objs.resize(t->_objs.size());
	_anims.resize(t->_anims.size());

	for (int k = 0; k < t->_objs.size(); ++k) {
		_objs[k] = t->_objs[k]->getLastCopy();
		_anims[k].keys = t->_anims[k].keys;
	}

	_rest = t->_rest;
	_base.resize(_objs.size());

	reset();
}

Animator::Animator(Object* obj, int frames) {
	addObjs(obj);
	_anims.resize(_objs.size());
	addSeq(frames);
	captureRest();
	reset();
}

Animator::Animator(const std::vector<Object*>& objs, int frames) :_objs(objs) {
	_anims.resize(_objs.size());
	addSeq(frames);
	captureRest();
	reset();
}

void Animator::reset() {
	_seq = _mode = _seq_len = _time = _speed = _trans_time = _trans_speed = 0;
	_blends.clear();
	_hasBase = false;
}

void Animator::captureRest() {
	_rest.resize(_objs.size());
	_base.resize(_objs.size());
	for (int k = 0; k < _objs.size(); ++k) {
		Object* obj = _objs[k];
		_rest[k].pos = obj->getLocalPosition(); _rest[k].p = true;
		_rest[k].scl = obj->getLocalScale(); _rest[k].s = true;
		_rest[k].rot = obj->getLocalRotation(); _rest[k].r = true;
	}
}

void Animator::addObjs(Object* obj) {
	_objs.push_back(obj);
	for (Entity* e = obj->children(); e; e = e->successor()) {
		addObjs(e->getObject());
	}
}

void Animator::addSeq(int frames) {
	Seq seq;
	seq.frames = frames;
	_seqs.push_back(seq);
	for (int k = 0; k < _objs.size(); ++k) {
		Object* obj = _objs[k];
		_anims[k].keys.push_back(obj->getAnimation());
		obj->setAnimation(Animation());
	}
}

void Animator::addSeqs(Animator* t) {
	for (int n = 0; n < t->_seqs.size(); ++n) {
		_seqs.push_back(t->_seqs[n]);
		for (int k = 0; k < _objs.size(); ++k) {
			int j;
			for (j = 0; j < t->_objs.size(); ++j) {
				if (_objs[k]->getName() == t->_objs[j]->getName()) break;
			}
			if (j == t->_objs.size()) {
				_anims[k].keys.push_back(Animation());
				continue;
			}
			_anims[k].keys.push_back(t->_anims[j].keys[n]);
		}
	}
}

void Animator::extractSeq(int first, int last, int seq) {
	Seq sq;
	sq.frames = last - first;
	_seqs.push_back(sq);

	for (int k = 0; k < _objs.size(); ++k) {
		Animation& keys = _anims[k].keys[seq];
		_anims[k].keys.push_back(Animation(keys, first, last));
	}
}

void Animator::updateAnim() {

	_base.resize(_objs.size());

	for (int k = 0; k < _objs.size(); ++k) {

		Object* obj = _objs[k];
		const Animation& keys = _anims[k].keys[_seq];
		Pose& bp = _base[k];
		bp = Pose();

		if (keys.numPositionKeys()) {
			bp.pos = keys.getPosition(_time); bp.p = true;
			obj->setLocalPosition(bp.pos);
		}
		if (keys.numScaleKeys()) {
			bp.scl = keys.getScale(_time); bp.s = true;
			obj->setLocalScale(bp.scl);
		}
		if (keys.numRotationKeys()) {
			bp.rot = keys.getRotation(_time); bp.r = true;
			obj->setLocalRotation(bp.rot);
		}
		_hasBase = true;
	}
}

int Animator::blend(int seq, float weight, int mode, float speed, float fade, bool additive, int ref) {
	if (seq < 0 || seq >= _seqs.size()) return -1;
	if (ref < 0 || ref >= _seqs.size()) ref = seq;

	for (int k = 0; k < _blends.size(); ++k) {
		if (_blends[k].seq != seq) continue;
		Blend& b = _blends[k];
		if (mode == ANIM_MODE_ONESHOT || (weight > 0 && b.weight <= 0)) {
			b.len = _seqs[seq].frames;
			b.time = speed >= 0 ? 0 : b.len;
			b.cur = fade > 0 ? 0 : weight;
		}
		b.mode = mode;
		b.speed = speed;
		b.weight = weight;
		b.fade = fade;
		b.additive = additive;
		b.ref = ref;
		return k;
	}

	if (weight <= 0) return -1;

	Blend b;
	b.seq = seq;
	b.mode = mode;
	b.len = _seqs[seq].frames;
	b.speed = speed;
	b.time = speed >= 0 ? 0 : b.len;
	b.weight = weight;
	b.cur = fade > 0 ? 0 : weight;
	b.fade = fade;
	b.additive = additive;
	b.ref = ref;
	_blends.push_back(b);
	return _blends.size() - 1;
}

void Animator::stopBlend(int seq) {
	for (int k = 0; k < _blends.size(); ++k) {
		if (_blends[k].seq == seq) {
			_blends[k].weight = 0;
		}
	}
}

float Animator::blendWeight(int seq)const {
	for (int k = 0; k < _blends.size(); ++k) {
		if (_blends[k].seq == seq) return _blends[k].cur;
	}
	return 0;
}

void Animator::advanceBlends(float elapsed) {
	for (int k = 0; k < _blends.size();) {
		Blend& b = _blends[k];

		//ramp current weight toward target
		if (b.fade > 0) {
			float d = b.weight - b.cur, step = b.fade * elapsed;
			if (d > -step && d < step) b.cur = b.weight;
			else b.cur += d > 0 ? step : -step;
		}
		else {
			b.cur = b.weight;
		}

		if (b.speed != 0 && b.len > 0) {
			b.time += b.speed * elapsed;
			switch (b.mode) {
			case ANIM_MODE_LOOP:
				b.time = fmod(b.time, b.len);
				if (b.time < 0) b.time += b.len;
				break;
			case ANIM_MODE_PINGPONG:
				b.time = fmod(b.time, b.len * 2);
				if (b.time < 0) b.time += b.len * 2;
				if (b.time >= b.len) { b.time = b.len - (b.time - b.len); b.speed = -b.speed; }
				break;
			case ANIM_MODE_ONESHOT:
				if (b.time < 0) { b.time = 0; b.speed = 0; }
				else if (b.time >= b.len) { b.time = b.len; b.speed = 0; }
				break;
			}
		}

		if (b.weight <= 0 && b.cur <= 0) {
			_blends.erase(_blends.begin() + k);
			continue;
		}
		++k;
	}
}

void Animator::updateBlend() {
	if (_blends.empty()) return;

	for (int k = 0; k < _objs.size(); ++k) {
		Object* obj = _objs[k];
		const Pose& rest = _rest[k];

		Vector pos = rest.pos, scl = rest.scl;
		Quat rot = rest.rot;
		bool has_p = false, has_s = false, has_r = false;

		if (_hasBase) {
			const Pose& bp = _base[k];
			if (bp.p) { pos = bp.pos; has_p = true; }
			if (bp.s) { scl = bp.scl; has_s = true; }
			if (bp.r) { rot = bp.rot; has_r = true; }

			for (int j = 0; j < _blends.size(); ++j) {
				const Blend& b = _blends[j];
				if (b.additive || b.cur <= 0 || b.seq < 0 || b.seq >= _anims[k].keys.size()) continue;
				const Animation& a = _anims[k].keys[b.seq];
				float w = b.cur;

				if (a.numPositionKeys()) {
					pos += (a.getPosition(b.time) - pos) * w; has_p = true;
				}
				if (a.numScaleKeys()) {
					scl += (a.getScale(b.time) - scl) * w; has_s = true;
				}
				if (a.numRotationKeys()) {
					rot = rot.slerpTo(a.getRotation(b.time), w); has_r = true;
				}
			}
		}
		else {
			float p_tot = 0, s_tot = 0, r_tot = 0;
			Vector p_sum, s_sum;
			Quat r_sum;

			for (int j = 0; j < _blends.size(); ++j) {
				const Blend& b = _blends[j];
				if (b.additive || b.cur <= 0 || b.seq < 0 || b.seq >= _anims[k].keys.size()) continue;
				const Animation& a = _anims[k].keys[b.seq];
				float w = b.cur;

				if (a.numPositionKeys()) {
					p_sum += a.getPosition(b.time) * w; p_tot += w;
				}
				if (a.numScaleKeys()) {
					s_sum += a.getScale(b.time) * w; s_tot += w;
				}
				if (a.numRotationKeys()) {
					Quat q = a.getRotation(b.time);
					if (r_tot <= 0) { r_sum = q; r_tot = w; }
					else { r_tot += w; r_sum = r_sum.slerpTo(q, w / r_tot); }
				}
			}

			if (p_tot > 0) { pos = p_sum / p_tot; has_p = true; }
			if (s_tot > 0) { scl = s_sum / s_tot; has_s = true; }
			if (r_tot > 0) { rot = r_sum.normalized(); has_r = true; }
		}

		for (int j = 0; j < _blends.size(); ++j) {
			const Blend& b = _blends[j];
			if (!b.additive || b.cur <= 0 || b.seq < 0 || b.seq >= _anims[k].keys.size()) continue;
			const Animation& a = _anims[k].keys[b.seq];
			int ref = (b.ref >= 0 && b.ref < _anims[k].keys.size()) ? b.ref : b.seq;
			const Animation& ra = _anims[k].keys[ref];
			float w = b.cur;

			if (a.numPositionKeys()) {
				Vector rp = ra.numPositionKeys() ? ra.getPosition(0) : rest.pos;
				pos += (a.getPosition(b.time) - rp) * w; has_p = true;
			}
			if (a.numScaleKeys()) {
				Vector rs = ra.numScaleKeys() ? ra.getScale(0) : rest.scl;
				scl += (a.getScale(b.time) - rs) * w; has_s = true;
			}
			if (a.numRotationKeys()) {
				Quat rq = ra.numRotationKeys() ? ra.getRotation(0) : Quat();
				Quat d = Quat(rq.w, -rq.v) * a.getRotation(b.time);
				d = d.normalized();
				rot = (rot * Quat().slerpTo(d, w)).normalized(); has_r = true;
			}
		}

		if (has_p) obj->setLocalPosition(pos);
		if (has_s) obj->setLocalScale(scl);
		if (has_r) obj->setLocalRotation(rot);
	}
}

void Animator::updateTrans() {

	_base.resize(_objs.size());

	for (int k = 0; k < _objs.size(); ++k) {

		Object* obj = _objs[k];
		const Anim& anim = _anims[k];
		Pose& bp = _base[k];
		bp = Pose();

		if (anim.pos) {
			bp.pos = (anim.dest_pos - anim.src_pos) * _trans_time + anim.src_pos; bp.p = true;
			obj->setLocalPosition(bp.pos);
		}
		if (anim.scl) {
			bp.scl = (anim.dest_scl - anim.src_scl) * _trans_time + anim.src_scl; bp.s = true;
			obj->setLocalScale(bp.scl);
		}
		if (anim.rot) {
			bp.rot = anim.src_rot.slerpTo(anim.dest_rot, _trans_time); bp.r = true;
			obj->setLocalRotation(bp.rot);
		}
		_hasBase = true;
	}
}

void Animator::beginTrans() {

	for (int k = 0; k < _objs.size(); ++k) {

		Object* obj = _objs[k];
		Anim& anim = _anims[k];
		const Animation& keys = _anims[k].keys[_seq];

		if (anim.pos = !!keys.numPositionKeys()) {
			anim.src_pos = obj->getLocalPosition();
			anim.dest_pos = keys.getPosition(_time);
		}
		if (anim.scl = !!keys.numScaleKeys()) {
			anim.src_scl = obj->getLocalScale();
			anim.dest_scl = keys.getScale(_time);
		}
		if (anim.rot = !!keys.numRotationKeys()) {
			anim.src_rot = obj->getLocalRotation();
			anim.dest_rot = keys.getRotation(_time);
		}
	}
}

void Animator::setAnimTime(float time, int seq) {
	if (seq<0 || seq>_seqs.size()) return;

	_mode = 0;
	_speed = 0;
	_seq = seq;
	_seq_len = _seqs[_seq].frames;

	//Ok, mod the anim time!
	if (time < 0 || time > _seq_len) {
		_time = fmod(time, _seq_len);
		if (time < 0) { _time += +_seq_len; }
	}
	else {
		_time = time;
	}

	updateAnim();
}

void Animator::animate(int mode, float speed, int seq, float trans) {
	if (!mode && !speed) { _mode = 0; return; }

	if (seq < 0 || seq >= _seqs.size()) return;

	_seq = seq;
	_mode = mode;
	_seq_len = _seqs[_seq].frames;
	_speed = speed;
	_time = _speed >= 0 ? 0 : _seq_len;

	if (trans <= 0) {
		updateAnim();
		if (!_speed) _mode = 0;
		return;
	}

	_mode |= 0x8000;
	_trans_time = 0;
	_trans_speed = 1 / trans;
	beginTrans();
}

void Animator::update(float elapsed) {

	if (!_mode && _blends.empty()) return;

	_hasBase = false;

	if (_mode & 0x8000) {
		_trans_time += _trans_speed * elapsed;
		if (_trans_time < 1) {
			updateTrans();
			advanceBlends(elapsed);
			updateBlend();
			return;
		}
		_mode &= 0x7fff;
		if (!_mode || !_speed) {
			updateAnim();
			_mode = 0;
			advanceBlends(elapsed);
			updateBlend();
			return;
		}
	}

	if (_mode) {
		//do anim...
		_time += _speed * elapsed;

		switch (_mode) {
		case ANIM_MODE_LOOP:
			_time = fmod(_time, _seq_len);
			if (_time < 0) _time += _seq_len;
			break;
		case ANIM_MODE_PINGPONG:
			_time = fmod(_time, _seq_len * 2);
			if (_time < 0) _time += _seq_len * 2;
			if (_time >= _seq_len) { _time = _seq_len - (_time - _seq_len); _speed = -_speed; }
			break;
		case ANIM_MODE_ONESHOT:
			if (_time < 0) { _time = 0; _mode = 0; }
			else if (_time >= _seq_len) { _time = _seq_len; _mode = 0; }
			break;
		}

		updateAnim();
	}

	advanceBlends(elapsed);
	updateBlend();
}