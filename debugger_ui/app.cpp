#include "stdafx.h"
#include "app.h"
#include "prefs.h"

#include "../theme.h"
#include "../imgui/imgui.h"
#include "../imgui/backends/imgui_impl_sdl3.h"
#include "../imgui/backends/imgui_impl_opengl3.h"
#include "../imgui/backends/imgui_impl_opengl3_loader.h"

#include <SDL3/SDL.h>

App* g_app = nullptr;

static ImU32 flameColor(const std::string& name) {
	static const ImU32 palette[] = {
		IM_COL32(231, 105, 97, 255),
		IM_COL32(244, 162, 97, 255),
		IM_COL32(244, 207, 111, 255),
		IM_COL32(120, 198, 121, 255),
		IM_COL32(120, 194, 218, 255),
		IM_COL32(130, 149, 226, 255),
		IM_COL32(200, 140, 220, 255),
		IM_COL32(230, 120, 170, 255),
		IM_COL32(110, 200, 160, 255),
		IM_COL32(220, 130, 90, 255),
		IM_COL32(150, 160, 200, 255),
		IM_COL32(180, 170, 130, 255),
	};
	unsigned int hash = 2166136261u;
	for (unsigned char c : name) {
		hash ^= c;
		hash *= 16777619u;
	}
	return palette[hash % (sizeof(palette) / sizeof(palette[0]))];
}

static void flameGradient(ImU32 base, ImU32& top, ImU32& bottom, ImU32& border) {
	int r = (base >> IM_COL32_R_SHIFT) & 0xFF;
	int g = (base >> IM_COL32_G_SHIFT) & 0xFF;
	int b = (base >> IM_COL32_B_SHIFT) & 0xFF;
	auto lighten = [](int c, int amt) { return c + amt > 255 ? 255 : c + amt; };
	auto darken = [](int c, int amt) { return c - amt < 0 ? 0 : c - amt; };
	top = IM_COL32(lighten(r, 24), lighten(g, 24), lighten(b, 24), 255);
	bottom = IM_COL32(darken(r, 18), darken(g, 18), darken(b, 18), 255);
	border = IM_COL32(darken(r, 42), darken(g, 42), darken(b, 42), 255);
}

App::App() :window(0), windowW(0), windowH(0), initialized(false), quitting(false), connected(false),
	gameProc(0),
	shmFile(0), shmView(0), shm(0), cmdShmFile(0), cmdShmView(0), cmdShm(0),
	snapEvent(0), cmdEvent(0), lastSnapSeq(0), state(DBG_STATE_STARTING), curRow(0), curCol(0),
	m_currentFilter(0), logPendingScroll(false), source(0),
	hoverValid(false), hoverSamples(0), hoverSelf(0) {
}

App::~App() {
	shutdown();
}

bool App::init(int pid) {
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) return false;

	prefs.open();

	gameProc = OpenProcess(SYNCHRONIZE, FALSE, pid);

	SDL_Rect wa;
	SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &wa);
	windowW = wa.w;
	windowH = 240;
	if (windowW < 640) windowW = 640;

	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
	window = SDL_CreateWindow("Blitz Debugger", windowW, windowH, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
	if (!window) { SDL_Quit(); return false; }
	SDL_SetWindowPosition(window, wa.x, wa.y + wa.h - windowH);
	if (!SDL_GL_CreateContext(window) || !SDL_GL_SetSwapInterval(1)) {
		SDL_DestroyWindow(window);
		SDL_Quit();
		return false;
	}

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	if (!prefs.configDir.empty()) {
		io.IniFilename = _strdup((prefs.configDir + "/imgui_debugger.ini").c_str());
	}

	themeApplyStyle(prefs.theme, 0.0f, 1.0f);
	ImGui_ImplSDL3_InitForOpenGL(window, SDL_GL_GetCurrentContext());
	ImGui_ImplOpenGL3_Init("#version 130");

	connected = false;
	for (int attempt = 0; attempt < 100 && !connected; ++attempt) {
		shmFile = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, shmNameForPid(pid).c_str());
		if (shmFile) {
			shmView = MapViewOfFile(shmFile, FILE_MAP_ALL_ACCESS, 0, 0, DBG_SHM_SIZE);
			if (shmView) shm = (DbgShm*)shmView;
		}
		cmdShmFile = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, cmdShmNameForPid(pid).c_str());
		if (cmdShmFile) {
			cmdShmView = MapViewOfFile(cmdShmFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(DbgCmdShm));
			if (cmdShmView) cmdShm = (DbgCmdShm*)cmdShmView;
		}
		snapEvent = OpenEventA(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, snapEventForPid(pid).c_str());
		cmdEvent = OpenEventA(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, cmdEventForPid(pid).c_str());
		if (shm && cmdShm && snapEvent && cmdEvent) {
			connected = true;
		}
		else {
			if (shmView) UnmapViewOfFile(shmView);
			if (shmFile) CloseHandle(shmFile);
			if (cmdShmView) UnmapViewOfFile(cmdShmView);
			if (cmdShmFile) CloseHandle(cmdShmFile);
			if (snapEvent) CloseHandle(snapEvent);
			if (cmdEvent) CloseHandle(cmdEvent);
			shm = 0; shmView = 0; shmFile = 0; cmdShm = 0; cmdShmView = 0; cmdShmFile = 0;
			snapEvent = 0; cmdEvent = 0;
			SDL_Delay(50);
		}
	}

	initialized = true;
	return true;
}

void App::shutdown() {
	if (!initialized) return;
	initialized = false;
	if (gameProc) { CloseHandle(gameProc); gameProc = 0; }
	if (shmView) UnmapViewOfFile(shmView);
	if (shmFile) CloseHandle(shmFile);
	if (cmdShmView) UnmapViewOfFile(cmdShmView);
	if (cmdShmFile) CloseHandle(cmdShmFile);
	if (snapEvent) CloseHandle(snapEvent);
	if (cmdEvent) CloseHandle(cmdEvent);
	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplSDL3_Shutdown();
	ImGui::DestroyContext();
	if (window) {
		SDL_GL_DestroyContext(SDL_GL_GetCurrentContext());
		SDL_DestroyWindow(window);
		window = nullptr;
	}
	SDL_Quit();
}

bool App::readSnapshot() {
	if (!connected || !shm) return false;
	LONG seq = InterlockedCompareExchange(&shm->snapSeq, 0, 0);
	if (seq == lastSnapSeq) return false;
	int size = shm->payloadSize;
	if (size < 0 || size > DBG_SHM_SIZE - (int)(offsetof(DbgShm, payload))) return false;

	DbgSerializer s(shm->payload, size);

	int st = 0;
	if (!s.readInt(st)) return false;
	state = st;
	{
		std::string file;
		int row = 0, col = 0;
		if (!s.readStr(file) || !s.readInt(row) || !s.readInt(col)) return false;
		curFile = file;
		curRow = row;
		curCol = col;
		if (!file.empty() && file != sourcePath) {
			loadSource(file, row, col);
		}
	}
	{
		int n = 0;
		if (!s.readInt(n) || n < 0 || n > 100000) return false;
		log.clear();
		log.reserve(n);
		for (int k = 0; k < n; ++k) {
			unsigned char sev = 0;
			std::string text;
			if (!s.readByte(sev) || !s.readStr(text)) return false;
			log.push_back({ (ELogSeverity)sev, text });
		}
		logPendingScroll = true;
	}
	{
		int n = 0;
		if (!s.readInt(n) || n < 0 || n > 100000) return false;
		constsNodes.resize(n);
		for (int k = 0; k < n; ++k) if (!readTree(s, constsNodes[k])) return false;
		if (!s.readInt(n) || n < 0 || n > 100000) return false;
		globalsNodes.resize(n);
		for (int k = 0; k < n; ++k) if (!readTree(s, globalsNodes[k])) return false;
		if (!s.readInt(n) || n < 0 || n > 100000) return false;
		localsNodes.resize(n);
		for (int k = 0; k < n; ++k) if (!readTree(s, localsNodes[k])) return false;
	}
	{
		std::string sum;
		int n = 0;
		if (!s.readStr(sum) || !s.readInt(n) || n < 0 || n > 100000) return false;
		profilerSummary = sum;
		profilerRows.resize(n);
		for (int k = 0; k < n; ++k) {
			DbgProfilerRow& r = profilerRows[k];
			if (!s.readStr(r.func) || !s.readDouble(r.selfMs) || !s.readDouble(r.totalMs) ||
				!s.readDouble(r.maxMs) || !s.readInt(r.callCount) || !s.readInt(r.netObjDelta) || !s.readInt(r.netStrDelta)) return false;
		}
	}
	{
		int n = 0;
		if (!s.readInt(n) || n < 0 || n > 100000) return false;
		flameNodes.resize(n);
		for (int k = 0; k < n; ++k) if (!readFlameNode(s, flameNodes[k])) return false;
	}

	lastSnapSeq = seq;
	return true;
}

void App::sendCmd(int cmd) {
	if (!connected || !cmdShm) return;
	cmdShm->cmd = cmd;
	InterlockedIncrement(&cmdShm->seq);
	SetEvent(cmdEvent);
}

void App::loadSource(const std::string& file, int row, int col) {
	if (file.empty()) return;
	if (!source) source = new SourceFile();
	if (sourcePath != file) {
		sourcePath = file;
		source->load(file.c_str());
	}
	source->highLight(row, col);
}

void App::run() {
	while (!quitting) {
		if (gameProc && WaitForSingleObject(gameProc, 0) == WAIT_OBJECT_0) quitting = true;
		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			ImGui_ImplSDL3_ProcessEvent(&event);
			if (event.type == SDL_EVENT_QUIT ||
				(event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window))) {
				sendCmd(DBG_CMD_END);
				quitting = true;
			}
		}
		readSnapshot();

		frame();
	}
}

void App::frame() {
	ImGui_ImplOpenGL3_NewFrame();
	ImGui_ImplSDL3_NewFrame();
	ImGui::NewFrame();

	ImGuiViewport* vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(vp->WorkPos, ImGuiCond_Always);
	ImGui::SetNextWindowSize(vp->WorkSize, ImGuiCond_Always);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
	ImGui::Begin("##debugger", nullptr,
		ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

	drawToolbar();
	ImGui::Separator();

	ImVec2 avail = ImGui::GetContentRegionAvail();
	float rightW = avail.x > 400 ? 360.0f : avail.x * 0.30f;

	ImGui::BeginChild("##vars", ImVec2(rightW, -1), true);
	drawVarsTabs();
	ImGui::EndChild();

	ImGui::SameLine();

	ImGui::BeginChild("##main", ImVec2(-1, -1), true);
	drawMainTabs();
	ImGui::EndChild();

	ImGui::End();
	ImGui::PopStyleVar();

	ImGui::Render();
	int fbw = 0, fbh = 0;
	SDL_GetWindowSizeInPixels(window, &fbw, &fbh);
	glViewport(0, 0, fbw, fbh);
	const ImVec4& bgc = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
	glClearColor(bgc.x, bgc.y, bgc.z, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
	SDL_GL_SwapWindow(window);
}

void App::drawToolbar() {
	bool running = (state == DBG_STATE_RUNNING);
	bool enable = (state == DBG_STATE_RUNNING || state == DBG_STATE_STOPPED);

	ImGui::BeginDisabled(!(enable && running));
	if (ImGui::Button("Stop")) sendCmd(DBG_CMD_STOP);
	ImGui::EndDisabled();
	ImGui::SameLine();

	ImGui::BeginDisabled(!(enable && !running));
	if (ImGui::Button("Run")) sendCmd(DBG_CMD_RUN);
	ImGui::SameLine();
	if (ImGui::Button("Step Over")) sendCmd(DBG_CMD_STEPOVER);
	ImGui::SameLine();
	if (ImGui::Button("Step Into")) sendCmd(DBG_CMD_STEPINTO);
	ImGui::SameLine();
	if (ImGui::Button("Step Out")) sendCmd(DBG_CMD_STEPOUT);
	ImGui::EndDisabled();
	ImGui::SameLine();

	ImGui::BeginDisabled(!enable);
	if (ImGui::Button("End")) sendCmd(DBG_CMD_END);
	ImGui::EndDisabled();

	ImGui::SameLine();
	ImGui::Separator();
	ImGui::SameLine();

	const char* items[] = { "All", "Info", "Warnings", "Errors" };
	ImGui::SetNextItemWidth(120.0f);
	ImGui::Combo("##filter", &m_currentFilter, items, 4);

	if (!connected) {
		ImGui::SameLine();
		ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "(not connected)");
	}
}

void App::drawVarsTabs() {
	if (!ImGui::BeginTabBar("##varstabs")) return;

	if (state != DBG_STATE_STOPPED) {
		ImGui::TextDisabled("Variables are shown while paused.");
		ImGui::EndTabBar();
		return;
	}

	if (ImGui::BeginTabItem("Locals")) {
		ImGui::BeginChild("##locals");
		drawTreeNodes(localsNodes);
		ImGui::EndChild();
		ImGui::EndTabItem();
	}
	if (ImGui::BeginTabItem("Globals")) {
		ImGui::BeginChild("##globals");
		drawTreeNodes(globalsNodes);
		ImGui::EndChild();
		ImGui::EndTabItem();
	}
	if (ImGui::BeginTabItem("Consts")) {
		ImGui::BeginChild("##consts");
		drawTreeNodes(constsNodes);
		ImGui::EndChild();
		ImGui::EndTabItem();
	}

	ImGui::EndTabBar();
}

void App::drawTreeNodes(const std::vector<DbgTreeNode>& items) {
	for (const DbgTreeNode& n : items) {
		if (n.expandable) {
			bool open = ImGui::TreeNodeEx((const void*)(intptr_t)ImGui::GetID(n.id.c_str()), 0, "%s", n.label.c_str());
			if (open) {
				drawTreeNodes(n.children);
				ImGui::TreePop();
			}
		}
		else {
			ImGui::TextUnformatted(n.label.c_str());
		}
	}
}

void App::drawMainTabs() {
	if (!ImGui::BeginTabBar("##maintabs", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_AutoSelectNewTabs)) return;

	if (ImGui::BeginTabItem("Source")) {
		drawSourceTab();
		ImGui::EndTabItem();
	}
	if (ImGui::BeginTabItem("Debug log")) {
		drawDebugLog();
		ImGui::EndTabItem();
	}
	if (ImGui::BeginTabItem("Profiler")) {
		drawProfilerTab();
		ImGui::EndTabItem();
	}
	if (ImGui::BeginTabItem("Flame Graph")) {
		drawFlameGraphTab();
		ImGui::EndTabItem();
	}

	ImGui::EndTabBar();
}

void App::drawSourceTab() {
	if (source) {
		source->render(source->getName().c_str());
	}
	else {
		ImGui::TextDisabled("No source file");
	}
}

void App::drawDebugLog() {
	bool scroll = logPendingScroll;
	logPendingScroll = false;

	ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(prefs.rgb_default[0], prefs.rgb_default[1], prefs.rgb_default[2], 255));
	ImGui::BeginChild("##logscroll", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
	const size_t logCount = log.size();
	const size_t startIndex = logCount > 2000 ? logCount - 2000 : 0;
	for (size_t k = startIndex; k < logCount; ++k) {
		const LogEntry& e = log[k];
		if (m_currentFilter == 1 && e.severity != LOG_INFO) continue;
		if (m_currentFilter == 2 && e.severity != LOG_WARNING) continue;
		if (m_currentFilter == 3 && e.severity != LOG_ERROR) continue;
		ImGui::TextUnformatted(e.text.c_str());
	}
	if (scroll) ImGui::SetScrollHereY(1.0f);
	ImGui::EndChild();
	ImGui::PopStyleColor();
}

void App::drawProfilerTab() {
	ImGui::BeginChild("##profchild");

	if (profilerRows.empty()) {
		ImGui::TextDisabled("No profiling data yet.");
		ImGui::EndChild();
		return;
	}

	ImGui::TextUnformatted(profilerSummary.c_str());

	ImGui::TextUnformatted("Samples (seconds):");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(120.0f);
	static int windowSeconds = 30;
	if (ImGui::InputInt("##windowsec", &windowSeconds)) {
		if (windowSeconds < 0) windowSeconds = 0;
		if (windowSeconds > 300) windowSeconds = 300;
	}

	if (!ImGui::BeginTable("##proftable", 10,
		ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
		ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
		ImGui::EndChild();
		return;
	}

	ImGui::TableSetupScrollFreeze(0, 1);
	ImGui::TableSetupColumn("Function", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoHeaderWidth, 190.0f);
	ImGui::TableSetupColumn("% CPU", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoHeaderWidth, 65.0f);
	ImGui::TableSetupColumn("% Mem", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoHeaderWidth, 65.0f);
	ImGui::TableSetupColumn("Calls", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoHeaderWidth, 55.0f);
	ImGui::TableSetupColumn("Self ms", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoHeaderWidth, 80.0f);
	ImGui::TableSetupColumn("Total ms", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoHeaderWidth, 80.0f);
	ImGui::TableSetupColumn("Max ms", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoHeaderWidth, 70.0f);
	ImGui::TableSetupColumn("Net Objs", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoHeaderWidth, 75.0f);
	ImGui::TableSetupColumn("Net Strs", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoHeaderWidth, 75.0f);
	ImGui::TableSetupColumn("Window s", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoHeaderWidth, 60.0f);
	ImGui::TableHeadersRow();

	double totalSelf = 0;
	for (const DbgProfilerRow& r : profilerRows) totalSelf += r.selfMs;
	int totalPosObj = 0;
	for (const DbgProfilerRow& r : profilerRows) if (r.netObjDelta > 0) totalPosObj += r.netObjDelta;

	char buf[64];
	for (const DbgProfilerRow& r : profilerRows) {
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::TextUnformatted(r.func.c_str());
		ImGui::TableSetColumnIndex(1);
		sprintf(buf, "%.1f%%", totalSelf > 0 ? 100.0 * r.selfMs / totalSelf : 0.0);
		ImGui::TextUnformatted(buf);
		ImGui::TableSetColumnIndex(2);
		sprintf(buf, "%.1f%%", (totalPosObj && r.netObjDelta > 0) ? 100.0 * r.netObjDelta / totalPosObj : 0.0);
		ImGui::TextUnformatted(buf);
		ImGui::TableSetColumnIndex(3);
		sprintf(buf, "%d", r.callCount);
		ImGui::TextUnformatted(buf);
		ImGui::TableSetColumnIndex(4);
		sprintf(buf, "%.3f", r.selfMs);
		ImGui::TextUnformatted(buf);
		ImGui::TableSetColumnIndex(5);
		sprintf(buf, "%.3f", r.totalMs);
		ImGui::TextUnformatted(buf);
		ImGui::TableSetColumnIndex(6);
		sprintf(buf, "%.3f", r.maxMs);
		ImGui::TextUnformatted(buf);
		ImGui::TableSetColumnIndex(7);
		sprintf(buf, "%+d", r.netObjDelta);
		ImGui::TextUnformatted(buf);
		ImGui::TableSetColumnIndex(8);
		sprintf(buf, "%+d", r.netStrDelta);
		ImGui::TextUnformatted(buf);
		ImGui::TableSetColumnIndex(9);
		sprintf(buf, "%d", windowSeconds);
		ImGui::TextUnformatted(buf);
	}

	ImGui::EndTable();
	ImGui::EndChild();
}

void App::drawFlameTree(const std::vector<DbgFlameNode>& items, float x, float y, float w, float boxH, float rowPitch, int parentSamples) {
	if (items.empty() || w < 1.0f || parentSamples <= 0 || boxH < 1.0f) return;
	ImDrawList* dl = ImGui::GetWindowDrawList();

	ImVec2 mouse = ImGui::GetIO().MousePos;

	double cx = (double)x;
	for (size_t i = 0; i < items.size(); ++i) {
		const DbgFlameNode& n = items[i];
		if (n.samples <= 0) continue;
		double cwTrue = (double)n.samples / (double)parentSamples * (double)w;
		if (cwTrue <= 0.0) continue;
		float x0 = (float)cx;
		float cw = (float)cwTrue;
		float x1 = x0 + cw, y0 = y, y1 = y + boxH;
		if (cw >= 2.0f) {
			ImU32 base = flameColor(n.name);
			ImU32 top, bottom, border;
			flameGradient(base, top, bottom, border);

			dl->AddRectFilledMultiColor(ImVec2(x0, y0), ImVec2(x1, y1), top, top, bottom, bottom);
			dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), border);

			if (cw > 28.0f && boxH >= 12.0f) {
				int r = (base >> IM_COL32_R_SHIFT) & 0xFF;
				int g = (base >> IM_COL32_G_SHIFT) & 0xFF;
				int b = (base >> IM_COL32_B_SHIFT) & 0xFF;
				int luma = (r * 299 + g * 587 + b * 114) / 1000;
				ImU32 tc = luma > 140 ? IM_COL32(20, 20, 20, 255) : IM_COL32(245, 245, 245, 255);

				char buf[64];
				sprintf(buf, "%s (%d)", n.name.c_str(), n.samples);
				ImVec2 ts = ImGui::CalcTextSize(buf);
				if (ts.x <= cw - 8.0f) {
					dl->AddText(ImVec2(x0 + 4.0f, y0 + (boxH - ImGui::GetTextLineHeight()) * 0.5f), tc, buf);
				}
			}

			if (mouse.x >= x0 && mouse.x <= x1 && mouse.y >= y0 && mouse.y <= y1) {
				hoverValid = true;
				hoverMin = ImVec2(x0, y0);
				hoverMax = ImVec2(x1, y1);
				hoverName = n.name;
				hoverSamples = n.samples;
				hoverSelf = n.selfSamples;
			}
		}

		if (!n.children.empty() && cwTrue >= 1.0) {
			drawFlameTree(n.children, x0, y + rowPitch, cw, boxH, rowPitch, n.samples);
		}
		cx += cwTrue;
	}
}

int App::measureFlameDepth(const std::vector<DbgFlameNode>& v, int d, int& maxDepth) {
	int totalSamples = 0;
	maxDepth = d;
	for (const DbgFlameNode& n : v) {
		totalSamples += n.samples;
		if (d > maxDepth) maxDepth = d;
		if (!n.children.empty()) {
			int childMax = d;
			measureFlameDepth(n.children, d + 1, childMax);
			if (childMax > maxDepth) maxDepth = childMax;
		}
	}
	return totalSamples;
}

void App::drawFlameGraphTab() {
	ImGui::BeginChild("##flamechild");

	ImVec2 origin = ImGui::GetCursorScreenPos();
	ImVec2 avail = ImGui::GetContentRegionAvail();
	if (flameNodes.empty() || avail.x < 8.0f || avail.y < 8.0f) {
		ImGui::TextDisabled("No samples collected.");
		ImGui::EndChild();
		return;
	}

	int maxDepth = 1;
	int totalSamples = measureFlameDepth(flameNodes, 1, maxDepth);
	if (totalSamples <= 0) {
		ImGui::TextDisabled("No samples collected.");
		ImGui::EndChild();
		return;
	}

	const float margin = 8.0f;
	const float rowGap = 2.0f;
	float yOff = origin.y + margin;
	float depthH = maxDepth > 0 ? (avail.y - margin * 2.0f) / (float)(maxDepth + 1) : 24.0f;
	if (depthH < 14.0f) depthH = 14.0f;

	hoverValid = false;
	drawFlameTree(flameNodes, origin.x + margin, yOff, avail.x - margin * 2.0f, depthH - rowGap, depthH, totalSamples);

	if (hoverValid) {
		ImDrawList* dl = ImGui::GetWindowDrawList();
		dl->AddRect(hoverMin, hoverMax, IM_COL32(255, 255, 255, 255), 0.0f, 0, 2.0f);
		char buf[256];
		sprintf(buf, "%s: %d samples (%.1f%% total, %.1f%% self)", hoverName.c_str(), hoverSamples,
			100.0 * (double)hoverSamples / (double)totalSamples,
			100.0 * (double)hoverSelf / (double)totalSamples);
		ImGui::SetTooltip("%s", buf);
	}

	ImGui::EndChild();
}
