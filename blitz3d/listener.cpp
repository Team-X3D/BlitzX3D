#include "std.h"
#include "listener.h"

extern sdlAudio* sdl_audio;

Listener::Listener(float roll, float dopp, float dist) {
	if (!sdl_audio) return;

	sdl_audio->set3dOptions(roll, dopp, dist);
	renderListener();
}

Listener::Listener(const Listener& t) :
	Object(t) {
}

Listener::~Listener() {
	if (!sdl_audio) return;

	Vector pos, vel, up(0, 1, 1), forward(0, 0, 1);
	sdl_audio->set3dListener(&pos.x, &vel.x, &forward.x, &up.x);
}

void Listener::renderListener() {
	if (!sdl_audio) return;

	const Vector& pos = getWorldTform().v;
	const Vector& vel = getVelocity();
	const Vector& forward = getWorldTform().m.k.normalized();
	const Vector& up = getWorldTform().m.j.normalized();

	sdl_audio->set3dListener(&pos.x, &vel.x, &forward.x, &up.x);
}