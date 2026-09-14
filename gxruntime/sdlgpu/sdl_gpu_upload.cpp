#include "sdl_gpu_upload.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace sdlgpu {

struct UploadHandle {
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

void WorkerMain() {
	for (;;) {
		UploadHandle* h = nullptr;
		{
			std::unique_lock<std::mutex> lk(g_m);
			g_cv.wait(lk, [] { return g_stop || !g_q.empty(); });
			if (g_stop && g_q.empty()) return;
			h = g_q.front();
			g_q.pop_front();
		}
		bool ok = h->fn ? h->fn(h->ctx) : false;
		{
			std::lock_guard<std::mutex> lk(h->m);
			h->ok = ok;
			h->done = true;
		}
		h->cv.notify_one();
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

UploadHandle* EnqueueUpload(UploadFn fn, void* ctx) {
	std::lock_guard<std::mutex> lk(g_m);
	UploadHandle* h = new UploadHandle;
	if (!EnsureWorker()) {
		h->ok = fn ? fn(ctx) : false;
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
