#ifndef SDLAUDIO_H
#define SDLAUDIO_H

#include <string>

#include "sdlsound.h"
#include "bass.h"

class sdlRuntime;

class sdlAudio {
public:
	sdlRuntime* runtime;

	sdlAudio(sdlRuntime* runtime);
	~sdlAudio();

	sdlChannel* play(HSAMPLE sample, float def_vol);
	sdlChannel* play3d(HSAMPLE sample, const float pos[3], const float vel[3], float def_vol);

	void pause();
	void resume();

private:
	HSTREAM reverb_stream;
	HFX reverb_fx;

	/***** GX INTERFACE *****/
public:
	enum {
		CD_MODE_ONCE = 1, CD_MODE_LOOP, CD_MODE_ALL
	};

	sdlSound* loadSound(const std::string& filename, bool use_3d);
	sdlSound* verifySound(sdlSound* sound);
	void freeSound(sdlSound* sound);

	void setPaused(bool paused);	//master pause
	void setVolume(float volume);	//master volume
	void setReverb(float in_gain, float reverb_mix, float reverb_time, float high_freq_ratio);

	void set3dOptions(float roll, float dopp, float dist);

	void set3dListener(const float pos[3], const float vel[3], const float forward[3], const float up[3]);

	sdlChannel* playCDTrack(int track, int mode);
	sdlChannel* playFile(const std::string& filename, bool use_3d, int mode);
};

#endif