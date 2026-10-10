#ifndef SDLSOUND_H
#define SDLSOUND_H

#include "sdlchannel.h"
#include "asyncsound.h"
#include "bass.h"

class sdlAudio;

class sdlSound {
public:
	sdlAudio* audio;

	sdlSound(sdlAudio* audio, HSAMPLE sample);
	sdlSound(sdlAudio* audio, const std::shared_ptr<AsyncSoundLoader::Job>& job, bool use_3d);
	~sdlSound();

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

	static std::vector<sdlSound*> pending;

	/***** GX INTERFACE *****/
public:
	//actions
	sdlChannel* play();
	sdlChannel* play3d(const float pos[3], const float vel[3]);

	//modifiers
	void setLoop(bool loop);
	void setPitch(int hertz);
	void setVolume(float volume);
	void setPan(float pan);
};

#endif