#include <string>
#include <vector>
#include <Windows.h>
#include "../MultiLang/MultiLang.h"
#include "../procutil.h"

static std::string getAppDir() {
	std::string buf(MAX_PATH, '\0');
	for(;;) {
		DWORD n = GetModuleFileNameA(NULL, buf.data(), (DWORD)buf.size());
		if(!n) return std::string();
		if(n < buf.size()) {
			buf.resize(n);
			break;
		}
		if(buf.size() >= 32768) {
			buf.resize(32767);
			break;
		}
		buf.resize(buf.size() * 2);
	}
	size_t n = buf.find_last_of("\\/");
	if(n != std::string::npos) buf.resize(n); //Remove filename from the path and return it
	return buf;
}

static void fail(const char* p) {
	::MessageBox(0, p, MultiLang::bb_err, MB_SETFOREGROUND | MB_TOPMOST | MB_ICONERROR);
	ExitProcess(-1);
}

int WINAPI WinMain(_In_ HINSTANCE inst, _In_opt_ HINSTANCE prev, _In_ char* cmd, _In_ int show) {
	std::string basedir = getAppDir();

	_putenv_s("blitzpath", basedir.c_str());
	SetCurrentDirectory(basedir.c_str());

	std::vector<std::string> args;
	args.push_back(basedir + "\\bin\\blitzide_imgui.exe");
	std::vector<std::string> extra = splitCommandLine(cmd);
	args.insert(args.end(), extra.begin(), extra.end());
	std::string cmdline = buildWindowsCommandLine(args);
	std::vector<char> mutableCmd(cmdline.begin(), cmdline.end());
	mutableCmd.push_back('\0');

	STARTUPINFO startupInfo;
	PROCESS_INFORMATION processInfo;

	ZeroMemory(&startupInfo, sizeof(startupInfo));
	startupInfo.cb = sizeof(startupInfo);
	ZeroMemory(&processInfo, sizeof(processInfo));

	if(!CreateProcess(NULL, mutableCmd.data(), NULL, NULL, FALSE, 0, NULL, NULL, &startupInfo, &processInfo)) {
		fail(MultiLang::init_err);
	}

	//Wait until BlitzIDE enters an idle state, then close bblaunch.
	WaitForInputIdle(processInfo.hProcess, INFINITE);

	CloseHandle(processInfo.hProcess);
	CloseHandle(processInfo.hThread);

	return 0; //Success!
}