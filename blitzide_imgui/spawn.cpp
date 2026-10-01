#include "spawn.h"

#include "../procutil.h"

#include <cstdio>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <cerrno>
#include <cstring>
#endif

int runProcess(const std::vector<std::string>& args, std::string& output, int* exitCode, bool* running) {
	output.clear();
	if (exitCode) *exitCode = -1;
	if (running) *running = true;
	
#if defined(_WIN32)
	HANDLE g_hChildStd_OUT_Rd = NULL, g_hChildStd_OUT_Wr = NULL;
	SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
	if (!CreatePipe(&g_hChildStd_OUT_Rd, &g_hChildStd_OUT_Wr, &sa, 0)) return -1;
	SetHandleInformation(g_hChildStd_OUT_Rd, HANDLE_FLAG_INHERIT, 0);

	STARTUPINFOA si = { sizeof(si) };
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdOutput = si.hStdError = g_hChildStd_OUT_Wr;
	si.hStdInput = NULL;

	PROCESS_INFORMATION pi = { 0 };
	std::string cmdline = buildWindowsCommandLine(args);
	std::vector<char> mutableCmd(cmdline.begin(), cmdline.end());
	mutableCmd.push_back('\0');
	if (!CreateProcessA(NULL, mutableCmd.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
		if (running) *running = false;
		CloseHandle(g_hChildStd_OUT_Rd);
		CloseHandle(g_hChildStd_OUT_Wr);
		return -1;
	}
	CloseHandle(g_hChildStd_OUT_Wr);

	char buf[4096];
	DWORD n = 0;
	while (ReadFile(g_hChildStd_OUT_Rd, buf, sizeof(buf), &n, NULL) && n) {
		{
			std::lock_guard<std::mutex> lock(processUpdateMutex);
			output.append(buf, n);
			processOutputChanged = true;
		}
		processUpdated.notify_one();
	}
	CloseHandle(g_hChildStd_OUT_Rd);

	WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD code = 0;
	GetExitCodeProcess(pi.hProcess, &code);
	{
		std::lock_guard<std::mutex> lock(processUpdateMutex);
		if (running) *running = false;
	}
	processUpdated.notify_one();
	if (exitCode) *exitCode = (int)code;
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return 0;
#else
	int pipefd[2];
	if (pipe(pipefd) == -1) return -1;

	pid_t pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return -1;
	}
	if (pid == 0) {
		dup2(pipefd[1], 1);
		dup2(pipefd[1], 2);
		close(pipefd[0]);
		close(pipefd[1]);
		std::vector<char*> argv;
		for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
		argv.push_back(nullptr);
		execvp(argv[0], argv.data());
		_exit(127);
	}
	close(pipefd[1]);
	char buf[4096];
	ssize_t n;
	while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
		output.append(buf, n);
	}
	close(pipefd[0]);
	int status = 0;
	waitpid(pid, &status, 0);
	if (exitCode) *exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	return 0;
#endif
}
