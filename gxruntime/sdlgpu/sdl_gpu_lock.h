#ifndef SDL_GPU_LOCK_H
#define SDL_GPU_LOCK_H

#include <mutex>

namespace sdlgpu {

inline std::recursive_mutex& GpuMutex() {
	static std::recursive_mutex m;
	return m;
}
struct GpuLock {
	std::lock_guard<std::recursive_mutex> g;
	GpuLock() : g(GpuMutex()) {}
};

}

#endif
