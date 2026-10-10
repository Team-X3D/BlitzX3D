#include "std.h"
#include "sdltimer.h"
#include "sdlruntime.h"

sdlTimer::sdlTimer(sdlRuntime* rt, int hertz) :
	runtime(rt), ticks_get(0), ticks_put(0) {
	event = CreateEvent(0, false, false, 0);
	timerID = timeSetEvent(1000 / hertz, 0, timerCallback, (DWORD)this, TIME_PERIODIC);
}

sdlTimer::~sdlTimer() {
	timeKillEvent(timerID);
	CloseHandle(event);
}

void CALLBACK sdlTimer::timerCallback(UINT id, UINT msg, DWORD user, DWORD dw1, DWORD dw2) {
	sdlTimer* t = (sdlTimer*)user;
	++t->ticks_put;
	SetEvent(t->event);
}

int sdlTimer::wait() {
	for(;;) {
		if(WaitForSingleObject(event, 1000) == WAIT_OBJECT_0) break;
	}
	int n = ticks_put - ticks_get;
	ticks_get += n;
	return n;
}