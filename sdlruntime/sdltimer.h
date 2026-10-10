#ifndef SDLTIMER_H
#define SDLTIMER_H

#include <mmsystem.h>

class sdlRuntime;

class sdlTimer {
public:
	sdlTimer(sdlRuntime* rt, int hertz);
	~sdlTimer();

	static void CALLBACK timerCallback(UINT id, UINT msg, DWORD user, DWORD dw1, DWORD dw2);

private:
	sdlRuntime* runtime;
	HANDLE event;
	MMRESULT timerID;
	int ticks_put, ticks_get;

	/***** GX INTERFACE *****/
public:
	int wait();
};

#endif