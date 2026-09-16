#ifndef GXSOUND_H
#define GXSOUND_H

#include "gxchannel.h"
#include "asyncsound.h"
#include "bass.h"

class gxAudio;

class gxSound {
public:
	gxAudio* audio;

	gxSound(gxAudio* audio, HSAMPLE sample);
	gxSound(gxAudio* audio, const std::shared_ptr<AsyncSoundLoader::Job>& job, bool use_3d);
	~gxSound();

	static void flushAll();

private:
	bool defs_valid;
	int def_freq;
	float def_vol, def_pan;
	HSAMPLE sample;
	std::shared_ptr<AsyncSoundLoader::Job> job;
	bool use_3d;
	bool materialized;
	bool failed;
	float pos[3], vel[3];

	void setDefaults();
	void cancelJob();
	bool materialize(bool blocking);

	static std::vector<gxSound*> pending;

	/***** GX INTERFACE *****/
public:
	//actions
	gxChannel* play();
	gxChannel* play3d(const float pos[3], const float vel[3]);

	//modifiers
	void setLoop(bool loop);
	void setPitch(int hertz);
	void setVolume(float volume);
	void setPan(float pan);
};

#endif