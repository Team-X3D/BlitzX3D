#ifndef SDLINPUT_H
#define SDLINPUT_H

#define DIRECTINPUT_VERSION 0x0800

#include <dinput.h>

#include "sdldevice.h"
#include <vector>

class sdlRuntime;
class SDLInputDevice;

class sdlInput {
public:
	sdlRuntime* runtime;
	IDirectInput8* dirInput;

	sdlInput(sdlRuntime* runtime, IDirectInput8* di);
	~sdlInput();

	void reset();
	bool acquire();
	void unacquire();

	void wm_keydown(int key);
	void wm_keyup(int key);
	void wm_mousedown(int key);
	void wm_mouseup(int key);
	void wm_mousemove(int x, int y);
	void wm_mousewheel(int dz);
	void wm_char(int wParam, int lParam);

	bool rumble(int port, float left, float right);

private:

	/***** GX INTERFACE *****/
public:
	enum {
		ASC_HOME = 1, ASC_END = 2, ASC_INSERT = 3, ASC_DELETE = 4,
		ASC_PAGEUP = 5, ASC_PAGEDOWN = 6,
		ASC_UP = 28, ASC_DOWN = 29, ASC_RIGHT = 30, ASC_LEFT = 31
	};

	std::vector<SDLInputDevice*> sdl_devices;
	int gamepad_count;
	bool sdl_ok;
	unsigned last_pump;

	SDLInputDevice* findDevice(int port);
	SDLInputDevice* findByInstance(unsigned id);
	SDLInputDevice* addGamepad(unsigned id);
	SDLInputDevice* addJoystick(unsigned id);
	void disconnectDevice(unsigned id);
	void pumpEvents(bool force);

	void moveMouse(int x, int y);

	sdlDevice* getMouse()const;
	sdlDevice* getKeyboard()const;
	sdlDevice* getJoystick(int port)const;
	std::vector<int> getChars();
	bool getControllerConnected(int port);
	int getJoystickType(int port)const;
	int numJoysticks()const;
	int toUnicode(int key)const;
};

#endif