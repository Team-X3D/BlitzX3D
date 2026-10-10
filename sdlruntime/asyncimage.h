#ifndef ASYNCIMAGE_H
#define ASYNCIMAGE_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct DecodedImage {
	int w = 0, h = 0;
	std::vector<uint8_t> rgba;
	bool hasAlpha = false;
};

std::unique_ptr<DecodedImage> DecodeImageFile(const std::string& file, std::string* err = nullptr);

class AsyncImageLoader {
public:
	enum {
		STATE_QUEUED = 0,
		STATE_DECODING = 1,
		STATE_DONE = 2,
		STATE_FAILED = 3,
		STATE_CANCELLED = 4
	};

	struct Job {
		std::string file;
		std::unique_ptr<DecodedImage> image;
		std::atomic<int> state;

		Job() : state(STATE_QUEUED) {}
		explicit Job(const std::string& f) : file(f), state(STATE_QUEUED) {}
	};

	static AsyncImageLoader& instance();

	std::shared_ptr<Job> load(const std::string& file);
	void wait(const std::shared_ptr<Job>& job);
	void waitAll();
	void cancel(const std::shared_ptr<Job>& job);
	bool isReady(const std::shared_ptr<Job>& job) const {
		return job->state.load() == STATE_DONE;
	}

private:
	AsyncImageLoader();
	~AsyncImageLoader();
	AsyncImageLoader(const AsyncImageLoader&) = delete;
	AsyncImageLoader& operator=(const AsyncImageLoader&) = delete;

	void worker();

	std::vector<std::shared_ptr<Job>> queue;
	std::vector<std::thread> threads;
	std::mutex mutex;
	std::condition_variable cv;
	int inFlight;
	bool shutdown;
};

#endif