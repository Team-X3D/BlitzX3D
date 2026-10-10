#include "std.h"
#include "bbaudio.h"
#include "../MultiLang/MultiLang.h"

sdlAudio* sdl_audio;

static inline void debugSound(sdlSound* s, const char* function) {
	if (!sdl_audio->verifySound(s)) ErrorLog(function, MultiLang::sound_not_exist);
}

static sdlSound* loadSound(BBStr* f, bool use_3d) {
	std::string t = *f; delete f;
	return sdl_audio ? sdl_audio->loadSound(t, use_3d) : 0;
}

static sdlChannel* playMusic(BBStr* f, bool use_3d, int mode) {
	std::string t = *f; delete f;
	return sdl_audio ? sdl_audio->playFile(t, use_3d, mode) : 0;
}

int bbVerifySound(sdlSound* sound) {
	return (bool)sdl_audio->verifySound(sound);
}

sdlSound* bbLoadSound(BBStr* f) {
	return loadSound(f, false);
}

void bbFreeSound(sdlSound* sound) {
	if (!sound) return;
	debugSound(sound, "FreeSound");
	sdl_audio->freeSound(sound);
}

void bbLoopSound(sdlSound* sound) {
	if (!sound) return;
	debugSound(sound, "LoopSound");
	sound->setLoop(true);
}

void bbSoundPitch(sdlSound* sound, int pitch) {
	if (!sound) return;
	debugSound(sound, "SoundPitch");
	sound->setPitch(pitch);
}

void bbSoundVolume(sdlSound* sound, float volume) {
	if (!sound) return;
	debugSound(sound, "SoundVolume");
	sound->setVolume(volume);
}

void bbSoundPan(sdlSound* sound, float pan) {
	if (!sound) return;
	debugSound(sound, "SoundPan");
	sound->setPan(pan);
}

sdlChannel* bbPlaySound(sdlSound* sound) {
	if (!sound) return 0;
	debugSound(sound, "PlaySound");
	return sound->play();
}

sdlChannel* bbPlayMusic(BBStr* f, int mode) {
	return playMusic(f, false, mode);
}

sdlChannel* bbPlayCDTrack(int track, int mode) {
	return sdl_audio ? sdl_audio->playCDTrack(track, mode) : 0;
}

void bbSetMasterVolume(float volume) {
	if (!sdl_audio) return;
	sdl_audio->setVolume(volume);
}

void bbSetReverb(float in_gain, float reverb_mix, float reverb_time, float high_freq_ratio) {
	if (!sdl_audio) return;
	sdl_audio->setReverb(in_gain, reverb_mix, reverb_time, high_freq_ratio);
}

void bbStopChannel(sdlChannel* channel) {
	if (!channel) return;
	channel->stop();
}

void bbPauseChannel(sdlChannel* channel) {
	if (!channel) return;
	channel->setPaused(true);
}

void bbResumeChannel(sdlChannel* channel) {
	if (!channel) return;
	channel->setPaused(false);
}

void bbChannelPitch(sdlChannel* channel, int pitch) {
	if (!channel) return;
	channel->setPitch(pitch);
}

void bbChannelVolume(sdlChannel* channel, float volume) {
	if (!channel) return;
	channel->setVolume(volume);
}

void bbChannelPan(sdlChannel* channel, float pan) {
	if (!channel) return;
	channel->setPan(pan);
}

int bbChannelPlaying(sdlChannel* channel) {
	return channel ? channel->isPlaying() : 0;
}

int bbChannelPosition(sdlChannel* channel) {
	return channel ? (int)(channel->getPosition() * 1000.0) : 0;
}

int bbChannelLength(sdlChannel* channel) {
	return channel ? (int)(channel->getLength() * 1000.0) : 0;
}

void bbSetChannelPosition(sdlChannel* channel, int position) {
	if (channel) channel->setPosition(position / 1000.0);
}

sdlSound* bbLoad3DSound(BBStr* f) {
	return loadSound(f, true);
}

bool audio_create() {
	sdl_audio = sdl_runtime->openAudio(0);
	return true;
}

bool audio_destroy() {
	if (sdl_audio) sdl_runtime->closeAudio(sdl_audio);
	sdl_audio = 0;
	return true;
}

void audio_link(void(*rtSym)(const char*, void*)) {
	rtSym("%VerifySound%sound", bbVerifySound);
	rtSym("%LoadSound$filename", bbLoadSound);
	rtSym("FreeSound%sound", bbFreeSound);
	rtSym("LoopSound%sound", bbLoopSound);
	rtSym("SoundPitch%sound%pitch", bbSoundPitch);
	rtSym("SoundVolume%sound#volume", bbSoundVolume);
	rtSym("SoundPan%sound#pan", bbSoundPan);
	rtSym("%PlaySound%sound", bbPlaySound);
	rtSym("%PlayMusic$midifile%mode=0", bbPlayMusic);
	rtSym("%PlayCDTrack%track%mode=1", bbPlayCDTrack);
	rtSym("StopChannel%channel", bbStopChannel);
	rtSym("PauseChannel%channel", bbPauseChannel);
	rtSym("ResumeChannel%channel", bbResumeChannel);
	rtSym("ChannelPitch%channel%pitch", bbChannelPitch);
	rtSym("ChannelVolume%channel#volume", bbChannelVolume);
	rtSym("ChannelPan%channel#pan", bbChannelPan);
	rtSym("%ChannelPlaying%channel", bbChannelPlaying);
	rtSym("%ChannelPosition%channel", bbChannelPosition);
	rtSym("%ChannelLength%channel", bbChannelLength);
	rtSym("SetChannelPosition%channel%position", bbSetChannelPosition);
	rtSym("%Load3DSound$filename", bbLoad3DSound);
	rtSym("SetMasterVolume#volume", bbSetMasterVolume);
	rtSym("SetReverb#in_gain#reverb_mix#reverb_time#high_freq_ratio", bbSetReverb);
}