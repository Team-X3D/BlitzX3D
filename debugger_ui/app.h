#ifndef APP_H
#define APP_H

#include <string>
#include <vector>

#include "dbgipc.h"
#include "sourcefile.h"

struct SDL_Window;

enum ELogSeverity
{
	LOG_INFO = 0,
	LOG_WARNING,
	LOG_ERROR
};

struct LogEntry {
	ELogSeverity severity;
	std::string text;
};

class App {
	SDL_Window* window;
	int windowW, windowH;
	bool initialized;
	bool quitting;
	bool connected;
	HANDLE gameProc;

	HANDLE shmFile;
	LPVOID shmView;
	DbgShm* shm;
	HANDLE cmdShmFile;
	LPVOID cmdShmView;
	DbgCmdShm* cmdShm;
	HANDLE snapEvent;
	HANDLE cmdEvent;
	LONG lastSnapSeq;

	int state;
	std::string curFile;
	int curRow, curCol;
	std::vector<LogEntry> log;
	int m_currentFilter;
	bool logPendingScroll;
	std::string logView;

	std::vector<DbgTreeNode> constsNodes, globalsNodes, localsNodes;
	std::vector<DbgProfilerRow> profilerRows;
	std::string profilerSummary;
	std::vector<DbgFlameNode> flameNodes;

	SourceFile* source;
	std::string sourcePath;

	bool hoverValid;
	ImVec2 hoverMin, hoverMax;
	std::string hoverName;
	int hoverSamples;
	int hoverSelf;

	bool readSnapshot();
	void sendCmd(int cmd);
	void loadSource(const std::string& file, int row, int col);

	void frame();
	void drawToolbar();
	void drawMainTabs();
	void drawVarsTabs();
	void drawDebugLog();
	void drawProfilerTab();
	void drawFlameGraphTab();
	void drawSourceTab();
	void drawTreeNodes(const std::vector<DbgTreeNode>& items);
	void drawFlameTree(const std::vector<DbgFlameNode>& items, float x, float y, float w, float boxH, float rowPitch, int parentSamples);
	int measureFlameDepth(const std::vector<DbgFlameNode>& v, int d, int& maxDepth);

public:
	App();
	~App();

	bool init(int pid);
	void shutdown();
	void run();
};

extern App* g_app;

#endif
