#include "std.h"
#include "gxaudio.h"
#include "asyncsound.h"
#include "bass.h"

struct StaticChannel : public gxChannel {
	virtual void play() = 0;
};

struct SoundChannel : public gxChannel {
	SoundChannel() :channel(0) {
	}
	void set(HCHANNEL n) {
		channel = n;
	}
	void stop() {
		BASS_ChannelStop(channel);
	}
	void setPaused(bool paused) {
		if (paused) {
			BASS_ChannelPause(channel);
		}
		else if (BASS_ChannelIsActive(channel) == BASS_ACTIVE_PAUSED) {
			BASS_ChannelPlay(channel, FALSE);
		}
	}
	void setPitch(int pitch) {
		BASS_ChannelSetAttribute(channel, BASS_ATTRIB_FREQ, (float)pitch);
	}
	void setVolume(float volume) {
		if (volume < 0.0f) volume = 0.0f;
		else if (volume > 1.0f) volume = 1.0f;
		BASS_ChannelSetAttribute(channel, BASS_ATTRIB_VOL, volume);
	}
	void setPan(float pan) {
		BASS_ChannelSetAttribute(channel, BASS_ATTRIB_PAN, pan);
	}
	void set3d(const float pos[3], const float vel[3]) {
		BASS_3DVECTOR p = { pos[0], pos[1], pos[2] };
		BASS_3DVECTOR v = { vel[0], vel[1], vel[2] };
		BASS_ChannelSet3DPosition(channel, &p, 0, &v);
		BASS_Apply3D();
	}
	bool isPlaying() {
		return BASS_ChannelIsActive(channel) != BASS_ACTIVE_STOPPED;
	}
private:
	HCHANNEL channel;
};

struct StreamChannel : public StaticChannel {
	StreamChannel(HSTREAM s) :stream(s), channel(s) {
		BASS_ChannelPlay(stream, TRUE);
	}
	~StreamChannel() {
		BASS_StreamFree(stream);
	}
	void play() {
		stop();
		channel = stream;
		BASS_ChannelPlay(stream, TRUE);
	}
	void stop() {
		BASS_ChannelStop(stream);
		channel = 0;
	}
	void setPaused(bool paused) {
		if (paused) {
			BASS_ChannelPause(channel);
		}
		else if (BASS_ChannelIsActive(channel) == BASS_ACTIVE_PAUSED) {
			BASS_ChannelPlay(channel, FALSE);
		}
	}
	void setPitch(int pitch) {
		BASS_ChannelSetAttribute(channel, BASS_ATTRIB_FREQ, (float)pitch);
	}
	void setVolume(float volume) {
		if (volume < 0.0f) volume = 0.0f;
		else if (volume > 1.0f) volume = 1.0f;
		BASS_ChannelSetAttribute(channel, BASS_ATTRIB_VOL, volume);
	}
	void setPan(float pan) {
		BASS_ChannelSetAttribute(channel, BASS_ATTRIB_PAN, pan);
	}
	void set3d(const float pos[3], const float vel[3]) {
	}
	bool isPlaying() {
		return channel && BASS_ChannelIsActive(channel) != BASS_ACTIVE_STOPPED;
	}
private:
	HSTREAM stream;
	HCHANNEL channel;
};

static std::set<gxSound*> sound_set;
static std::vector<gxChannel*> channels;
static std::map<std::string, StaticChannel*> songs;

static int next_chan;
static std::vector<SoundChannel*> soundChannels;

static gxChannel* allocSoundChannel(HCHANNEL n) {

	SoundChannel* chan = 0;
	for (int k = 0; k < soundChannels.size(); ++k) {
		chan = soundChannels[next_chan];
		if (!chan) {
			chan = soundChannels[next_chan] = new SoundChannel();
			channels.push_back(chan);
		}
		else if (chan->isPlaying()) {
			chan = 0;
		}
		if (++next_chan == soundChannels.size()) next_chan = 0;
		if (chan) break;
	}

	if (!chan) {
		next_chan = soundChannels.size();
		soundChannels.resize(soundChannels.size() * 2);
		for (int k = next_chan; k < soundChannels.size(); ++k) soundChannels[k] = 0;
		chan = soundChannels[next_chan++] = new SoundChannel();
		channels.push_back(chan);
	}

	chan->set(n);
	return chan;
}

gxAudio::gxAudio(gxRuntime* r) :
	runtime(r) {
	next_chan = 0;
	static const size_t kInitialSoundChannels = 32;
	soundChannels.resize(kInitialSoundChannels);
	for (size_t k = 0; k < kInitialSoundChannels; ++k) soundChannels[k] = 0;
}

gxAudio::~gxAudio() {
	//free all channels
	for (; channels.size(); channels.pop_back()) delete channels.back();
	//free all sound_set
	while (sound_set.size()) freeSound(*sound_set.begin());
	soundChannels.clear();
	songs.clear();
}

gxChannel* gxAudio::play(HSAMPLE sample, float def_vol) {
	HCHANNEL n = BASS_SampleGetChannel(sample, 0);
	if (!n) return 0;
	BASS_ChannelSetAttribute(n, BASS_ATTRIB_VOL, def_vol);
	BASS_ChannelSetAttribute(n, BASS_ATTRIB_PAN, 0.0f);
	BASS_ChannelPlay(n, TRUE);
	return allocSoundChannel(n);
}

gxChannel* gxAudio::play3d(HSAMPLE sample, const float pos[3], const float vel[3], float def_vol) {
	HCHANNEL n = BASS_SampleGetChannel(sample, 0);
	if (!n) return 0;
	BASS_3DVECTOR p = { pos[0], pos[1], pos[2] };
	BASS_3DVECTOR v = { vel[0], vel[1], vel[2] };
	BASS_ChannelSetAttribute(n, BASS_ATTRIB_VOL, def_vol);
	BASS_ChannelSet3DPosition(n, &p, 0, &v);
	BASS_ChannelPlay(n, TRUE);
	BASS_Apply3D();
	return allocSoundChannel(n);
}

void gxAudio::pause() {
	BASS_Stop();
}

void gxAudio::resume() {
	BASS_Start();
}

static bool soundFileExists(const std::string& f) {
	DWORD attrs = GetFileAttributesA(f.c_str());
	return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static bool isAudioFile(const std::string& f) {
	HSTREAM probe = BASS_StreamCreateFile(FALSE, f.c_str(), 0, 0, 0);
	if (!probe) return false;
	BASS_StreamFree(probe);
	return true;
}

gxSound* gxAudio::loadSound(const std::string& f, bool use3d) {
	if (!soundFileExists(f)) return 0;
	if (!isAudioFile(f)) return 0;

	std::shared_ptr<AsyncSoundLoader::Job> job = AsyncSoundLoader::instance().load(f);
	gxSound* sound = new gxSound(this, job, use3d);
	sound_set.insert(sound);
	return sound;
}

gxSound* gxAudio::verifySound(gxSound* s) {
	return sound_set.count(s) ? s : 0;
}

void gxAudio::freeSound(gxSound* s) {
	if (sound_set.erase(s)) delete s;
}

void gxAudio::setPaused(bool paused) {
	if (paused) BASS_Stop();
	else BASS_Start();
}

void gxAudio::setVolume(float volume) {
}

void gxAudio::set3dOptions(float roll, float dopp, float dist) {
	BASS_Set3DFactors(dist, roll, dopp);
}

void gxAudio::set3dListener(const float pos[3], const float vel[3], const float forward[3], const float up[3]) {
	BASS_3DVECTOR p = { pos[0], pos[1], pos[2] };
	BASS_3DVECTOR v = { vel[0], vel[1], vel[2] };
	BASS_3DVECTOR f = { forward[0], forward[1], forward[2] };
	BASS_3DVECTOR t = { up[0], up[1], up[2] };
	BASS_Set3DPosition(&p, &v, &f, &t);
	BASS_Apply3D();
}

gxChannel* gxAudio::playFile(const std::string& t, bool use_3d, int mode) {
	std::string f = tolower(t);
	StaticChannel* chan = 0;
	std::map<std::string, StaticChannel*>::iterator it = songs.find(f);
	if (it != songs.end()) {
		chan = it->second;
		chan->play();
		return chan;
	}
	else {
		DWORD flags = 0;
		if (use_3d) flags |= BASS_SAMPLE_3D;
		if (mode & 0x00000002) flags |= BASS_SAMPLE_LOOP;
		HSTREAM stream = BASS_StreamCreateFile(FALSE, f.c_str(), 0, 0, flags);
		if (!stream) return 0;
		chan = new StreamChannel(stream);
	}
	channels.push_back(chan);
	songs[f] = chan;
	return chan;
}

gxChannel* gxAudio::playCDTrack(int track, int mode) {
	return 0;
}