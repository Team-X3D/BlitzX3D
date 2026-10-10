#include "std.h"
#include "asyncsound.h"

#include <chrono>

AsyncSoundLoader& AsyncSoundLoader::instance() {
	static AsyncSoundLoader loader;
	return loader;
}

static std::shared_ptr<std::vector<char>> readFile(const std::string& file) {
	std::ifstream in(file.c_str(), std::ios::binary);
	if (!in) return nullptr;

	in.seekg(0, std::ios::end);
	std::streamoff size = in.tellg();
	if (size <= 0) return nullptr;
	in.seekg(0, std::ios::beg);

	auto data = std::make_shared<std::vector<char>>((size_t)size);
	in.read(data->data(), size);
	if (!in) return nullptr;

	return data;
}

AsyncSoundLoader::AsyncSoundLoader() :
	inFlight(0), shutdown(false) {
	unsigned count = std::thread::hardware_concurrency() / 2;
	if (count < 2) count = 2;
	if (count > 4) count = 4;
	for (unsigned n = 0; n < count; ++n) threads.push_back(std::thread(&AsyncSoundLoader::worker, this));
}

AsyncSoundLoader::~AsyncSoundLoader() {
	{
		std::unique_lock<std::mutex> lock(mutex);
		shutdown = true;
	}
	cv.notify_all();
	for (auto& t : threads) t.join();
}

std::shared_ptr<AsyncSoundLoader::Job> AsyncSoundLoader::load(const std::string& file) {
	auto job = std::make_shared<Job>(file);
	{
		std::unique_lock<std::mutex> lock(mutex);
		queue.push_back(job);
	}
	cv.notify_one();
	return job;
}

void AsyncSoundLoader::worker() {
	for (;;) {
		std::shared_ptr<Job> job;
		{
			std::unique_lock<std::mutex> lock(mutex);
			cv.wait(lock, [this]() { return shutdown || !queue.empty(); });
			if (shutdown && queue.empty()) return;
			job = queue.back();
			queue.pop_back();
			++inFlight;
			job->state.store(STATE_DECODING);
		}

		auto data = readFile(job->file);

		std::unique_lock<std::mutex> lock(mutex);
		if (job->state.load() == STATE_CANCELLED) {
			--inFlight;
			cv.notify_all();
			continue;
		}
		if (data) {
			job->data = data;
			job->state.store(STATE_DONE);
		}
		else {
			job->state.store(STATE_FAILED);
		}
		--inFlight;
		cv.notify_all();
	}
}

void AsyncSoundLoader::wait(const std::shared_ptr<Job>& job) {
	std::unique_lock<std::mutex> lock(mutex);

	if (job->state.load() == STATE_QUEUED) {
		for (auto it = queue.begin(); it != queue.end(); ++it) {
			if (*it == job) { queue.erase(it); break; }
		}
		job->state.store(STATE_DECODING);
		lock.unlock();

		auto data = readFile(job->file);

		lock.lock();
		if (data) {
			job->data = data;
			job->state.store(STATE_DONE);
		}
		else {
			job->state.store(STATE_FAILED);
		}
		cv.notify_all();
		return;
	}

	cv.wait(lock, [&]() {
		int s = job->state.load();
		return s == STATE_DONE || s == STATE_FAILED || s == STATE_CANCELLED;
	});
}

void AsyncSoundLoader::waitAll() {
	std::unique_lock<std::mutex> lock(mutex);
	cv.wait_for(lock, std::chrono::seconds(20), [this]() { return queue.empty() && inFlight == 0; });
}

void AsyncSoundLoader::cancel(const std::shared_ptr<Job>& job) {
	std::unique_lock<std::mutex> lock(mutex);
	for (auto it = queue.begin(); it != queue.end(); ++it) {
		if (*it == job) {
			queue.erase(it);
			break;
		}
	}
	int s = job->state.load();
	if (s == STATE_DONE || s == STATE_FAILED) {
		job->data.reset();
	}
	job->state.store(STATE_CANCELLED);
}