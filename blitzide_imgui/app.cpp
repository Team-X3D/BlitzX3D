#include "app.h"

#include "../theme.h"
#include "blitzlang.h"
#include "filedialog.h"
#include "publish.h"
#include "spawn.h"
#include "../procutil.h"
#include "update.h"

#include "../imgui/imgui.h"
#include "../imgui/imgui_internal.h"
#include "../imgui/backends/imgui_impl_sdl3.h"
#include "../imgui/backends/imgui_impl_opengl3.h"
#include "../imgui/backends/imgui_impl_opengl3_loader.h"

#include <SDL3/SDL.h>

#if !defined(_WIN32)
#include <unistd.h>
#include <sys/wait.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>

namespace fs = std::filesystem;

static App* g_app = nullptr;

static std::string toLower(const std::string& s) {
	std::string t = s;
	std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return t;
}

static bool startsWithWord(const std::string& s, const std::string& w) {
	if (s.size() < w.size()) return false;
	if (s.compare(0, w.size(), w) != 0) return false;
	if (s.size() == w.size()) return true;
	return std::isspace((unsigned char)s[w.size()]) != 0;
}

static void parseBlitzDecl(const std::string& text, std::set<std::string>& names) {
	std::vector<std::string> parts;
	int depth = 0;
	bool inStr = false;
	std::string cur;
	for (char c : text) {
		if (inStr) { if (c == '"') inStr = false; cur += c; continue; }
		if (c == '"') { inStr = true; cur += c; continue; }
		if (c == '(' || c == '[' || c == '{') { ++depth; cur += c; continue; }
		if (c == ')' || c == ']' || c == '}') { if (depth > 0) --depth; cur += c; continue; }
		if (c == ',' && depth == 0) { parts.push_back(cur); cur.clear(); continue; }
		cur += c;
	}
	if (!cur.empty()) parts.push_back(cur);

	for (const auto& part : parts) {
		size_t i = 0;
		while (i < part.size() && (std::isspace((unsigned char)part[i]) || part[i] == ':')) ++i;
		if (i >= part.size() || !(std::isalpha((unsigned char)part[i]) || part[i] == '_')) continue;
		size_t start = i;
		while (i < part.size() && (std::isalnum((unsigned char)part[i]) || part[i] == '_')) ++i;
		if (i > start) names.insert(part.substr(start, i - start));
	}
}

static std::string stripDeclSuffix(const std::string& s) {
	if (s.empty()) return s;
	char last = s.back();
	if (last == '$' || last == '#' || last == '%') return s.substr(0, s.size() - 1);
	return s;
}

static std::string stripBOM(const std::string& s) {
	if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
		return s.substr(3);
	return s;
}

static std::string normalizePath(const std::string& p) {
	try { return fs::weakly_canonical(p).string(); }
	catch (...) { return p; }
}

static bool samePath(const std::string& a, const std::string& b) {
#if defined(_WIN32)
	return toLower(normalizePath(a)) == toLower(normalizePath(b));
#else
	return normalizePath(a) == normalizePath(b);
#endif
}

static void constrainFloatingWindow() {
	if (ImGui::GetWindowDockNode() != nullptr) return;
	ImGuiViewport* viewport = ImGui::GetMainViewport();
	ImVec2 pos = ImGui::GetWindowPos();
	ImVec2 size = ImGui::GetWindowSize();
	const float minX = viewport->WorkPos.x - size.x + 48.0f;
	const float maxX = viewport->WorkPos.x + viewport->WorkSize.x - 48.0f;
	const float minY = viewport->WorkPos.y;
	const float maxY = viewport->WorkPos.y + viewport->WorkSize.y - ImGui::GetFrameHeight();
	auto clampValue = [](float value, float low, float high) {
		if (high < low) high = low;
		if (value < low) return low;
		if (value > high) return high;
		return value;
	};
	ImGui::SetWindowPos(ImVec2(clampValue(pos.x, minX, maxX), clampValue(pos.y, minY, maxY)), ImGuiCond_Always);
}

static std::string getXmlAttr(const std::string& text, const std::string& key) {
	size_t k = 0;
	while ((k = text.find(key, k)) != std::string::npos) {
		size_t end = k + key.size();
		if (k > 0 && (std::isalnum((unsigned char)text[k - 1]) || text[k - 1] == '_')) {
			k = end;
			continue;
		}
		while (end < text.size() && std::isspace((unsigned char)text[end])) ++end;
		if (end >= text.size() || text[end] != '=') {
			k = end;
			continue;
		}
		++end;
		while (end < text.size() && std::isspace((unsigned char)text[end])) ++end;
		if (end >= text.size() || (text[end] != '"' && text[end] != '\'')) return "";
		char quote = text[end++];
		size_t close = text.find(quote, end);
		if (close == std::string::npos) return "";
		return text.substr(end, close - end);
	}
	return "";
}

static std::string unescapeXml(const std::string& value) {
	std::string result;
	result.reserve(value.size());
	for (size_t i = 0; i < value.size(); ++i) {
		if (value.compare(i, 5, "&amp;") == 0) { result += '&'; i += 4; }
		else if (value.compare(i, 6, "&quot;") == 0) { result += '"'; i += 5; }
		else result += value[i];
	}
	return result;
}

static bool fileDefinesFunction(const std::string& path, const std::string& name, int& outLine) {
	std::ifstream in(path, std::ios::binary);
	if (!in.good()) return false;
	std::stringstream ss;
	ss << in.rdbuf();
	std::stringstream lines(stripBOM(ss.str()));
	std::string line;
	int ln = 0;
	while (std::getline(lines, line, '\n')) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		size_t lead = line.find_first_not_of(" \t");
		std::string t = lead == std::string::npos ? "" : toLower(line.substr(lead));
		if (startsWithWord(t, "function")) {
			size_t p = line.find_first_of(" \t", lead);
			std::string fname = p == std::string::npos ? "" : line.substr(p + 1);
			fname = fname.substr(0, fname.find_first_of(" ("));
			if (!fname.empty() && stripDeclSuffix(toLower(fname)) == name) {
				outLine = ln;
				return true;
			}
		}
		++ln;
	}
	return false;
}

static std::vector<std::string> getIncludePaths(const std::string& path) {
	std::vector<std::string> result;
	if (path.empty()) return result;
	std::ifstream in(path, std::ios::binary);
	if (!in.good()) return result;
	std::stringstream ss;
	ss << in.rdbuf();
	std::stringstream lines(stripBOM(ss.str()));
	std::string line;
	while (std::getline(lines, line, '\n')) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		size_t lead = line.find_first_not_of(" \t");
		std::string t = lead == std::string::npos ? "" : toLower(line.substr(lead));
		if (!t.empty() && t[0] == '#') t.erase(0, 1);
		if (startsWithWord(t, "include")) {
			size_t q1 = line.find('"');
			size_t q2 = q1 == std::string::npos ? std::string::npos : line.find('"', q1 + 1);
			if (q1 != std::string::npos && q2 != std::string::npos) {
				std::string rel = line.substr(q1 + 1, q2 - q1 - 1);
				fs::path target(rel);
				if (target.is_relative())
					target = fs::path(path).parent_path() / target;
				if (fs::exists(target)) result.push_back(target.string());
				else if (fs::exists(rel)) result.push_back(fs::absolute(rel).string());
			}
		}
	}
	return result;
}

static std::string findFunctionInIncludes(const std::string& startFile, const std::string& name, std::vector<std::string>& visited, int& outLine) {
	if (startFile.empty()) return "";
	std::string key = normalizePath(startFile);
	if (std::find(visited.begin(), visited.end(), key) != visited.end()) return "";
	visited.push_back(key);

	if (fileDefinesFunction(startFile, name, outLine)) return startFile;

	for (const auto& inc : getIncludePaths(startFile)) {
		std::string found = findFunctionInIncludes(inc, name, visited, outLine);
		if (!found.empty()) return found;
	}
	return "";
}

static void openUrlImpl(const std::string& url) {
	SDL_OpenURL(url.c_str());
}

void App::openUrl(const std::string& url) { openUrlImpl(url); }

static ImU32 themeColU32(const ImVec4& c) {
	return IM_COL32((int)(c.x * 255.0f + 0.5f), (int)(c.y * 255.0f + 0.5f), (int)(c.z * 255.0f + 0.5f), (int)(c.w * 255.0f + 0.5f));
}

static ImVec4 themeCol3(const int* rgb) {
	return ImVec4(rgb[0] / 255.0f, rgb[1] / 255.0f, rgb[2] / 255.0f, 1.0f);
}

static void applyEditorColorsToPrefs(const std::string& name) {
	int cols[ThemeEditorColorCount][3];
	if (!themeEditorColors(name, cols)) return;
	memcpy(prefs.rgb_bkgrnd, cols[0], sizeof(prefs.rgb_bkgrnd));
	memcpy(prefs.rgb_string, cols[1], sizeof(prefs.rgb_string));
	memcpy(prefs.rgb_ident, cols[2], sizeof(prefs.rgb_ident));
	memcpy(prefs.rgb_keyword, cols[3], sizeof(prefs.rgb_keyword));
	memcpy(prefs.rgb_comment, cols[4], sizeof(prefs.rgb_comment));
	memcpy(prefs.rgb_digit, cols[5], sizeof(prefs.rgb_digit));
	memcpy(prefs.rgb_default, cols[6], sizeof(prefs.rgb_default));
	memcpy(prefs.rgb_known, cols[7], sizeof(prefs.rgb_known));
	memcpy(prefs.rgb_preproc, cols[8], sizeof(prefs.rgb_preproc));
	memcpy(prefs.rgb_global, cols[9], sizeof(prefs.rgb_global));
	memcpy(prefs.rgb_const, cols[10], sizeof(prefs.rgb_const));
	memcpy(prefs.rgb_cursor, cols[11], sizeof(prefs.rgb_cursor));
	memcpy(prefs.rgb_selection, cols[12], sizeof(prefs.rgb_selection));
}

static void currentEditorColors(int out[ThemeEditorColorCount][3]) {
	memcpy(out[0], prefs.rgb_bkgrnd, sizeof(prefs.rgb_bkgrnd));
	memcpy(out[1], prefs.rgb_string, sizeof(prefs.rgb_string));
	memcpy(out[2], prefs.rgb_ident, sizeof(prefs.rgb_ident));
	memcpy(out[3], prefs.rgb_keyword, sizeof(prefs.rgb_keyword));
	memcpy(out[4], prefs.rgb_comment, sizeof(prefs.rgb_comment));
	memcpy(out[5], prefs.rgb_digit, sizeof(prefs.rgb_digit));
	memcpy(out[6], prefs.rgb_default, sizeof(prefs.rgb_default));
	memcpy(out[7], prefs.rgb_known, sizeof(prefs.rgb_known));
	memcpy(out[8], prefs.rgb_preproc, sizeof(prefs.rgb_preproc));
	memcpy(out[9], prefs.rgb_global, sizeof(prefs.rgb_global));
	memcpy(out[10], prefs.rgb_const, sizeof(prefs.rgb_const));
	memcpy(out[11], prefs.rgb_cursor, sizeof(prefs.rgb_cursor));
	memcpy(out[12], prefs.rgb_selection, sizeof(prefs.rgb_selection));
}

static void applyCurrentTheme() {
	themeApplyStyle(prefs.theme, (float)prefs.ui_rounding, prefs.ui_alpha);
}

App::App() {}
App::~App() {}

int App::run(int argc, char* argv[]) {
	App app;
	g_app = &app;
	if (!app.init(argc, argv)) return 1;
	app.mainloop();
	app.shutdown();
	g_app = nullptr;
	return 0;
}

bool App::init(int argc, char* argv[]) {
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) return false;

	prefs.open();
	applyEditorColorsToPrefs(prefs.theme);

	windowW = prefs.win_w; windowH = prefs.win_h;

	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
	window = SDL_CreateWindow("BlitzX3D IDE", windowW, windowH, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
	if (!window) { SDL_Quit(); return false; }
	if (!SDL_GL_CreateContext(window) || !SDL_GL_SetSwapInterval(1)) {
		SDL_DestroyWindow(window);
		SDL_Quit();
		return false;
	}

	{
		SDL_Rect bounds;
		if (SDL_GetDisplayBounds(SDL_GetPrimaryDisplay(), &bounds)) {
			int x = bounds.x + (bounds.w - windowW) / 2;
			int y = bounds.y + (bounds.h - windowH) / 2;
			SDL_SetWindowPosition(window, x, y);
			SDL_MaximizeWindow(window);
		}
	}

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	io.ConfigWindowsResizeFromEdges = true;
	io.ConfigDockingAlwaysTabBar = true;
	io.Fonts->AddFontDefaultBitmap();
	if (!prefs.configDir.empty()) {
		io.IniFilename = strdup((prefs.configDir + "/imgui.ini").c_str());
	}

	applyCurrentTheme();
	ImGui_ImplSDL3_InitForOpenGL(window, SDL_GL_GetCurrentContext());
	ImGui_ImplOpenGL3_Init("#version 130");

	initKeywords();
	startUpdateCheck(this);

	for (int k = 1; k < argc; ++k) {
		std::string a = argv[k];
		if (a.size() && a[0] == '-') continue;
		openPath(a);
	}
	if (docs.empty()) fileNew();

	return true;
}

void App::shutdown() {
	if (compileThread.joinable()) compileThread.join();
	if (keywordThread.joinable()) keywordThread.join();

	if (currentIndex >= 0) {
		SDL_GetWindowSize(window, &windowW, &windowH);
		prefs.win_w = windowW;
		prefs.win_h = windowH;
	}
	if (ImGui::GetIO().IniFilename) ImGui::SaveIniSettingsToDisk(ImGui::GetIO().IniFilename);
	prefs.close();

	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplSDL3_Shutdown();
	SDL_StopTextInput(window);
	ImGui::DestroyContext();
	SDL_GL_DestroyContext(SDL_GL_GetCurrentContext());
	SDL_DestroyWindow(window);
	SDL_Quit();
}

void App::mainloop() {
	while (!quitting) {
		SDL_StartTextInput(window);
		while (SDL_PollEvent(&event)) {
			ImGui_ImplSDL3_ProcessEvent(&event);
			switch (event.type)
			{
			case SDL_EVENT_QUIT:
			case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
		   		if (event.window.windowID == SDL_GetWindowID(window))
					requestQuit();
				break;
			case SDL_EVENT_DROP_FILE:
				if (event.drop.data)
					openPath(event.drop.data);
				break;
			case SDL_EVENT_WINDOW_FOCUS_LOST:
				focused = false;
				break;
			case SDL_EVENT_WINDOW_FOCUS_GAINED:
				focused = true;
				break;
			default:
				drawIde = true;
				break;
			}
		}

		if (keywordsLoaded)
			drawIde = true;

		if (drawIde || focused)
			frame();
		
		SDL_WaitEventTimeout(nullptr, 16);

		drawIde = false;
	}
}

void App::frame() {
	ImGui_ImplOpenGL3_NewFrame();
	ImGui_ImplSDL3_NewFrame();
	ImGui::NewFrame();

	setupDockLayout();
	ImGui::DockSpaceOverViewport(ImHashStr("BlitzX3DDockSpaceV5"));

	ImGuiIO& kio = ImGui::GetIO();
	bool ctrl = kio.KeyCtrl;
	bool shift = kio.KeyShift;
	if (ctrl && ImGui::IsKeyPressed(ImGuiKey_F)) editFind();
	if (ctrl && ImGui::IsKeyPressed(ImGuiKey_H)) editReplace();
	if (ImGui::IsKeyPressed(ImGuiKey_F3)) editFindNext(shift);
	if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S)) { if (currentIndex >= 0) fileSave(currentIndex); }
	if (ctrl && ImGui::IsKeyPressed(ImGuiKey_N)) fileNew();
	if (ctrl && ImGui::IsKeyPressed(ImGuiKey_O)) fileOpen();
	if (ctrl && ImGui::IsKeyPressed(ImGuiKey_F5, false)) programCompile();
	if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_F5, false)) programExecute();
	(void)shift;

	if (keywordsLoaded) {
		keywordsLoaded = false;
		if (projectOpen)
			refreshProjectSymbols();
		else
			for (auto& d : docs) {
				std::set<std::string> custom;
				for (const auto& f : d.funcs) {
					if (f.kind == 0) custom.insert(f.label);
				}
				d.editor.SetLanguageDefinition(makeBlitzLangDef(keywords, funcs, custom, d.globals, d.consts));
			}
	}

	menuBar();

	drawPaneBackground();

	processPendingGoto();

	drawEditorPane();

	if (showFuncList) drawFuncList();
	if (showProjectNavigator) drawProjectNavigator();

	if (showOutput) drawOutput();

	drawFindReplace();

	drawCommandLine();

	drawStylize();
	drawProjectWindow();

	drawUpdate();
	drawUpdateDialog();

	drawExitPrompt();

	if (aboutOpen) {
		ImGui::Begin("About BlitzX3D", &aboutOpen);
		ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
		ImGui::Text("BlitzX3D IDE");
		ImGui::Text("Version V2.0.0");
		ImGui::Separator();
		ImGui::Text("blitzpath: %s", prefs.homeDir.c_str());
		ImGui::End();
	}

	ImGui::Render();

	int fbw = 0, fbh = 0;
	SDL_GetWindowSizeInPixels(window, &fbw, &fbh);
	glViewport(0, 0, fbw, fbh);
	const ImVec4& bgc = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
	glClearColor(bgc.x, bgc.y, bgc.z, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

	ImGuiIO& io = ImGui::GetIO();
	if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
		SDL_GLContext backup = SDL_GL_GetCurrentContext();
		ImGui::UpdatePlatformWindows();
		ImGui::RenderPlatformWindowsDefault();
		SDL_GL_MakeCurrent(window, backup);
	}
	SDL_GL_SwapWindow(window);
}

void App::menuBar() {
	if (!ImGui::BeginMainMenuBar()) return;

	if (ImGui::BeginMenu("File")) {
		if (ImGui::MenuItem("New", "Ctrl+N")) fileNew();
		if (ImGui::MenuItem("Open...", "Ctrl+O")) fileOpen();
		ImGui::Separator();
		if (ImGui::MenuItem("Close", "Ctrl+W")) if (currentIndex >= 0) fileClose(currentIndex);
		if (ImGui::MenuItem("Close All")) while (!docs.empty()) fileClose((int)docs.size() - 1);
		ImGui::Separator();
		if (ImGui::MenuItem("Save", "Ctrl+S")) if (currentIndex >= 0) fileSave(currentIndex);
		if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S")) if (currentIndex >= 0) fileSaveAs(currentIndex);
		if (ImGui::MenuItem("Save All")) fileSaveAll();
		ImGui::Separator();
		if (ImGui::BeginMenu("Recent Files")) {
			for (size_t k = 0; k < prefs.recentFiles.size(); ++k) {
				if (ImGui::MenuItem(prefs.recentFiles[k].c_str())) {
					openPath(prefs.recentFiles[k]);
				}
			}
			if (prefs.recentFiles.empty()) ImGui::TextDisabled("(none)");
			ImGui::EndMenu();
		}
		if (ImGui::MenuItem("Project...")) openProjectWindow();
		if (ImGui::MenuItem("Open User libs folder")) SDL_OpenURL((prefs.homeDir + "/userlibs").c_str());
		ImGui::Separator();
		if (ImGui::MenuItem("Exit", "Alt+F4")) requestQuit();
		ImGui::EndMenu();
	}

	if (ImGui::BeginMenu("Edit")) {
		if (ImGui::MenuItem("Undo", "Ctrl+Z")) if (Doc* d = currentDoc()) d->editor.Undo();
		if (ImGui::MenuItem("Redo", "Ctrl+Y")) if (Doc* d = currentDoc()) d->editor.Redo();
		ImGui::Separator();
		if (ImGui::MenuItem("Cut", "Ctrl+X")) editCut();
		if (ImGui::MenuItem("Copy", "Ctrl+C")) editCopy();
		if (ImGui::MenuItem("Paste", "Ctrl+V")) editPaste();
		if (ImGui::MenuItem("Select All", "Ctrl+A")) editSelectAll();
		ImGui::Separator();
		if (ImGui::MenuItem("Find / Replace...", "Ctrl+F")) editFind();
		if (ImGui::MenuItem("Find Next", "F3")) editFindNext();
		if (ImGui::MenuItem("Find Previous", "Shift+F3")) editFindNext(true);
		ImGui::Separator();
		if (ImGui::MenuItem("Auto Complete", nullptr, &prefs.edit_autocomplete)) {
			for (auto& doc : docs)
				doc.editor.SetAutocompleteEnabled(prefs.edit_autocomplete);
		}
		ImGui::EndMenu();
	}

	if (ImGui::BeginMenu("View")) {
		if (ImGui::MenuItem("Functions Panel", nullptr, &showFuncList)) {}
		if (ImGui::MenuItem("Project Navigator", nullptr, &showProjectNavigator)) {}
		if (ImGui::MenuItem("Output Panel", nullptr, &showOutput)) {}
		ImGui::Separator();
		if (ImGui::MenuItem("Stylization...")) showStylize = true;
		ImGui::EndMenu();
	}

	if (ImGui::BeginMenu("Program")) {
		if (ImGui::MenuItem("Run", "F5")) programExecute();
		if (ImGui::MenuItem("Compile", "Ctrl+F5")) programCompile();
		if (ImGui::MenuItem("Publish...")) programPublish();
		if (ImGui::MenuItem("Command Line...")) showCommandLine = true;
		ImGui::Separator();
		bool* optNoAutoDecl = &prefs.prg_noautodecl;
		bool* optEncrypt = &prefs.prg_encrypt;
		if (prefs.projectOptionsActive) {
			optNoAutoDecl = &prefs.projectOptions.noautodecl;
			optEncrypt = &prefs.projectOptions.encrypt;
		}
		auto persistOpts = [&]() { if (prefs.projectOptionsActive) prefs.saveProjectOptions(projectPath); };
		bool optsChanged = false;
		if (ImGui::MenuItem("Debug", nullptr, &prefs.prg_debug)) {}
		if (ImGui::MenuItem("No LAA", nullptr, &prefs.prg_nolaa)) {}
		if (ImGui::MenuItem("No Auto Declaration ", nullptr, optNoAutoDecl)) optsChanged = true;
		ImGui::Separator();
		if (ImGui::BeginMenu("Compile Options")) {
			ImGui::MenuItem("Dump assembly", nullptr, &prefs.prg_dumpasm);
			if (ImGui::MenuItem("Quiet", nullptr, &prefs.prg_quiet) && !prefs.prg_quiet)
				prefs.prg_veryquiet = false;
			if (ImGui::MenuItem("Very quiet", nullptr, &prefs.prg_veryquiet) && prefs.prg_veryquiet)
				prefs.prg_quiet = true;
			ImGui::MenuItem("Dump keys", nullptr, &prefs.prg_dumpkeys);
			if (ImGui::MenuItem("Encrypt", nullptr, optEncrypt)) optsChanged = true;
			ImGui::EndMenu();
		}
		if (optsChanged) persistOpts();
		ImGui::EndMenu();
	}

	if (ImGui::BeginMenu("Help")) {
		if (ImGui::MenuItem("Help Home")) helpHome();
		ImGui::Separator();
		if (ImGui::MenuItem("About BlitzX3D")) aboutOpen = true;
		ImGui::EndMenu();
	}

	ImGui::EndMainMenuBar();
}

void App::drawTabs() {
	if (docs.empty()) return;
	if (currentIndex < 0) currentIndex = 0;
	int closeIdx = -1;
	if (ImGui::BeginTabBar("##doctabs", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_AutoSelectNewTabs)) {
		if (requestedIndex >= 0 && requestedIndex < (int)docs.size()) {
			currentIndex = requestedIndex;
			Doc& requested = docs[currentIndex];
			std::string label = requested.name;
			if (requested.modified) label += '*';
			label += "##doctab" + std::to_string(currentIndex);
			ImGui::GetCurrentTabBar()->NextSelectedTabId = ImGui::GetID(label.c_str());
			requestedIndex = -1;
		}
		for (int k = 0; k < (int)docs.size(); ++k) {
			Doc& d = docs[k];
			std::string label = d.name;
			if (d.modified) label += "*";
			label += "##doctab" + std::to_string(k);
			bool open = true;
			if (ImGui::BeginTabItem(label.c_str(), &open)) {
				currentIndex = k;
				ImGui::EndTabItem();
			}
			if (!open) closeIdx = k;
		}
		ImGui::EndTabBar();
	}
	if (closeIdx >= 0) fileClose(closeIdx);
}

void App::drawEditorPane() {
	ImGui::SetNextWindowSize(ImVec2(800, 500), ImGuiCond_FirstUseEver);
	ImGui::Begin("Editor", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	constrainFloatingWindow();
	drawTabs();

	Doc* d = currentDoc();
	if (!d) { ImGui::End(); return; }

	ImGuiIO& io = ImGui::GetIO();
	bool ctrl = io.ConfigMacOSXBehaviors ? io.KeySuper : io.KeyCtrl;
	if (ctrl && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
		float delta = 0.0f;
		if (io.MouseWheel != 0.0f) delta = io.MouseWheel;
		if (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract)) delta = -1.0f;
		if (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd)) delta = 1.0f;
		if (delta != 0.0f) {
			const float base = ImGui::GetStyle().FontSizeBase;
			int size = (int)std::lround(base * editorFontScale * (delta > 0.0f ? 1.1f : 1.0f / 1.1f));
			size = std::clamp(size, 7, 48);
			editorFontScale = (float)size / base;
			io.InputQueueCharacters.resize(0);
		}
	}

	applyPalette(*d);
	d->editor.SetAutocompleteEnabled(prefs.edit_autocomplete);
	bool textChanged = d->editor.IsTextChanged();

	ImVec2 avail = ImGui::GetContentRegionAvail();
	ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * editorFontScale);
	d->editor.Render(d->name.c_str(), avail, false);
	ImGui::PopFont();
	textChanged = textChanged || d->editor.IsTextChanged();
	if (textChanged) {
		d->modified = true;
		rebuildFuncList(*d);
	}

	std::string word;
	int cline, ccol;
	if (d->editor.TakeCtrlClick(word, cline, ccol))
		handleCtrlClick(*d, word, cline, ccol);

	int rcline, rccol;
	if (d->editor.TakeRightClick(rcline, rccol))
		ImGui::OpenPopup("editor_context");

	if (ImGui::BeginPopup("editor_context")) {
		TextEditor& ed = d->editor;
		bool didEdit = false;
		if (ImGui::MenuItem("Cut", "Ctrl+X")) { ed.Cut(); didEdit = true; }
		if (ImGui::MenuItem("Copy", "Ctrl+C")) ed.Copy();
		if (ImGui::MenuItem("Paste", "Ctrl+V")) { ed.Paste(); didEdit = true; }
		if (ImGui::MenuItem("Select All", "Ctrl+A")) ed.SelectAll();
		ImGui::Separator();
		if (ImGui::MenuItem("Duplicate Line")) { ed.DuplicateLine(); didEdit = true; }
		if (ImGui::MenuItem("Delete Line")) { ed.DeleteLine(); didEdit = true; }
		ImGui::Separator();
		if (ImGui::MenuItem("Toggle Comment", ";")) { ed.ToggleComment(); didEdit = true; }
		if (ImGui::MenuItem("Indent", "Tab")) { ed.Indent(); didEdit = true; }
		if (ImGui::MenuItem("Outdent", "Shift+Tab")) { ed.Outdent(); didEdit = true; }
		ImGui::Separator();
		if (ImGui::MenuItem("Undo", "Ctrl+Z")) ed.Undo();
		if (ImGui::MenuItem("Redo", "Ctrl+Y")) ed.Redo();
		if (didEdit) d->modified = true;
		ImGui::EndPopup();
	}

	ImGui::End();
}

void App::applyPalette(Doc& d) {
	TextEditor::Palette pal = d.editor.GetPalette();
	auto col = [](const int* rgb) -> ImU32 {
		return IM_COL32(rgb[0], rgb[1], rgb[2], 255);
	};
	pal[(int)TextEditor::PaletteIndex::Background] = col(prefs.rgb_bkgrnd);
	pal[(int)TextEditor::PaletteIndex::String] = col(prefs.rgb_string);
	pal[(int)TextEditor::PaletteIndex::Identifier] = col(prefs.rgb_ident);
	pal[(int)TextEditor::PaletteIndex::KnownIdentifier] = col(prefs.rgb_known);
	pal[(int)TextEditor::PaletteIndex::PreprocIdentifier] = col(prefs.rgb_preproc);
	pal[(int)TextEditor::PaletteIndex::Global] = col(prefs.rgb_global);
	pal[(int)TextEditor::PaletteIndex::Const] = col(prefs.rgb_const);
	pal[(int)TextEditor::PaletteIndex::Keyword] = col(prefs.rgb_keyword);
	pal[(int)TextEditor::PaletteIndex::Comment] = col(prefs.rgb_comment);
	pal[(int)TextEditor::PaletteIndex::MultiLineComment] = col(prefs.rgb_comment);
	pal[(int)TextEditor::PaletteIndex::Number] = col(prefs.rgb_digit);
	pal[(int)TextEditor::PaletteIndex::Default] = col(prefs.rgb_default);
	pal[(int)TextEditor::PaletteIndex::Cursor] = col(prefs.rgb_cursor);
	pal[(int)TextEditor::PaletteIndex::Selection] = IM_COL32(prefs.rgb_selection[0], prefs.rgb_selection[1], prefs.rgb_selection[2], 170);
	pal[(int)TextEditor::PaletteIndex::LineNumber] = IM_COL32(120, 120, 120, 200);
	d.editor.SetPalette(pal);
}

void App::drawFuncList() {
	ImGui::SetNextWindowSize(ImVec2(280, 600), ImGuiCond_FirstUseEver);
	ImGui::Begin("Functions");
	constrainFloatingWindow();
	Doc* d = currentDoc();
	if (d) {
		std::vector<int> functionDocs;
		if (projectOpen)
			functionDocs = projectIncludedDocs;
		if (functionDocs.empty()) functionDocs.push_back(currentIndex);

		int item = 0;
		for (int docIndex : functionDocs) {
			if (docIndex < 0 || docIndex >= (int)docs.size()) continue;
			Doc& source = docs[docIndex];
			if (projectOpen && functionDocs.size() > 1)
				ImGui::TextDisabled("%s", source.name.c_str());
			for (const auto& f : source.funcs) {
				const char* prefix = f.kind == 0 ? "F " : f.kind == 1 ? "T " : ". ";
				std::string label = prefix + f.label;
				ImGui::PushID(item++);
				if (ImGui::Selectable(label.c_str())) {
					currentIndex = docIndex;
					requestedIndex = docIndex;
					source.editor.SetCursorPosition(TextEditor::Coordinates(f.line, 0));
					source.editor.SetSelection(TextEditor::Coordinates(f.line, 0),
						TextEditor::Coordinates(f.line, 0));
				}
				ImGui::PopID();
			}
		}
		if (item == 0) ImGui::TextDisabled("(no functions)");
	}
	ImGui::End();
}

void App::drawProjectNavigator() {
	ImGui::SetNextWindowSize(ImVec2(330, 520), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Project Navigator", &showProjectNavigator)) {
		ImGui::End();
		return;
	}
	constrainFloatingWindow();
	if (!projectOpen) {
		ImGui::Spacing();
		ImGui::TextWrapped("Open a project to use this navigator.");
		ImGui::End();
		return;
	}
	const std::vector<int>& sourceDocs = projectNavigatorDocs.empty() ? projectIncludedDocs : projectNavigatorDocs;

	if (ImGui::BeginTabBar("##project_navigator_tabs")) {
		if (ImGui::BeginTabItem("Files")) {
			ImGui::SetNextItemWidth(-1);
			ImGui::InputTextWithHint("##project_filter", "Filter files...", projectFilterBuf, sizeof(projectFilterBuf));
			std::string filter = toLower(projectFilterBuf);
			ImGui::BeginChild("##project_file_list", ImVec2(0, 0), false);
			for (int docIndex : sourceDocs) {
				if (docIndex < 0 || docIndex >= (int)docs.size()) continue;
				Doc& d = docs[docIndex];
				if (!filter.empty() && toLower(d.name).find(filter) == std::string::npos && toLower(d.path).find(filter) == std::string::npos) continue;
				ImGui::PushID(docIndex);
				if (ImGui::Selectable(d.name.c_str(), currentIndex == docIndex)) {
					currentIndex = docIndex;
					requestedIndex = docIndex;
				}
				ImGui::TextDisabled("%s", d.path.c_str());
				ImGui::PopID();
			}
			ImGui::EndChild();
			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem("Symbols")) {
			ImGui::SetNextItemWidth(-1);
			ImGui::InputTextWithHint("##project_symbol_filter", "Search symbols...", projectSymbolFilterBuf, sizeof(projectSymbolFilterBuf));
			std::string filter = toLower(projectSymbolFilterBuf);
			ImGui::BeginChild("##project_symbol_list", ImVec2(0, 0), false);
			const std::vector<int>& symbolDocs = projectIncludedDocs.empty() ? sourceDocs : projectIncludedDocs;
			int item = 0;
			for (int docIndex : symbolDocs) {
				if (docIndex < 0 || docIndex >= (int)docs.size()) continue;
				Doc& d = docs[docIndex];
				ImGui::TextDisabled("%s", d.name.c_str());
				for (const auto& f : d.funcs) {
					if (!filter.empty() && toLower(f.label).find(filter) == std::string::npos && toLower(d.name).find(filter) == std::string::npos) continue;
					const char* prefix = f.kind == 0 ? "F " : f.kind == 1 ? "T " : ". ";
					std::string label = prefix + f.label;
					ImGui::PushID(item++);
					if (ImGui::Selectable(label.c_str())) {
						currentIndex = docIndex;
						requestedIndex = docIndex;
						d.editor.SetCursorPosition(TextEditor::Coordinates(f.line, 0));
						d.editor.SetSelection(TextEditor::Coordinates(f.line, 0), TextEditor::Coordinates(f.line, 0));
					}
					ImGui::PopID();
				}
			}
			if (item == 0) ImGui::TextDisabled("(no symbols)");
			ImGui::EndChild();
			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem("Project")) {
			ImGui::TextUnformatted("Project");
			ImGui::TextWrapped("%s", projectPath.c_str());
			ImGui::Separator();
			ImGui::TextUnformatted("Main file");
			ImGui::TextWrapped("%s", projectMainPath.c_str());
			if (ImGui::Button("Project Manager")) openProjectWindow();
			ImGui::SameLine();
			if (ImGui::Button("Refresh")) refreshProjectSymbols();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
	ImGui::End();
}

void App::setupDockLayout() {
	static bool dockLayoutInitialized = false;
	if (dockLayoutInitialized) return;
	dockLayoutInitialized = true;
	ImGuiViewport* viewport = ImGui::GetMainViewport();
	ImGuiID dockspace = ImHashStr("BlitzX3DDockSpaceV5");
	ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockspace);
	if (node && (node->ChildNodes[0] != nullptr || node->ChildNodes[1] != nullptr)) return;

	ImGui::DockBuilderRemoveNode(dockspace);
	ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
	ImGui::DockBuilderSetNodeSize(dockspace, viewport->WorkSize);

	ImGuiID left, right, center, bottom;
	ImGui::DockBuilderSplitNode(dockspace, ImGuiDir_Left, 0.145f, &left, &center);
	ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.18f, &right, &center);
	ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.22f, &bottom, &center);
	ImGui::DockBuilderDockWindow("Functions", left);
	ImGui::DockBuilderDockWindow("Project Navigator", right);
	ImGui::DockBuilderDockWindow("Editor", center);
	ImGui::DockBuilderDockWindow("Output", bottom);
	ImGui::DockBuilderFinish(dockspace);
}

static int OutputTextResizeCallback(ImGuiInputTextCallbackData* data) {
	if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
		std::string* str = (std::string*)data->UserData;
		str->resize(data->BufTextLen);
		data->Buf = (char*)str->c_str();
	}
	return 0;
}

void App::drawOutput() {
	ImGui::SetNextWindowSize(ImVec2(900, 220), ImGuiCond_FirstUseEver);
	ImGui::Begin("Output");
	constrainFloatingWindow();
	{
		std::lock_guard<std::mutex> lock(outputMutex);
		outputView = output;
	}
	if (outputView.empty())
		outputView.push_back('\n');

	ImGui::PushStyleColor(ImGuiCol_Text, compileOK ? IM_COL32(200, 255, 200, 255) : IM_COL32(255, 200, 200, 255));
	ImGui::InputTextMultiline("##output", (char*)outputView.c_str(), (int)outputView.capacity() + 1,
		ImVec2(ImGui::GetContentRegionAvail().x, -ImGui::GetFrameHeightWithSpacing() - 4),
		ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_NoUndoRedo | ImGuiInputTextFlags_CallbackResize,
		OutputTextResizeCallback, (void*)&outputView);
	ImGui::PopStyleColor();

	if (ImGui::Button(compiling ? "Compiling..." : "Clear")) {
		if (!compiling) {
			std::lock_guard<std::mutex> lock(outputMutex);
			output.clear();
			outputLines.clear();
		}
	}
	ImGui::End();
}

void App::drawFindReplace() {
	if (!showFind && !showReplace) return;
	int flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking;
	ImGui::SetNextWindowPos(ImVec2(windowW / 2.0f - 200, 40), ImGuiCond_Appearing);
	if (ImGui::Begin("Find / Replace", &showFind, flags)) {
		ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
		ImGuiIO& fio = ImGui::GetIO();
		bool doFind = false, doFindPrev = false, doReplace = false, doReplaceAll = false;
		bool focusFind = false, focusReplace = false;
		if (findFocusPending) {
			strncpy(findBuf, findStr.c_str(), sizeof(findBuf) - 1);
			findBuf[sizeof(findBuf) - 1] = '\0';
			strncpy(replaceBuf, replaceStr.c_str(), sizeof(replaceBuf) - 1);
			replaceBuf[sizeof(replaceBuf) - 1] = '\0';
			findFocusPending = false;
			focusFind = !findFocusReplace;
			focusReplace = findFocusReplace;
			findFocusReplace = false;
			ImGui::SetWindowFocus();
		}
		ImGui::SetNextItemWidth(300);
		if (focusFind) ImGui::SetKeyboardFocusHere();
		bool findEnter = ImGui::InputText("Find text", findBuf, sizeof(findBuf), ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::SameLine();
		if (ImGui::Button("Find Next")) doFind = true;
		if (findEnter) {
			if (fio.KeyShift) doFindPrev = true; else doFind = true;
		}
		ImGui::Checkbox("Match case", &matchCase);
		ImGui::Checkbox("Search all open files", &findAllFiles);
		ImGui::SameLine();
		ImGui::SetNextItemWidth(300);
		if (focusReplace) ImGui::SetKeyboardFocusHere();
		bool replaceEnter = ImGui::InputText("Replace text", replaceBuf, sizeof(replaceBuf), ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::SameLine();
		if (ImGui::Button("Replace")) doReplace = true;
		if (replaceEnter) doReplace = true;
		if (ImGui::Button("Replace All")) doReplaceAll = true;

		if (ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
			showFind = showReplace = false;
			if (Doc* d = currentDoc())
				d->editor.RequestWindowFocus();
			ImGui::End();
			return;
		}

		findStr = findBuf;
		replaceStr = replaceBuf;
		if ((doFind || doFindPrev || doReplace || doReplaceAll) && currentDoc()) {
			Doc* d = currentDoc();
			if (!findStr.empty()) {
				if (doReplaceAll) {
					std::string text = d->editor.GetText();
					size_t pos = 0;
					while ((pos = text.find(findStr, pos)) != std::string::npos) {
						text.replace(pos, findStr.size(), replaceStr);
						pos += replaceStr.size();
					}
					d->editor.SetText(text);
					d->modified = true;
				}
				else {
					if (doReplace && d->editor.HasSelection()) {
						std::string sel = d->editor.GetSelectedText();
						bool same = matchCase ? (sel == findStr) : (toLower(sel) == toLower(findStr));
						if (same) {
							d->editor.Delete();
							d->editor.InsertText(replaceStr);
							d->modified = true;
						}
					}
					if (doFind) editFindNext(false);
					if (doFindPrev) editFindNext(true);
					if (doReplace) editFindNext(false);
				}
			}
		}
		if (!findStatus.empty()) {
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 380.0f);
			ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", findStatus.c_str());
			ImGui::PopTextWrapPos();
		}
	}
	ImGui::End();
	if (!showFind) showReplace = false;
}

int App::addDoc(const std::string& path) {
	Doc d;
	if (path.empty()) {
		d.name = "untitled";
	}
	else {
		d.path = path;
		fs::path p(path);
		d.name = p.filename().string();
		std::ifstream in(path, std::ios::binary);
		if (in.good()) {
			std::stringstream ss;
			ss << in.rdbuf();
			d.editor.SetText(ss.str());
		}
	}
	d.editor.SetLanguageDefinition(makeBlitzLangDef(keywords, funcs, {}));
	rebuildFuncList(d);
	std::set<std::string> custom;
	for (const auto& f : d.funcs) {
		if (f.kind == 0) custom.insert(f.label);
	}
	d.editor.SetLanguageDefinition(makeBlitzLangDef(keywords, funcs, custom, d.globals, d.consts));
	d.editor.ClearTextChanged();
	d.modified = false;
	docs.push_back(std::move(d));
	currentIndex = (int)docs.size() - 1;
	return currentIndex;
}

bool App::openFile(const std::string& path, bool recent) {
	for (int k = 0; k < (int)docs.size(); ++k) {
		if (samePath(docs[k].path, path)) {
			currentIndex = k;
			requestedIndex = k;
			if (recent) addRecent(path);
			return true;
		}
	}
	fs::path p(path);
	if (!fs::exists(p)) return false;
	addDoc(path);
	if (recent) addRecent(path);
	return true;
}

static std::string pathKey(const std::string& path) {
	std::string key = normalizePath(path);
#if defined(_WIN32)
	return toLower(key);
#else
	return key;
#endif
}

bool App::openProject(const std::string& path) {
	fs::path p(path);
	if (!fs::exists(p)) return false;
	projectOpen = false;
	projectPath.clear();
	prefs.clearProjectOptions();
	projectMainPath.clear();
	projectFiles.clear();
	projectIncludedDocs.clear();
	projectNavigatorDocs.clear();
	fs::path dir = p.parent_path();

	std::ifstream in(path, std::ios::binary);
	if (!in.good()) return false;
	std::stringstream ss;
	ss << in.rdbuf();
	std::string text = ss.str();

	std::string mainFile;
	std::vector<std::string> absPaths;

	size_t pos = 0;
	while ((pos = text.find("AbsPath", pos)) != std::string::npos) {
		std::string v = getXmlAttr(text.substr(pos), "AbsPath");
		if (!v.empty()) absPaths.push_back(v);
		pos += 7;
	}
	mainFile = getXmlAttr(text, "MainFile");

	if (absPaths.empty()) return false;

	std::string mainRel;
	if (!mainFile.empty()) {
		std::string mainName = toLower(fs::path(mainFile).filename().string());
		for (const auto& rel : absPaths) {
			if (toLower(fs::path(rel).filename().string()) == mainName) { mainRel = rel; break; }
		}
	}

	auto resolveProjectPath = [&](const std::string& rel) {
		fs::path f(rel);
		if (f.is_relative()) f = dir / f;
		return f.lexically_normal();
	};
	auto openRel = [&](const std::string& rel) {
		fs::path f = resolveProjectPath(rel);
		if (fs::exists(f)) {
			std::string absolute = normalizePath(f.string());
			if (std::find_if(projectFiles.begin(), projectFiles.end(), [&](const std::string& existing) { return samePath(existing, absolute); }) == projectFiles.end())
				projectFiles.push_back(absolute);
			openFile(f.string(), false);
		}
	};

	if (!mainRel.empty()) openRel(mainRel);
	for (const auto& rel : absPaths) {
		if (rel == mainRel) continue;
		openRel(rel);
	}

	std::string mainTarget = path;
	if (!mainRel.empty()) {
		mainTarget = resolveProjectPath(mainRel).string();
	}
	for (size_t k = 0; k < docs.size(); ++k) {
		if (samePath(docs[k].path, mainTarget)) {
			currentIndex = (int)k;
			requestedIndex = (int)k;
			break;
		}
	}
	addRecent(path);
	return true;
}

bool App::openBlitzProject(const std::string& path) {
	fs::path projectFile(path);
	if (!fs::exists(projectFile)) return false;
	prefs.clearProjectOptions();

	std::ifstream in(path, std::ios::binary);
	if (!in.good()) return false;
	std::stringstream ss;
	ss << in.rdbuf();
	std::string text = ss.str();

	std::string mainFile = unescapeXml(getXmlAttr(text, "MainFile"));
	std::vector<std::string> files;
	size_t pos = 0;
	while ((pos = text.find("<File", pos)) != std::string::npos) {
		size_t end = text.find('>', pos);
		if (end == std::string::npos) break;
		std::string file = unescapeXml(getXmlAttr(text.substr(pos, end - pos + 1), "Path"));
		if (!file.empty()) files.push_back(file);
		pos = end + 1;
	}
	if (mainFile.empty() && !files.empty()) mainFile = files.front();
	if (mainFile.empty()) return false;

	fs::path dir = projectFile.parent_path();
	auto resolve = [&](const std::string& file) {
		fs::path result(file);
		if (result.is_relative()) result = dir / result;
		return result.lexically_normal();
	};

	fs::path mainPath = resolve(mainFile);
	if (std::find_if(files.begin(), files.end(), [&](const std::string& file) { return samePath(resolve(file).string(), mainPath.string()); }) == files.end())
		files.insert(files.begin(), mainFile);

	projectOpen = true;
	projectPath = normalizePath(path);
	projectMainPath = normalizePath(mainPath.string());
	projectFiles.clear();

	auto openProjectFile = [&](const std::string& file) {
		fs::path resolved = resolve(file);
		if (!fs::exists(resolved)) return;
		std::string absolute = normalizePath(resolved.string());
		if (std::find_if(projectFiles.begin(), projectFiles.end(), [&](const std::string& existing) { return samePath(existing, absolute); }) != projectFiles.end()) return;
		projectFiles.push_back(absolute);
		openFile(absolute, false);
	};

	openProjectFile(mainFile);
	for (const auto& file : files)
		if (!samePath(resolve(file).string(), mainPath.string())) openProjectFile(file);

	if (projectFiles.empty()) {
		projectOpen = false;
		projectPath.clear();
		projectMainPath.clear();
		return false;
	}

	for (size_t k = 0; k < docs.size(); ++k) {
		if (samePath(docs[k].path, projectMainPath)) {
			currentIndex = (int)k;
			requestedIndex = (int)k;
			break;
		}
	}
	refreshProjectSymbols();
	prefs.loadProjectOptions(projectPath);
	addRecent(path);
	return true;
}

bool App::openPath(const std::string& path) {
	std::string extension = toLower(fs::path(path).extension().string());
	bool opened = extension == ".ipf"
		? openProject(path)
		: extension == ".bxp"
		? openBlitzProject(path)
		: openFile(path);
	if (!opened) { removeRecent(path); return opened; }
	if (extension != ".ipf" && extension != ".bxp") {
		bool inProject = false;
		if (projectOpen) {
			std::string np = normalizePath(path);
			if (samePath(np, projectMainPath)) inProject = true;
			else for (const auto& f : projectFiles)
				if (samePath(f, np)) { inProject = true; break; }
		}
		if (!inProject)
			autoSetupProjectFromIncludes(path);
	}
	return opened;
}

void App::autoSetupProjectFromIncludes(const std::string& path) {
	if (projectOpen && !projectPath.empty()) return;
	if (!fs::exists(path)) return;

	std::vector<std::string> visited;
	std::vector<std::string> reachable;
	std::function<void(const std::string&)> visit = [&](const std::string& p) {
		std::string key = pathKey(p);
		if (std::find(visited.begin(), visited.end(), key) != visited.end()) return;
		visited.push_back(key);
		reachable.push_back(key);
		for (const auto& inc : getIncludePaths(p)) visit(inc);
	};
	visit(path);

	std::vector<std::string> others;
	for (const auto& r : reachable)
		if (!samePath(r, path)) others.push_back(r);
	if (others.empty()) return;

	prefs.clearProjectOptions();
	projectOpen = true;
	projectPath.clear();
	projectMainPath = normalizePath(path);
	projectFiles.clear();
	for (const auto& r : reachable) {
		std::string abs = normalizePath(r);
		if (std::find_if(projectFiles.begin(), projectFiles.end(), [&](const std::string& e) { return samePath(e, abs); }) == projectFiles.end())
			projectFiles.push_back(abs);
	}
	for (const auto& f : projectFiles) {
		if (!samePath(f, path)) openFile(f, false);
	}
	refreshProjectSymbols();
}

static int editorColumn(const std::string& line, size_t bytePos, int tabSize) {
	int column = 0;
	for (size_t k = 0; k < bytePos && k < line.size(); ++k) {
		if (line[k] == '\t') column = (column / tabSize + 1) * tabSize;
		else if ((line[k] & 0xc0) != 0x80) ++column;
	}
	return column;
}

static size_t columnToByte(const std::string& line, int column, int tabSize) {
	if (column <= 0) return 0;
	int c = 0;
	size_t i = 0;
	while (i < line.size() && c < column) {
		if (line[i] == '\t') c = (c / tabSize + 1) * tabSize;
		else if ((line[i] & 0xc0) != 0x80) ++c;
		++i;
	}
	return i;
}

void App::fileNew() { addDoc(""); }
void App::fileOpen() {
	std::string path;
	if (fileOpenDialog(path)) openPath(path);
}
void App::openProjectWindow() {
	projectDraftMainPath = projectOpen ? projectMainPath : "";
	if (projectDraftMainPath.empty()) {
		if (Doc* d = currentDoc()) projectDraftMainPath = d->path;
	}
	std::memset(projectSavePathBuf, 0, sizeof(projectSavePathBuf));
	std::string savePath = projectPath;
	if (!savePath.empty()) std::strncpy(projectSavePathBuf, savePath.c_str(), sizeof(projectSavePathBuf) - 1);
	projectStatus.clear();
	showProjectWindow = true;
}
bool App::convertIpfToBxp(const std::string& path) {
	if (!openProject(path)) return false;
	Doc* main = currentDoc();
	if (!main || main->path.empty()) return false;
	projectDraftMainPath = main->path;
	fs::path target(path);
	target.replace_extension(".bxp");
	return saveProjectFile(target.string());
}
bool App::saveProjectFile(const std::string& path) {
	if (path.empty()) return false;
	std::string mainPath = normalizePath(projectDraftMainPath);
	if (mainPath.empty() || !fs::exists(mainPath)) return false;

	std::vector<std::string> files;
	if (!projectFiles.empty()) {
		for (const auto& file : projectFiles) {
			if (!fs::exists(file)) continue;
			std::string normalized = normalizePath(file);
			if (std::find_if(files.begin(), files.end(), [&](const std::string& existing) { return samePath(existing, normalized); }) == files.end())
				files.push_back(normalized);
		}
	}
	else {
		for (const auto& d : docs) {
			if (d.path.empty() || !fs::exists(d.path)) continue;
			std::string file = normalizePath(d.path);
			if (std::find_if(files.begin(), files.end(), [&](const std::string& existing) { return samePath(existing, file); }) == files.end())
				files.push_back(file);
		}
	}
	if (std::find_if(files.begin(), files.end(), [&](const std::string& file) { return samePath(file, mainPath); }) == files.end())
		files.insert(files.begin(), mainPath);

	fs::path projectFile(path);
	fs::path dir = projectFile.parent_path();
	if (dir.empty()) dir = fs::current_path();
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out.good()) return false;

	auto escape = [](const std::string& value) {
		std::string result;
		for (char c : value) {
			if (c == '&') result += "&amp;";
			else if (c == '"') result += "&quot;";
			else result += c;
		}
		return result;
	};
	auto relativePath = [&](const std::string& file) {
		std::error_code ec;
		fs::path relative = fs::relative(file, dir, ec);
		if (ec || relative.empty()) relative = fs::path(file).filename();
		return relative.generic_string();
	};

	out << "<BlitzX3DProject MainFile=\"" << escape(relativePath(mainPath)) << "\">\r\n";
	for (const auto& file : files)
		out << "  <File Path=\"" << escape(relativePath(file)) << "\"/>\r\n";
	if (prefs.projectOptionsActive)
		out << "  <CompileOptions " << prefs.compileOptionsXml() << "/>\r\n";
	out << "</BlitzX3DProject>\r\n";
	out.close();
	if (!out) return false;

	projectOpen = true;
	projectPath = normalizePath(path);
	projectMainPath = mainPath;
	projectFiles = files;
	refreshProjectSymbols();
	addRecent(path);
	return true;
}
void App::addRecent(const std::string& path) {
	if (path.empty()) return;
	const std::string storedPath = normalizePath(path);
	const std::string& recentPath = storedPath.empty() ? path : storedPath;
	for (auto it = prefs.recentFiles.begin(); it != prefs.recentFiles.end();) {
		if (samePath(*it, recentPath))
			it = prefs.recentFiles.erase(it);
		else
			++it;
	}
	prefs.recentFiles.insert(prefs.recentFiles.begin(), recentPath);
	if (prefs.recentFiles.size() > 10) prefs.recentFiles.pop_back();
}
void App::removeRecent(const std::string& path) {
	for (auto it = prefs.recentFiles.begin(); it != prefs.recentFiles.end();) {
		if (samePath(*it, path))
			it = prefs.recentFiles.erase(it);
		else
			++it;
	}
}
void App::fileRecent(const std::string& path) { openPath(path); }

bool App::fileSave(int idx) {
	Doc* d = doc(idx);
	if (!d) return false;
	if (d->path.empty()) return fileSaveAs(idx);
	std::ofstream out(d->path, std::ios::binary | std::ios::trunc);
	if (!out.good()) return false;
	out << d->editor.GetText();
	out.close();
	d->modified = false;
	return true;
}
bool App::fileSaveAs(int idx) {
	Doc* d = doc(idx);
	if (!d) return false;
	std::string defaultName = d->name;
	if (defaultName == "untitled") defaultName = "untitled.bb";
	std::string path;
	if (!fileSaveDialog(path, defaultName.c_str())) return false;
	d->path = path;
	fs::path p(path);
	d->name = p.filename().string();
	rebuildFuncList(*d);
	addRecent(path);
	return fileSave(idx);
}
bool App::fileSaveAll() {
	bool ok = true;
	for (int k = 0; k < (int)docs.size(); ++k) {
		if (docs[k].modified) { if (!fileSave(k)) ok = false; }
	}
	return ok;
}
void App::fileClose(int idx) {
	if (docs[idx].modified) {
		if (!docs[idx].path.empty()) fileSave(idx);
	}
	std::string closedPath = docs[idx].path;
	docs.erase(docs.begin() + idx);
	if (currentIndex >= (int)docs.size()) currentIndex = (int)docs.size() - 1;
	if (docs.empty()) currentIndex = -1;
	if (projectOpen) {
		bool anyProjectDocOpen = false;
		for (const auto& d : docs) {
			if (!d.path.empty() &&
				std::any_of(projectFiles.begin(), projectFiles.end(),
					[&](const std::string& f) { return samePath(f, d.path); })) {
				anyProjectDocOpen = true;
				break;
			}
		}
		if (!anyProjectDocOpen) {
			projectOpen = false;
			projectPath.clear();
			prefs.clearProjectOptions();
			projectMainPath.clear();
			projectFiles.clear();
			projectIncludedDocs.clear();
			projectNavigatorDocs.clear();
		}
		else {
			refreshProjectSymbols();
		}
	}
}
void App::fileExit() { requestQuit(); }

void App::requestQuit() {
	for (auto& d : docs) {
		if (d.modified) {
			showExitPrompt = true;
			return;
		}
	}
	quitting = true;
}

void App::drawExitPrompt() {
	if (!showExitPrompt) return;

	ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_Always);
	ImGui::OpenPopup("Unsaved Changes");
	if (ImGui::BeginPopupModal("Unsaved Changes", nullptr,
		ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize)) {
		ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
		ImGui::TextUnformatted("You have unsaved changes in the following file(s):");
		ImGui::BeginChild("##exitprompt_list", ImVec2(0, 120), true);
		for (auto& d : docs) {
			if (d.modified) {
				ImGui::PushID(&d);
				ImGui::Bullet();
				ImGui::TextWrapped("%s", (d.path.empty() ? d.name : d.path).c_str());
				ImGui::PopID();
			}
		}
		ImGui::EndChild();

		float width = ImGui::GetContentRegionAvail().x;
		float btn = (width - ImGui::GetStyle().ItemSpacing.x * 2) / 3.0f;
		if (ImGui::Button("Save All", ImVec2(btn, 0))) {
			if (fileSaveAll()) { showExitPrompt = false; quitting = true; }
		}
		ImGui::SameLine();
		if (ImGui::Button("Discard", ImVec2(btn, 0))) {
			showExitPrompt = false;
			quitting = true;
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(btn, 0))) {
			showExitPrompt = false;
		}
		ImGui::EndPopup();
	}
}

void App::editCut() { if (Doc* d = currentDoc()) d->editor.Cut(); }
void App::editCopy() { if (Doc* d = currentDoc()) d->editor.Copy(); }
void App::editPaste() { if (Doc* d = currentDoc()) d->editor.Paste(); }
void App::editSelectAll() { if (Doc* d = currentDoc()) d->editor.SelectAll(); }
void App::editFind() {
	showFind = true;
	findFocusReplace = false;
	findFocusPending = true;
	findStatus.clear();
}

void App::editReplace() {
	if (!showFind) {
		showFind = true;
		showReplace = true;
		findFocusReplace = true;
	}
	else if (!findFocusReplace) {
		showReplace = true;
		findFocusReplace = true;
	}
	else {
		findFocusReplace = false;
	}
	findFocusPending = true;
	findStatus.clear();
}

void App::editFindNext(bool aBackwards) {
	if (findStr.empty()) { editFind(); return; }
	if (docs.empty()) return;
	bool ic = !matchCase;
	std::string needle = ic ? toLower(findStr) : findStr;

	auto searchDocFwd = [&](Doc& d, int startLine, int startCol) -> bool {
		std::vector<std::string> lines = d.editor.GetTextLines();
		int tab = d.editor.GetTabSize() > 0 ? d.editor.GetTabSize() : 4;
		if (startLine < 0) { startLine = 0; startCol = 0; }
		for (int row = startLine; row < (int)lines.size(); ++row) {
			std::string hay = ic ? toLower(lines[row]) : lines[row];
			size_t from = 0;
			if (row == startLine) {
				from = columnToByte(lines[row], startCol, tab);
				if (from > hay.size()) from = hay.size();
			}
			size_t pos = hay.find(needle, from);
			if (pos == std::string::npos) continue;
			size_t endPos = pos + findStr.size();
			int startColumn = editorColumn(lines[row], pos, tab);
			int endColumn = editorColumn(lines[row], endPos, tab);
			d.editor.SetCursorPosition(TextEditor::Coordinates(row, endColumn));
			d.editor.SetSelection(TextEditor::Coordinates(row, startColumn),
				TextEditor::Coordinates(row, endColumn));
			d.editor.EnsureCursorVisible();
			return true;
		}
		return false;
	};

	auto searchDocBack = [&](Doc& d, int startLine, int startCol) -> bool {
		std::vector<std::string> lines = d.editor.GetTextLines();
		int tab = d.editor.GetTabSize() > 0 ? d.editor.GetTabSize() : 4;
		int last = (int)lines.size() - 1;
		if (startLine > last) { startLine = last; startCol = INT_MAX; }
		for (int row = startLine; row >= 0; --row) {
			std::string hay = ic ? toLower(lines[row]) : lines[row];
			size_t limit = std::string::npos;
			if (row == startLine && startCol != INT_MAX) {
				limit = columnToByte(lines[row], startCol, tab);
				if (limit > hay.size()) limit = hay.size();
			}
			size_t best = std::string::npos;
			size_t pos = 0;
			while ((pos = hay.find(needle, pos)) != std::string::npos) {
				if (limit != std::string::npos && pos >= limit) break;
				best = pos;
				pos += needle.size();
			}
			if (best == std::string::npos) continue;
			size_t endPos = best + findStr.size();
			int startColumn = editorColumn(lines[row], best, tab);
			int endColumn = editorColumn(lines[row], endPos, tab);
			d.editor.SetCursorPosition(TextEditor::Coordinates(row, endColumn));
			d.editor.SetSelection(TextEditor::Coordinates(row, startColumn),
				TextEditor::Coordinates(row, endColumn));
			d.editor.EnsureCursorVisible();
			return true;
		}
		return false;
	};

	int first = currentIndex >= 0 ? currentIndex : 0;
	if (first < 0 || first >= (int)docs.size()) return;
	Doc& current = docs[first];

	if (!aBackwards) {
		TextEditor::Coordinates cur = current.editor.GetCursorPosition();
		if (searchDocFwd(current, cur.mLine, cur.mColumn)) {
			findStatus.clear();
			return;
		}

		if (findAllFiles) {
			for (int offset = 1; offset < (int)docs.size(); ++offset) {
				int index = (first + offset) % (int)docs.size();
				if (searchDocFwd(docs[index], 0, 0)) {
					currentIndex = index;
					requestedIndex = index;
					findStatus.clear();
					return;
				}
			}
		}

		if (searchDocFwd(current, 0, 0)) {
			findStatus.clear();
			return;
		}
	}
	else {
		TextEditor::Coordinates selStart = current.editor.GetSelectionStart();
		if (searchDocBack(current, selStart.mLine, selStart.mColumn)) {
			findStatus.clear();
			return;
		}

		if (findAllFiles) {
			for (int offset = (int)docs.size() - 1; offset >= 1; --offset) {
				int index = (first + offset) % (int)docs.size();
				if (searchDocBack(docs[index], INT_MAX, INT_MAX)) {
					currentIndex = index;
					requestedIndex = index;
					findStatus.clear();
					return;
				}
			}
		}

		if (searchDocBack(current, INT_MAX, INT_MAX)) {
			findStatus.clear();
			return;
		}
	}
	findStatus = "Failed to find text in \"" + findStr + "\"";
	showFind = true;
}

void App::programExecute() { build(true, false); }
void App::programCompile() { build(false, false); }

void App::programPublish() {
	Doc* e = currentDoc();
	if (!e) return;
	if (prefs.prg_debug) {
		appendOutput("Warning: Debug is enabled; publish will produce a slower executable.\n");
	}
	std::string defaultName = e->name;
	if (defaultName.empty() || defaultName == "untitled") defaultName = "untitled.exe";
	else {
		size_t dot = defaultName.rfind('.');
		if (dot != std::string::npos) defaultName = defaultName.substr(0, dot);
		defaultName += ".exe";
	}
	if (!fileSaveDialog(publishExePath, defaultName.c_str(),
		"Executable files (*.exe)|*.exe|All files (*.*)|*.*")) return;

	publishIconPath.clear();
	std::string iconPath;
	if (fileOpenDialog(iconPath, "Icon files (*.ico)|*.ico|All files (*.*)|*.*")) {
		publishIconPath = iconPath;
	}
	build(true, true);
}
void App::programDebug() { prefs.prg_debug = !prefs.prg_debug; }
void App::programNoLAA() { prefs.prg_nolaa = !prefs.prg_nolaa; }

void App::helpHome() {
	App::openUrl("https://kippykip.com/b3ddocs/commands/index.htm");
}
void App::helpAbout() { aboutOpen = true; }

void App::drawCommandLine() {
	if (!showCommandLine) return;
	int flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize;
	ImGui::SetNextWindowPos(ImVec2(windowW / 2.0f - 220, 60), ImGuiCond_Appearing);
	if (ImGui::Begin("Command Line", &showCommandLine, flags)) {
		ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
		static char buf[512];
		strcpy(buf, prefs.cmd_line.c_str());
		ImGui::SetNextItemWidth(400);
		ImGui::InputText("Arguments", buf, sizeof(buf));
		if (ImGui::Button("OK")) {
			prefs.cmd_line = buf;
			showCommandLine = false;
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel")) showCommandLine = false;
	}
	ImGui::End();
}

void App::drawStylize() {
	if (!showStylize) return;
	int flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize;
	ImGui::SetNextWindowPos(ImVec2(windowW / 2.0f - 260, 60), ImGuiCond_Appearing);
	if (ImGui::Begin("Stylization", &showStylize, flags)) {
		ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
		if (ImGui::BeginTabBar("##stylize_tabs")) {

			if (ImGui::BeginTabItem("Themes")) {
				ImGui::TextUnformatted("Premade themes");
				ImGui::Separator();
				auto themeRow = [&](const char* name, const ImVec4& bg, const ImVec4& accent, bool deletable) {
					ImGui::PushID(name);
					ImVec2 p = ImGui::GetCursorScreenPos();
					ImVec2 sw = ImVec2(22, 14);
					ImDrawList* dl = ImGui::GetWindowDrawList();
					dl->AddRectFilled(p, ImVec2(p.x + sw.x, p.y + sw.y), themeColU32(bg));
					dl->AddRectFilled(ImVec2(p.x + sw.x * 0.6f, p.y), ImVec2(p.x + sw.x, p.y + sw.y * 0.5f), themeColU32(accent));
					dl->AddRect(ImVec2(p.x - 1, p.y - 1), ImVec2(p.x + sw.x + 1, p.y + sw.y + 1), IM_COL32(255, 255, 255, 40));
					ImGui::Dummy(sw);
					ImGui::SameLine();
					bool clicked = ImGui::Selectable(name, prefs.theme == name);
					if (deletable) {
						ImGui::SameLine();
						if (ImGui::SmallButton("-")) {
							themeRemoveUserTheme(name);
							if (prefs.theme == name) {
								prefs.theme = themeBuiltin(0)->name;
								applyCurrentTheme();
							}
						}
					}
					ImGui::PopID();
					return clicked;
				};

				for (int i = 0; i < themeBuiltinCount(); ++i) {
					const ThemeSpec& t = *themeBuiltin(i);
					if (themeRow(t.name.c_str(), t.bg, t.accent, false)) {
						prefs.theme = t.name;
						applyEditorColorsToPrefs(t.name);
						applyCurrentTheme();
						prefs.close();
					}
				}
				ImGui::Separator();
				ImGui::TextUnformatted("User themes");
				for (int i = 0; i < themeUserCount(); ++i) {
					const UserTheme& t = *themeUser(i);
					if (themeRow(t.name.c_str(), themeCol3(t.bg), themeCol3(t.accent), true)) {
						prefs.theme = t.name;
						applyEditorColorsToPrefs(t.name);
						applyCurrentTheme();
						prefs.close();
					}
				}
				if (themeUserCount() == 0) ImGui::TextDisabled("(none)");
				ImGui::Separator();
				static char themeNameBuf[128] = "";
				ImGui::SetNextItemWidth(280);
				ImGui::InputText("Name", themeNameBuf, sizeof(themeNameBuf));
				ImGui::SameLine();
				if (ImGui::Button("Save current as theme")) {
					std::string name = themeNameBuf;
					if (!name.empty()) {
					int cols[ThemeEditorColorCount][3];
						currentEditorColors(cols);
						UserTheme t;
						t.name = name;
						themeCapture(t, cols);
						themeAddUserTheme(t);
						prefs.theme = name;
						applyEditorColorsToPrefs(name);
						applyCurrentTheme();
						prefs.close();
						themeNameBuf[0] = 0;
					}
				}
				ImGui::EndTabItem();
			}

			if (ImGui::BeginTabItem("Syntax Colors")) {
				bool deactivated = false;
				auto pick = [&](const char* label, int rgb[3]) {
					float f[3] = { rgb[0] / 255.0f, rgb[1] / 255.0f, rgb[2] / 255.0f };
					if (ImGui::ColorEdit3(label, f)) {
						rgb[0] = (int)(f[0] * 255.0f + 0.5f);
						rgb[1] = (int)(f[1] * 255.0f + 0.5f);
						rgb[2] = (int)(f[2] * 255.0f + 0.5f);
						if (ImGui::IsItemDeactivatedAfterEdit()) deactivated = true;
					}
				};
				pick("Background", prefs.rgb_bkgrnd);
				pick("String", prefs.rgb_string);
				pick("Identifier", prefs.rgb_ident);
				pick("Keyword", prefs.rgb_keyword);
				pick("Comment", prefs.rgb_comment);
				pick("Number", prefs.rgb_digit);
				pick("Default", prefs.rgb_default);
				pick("Known identifier", prefs.rgb_known);
				pick("Preprocessor identifier", prefs.rgb_preproc);
				pick("Global", prefs.rgb_global);
				pick("Constant", prefs.rgb_const);
				pick("Cursor", prefs.rgb_cursor);
				pick("Selection", prefs.rgb_selection);
				if (deactivated) {
					int cols[ThemeEditorColorCount][3];
					currentEditorColors(cols);
					themeSetUserEditorColors(prefs.theme, cols);
					prefs.close();
				}
				ImGui::EndTabItem();
			}

			if (ImGui::BeginTabItem("Interface")) {
				bool changed = false;
				changed |= ImGui::SliderInt("Corner rounding", &prefs.ui_rounding, 0, 12);
				bool roundingDone = ImGui::IsItemDeactivatedAfterEdit();
				changed |= ImGui::SliderFloat("Opacity", &prefs.ui_alpha, 0.40f, 1.00f, "%.2f");
				bool alphaDone = ImGui::IsItemDeactivatedAfterEdit();
				if (changed) {
					applyCurrentTheme();
				}
				if (roundingDone || alphaDone) prefs.close();
				ImGui::Separator();
				if (ImGui::Button("Reset to defaults")) {
					prefs.ui_rounding = 0;
					prefs.ui_alpha = 1.0f;
					applyCurrentTheme();
					prefs.close();
				}
				ImGui::EndTabItem();
			}

			ImGui::EndTabBar();
		}
	}
	ImGui::End();
}

void App::drawProjectWindow() {
	if (!showProjectWindow) return;
	int flags = ImGuiWindowFlags_NoCollapse;
	ImGui::SetNextWindowSize(ImVec2(620, 480), ImGuiCond_Appearing);
	if (ImGui::Begin("BlitzX3D Project", &showProjectWindow, flags)) {
		ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
		constrainFloatingWindow();
		ImGui::TextUnformatted("Project file");
		ImGui::SetNextItemWidth(-110);
		ImGui::InputText("##project_path", projectSavePathBuf, sizeof(projectSavePathBuf));
		ImGui::SameLine();
		if (ImGui::Button("Save As...")) {
			std::string path;
			if (fileSaveDialog(path, "project.bxp", "BlitzX3D project (*.bxp)|*.bxp|All files (*.*)|*.*")) {
				std::strncpy(projectSavePathBuf, path.c_str(), sizeof(projectSavePathBuf) - 1);
				projectSavePathBuf[sizeof(projectSavePathBuf) - 1] = 0;
			}
		}
		ImGui::Separator();
		ImGui::TextUnformatted("Main file");
		if (projectDraftMainPath.empty()) ImGui::TextDisabled("(none)");
		else ImGui::TextWrapped("%s", projectDraftMainPath.c_str());
		ImGui::Separator();
		ImGui::TextUnformatted("Source files");
		ImGui::BeginChild("##project_sources", ImVec2(0, -78), true);
		for (size_t k = 0; k < docs.size(); ++k) {
			if (docs[k].path.empty()) continue;
			ImGui::PushID((int)k);
			bool selected = samePath(projectDraftMainPath, docs[k].path);
			if (ImGui::Selectable(docs[k].name.c_str(), selected)) projectDraftMainPath = docs[k].path;
			ImGui::SameLine();
			ImGui::TextDisabled("%s", docs[k].path.c_str());
			ImGui::PopID();
		}
		ImGui::EndChild();
		if (ImGui::Button("Open Source...")) {
			std::string path;
			if (fileOpenDialog(path, "Blitz source (*.bb)|*.bb|All files (*.*)|*.*") && openFile(path)) {
				if (projectOpen) projectFiles.push_back(normalizePath(path));
				if (projectOpen) refreshProjectSymbols();
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Save Project")) {
			std::string path = projectSavePathBuf;
			if (path.empty()) {
				if (fileSaveDialog(path, "project.bxp", "BlitzX3D project (*.bxp)|*.bxp|All files (*.*)|*.*")) {
					std::strncpy(projectSavePathBuf, path.c_str(), sizeof(projectSavePathBuf) - 1);
					projectSavePathBuf[sizeof(projectSavePathBuf) - 1] = 0;
				}
			}
			if (!path.empty()) projectStatus = saveProjectFile(path) ? "Project saved." : "Project could not be saved.";
		}
		ImGui::SameLine();
		if (ImGui::Button("Import IPF...")) {
			std::string path;
			if (fileOpenDialog(path, "Old Blitz project (*.ipf)|*.ipf|All files (*.*)|*.*")) {
				if (convertIpfToBxp(path)) {
					openProjectWindow();
					projectStatus = "IPF converted to BXP.";
				}
				else projectStatus = "IPF conversion failed.";
			}
		}
		if (!projectStatus.empty()) ImGui::TextUnformatted(projectStatus.c_str());
	}
	ImGui::End();
}

void App::drawPaneBackground() {
	ImGuiViewport* vp = ImGui::GetMainViewport();
	float menuH = ImGui::GetFrameHeight();
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyle().Colors[ImGuiCol_WindowBg]);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + menuH), ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, vp->WorkSize.y - menuH), ImGuiCond_Always);
	ImGui::Begin("##panebackground", nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing);
	ImGui::End();
	ImGui::PopStyleVar();
	ImGui::PopStyleColor();
}

Prefs::CompileOptions App::effectiveCompileOptions() {
	Prefs::CompileOptions co;
	co.noautodecl = prefs.prg_noautodecl;
	co.encrypt = prefs.prg_encrypt;
	if (projectOpen && !projectPath.empty()) {
		if (!prefs.projectOptionsActive) {
			prefs.projectOptions = co;
			prefs.projectOptionsActive = true;
			prefs.saveProjectOptions(projectPath);
		}
		return prefs.projectOptions;
	}
	return co;
}

void App::build(bool exec, bool publish) {
	if (compiling) return;
	compiling = true;
	Doc* e = currentDoc();
	if (!e) { compiling = false; return; }

	if (!fileSaveAll()) {
		appendOutput("Save failed; compile aborted.\n");
		compiling = false;
		return;
	}

	std::string src_file = e->path;
	if (projectOpen && !projectMainPath.empty()) {
		bool mainOpen = false;
		for (const auto& d : docs) {
			if (samePath(d.path, projectMainPath)) {
				mainOpen = true;
				break;
			}
		}
		if (mainOpen) src_file = projectMainPath;
	}

	std::vector<std::string> args;
	args.push_back(prefs.homeDir + "/bin/blitzcc");
	Prefs::CompileOptions co = effectiveCompileOptions();
	if (prefs.prg_dumpasm) args.push_back("-a");
	if (prefs.prg_veryquiet) args.push_back("+q");
	else if (prefs.prg_quiet) args.push_back("-q");
	if (!publish && !exec) args.push_back("-c");
	if (prefs.prg_debug) args.push_back("-d");
	if (prefs.prg_dumpkeys) args.push_back("-k");
	if (prefs.prg_nolaa) args.push_back("-nlaa");
	if (co.noautodecl) args.push_back("-noautodecl");
	if (co.encrypt) args.push_back("-encrypt");

	if (publish) {
		std::string exe = publishExePath.empty() ? src_file : publishExePath;
		if (exe.empty()) exe = "untitled.exe";
		args.push_back("-o");
		args.push_back(exe);
	}

	std::string src = src_file;
	if (src.empty()) {
		src = prefs.homeDir + "/temp/tmp.bb";
		std::ofstream out(src, std::ios::binary | std::ios::trunc);
		if (!out.good()) {
			appendOutput("Error writing temporary file.\n");
			compiling = false;
			return;
		}
		out << e->editor.GetText();
		out.close();
		e->path = src;
		e->name = "tmp.bb";
		rebuildFuncList(*e);
	}
	else {
		prefs.prg_lastbuild = src_file;
	}

	args.push_back(src);
	if (!prefs.cmd_line.empty()) {
		std::vector<std::string> extra = splitCommandLine(prefs.cmd_line);
		args.insert(args.end(), extra.begin(), extra.end());
	}
	compile(args);
}

void App::compile(const std::vector<std::string>& args) {
	if (compileThread.joinable()) compileThread.join();
	compileOK = true;
	std::string cmd;
	for (size_t k = 0; k < args.size(); ++k) {
		if (k) cmd += ' ';
		cmd += args[k];
	}
	appendOutput(">>> " + cmd + "\n");

	compileThread = std::thread([this, args]() {
		std::string output;
		int code = runProcess(args, output);

		std::vector<std::string> newLines;
		{
			std::stringstream ss(output);
			std::string line;
			while (std::getline(ss, line, '\n')) {
				if (!line.empty() && line.back() == '\r') line.pop_back();
				newLines.push_back(line);
			}
		}

		{
			std::lock_guard<std::mutex> lock(outputMutex);
			this->output += output;
			for (const auto& line : newLines)
				this->outputLines.push_back(line);
		}
		for (const auto& line : newLines)
			parseOutputLine(line);

		if (code != 0) compileOK = false;
		else if (!publishIconPath.empty() && !publishExePath.empty()) {
			if (applyIconToExe(publishExePath, publishIconPath)) {
				appendOutput("Icon applied to executable.\n");
			}
			else {
				appendOutput("Warning: could not apply icon to executable.\n");
			}
		}
		drawIde = true;
		compiling = false;
	});
}

void App::appendOutput(const std::string& text) {
	std::lock_guard<std::mutex> lock(outputMutex);
	output += text;
	std::stringstream ss(text);
	std::string line;
	while (std::getline(ss, line, '\n')) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		outputLines.push_back(line);
	}
}

void App::parseOutputLine(const std::string& line) {
	if (line.empty() || line[0] != '"') return;
	size_t n = line.find('"', 1);
	if (n == std::string::npos) return;
	if (n + 1 >= line.size() || line[n + 1] != ':') return;
	std::string file = line.substr(1, n - 1);
	std::string rest = line.substr(n + 2);
	int row1 = 0, col1 = 0, row2 = 0, col2 = 0;
	if (sscanf(rest.c_str(), "%d:%d:%d:%d", &row1, &col1, &row2, &col2) == 4) {
		std::lock_guard<std::mutex> lock(outputMutex);
		pendingGotoPath = file;
		pendingGotoRow = row1;
		pendingGotoCol = col1;
		pendingGoto = true;
	}
}

void App::processPendingGoto() {
	std::string path;
	int row = 0, col = 0;
	{
		std::lock_guard<std::mutex> lock(outputMutex);
		if (!pendingGoto) return;
		pendingGoto = false;
		path = std::move(pendingGotoPath);
		row = pendingGotoRow;
		col = pendingGotoCol;
	}
	openFile(path);
	Doc* d = currentDoc();
	if (d && row >= 1) {
		TextEditor::Coordinates pos(row - 1, (std::max)(0, col - 1));
		d->editor.SetCursorPosition(pos);
		d->editor.SetSelection(pos, pos);
	}
}

void App::rebuildFuncList(Doc& d) {
	d.funcs.clear();
	d.globals.clear();
	d.consts.clear();
	std::string text = stripBOM(d.editor.GetText());
	std::stringstream ss(text);
	std::string line;
	int ln = 0;
	while (std::getline(ss, line, '\n')) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		size_t lead = line.find_first_not_of(" \t");
		std::string t = lead == std::string::npos ? "" : toLower(line.substr(lead));
		if (startsWithWord(t, "function")) {
			size_t p = line.find_first_of(" \t", lead);
			std::string name = p == std::string::npos ? "" : line.substr(p + 1);
			name = name.substr(0, name.find_first_of(" ("));
			name = stripDeclSuffix(name);
			if (name.size()) d.funcs.push_back({ name, ln, 0 });
		}
		else if (startsWithWord(t, "type")) {
			size_t p = line.find_first_of(" \t", lead);
			std::string name = p == std::string::npos ? "" : line.substr(p + 1);
			if (name.size()) d.funcs.push_back({ name, ln, 1 });
		}
		else if (startsWithWord(t, "enum")) {
			size_t p = line.find_first_of(" \t", lead);
			if (p != std::string::npos) {
				std::string name = line.substr(p + 1);
				size_t end = name.find_first_of(" \t({");
				if (end != std::string::npos) name.resize(end);
				if (!name.empty()) d.funcs.push_back({ name, ln, 1 });
			}
		}
		else if (startsWithWord(t, "global")) {
			parseBlitzDecl(line.substr(lead + 6), d.globals);
		}
		else if (startsWithWord(t, "const")) {
			parseBlitzDecl(line.substr(lead + 5), d.consts);
		}
		else if (t.size() && t[0] == '.') {
			size_t p = line.find_first_of(" \t", lead);
			std::string name = p == std::string::npos ? line.substr(lead + 1) : line.substr(lead + 1, p - lead - 1);
			if (name.size()) d.funcs.push_back({ name, ln, 2 });
		}
		++ln;
	}
}

void App::refreshProjectSymbols() {
	if (!projectOpen || projectMainPath.empty()) return;

	std::vector<std::string> visited;
	std::vector<std::string> reachable;
	std::function<void(const std::string&)> visit = [&](const std::string& path) {
		std::string key = pathKey(path);
		if (std::find(visited.begin(), visited.end(), key) != visited.end()) return;
		visited.push_back(key);
		reachable.push_back(key);
		for (const auto& include : getIncludePaths(path)) visit(include);
	};
	visit(projectMainPath);
	for (const auto& path : projectFiles) {
		std::string key = pathKey(path);
		if (std::find(reachable.begin(), reachable.end(), key) == reachable.end()) reachable.push_back(key);
	}
	for (const auto& d : docs) {
		if (d.path.empty()) continue;
		std::string key = pathKey(d.path);
		if (std::find(reachable.begin(), reachable.end(), key) == reachable.end()) reachable.push_back(key);
	}

	projectIncludedDocs.clear();
	projectNavigatorDocs.clear();
	std::map<std::string, int> docsByPath;
	for (size_t docIndex = 0; docIndex < docs.size(); ++docIndex) {
		if (!docs[docIndex].path.empty()) docsByPath[pathKey(docs[docIndex].path)] = (int)docIndex;
	}
	std::set<std::string> projectGlobals;
	std::set<std::string> projectConsts;
	std::set<std::string> projectFuncs;
	for (const auto& path : reachable) {
		auto docIt = docsByPath.find(path);
		if (docIt == docsByPath.end()) continue;
		int docIndex = docIt->second;
		auto& d = docs[docIndex];
		projectIncludedDocs.push_back(docIndex);
		projectGlobals.insert(d.globals.begin(), d.globals.end());
		projectConsts.insert(d.consts.begin(), d.consts.end());
		for (const auto& f : d.funcs)
			if (f.kind == 0) projectFuncs.insert(f.label);
	}
	projectNavigatorDocs = projectIncludedDocs;

	for (auto& d : docs)
		d.editor.SetLanguageDefinition(makeBlitzLangDef(keywords, funcs, projectFuncs, projectGlobals, projectConsts));
}

void App::handleCtrlClick(Doc& d, const std::string& word, int line, int column) {
	(void)column;
	std::string ln = d.editor.GetLineText(line);
	size_t lead = ln.find_first_not_of(" \t");
	std::string t = lead == std::string::npos ? "" : toLower(ln.substr(lead));
	if (!t.empty() && t[0] == '#') t.erase(0, 1);

	if (startsWithWord(t, "include")) {
		size_t q1 = ln.find('"');
		size_t q2 = q1 == std::string::npos ? std::string::npos : ln.find('"', q1 + 1);
		if (q1 != std::string::npos && q2 != std::string::npos) {
			std::string rel = ln.substr(q1 + 1, q2 - q1 - 1);
			fs::path target(rel);
			if (target.is_relative() && !d.path.empty())
				target = fs::path(d.path).parent_path() / target;
			if (fs::exists(target)) { openFile(target.string()); return; }
			if (fs::exists(rel)) { openFile(rel); return; }
		}
	}

	if (word.empty()) return;

	std::string lw = stripDeclSuffix(toLower(word));
	for (size_t k = 0; k < docs.size(); ++k) {
		if (projectOpen && std::find(projectIncludedDocs.begin(), projectIncludedDocs.end(), (int)k) == projectIncludedDocs.end()) continue;
		Doc& t = docs[k];
		for (const auto& f : t.funcs) {
			if (stripDeclSuffix(toLower(f.label)) == lw) {
				currentIndex = (int)k;
				requestedIndex = (int)k;
				t.editor.SetCursorPosition(TextEditor::Coordinates(f.line, 0));
				t.editor.SetSelection(TextEditor::Coordinates(f.line, 0),
					TextEditor::Coordinates(f.line, 0));
				return;
			}
		}
	}

	int foundLine = 0;
	std::vector<std::string> visited;
	std::string foundFile = findFunctionInIncludes(d.path, lw, visited, foundLine);
	if (!foundFile.empty()) {
		openFile(foundFile);
		Doc* nd = currentDoc();
		if (nd) {
			nd->editor.SetCursorPosition(TextEditor::Coordinates(foundLine, 0));
			nd->editor.SetSelection(TextEditor::Coordinates(foundLine, 0),
				TextEditor::Coordinates(foundLine, 0));
		}
	}
}

void App::initKeywords() {
	keywords = builtinBlitzKeywords();
	if (prefs.homeDir.empty()) return;
	keywordThread = std::thread([this]() {
		std::string kws;
		int code = 0;
		runProcess({ prefs.homeDir + "/bin/blitzcc", "+k" }, kws, &code);
		std::set<std::string> loaded;
		std::set<std::string> loadedFuncs;
		std::stringstream ss(kws);
		std::string line;
		const std::set<std::string> builtin = builtinBlitzKeywords();
		while (std::getline(ss, line, '\n')) {
			if (!line.empty() && line.back() == '\r') line.pop_back();
			if (line.empty()) continue;
			std::string kw = line.substr(0, line.find(' '));
			if (kw.find('(') != std::string::npos) kw = kw.substr(0, kw.find('('));
			if (kw.empty()) continue;
			if (!isalnum((unsigned char)kw.back())) kw.pop_back();
			if (kw.find("Blitz_") == 0) kw = kw.substr(6);
			std::string kwLow = kw;
			std::transform(kwLow.begin(), kwLow.end(), kwLow.begin(), ::tolower);
			if (builtin.find(kwLow) != builtin.end()) loaded.insert(kw);
			else loadedFuncs.insert(kw);
		}
		{
			std::lock_guard<std::mutex> lock(keywordMutex);
			if (!loaded.empty()) {
				for (const auto& kw : loaded) keywords.insert(kw);
				for (const auto& f : loadedFuncs) funcs.insert(f);
				keywordsLoaded = true;
			}
		}
	});
}


