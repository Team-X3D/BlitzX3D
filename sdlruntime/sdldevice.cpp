#include "std.h"
#include "sdldevice.h"
#include "sdlruntime.h"

sdlDevice::sdlDevice() {
	reset();
}

sdlDevice::~sdlDevice() {
}

void sdlDevice::reset() {
	memset(down_state, 0, sizeof(down_state));
	memset(axis_states, 0, sizeof(axis_states));
	memset(hit_count, 0, sizeof(hit_count));
	put = get = 0;
}

void sdlDevice::downEvent(int key) {
	down_state[key] = true;
	++hit_count[key];
	if(put - get < QUE_SIZE) que[put++ & QUE_MASK] = key;
}

void sdlDevice::upEvent(int key) {
	down_state[key] = false;
}

void sdlDevice::setDownState(int key, bool down) {
	if(down == down_state[key]) return;
	if(down) downEvent(key);
	else upEvent(key);
}

void sdlDevice::flush() {
	update();
	memset(hit_count, 0, sizeof(hit_count));
	put = get = 0;
}

bool sdlDevice::keyDown(int key) {
	update();
	return down_state[key];
}

int sdlDevice::keyHit(int key) {
	update();
	int n = hit_count[key];
	hit_count[key] -= n;
	return n;
}

int sdlDevice::getKey() {
	update();
	return get < put ? que[get++ & QUE_MASK] : 0;
}

float sdlDevice::getAxisState(int axis) {
	update();
	return axis_states[axis];
}