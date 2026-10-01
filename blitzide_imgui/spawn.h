#ifndef SPAWN_H
#define SPAWN_H

#include <string>
#include <vector>

#include <mutex>
#include <condition_variable>

inline std::mutex processUpdateMutex;
inline std::condition_variable processUpdated;
inline bool processOutputChanged = false;

int runProcess(const std::vector<std::string>& args, std::string& output, int* exitCode = nullptr, bool* running = nullptr);

#endif
