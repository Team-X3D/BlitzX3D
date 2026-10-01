#ifndef SPAWN_H
#define SPAWN_H

#include <string>
#include <vector>

#include <mutex>
#include <condition_variable>

struct ProcessOutput {
	std::mutex mutex;
	std::condition_variable cv;
	bool changed = false;
	bool done = false;
	int exitCode = -1;
};

int runProcess(const std::vector<std::string>& args, std::string& output, int* exitCode = nullptr, ProcessOutput* progress = nullptr);

#endif
