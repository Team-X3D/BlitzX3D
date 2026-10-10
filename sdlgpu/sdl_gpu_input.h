#ifndef SDL_GPU_INPUT_H
#define SDL_GPU_INPUT_H

struct SDL_Window;

namespace sdlgpu {

struct InputSink {
	virtual ~InputSink() {}
	virtual void keyDown(int dik) {}
	virtual void keyUp(int dik) {}
	virtual void charInput(int codepoint) {}
	virtual void mouseDown(int button) {}
	virtual void mouseUp(int button) {}
	virtual void mouseMove(int x, int y) {}
	virtual void mouseWheel(int delta) {}
};

struct WindowHost {
	virtual ~WindowHost() {}
	virtual void requestQuit() {}
	virtual void gameSize(int* w, int* h) { *w = 0; *h = 0; }
};

void PumpEvents(SDL_Window* win, InputSink* input, WindowHost* host);

}

#endif
