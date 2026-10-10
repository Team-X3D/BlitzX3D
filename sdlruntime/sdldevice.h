#ifndef SDLDEVICE_H
#define SDLDEVICE_H

class sdlDevice {
public:
	float axis_states[32];

	sdlDevice();
	virtual ~sdlDevice();

	virtual void update() {}

	void reset();
	void downEvent(int key);
	void upEvent(int key);
	void setDownState(int key, bool down);

private:
	enum {
		QUE_SIZE = 32, QUE_MASK = QUE_SIZE - 1
	};
	int hit_count[256];			//how many hits of key
	bool down_state[256];			//time key went down
	int que[QUE_SIZE], put, get;

	/***** GX INTERFACE *****/
public:
	void flush();
	bool keyDown(int key);
	int keyHit(int key);
	int getKey();
	float getAxisState(int axis);
};

#endif