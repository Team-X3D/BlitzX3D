#ifndef ASYNCSOUND_H
#define ASYNCSOUND_H

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class AsyncSoundLoader {
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
		std::shared_ptr<std::vector<char>> data;
		std::atomic<int> state;

		Job() : data(nullptr), state(STATE_QUEUED) {}
		explicit Job(const std::string& f) : file(f), data(nullptr), state(STATE_QUEUED) {}
	};

	static AsyncSoundLoader& instance();

	std::shared_ptr<Job> load(const std::string& file);
	void wait(const std::shared_ptr<Job>& job);
	void waitAll();
	void cancel(const std::shared_ptr<Job>& job);
	bool isReady(const std::shared_ptr<Job>& job) const {
		return job->state.load() == STATE_DONE;
	}

private:
	AsyncSoundLoader();
	~AsyncSoundLoader();
	AsyncSoundLoader(const AsyncSoundLoader&) = delete;
	AsyncSoundLoader& operator=(const AsyncSoundLoader&) = delete;

	void worker();

	std::vector<std::shared_ptr<Job>> queue;
	std::vector<std::thread> threads;
	std::mutex mutex;
	std::condition_variable cv;
	int inFlight;
	bool shutdown;
};

#endif