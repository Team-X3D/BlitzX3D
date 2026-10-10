#ifndef BBAUDIO_H
#define BBAUDIO_H

#include "bbsys.h"
#include "../sdlruntime/sdlaudio.h"

extern sdlAudio* sdl_audio;

sdlSound* bbLoadSound(BBStr* file);
void		 bbFreeSound(sdlSound* sound);
sdlChannel* bbPlaySound(sdlSound* sound);
void		 bbLoopSound(sdlSound* sound);
void		 bbSoundPitch(sdlSound* sound, int pitch);
void		 bbSoundVolume(sdlSound* sound, float volume);
void		 bbSoundPan(sdlSound* sound, float pan);
sdlChannel* bbPlayMusic(BBStr* s, int mode);
sdlChannel* bbPlayCDTrack(int track, int mode);
void		 bbSetMasterVolume(float volume);
void		 bbSetReverb(float in_gain, float reverb_mix, float reverb_time, float high_freq_ratio);
void		 bbStopChannel(sdlChannel* channel);
void		 bbPauseChannel(sdlChannel* channel);
void		 bbResumeChannel(sdlChannel* channel);
void		 bbChannelPitch(sdlChannel* channel, int pitch);
void		 bbChannelVolume(sdlChannel* channel, float volume);
void		 bbChannelPan(sdlChannel* channel, float pan);
int			 bbChannelPlaying(sdlChannel* channel);

#endif
