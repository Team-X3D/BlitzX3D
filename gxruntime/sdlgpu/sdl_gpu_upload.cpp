#include "sdl_gpu_upload.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include <SDL3/SDL_gpu.h>

namespace sdlgpu {

struct UploadHandle {
	SDL_GPUDevice* dev = nullptr;
	UploadFn fn = nullptr;
	void* ctx = nullptr;
	std::mutex m;
	std::condition_variable cv;
	bool done = false;
	bool ok = false;
};

namespace {
std::mutex g_m;
std::condition_variable g_cv;
std::deque<UploadHandle*> g_q;
std::thread g_worker;
bool g_started = false;
bool g_stop = false;

void Finish(UploadHandle* h, bool ok) {
	{
		std::lock_guard<std::mutex> lk(h->m);
		h->ok = ok;
		h->done = true;
	}
	h->cv.notify_one();
}

void RunGroup(SDL_GPUDevice* dev, const std::vector<UploadHandle*>& group) {
	SDL_GPUCommandBuffer* cmds = dev ? SDL_AcquireGPUCommandBuffer(dev) : nullptr;
	if (!cmds) {
		for (UploadHandle* h : group) Finish(h, h->fn ? h->fn(h->ctx, nullptr) : false);
		return;
	}
	std::vector<char> ok(group.size());
	for (size_t i = 0; i < group.size(); ++i)
		ok[i] = group[i]->fn ? group[i]->fn(group[i]->ctx, cmds) : false;
	bool submitted = SDL_SubmitGPUCommandBuffer(cmds);
	for (size_t i = 0; i < group.size(); ++i)
		Finish(group[i], submitted && ok[i]);
}

void WorkerMain() {
	for (;;) {
		std::deque<UploadHandle*> batch;
		{
			std::unique_lock<std::mutex> lk(g_m);
			g_cv.wait(lk, [] { return g_stop || !g_q.empty(); });
			if (g_stop && g_q.empty()) return;
			batch.swap(g_q);
		}
		while (!batch.empty()) {
			SDL_GPUDevice* dev = batch.front()->dev;
			std::vector<UploadHandle*> group;
			for (auto it = batch.begin(); it != batch.end(); ) {
				if ((*it)->dev == dev) { group.push_back(*it); it = batch.erase(it); }
				else ++it;
			}
			RunGroup(dev, group);
		}
	}
}

bool EnsureWorker() {
	if (g_started) return !g_stop;
	if (g_stop) return false;
	try {
		g_worker = std::thread(WorkerMain);
	} catch (...) {
		return false;
	}
	g_started = true;
	return true;
}
}

UploadHandle* EnqueueUpload(SDL_GPUDevice* dev, UploadFn fn, void* ctx) {
	std::lock_guard<std::mutex> lk(g_m);
	UploadHandle* h = new UploadHandle;
	h->dev = dev;
	if (!EnsureWorker()) {
		h->ok = fn ? fn(ctx, nullptr) : false;
		h->done = true;
		return h;
	}
	h->fn = fn;
	h->ctx = ctx;
	g_q.push_back(h);
	g_cv.notify_one();
	return h;
}

bool WaitUpload(UploadHandle* h) {
	if (!h) return false;
	{
		std::unique_lock<std::mutex> lk(h->m);
		h->cv.wait(lk, [h] { return h->done; });
	}
	bool ok = h->ok;
	delete h;
	return ok;
}

void ShutdownUploads() {
	{
		std::lock_guard<std::mutex> lk(g_m);
		if (!g_started) return;
		g_stop = true;
	}
	g_cv.notify_all();
	if (g_worker.joinable()) g_worker.join();
	std::lock_guard<std::mutex> lk(g_m);
	g_started = false;
	g_stop = false;
}

}
