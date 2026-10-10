#ifndef SDLCHANNEL_H
#define SDLCHANNEL_H

class sdlChannel {

	/***** GX INTERFACE *****/
public:
	virtual ~sdlChannel();
	//modifiers
	virtual void stop() = 0;
	virtual void setPaused(bool paused) = 0;
	virtual void setPitch(int pitch) = 0;
	virtual void setVolume(float volume) = 0;
	virtual void setPan(float pan) = 0;
	virtual void set3d(const float pos[3], const float vel[3]) = 0;

	virtual double getPosition() = 0;	//seconds
	virtual double getLength() = 0;		//seconds
	virtual void setPosition(double seconds) = 0;

	virtual bool isPlaying() = 0;
};

#endif