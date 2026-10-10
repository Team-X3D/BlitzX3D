#include "std.h"
#include "sdlsound.h"
#include "sdlaudio.h"
#include "bass.h"

std::vector<sdlSound*> sdlSound::pending;

sdlSound::sdlSound(sdlAudio* a, HSAMPLE s) :
	audio(a), sample(s), job(), use_3d(false), materialized(true), failed(false), defs_valid(true) {
	BASS_SAMPLE info;
	BASS_SampleGetInfo(sample, &info);
	def_freq = info.freq;
	def_vol = info.volume;
	def_pan = info.pan;
}

sdlSound::sdlSound(sdlAudio* a, const std::shared_ptr<AsyncSoundLoader::Job>& j, bool u3d) :
	audio(a), sample(0), job(j), use_3d(u3d), materialized(false), failed(false), defs_valid(false) {
	pending.push_back(this);
}

sdlSound::~sdlSound() {
	cancelJob();
	if (sample) {
		BASS_SampleFree(sample);
		sample = 0;
	}
}

void sdlSound::cancelJob() {
	for (auto it = pending.begin(); it != pending.end(); ++it) {
		if (*it == this) {
			pending.erase(it);
			break;
		}
	}
	if (job) {
		AsyncSoundLoader::instance().cancel(job);
		job.reset();
	}
}

bool sdlSound::materialize(bool blocking) {
	if (materialized) return !failed;

	if (job) {
		if (blocking) {
			AsyncSoundLoader::instance().wait(job);
		}
		else {
			int s = job->state.load();
			if (s == AsyncSoundLoader::STATE_QUEUED || s == AsyncSoundLoader::STATE_DECODING) return false;
		}
		if (job->state.load() == AsyncSoundLoader::STATE_FAILED) {
			failed = true;
			cancelJob();
			materialized = true;
			return false;
		}
		if (job->state.load() == AsyncSoundLoader::STATE_CANCELLED) {
			failed = true;
			cancelJob();
			materialized = true;
			return false;
		}

		std::shared_ptr<std::vector<char>> data = job->data;
		if (!data || data->empty()) {
			failed = true;
			cancelJob();
			materialized = true;
			return false;
		}

		DWORD flags = 0;
		if (use_3d) flags |= BASS_SAMPLE_3D | BASS_SAMPLE_MONO;
		sample = BASS_SampleLoad(BASS_FILE_MEM, data->data(), 0, (DWORD)data->size(), 64, flags);
		if (!sample) {
			sample = BASS_SampleLoad(FALSE, job->file.c_str(), 0, 0, 64, flags);
		}
		if (!sample) {
			failed = true;
			cancelJob();
			materialized = true;
			return false;
		}
		cancelJob();
		BASS_SAMPLE info;
		BASS_SampleGetInfo(sample, &info);
		def_freq = info.freq;
		def_vol = info.volume;
		def_pan = info.pan;
		defs_valid = true;
		materialized = true;
	}
	return !failed;
}

void sdlSound::setDefaults() {
	if(!defs_valid) {
		BASS_SAMPLE info;
		BASS_SampleGetInfo(sample, &info);
		info.freq = def_freq;
		info.pan = def_pan;
		BASS_SampleSetInfo(sample, &info);
		defs_valid = true;
	}
}

sdlChannel* sdlSound::play() {
	if (!materialize(true)) return 0;
	setDefaults();
	return audio->play(sample, def_vol);
}

sdlChannel* sdlSound::play3d(const float pos[3], const float vel[3]) {
	if (!materialize(true)) return 0;
	setDefaults();
	return audio->play3d(sample, pos, vel, def_vol);
}

void sdlSound::setLoop(bool loop) {
	if (!materialize(true)) return;
	BASS_SAMPLE info;
	BASS_SampleGetInfo(sample, &info);
	if (loop) info.flags |= BASS_SAMPLE_LOOP;
	else info.flags &= ~BASS_SAMPLE_LOOP;
	BASS_SampleSetInfo(sample, &info);
}

void sdlSound::setPitch(int hertz) {
	if (!materialize(true)) return;
	def_freq = hertz;
	defs_valid = false;
}

void sdlSound::setVolume(float volume) {
	if (!materialize(true)) return;
	if (volume < 0.0f) volume = 0.0f;
	else if (volume > 1.0f) volume = 1.0f;
	def_vol = volume;
	defs_valid = false;
}

void sdlSound::setPan(float pan) {
	if (!materialize(true)) return;
	def_pan = pan;
	defs_valid = false;
}

void sdlSound::flushAll() {
	if (pending.empty()) return;
	std::vector<sdlSound*> snapshot(pending.begin(), pending.end());
	for (size_t idx = 0; idx < snapshot.size(); ++idx) {
		snapshot[idx]->materialize(false);
	}
}