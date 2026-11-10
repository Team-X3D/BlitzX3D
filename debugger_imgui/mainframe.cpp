#include "stdafx.h"
#include "mainframe.h"

#include "../MultiLang/MultiLang.h"
#include "../procutil.h"

#include <functional>
#include <unordered_map>

enum {
	WM_STOP = WM_APP + 1, WM_RUN, WM_END
};

enum {
	STARTING, RUNNING, STOPPED, ENDING
};

MainFrame* g_mainFrame = nullptr;

static DWORD WINAPI dbgCmdThreadFunc(LPVOID param) {
	MainFrame* mf = (MainFrame*)param;
	mf->cmdThreadLoop();
	return 0;
}

static std::string dllDir() {
	HMODULE h = GetModuleHandleA("debugger_imgui.dll");
	if (!h) h = GetModuleHandleA(NULL);
	std::string p(MAX_PATH, '\0');
	for (;;) {
		DWORD n = GetModuleFileNameA(h, p.data(), (DWORD)p.size());
		if (!n) return "";
		if (n < p.size()) {
			p.resize(n);
			break;
		}
		if (p.size() >= 32768) {
			p.resize(32767);
			break;
		}
		p.resize(p.size() * 2);
	}
	size_t s = p.find_last_of("\\/");
	return s == std::string::npos ? "" : p.substr(0, s);
}

MainFrame::MainFrame() :state(STARTING), step_level(-1), cur_pos(0), cur_file(0),
	last_obj_cnt(0), last_unrel_cnt(0), last_str_cnt(0), last_working_set_bytes(0),
	gameThreadId(0), cmdThread(0), cmdRunning(false),
	shmFile(0), shmView(0), shm(0), cmdShmFile(0), cmdShmView(0), cmdShm(0),
	snapEvent(0), cmdEvent(0), lastProfilerSnap(0), lastFlameSnap(0), lastSnapshot(0), running(false) {
}

MainFrame::~MainFrame() {
	cmdRunning = false;
	if (snapEvent) SetEvent(snapEvent);
	if (cmdThread) {
		WaitForSingleObject(cmdThread, 2000);
		CloseHandle(cmdThread);
	}
	if (shmView) UnmapViewOfFile(shmView);
	if (shmFile) CloseHandle(shmFile);
	if (cmdShmView) UnmapViewOfFile(cmdShmView);
	if (cmdShmFile) CloseHandle(cmdShmFile);
	if (snapEvent) CloseHandle(snapEvent);
	if (cmdEvent) CloseHandle(cmdEvent);
}

void MainFrame::start(void* mod, void* env) {
	gameThreadId = GetCurrentThreadId();
	setRuntime(mod, env);

	int pid = (int)GetCurrentProcessId();

	shmFile = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, DBG_SHM_SIZE, shmNameForPid(pid).c_str());
	if (shmFile) {
		shmView = MapViewOfFile(shmFile, FILE_MAP_ALL_ACCESS, 0, 0, DBG_SHM_SIZE);
		if (shmView) shm = (DbgShm*)shmView;
	}
	cmdShmFile = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(DbgCmdShm), cmdShmNameForPid(pid).c_str());
	if (cmdShmFile) {
		cmdShmView = MapViewOfFile(cmdShmFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(DbgCmdShm));
		if (cmdShmView) cmdShm = (DbgCmdShm*)cmdShmView;
	}
	snapEvent = CreateEventA(NULL, FALSE, FALSE, snapEventForPid(pid).c_str());
	cmdEvent = CreateEventA(NULL, FALSE, FALSE, cmdEventForPid(pid).c_str());

	cmdRunning = true;
	cmdThread = CreateThread(NULL, 0, dbgCmdThreadFunc, this, 0, NULL);

	std::string dir = dllDir();
	std::string uiPath = dir + "\\blitz_debugger.exe";
	std::vector<std::string> args = { uiPath, std::to_string(pid) };
	std::string cmdline = buildWindowsCommandLine(args);
	STARTUPINFOA si = { sizeof(si) };
	PROCESS_INFORMATION pi = { 0 };
	std::vector<char> mutableCmd(cmdline.begin(), cmdline.end());
	mutableCmd.push_back('\0');
	if (CreateProcessA(NULL, mutableCmd.data(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
	}
}

void MainFrame::setRuntime(void* mod, void* env) {
	consts_tree.reset((Environ*)env);
	globals_tree.reset((Module*)mod, (Environ*)env);
	locals_tree.reset((Environ*)env);
	profiler.reset();
	profiler.clearSamples();
}

void MainFrame::writeSnapshot() {
	if (!shm) return;
	DbgSerializer s;

	s.writeInt((int)state.load());
	{
		int row = (cur_pos >> 16) & 0xffff, col = cur_pos & 0xffff;
		s.writeStr(cur_file ? cur_file : "");
		s.writeInt(row);
		s.writeInt(col);
	}
	{
		s.writeInt((int)m_logEntries.size());
		for (const LogEntry& e : m_logEntries) {
			s.writeByte((unsigned char)e.severity);
			s.writeStr(e.text);
		}
	}
	{
		s.writeInt((int)constsNodes.size());
		for (const DbgTreeNode& n : constsNodes) writeTree(s, n);
		s.writeInt((int)globalsNodes.size());
		for (const DbgTreeNode& n : globalsNodes) writeTree(s, n);
		s.writeInt((int)localsNodes.size());
		for (const DbgTreeNode& n : localsNodes) writeTree(s, n);
	}
	{
		s.writeStr(profilerSummary);
		s.writeInt((int)profilerRows.size());
		for (const DbgProfilerRow& r : profilerRows) {
			s.writeStr(r.func);
			s.writeDouble(r.selfMs);
			s.writeDouble(r.totalMs);
			s.writeDouble(r.maxMs);
			s.writeInt(r.callCount);
			s.writeInt(r.netObjDelta);
			s.writeInt(r.netStrDelta);
		}
	}
	{
		s.writeInt((int)flameNodes.size());
		for (const DbgFlameNode& n : flameNodes) writeFlameNode(s, n);
	}

	int size = s.size();
	if (size > DBG_SHM_SIZE - (int)(offsetof(DbgShm, payload)) - 8) size = DBG_SHM_SIZE - (int)(offsetof(DbgShm, payload)) - 8;
	std::memcpy(shm->payload, s.data(), size);
	shm->payloadSize = size;
	InterlockedIncrement(&shm->snapSeq);
	SetEvent(snapEvent);
}

void MainFrame::snapshotTrees() {
	buildTreesGuarded();
	std::lock_guard<std::mutex> lock(dataMutex);
	constsNodes = consts_tree.getNodes();
	globalsNodes = globals_tree.getNodes();
	localsNodes = locals_tree.getNodes();
}

void MainFrame::buildTreesGuarded() {
	__try {
		g_mainFrame->consts_tree.buildCache();
	}
	__except(EXCEPTION_EXECUTE_HANDLER) {
		g_mainFrame->consts_tree.clearNodes();
	}
	__try {
		g_mainFrame->globals_tree.buildCache();
	}
	__except(EXCEPTION_EXECUTE_HANDLER) {
		g_mainFrame->globals_tree.clearNodes();
	}
	__try {
		g_mainFrame->locals_tree.buildCache();
	}
	__except(EXCEPTION_EXECUTE_HANDLER) {
		g_mainFrame->locals_tree.clearNodes();
	}
}

void MainFrame::snapshotProfiler() {
	std::vector<DbgProfilerRow> rows;
	__int64 totalSelf = 0;
	const std::map<std::string, ProfileStats>& results = profiler.results();
	rows.reserve(results.size());
	for (std::map<std::string, ProfileStats>::const_iterator it = results.begin(); it != results.end(); ++it) {
		totalSelf += it->second.selfTicks;
	}
	for (std::map<std::string, ProfileStats>::const_iterator it = results.begin(); it != results.end(); ++it) {
		DbgProfilerRow r;
		r.func = it->first;
		r.selfMs = profiler.ticksToMs(it->second.selfTicks);
		r.totalMs = profiler.ticksToMs(it->second.totalTicks);
		r.maxMs = profiler.ticksToMs(it->second.maxTicks);
		r.callCount = it->second.callCount;
		r.netObjDelta = it->second.netObjDelta;
		r.netStrDelta = it->second.netStrDelta;
		rows.push_back(r);
	}
	std::string summary;
	char buf[512];
	__int64 topSelf = 0;
	std::string topFunc;
	for (std::map<std::string, ProfileStats>::const_iterator it = results.begin(); it != results.end(); ++it) {
		if (it->second.selfTicks > topSelf) {
			topSelf = it->second.selfTicks;
			topFunc = it->first;
		}
	}
	double memMb = (double)last_working_set_bytes / (1024.0 * 1024.0);
	if (!topFunc.empty() && totalSelf > 0) {
		double topPct = 100.0 * (double)topSelf / (double)totalSelf;
		sprintf(buf, "Top: %s (%.1f%%)    Active memory: %.2f MB    Objects: %d    Unreleased: %d    Strings: %d    |    Sampled CPU time: %.2f ms",
			topFunc.c_str(), topPct, memMb, last_obj_cnt, last_unrel_cnt, last_str_cnt, profiler.ticksToMs(totalSelf));
	}
	else {
		sprintf(buf, "Active memory: %.2f MB    Objects: %d    Unreleased: %d    Strings: %d    |    Sampled CPU time: %.2f ms",
			memMb, last_obj_cnt, last_unrel_cnt, last_str_cnt, profiler.ticksToMs(totalSelf));
	}
	summary = buf;
	std::lock_guard<std::mutex> lock(dataMutex);
	profilerRows = std::move(rows);
	profilerSummary = std::move(summary);
}

void MainFrame::snapshotFlame() {
	const auto& samples = profiler.getStackSamples();
	std::vector<DbgFlameNode> out;
	if (samples.empty()) {
		std::lock_guard<std::mutex> lock(dataMutex);
		flameNodes = std::move(out);
		return;
	}
	size_t start = 0;
	if (samples.size() > 4000) start = samples.size() - 4000;

	struct TNode {
		int selfSamples = 0;
		int samples = 0;
		std::unordered_map<std::string, std::unique_ptr<TNode>> children;
	};
	TNode trieRoot;
	const int maxDepthLimit = 14;
	for (auto it = samples.begin() + (ptrdiff_t)start; it != samples.end(); ++it) {
		TNode* cur = &trieRoot;
		int depth = 0;
		for (const std::string& func : *it) {
			if (depth >= maxDepthLimit) break;
			std::unique_ptr<TNode>& slot = cur->children[func];
			if (!slot) slot.reset(new TNode());
			cur = slot.get();
			++depth;
		}
		++cur->selfSamples;
	}

	std::vector<TNode*> post;
	std::vector<TNode*> todo;
	todo.push_back(&trieRoot);
	while (!todo.empty()) {
		TNode* n = todo.back();
		todo.pop_back();
		post.push_back(n);
		for (auto& kv : n->children) todo.push_back(kv.second.get());
	}
	for (auto it = post.rbegin(); it != post.rend(); ++it) {
		TNode* n = *it;
		int inc = n->selfSamples;
		for (auto& kv : n->children) inc += kv.second->samples;
		n->samples = inc;
	}

	std::function<void(TNode*, std::vector<DbgFlameNode>&)> convert =
		[&convert](TNode* t, std::vector<DbgFlameNode>& outv) {
			for (auto& kv : t->children) {
				DbgFlameNode fn;
				fn.name = kv.first;
				fn.samples = kv.second->samples;
				fn.selfSamples = kv.second->selfSamples;
				convert(kv.second.get(), fn.children);
				outv.push_back(std::move(fn));
			}
		};
	convert(&trieRoot, out);

	std::function<void(std::vector<DbgFlameNode>&)> sortNodes =
		[&sortNodes](std::vector<DbgFlameNode>& v) {
			std::sort(v.begin(), v.end(), [](const DbgFlameNode& a, const DbgFlameNode& b) {
				if (a.samples != b.samples) return a.samples > b.samples;
				return a.name < b.name;
			});
			for (DbgFlameNode& n : v) sortNodes(n.children);
		};
	sortNodes(out);

	std::lock_guard<std::mutex> lock(dataMutex);
	flameNodes = std::move(out);
}

void MainFrame::debugRun() {
	state = RUNNING;
	std::lock_guard<std::mutex> lock(dataMutex);
	writeSnapshot();
}

void MainFrame::debugStop() {
	step_level = locals_tree.size();
	state = STOPPED;
	snapshotTrees();
	{
		std::lock_guard<std::mutex> lock(dataMutex);
		writeSnapshot();
	}
}

bool MainFrame::debugStmt(int pos, const char* file) {
	cur_pos = pos;
	cur_file = file;
	if (shouldRun()) return true;
	::PostMessage(0, WM_STOP, 0, 0);
	return false;
}

void MainFrame::debugEnter(void* frame, void* env, const char* func) {
	profiler.enter(func);
	call_stack.push_back(func);
	locals_tree.pushFrame(frame, env, func);
	if (locals_tree.size() > 1) return;
	debugRun();
}

void MainFrame::debugLeave() {
	profiler.leave();
	if (!call_stack.empty()) call_stack.pop_back();
	locals_tree.popFrame();
}

std::string MainFrame::buildCrashReport(const char* msg)const {
	std::string s = msg ? msg : "";
	s += "\r\n";
	if (cur_file) {
		int row = (cur_pos >> 16) & 0xffff, col = cur_pos & 0xffff;
		s += "\r\nLocation: ";
		s += cur_file;
		s += " (line ";
		s += std::to_string(row + 1);
		s += ", col ";
		s += std::to_string(col + 1);
		s += ")\r\n";
	}
	if (!call_stack.empty()) {
		s += "\r\nCall stack (innermost first):\r\n";
		for (int i = (int)call_stack.size() - 1; i >= 0; --i) {
			s += "  ";
			s += call_stack[i];
			s += "\r\n";
		}
	}
	return s;
}

void MainFrame::debugMsg(const char* msg, bool serious) {
	if (serious) {
		std::string report = buildCrashReport(msg);
		{
			std::lock_guard<std::mutex> lock(dataMutex);
			m_logEntries.push_back({ LOG_ERROR, "RUNTIME ERROR: " + report });
		}
		profiler.resyncStack();
		call_stack.clear();
	}
	else {
		std::string text = msg ? msg : "";
		{
			std::lock_guard<std::mutex> lock(dataMutex);
			m_logEntries.push_back({ LOG_INFO, text });
		}
	}
	std::lock_guard<std::mutex> lock(dataMutex);
	writeSnapshot();
}

void MainFrame::debugLog(const char* msg) {
	std::string full = msg;
	ELogSeverity severity = LOG_INFO;
	std::string displayText;
	if (full.find("[WARNING] ") == 0) {
		severity = LOG_WARNING;
		displayText = full.substr(10);
	}
	else if (full.find("[ERROR] ") == 0) {
		severity = LOG_ERROR;
		displayText = full.substr(8);
	}
	else {
		displayText = full;
	}
	{
		std::lock_guard<std::mutex> lock(dataMutex);
		m_logEntries.push_back({ severity, displayText });
		if (m_logEntries.size() > 10000) {
			m_logEntries.erase(m_logEntries.begin(), m_logEntries.begin() + (m_logEntries.size() - 5000));
		}
	}
	writeSnapshot();
}

void MainFrame::debugSys(void* m) {
	if (!m) return;
	int tag = *(int*)m;
	if (tag == DBGSYS_MEMSTATS) {
		DbgSysMemStats* s = (DbgSysMemStats*)m;
		last_obj_cnt = s->objCnt;
		last_unrel_cnt = s->unrelObjCnt;
		last_str_cnt = s->stringCnt;
		last_working_set_bytes = s->workingSetBytes;
	}

	DWORD now = GetTickCount();
	if (now - lastProfilerSnap > 250) {
		lastProfilerSnap = now;
		snapshotProfiler();
	}
	if (now - lastFlameSnap > 1000) {
		lastFlameSnap = now;
		snapshotFlame();
	}
	if (now - lastSnapshot > 100) {
		lastSnapshot = now;
		std::lock_guard<std::mutex> lock(dataMutex);
		writeSnapshot();
	}
}

void MainFrame::cmdStop() {
	PostThreadMessage(gameThreadId, WM_STOP, 0, 0);
}

void MainFrame::cmdRun() {
	step_level = -1;
	PostThreadMessage(gameThreadId, WM_RUN, 0, 0);
}

void MainFrame::cmdStepOver() {
	PostThreadMessage(gameThreadId, WM_RUN, 0, 0);
}

void MainFrame::cmdStepInto() {
	std::lock_guard<std::mutex> lock(dataMutex);
	step_level = locals_tree.size() + 1;
	PostThreadMessage(gameThreadId, WM_RUN, 0, 0);
}

void MainFrame::cmdStepOut() {
	std::lock_guard<std::mutex> lock(dataMutex);
	step_level = locals_tree.size() - 1;
	PostThreadMessage(gameThreadId, WM_RUN, 0, 0);
}

void MainFrame::cmdEnd() {
	PostThreadMessage(gameThreadId, WM_END, 0, 0);
}

void MainFrame::handleCommand(int cmd) {
	switch (cmd) {
		case DBG_CMD_STOP: cmdStop(); break;
		case DBG_CMD_RUN: cmdRun(); break;
		case DBG_CMD_STEPOVER: cmdStepOver(); break;
		case DBG_CMD_STEPINTO: cmdStepInto(); break;
		case DBG_CMD_STEPOUT: cmdStepOut(); break;
		case DBG_CMD_END: cmdEnd(); break;
	}
}

void MainFrame::cmdThreadLoop() {
	while (cmdRunning) {
		WaitForSingleObject(cmdEvent, INFINITE);
		if (!cmdRunning) break;
		if (cmdShm) {
			int cmd = cmdShm->cmd;
			handleCommand(cmd);
			InterlockedIncrement(&cmdShm->seq);
		}
	}
}
