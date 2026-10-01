#ifndef APP_H
#define APP_H

#include <string>
#include <vector>
#include <set>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>
#include <filesystem>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_video.h>

#include "prefs.h"
#include "TextEditor.h"

struct Doc {
	std::string path;
	std::string name;
	TextEditor editor;
	bool modified = false;
	std::filesystem::file_time_type lastWrite{};
	bool externalChanged = false;

	struct FuncItem { std::string label; int line; int kind; };
	std::vector<FuncItem> funcs;
	std::set<std::string> globals;
	std::set<std::string> consts;

	std::string getText() const { return editor.GetText(); }
};

class App {
public:
	static int run(int argc, char* argv[]);
	static void openUrl(const std::string& url);

private:
	App();
	~App();

	bool init(int argc, char* argv[]);
	void shutdown();
	void frame();
	void mainloop();

	void fileNew();
	void fileOpen();
	bool fileSave(int idx);
	bool fileSaveAs(int idx);
	bool fileSaveAll();
	void fileClose(int idx);
	void fileExit();
	void requestQuit();
	void drawExitPrompt();
	void checkDiskChanges(bool force = false);
	void drawDiskPrompt();
	void fileRecent(const std::string& path);
	void addRecent(const std::string& path);
	void removeRecent(const std::string& path);

	void editCut();
	void editCopy();
	void editPaste();
	void editSelectAll();
	void editFind();
	void editFindNext(bool aBackwards = false);
	void editReplace();

	void programExecute();
	void programCompile();
	void programPublish();
	void programDebug();
	void programNoLAA();

	void helpHome();
	void helpAbout();
	void drawUpdate();
	void drawUpdateDialog();

	void build(bool exec, bool publish);
	Prefs::CompileOptions effectiveCompileOptions();
	void compile(const std::vector<std::string> args);
	void appendOutput(const std::string& text);
	void parseOutputLine(const std::string& line);
	void processPendingGoto();

	int current() const { return currentIndex; }
	Doc* doc(int idx) { return idx >= 0 && idx < (int)docs.size() ? &docs[idx] : nullptr; }
	Doc* currentDoc() { return doc(currentIndex); }
	int addDoc(const std::string& path);
	bool openFile(const std::string& path, bool recent = true);
	bool openProject(const std::string& path);
	bool openBlitzProject(const std::string& path);
	bool openPath(const std::string& path);
	void autoSetupProjectFromIncludes(const std::string& path);
	void rebuildFuncList(Doc& d);
	void refreshProjectSymbols();
	void openProjectWindow();
	bool convertIpfToBxp(const std::string& path);
	bool saveProjectFile(const std::string& path);
	void handleCtrlClick(Doc& d, const std::string& word, int line, int column);
	void applyPalette(Doc& d);

	void menuBar();
	void drawTabs();
	void drawEditorPane();
	void drawFuncList();
	void drawOutput();
	void drawFindReplace();
	void drawMenus();
	void drawCommandLine();
	void drawStylize();
	void drawProjectWindow();
	void drawProjectNavigator();
	void setupDockLayout();
	void drawPaneBackground();

	void initKeywords();

	SDL_Window* window = nullptr;
	int windowW = 800, windowH = 600;

	std::vector<Doc> docs;
	int currentIndex = -1;

	bool showFuncList = true;
	bool showOutput = true;
	bool showFind = false;
	bool showReplace = false;
	std::string findStr, replaceStr;
	char findBuf[512] = {};
	char replaceBuf[512] = {};
	bool findFocusPending = false;
	bool findFocusReplace = false;
	bool findAllFiles = false;
	std::string findStatus;
	int requestedIndex = -1;
	int findFlags = 0;
	bool matchCase = false;

	std::string output;
	std::string outputView;
	std::thread compileThread;
	std::atomic<bool> compiling{ false };
	std::mutex outputMutex;
	bool compileOK = false;

	std::string pendingGotoPath;
	int pendingGotoRow = 0;
	int pendingGotoCol = 0;
	bool pendingGoto = false;

	std::string publishExePath;
	std::string publishIconPath;

	std::set<std::string> keywords;
	std::set<std::string> funcs;
	std::thread keywordThread;
	std::mutex keywordMutex;
	std::atomic<bool> keywordsLoaded = false;

	float editorFontScale = 1.0f;

	bool showCommandLine = false;
	bool updateOpen = false;
	bool updateIgnore = false;

	bool showStylize = false;
	bool showProjectWindow = false;
	bool showProjectNavigator = true;
	std::string projectDraftMainPath;
	std::string projectStatus;
	char projectSavePathBuf[1024] = {};
	char projectFilterBuf[256] = {};
	char projectSymbolFilterBuf[256] = {};

	bool projectOpen = false;
	std::string projectPath;
	std::string projectMainPath;
	std::vector<std::string> projectFiles;
	std::vector<int> projectIncludedDocs;
	std::vector<int> projectNavigatorDocs;

	bool aboutOpen = false;
	bool quitting = false;
	bool showExitPrompt = false;
	std::string diskPromptPath;
	unsigned long long lastDiskCheck = 0;
	bool focused = false;
	bool drawIde = false;
	std::atomic<bool> drawIdeAtomic = false;

	SDL_Event event;
};

#endif
