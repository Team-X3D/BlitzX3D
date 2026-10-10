#include "stdafx.h"
#include "mainframe.h"
#include "resource.h"
#include "debuggerapp.h"
#include "prefs.h"
#include "flamegraph.h"
#include "../MultiLang/MultiLang.h"

#include "../sdlruntime/sdlutf8.h"

#define WM_IDLEUPDATECMDUI  0x0363

enum {
	WM_STOP = WM_APP + 1, WM_RUN, WM_END
};

enum {
	STARTING, RUNNING, STOPPED, ENDING
};

IMPLEMENT_DYNAMIC(MainFrame, CFrameWnd)
BEGIN_MESSAGE_MAP(MainFrame, CFrameWnd)
	ON_WM_CREATE()
	ON_WM_SIZE()
	ON_WM_CLOSE()
	ON_WM_WINDOWPOSCHANGING()
	ON_WM_TIMER()
	ON_WM_DESTROY()
	ON_WM_KEYDOWN()

	ON_COMMAND(ID_STOP, cmdStop)
	ON_COMMAND(ID_RUN, cmdRun)
	ON_COMMAND(ID_STEPOVER, cmdStepOver)
	ON_COMMAND(ID_STEPINTO, cmdStepInto)
	ON_COMMAND(ID_STEPOUT, cmdStepOut)
	ON_COMMAND(ID_END, cmdEnd)
	ON_COMMAND(ID_PROFILE_TOGGLE, cmdProfileToggle)
	ON_COMMAND(ID_PROFILE_RESET, cmdProfileReset)

	ON_UPDATE_COMMAND_UI(ID_STOP, updateCmdUI)
	ON_UPDATE_COMMAND_UI(ID_RUN, updateCmdUI)
	ON_UPDATE_COMMAND_UI(ID_STEPOVER, updateCmdUI)
	ON_UPDATE_COMMAND_UI(ID_STEPINTO, updateCmdUI)
	ON_UPDATE_COMMAND_UI(ID_STEPOUT, updateCmdUI)
	ON_UPDATE_COMMAND_UI(ID_END, updateCmdUI)
	ON_CBN_SELCHANGE(1002, OnFilterSelChange)

END_MESSAGE_MAP()

#define PROFILER_TIMER_ID 1
#define PROFILER_TIMER_MS 250
#define PROFILER_CLEAR_HOTKEY VK_DELETE

MainFrame::MainFrame() :state(STARTING), step_level(-1), cur_pos(0), cur_file(0),
	last_obj_cnt(0), last_unrel_cnt(0), last_str_cnt(0), last_working_set_bytes(0) {
}

MainFrame::~MainFrame() {
	std::map<const char*, SourceFile*>::iterator it;
	for(it = files.begin(); it != files.end(); ++it) delete it->second;
}

int MainFrame::OnCreate(LPCREATESTRUCT lpCreateStruct) {
	CFrameWnd::OnCreate(lpCreateStruct);

	HICON hIcon = LoadIcon(AfxGetInstanceHandle(), MAKEINTRESOURCE(IDI_ICON1));
	SetIcon(hIcon, FALSE);

	prefs.open();

	std::string tb = prefs.homeDir + "/cfg/dbg_toolbar.bmp";

	//Toolbar
	HBITMAP toolbmp = (HBITMAP)LoadImage(
		0, tb.c_str(), IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE | LR_LOADMAP3DCOLORS);

	BITMAP bm;
	GetObject(toolbmp, sizeof(bm), &bm);

	int n = 0;
	UINT toolbuts[] = { ID_STOP,ID_RUN,ID_STEPOVER,ID_STEPINTO,ID_STEPOUT,ID_END };
	int toolcnt = sizeof(toolbuts) / sizeof(UINT);
	for(int k = 0; k < toolcnt; ++k) if(toolbuts[k] != ID_SEPARATOR) ++n;

	SIZE imgsz, butsz;
	imgsz.cx = bm.bmWidth / n; imgsz.cy = bm.bmHeight;
	butsz.cx = imgsz.cx + 7; butsz.cy = imgsz.cy + 6;

	toolBar.CreateEx(this, TBSTYLE_FLAT, WS_CHILD | WS_VISIBLE | CBRS_TOP | CBRS_TOOLTIPS);
	toolBar.SetBitmap(toolbmp);
	toolBar.SetSizes(butsz, imgsz);
	toolBar.SetButtons(toolbuts, toolcnt);

	//Filter
	m_filterCombo.Create(WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, CRect(0, 0, 110, 300), this, 1002);
	m_filterCombo.AddString("All");
	m_filterCombo.AddString("Info");
	m_filterCombo.AddString("Warnings");
	m_filterCombo.AddString("Errors");
	m_filterCombo.SetCurSel(0);
	m_currentFilter = 0;

	//Tabber
	tabber.Create(
		WS_VISIBLE | WS_CHILD |
		TCS_HOTTRACK,
		CRect(0, 0, 0, 0), this, 1);
	tabber.SetFont(&prefs.tabsFont);

	//Second tabber
	tabber2.Create(
		WS_VISIBLE | WS_CHILD |
		TCS_HOTTRACK,
		CRect(0, 0, 0, 0), this, 2);
	tabber2.SetFont(&prefs.tabsFont);

	//Debug Log
	debug_log.Create(
		WS_CHILD | WS_HSCROLL | WS_VSCROLL |
		ES_NOHIDESEL | ES_MULTILINE | ES_AUTOHSCROLL | ES_AUTOVSCROLL,
		CRect(0, 0, 0, 0), &tabber, 1);
	tabber.insert(0, &debug_log, "Debug log");

	//Profiler
	profiler_panel.Create(
		0, 0, WS_CHILD,
		CRect(0, 0, 0, 0), &tabber, 4, 0);
	tabber.insert(1, &profiler_panel, MultiLang::debugger_profiler);
	tabber.setCurrent(0);

	//Flame Graph
	flame_graph_panel.Create(NULL, NULL, WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), &tabber, 5);
	tabber.insert(2, &flame_graph_panel, "Flame Graph");

	//Debug trees
	locals_tree.Create(
		WS_VISIBLE | WS_CHILD |
		TVS_HASLINES | TVS_LINESATROOT | TVS_HASBUTTONS,
		CRect(0, 0, 0, 0), &tabber2, 3);

	globals_tree.Create(
		WS_VISIBLE | WS_CHILD |
		TVS_HASLINES | TVS_LINESATROOT | TVS_HASBUTTONS,
		CRect(0, 0, 0, 0), &tabber2, 3);

	consts_tree.Create(
		WS_VISIBLE | WS_CHILD |
		TVS_HASLINES | TVS_LINESATROOT | TVS_HASBUTTONS,
		CRect(0, 0, 0, 0), &tabber2, 3);

	tabber2.insert(0, &locals_tree, MultiLang::debugger_locals);
	tabber2.insert(1, &globals_tree, MultiLang::debugger_globals);
	tabber2.insert(2, &consts_tree, MultiLang::debugger_consts);
	tabber2.setCurrent(0);

	SetTimer(PROFILER_TIMER_ID, PROFILER_TIMER_MS, 0);

	setState(STARTING);

	return 0;
}

void MainFrame::OnDestroy() {
	KillTimer(PROFILER_TIMER_ID);
	CFrameWnd::OnDestroy();
}

void MainFrame::setState(int n) {
	state = n;
	SendMessageToDescendants(WM_IDLEUPDATECMDUI, (WPARAM)TRUE, 0, TRUE, TRUE);
	if(shouldRun()) {
		if(HWND app = ::FindWindow("Blitz Runtime Class", 0)) {
			::SetActiveWindow(app);
		}
	}
	else {
		SetActiveWindow();
	}
}

void MainFrame::OnClose() {
	cmdEnd();
}

void MainFrame::OnSize(UINT type, int sw, int sh) {
	CFrameWnd::OnSize(type, sw, sh);

	CRect r, t;
	GetClientRect(&r);

	int x = r.left;
	int y = r.top;
	int w = r.Width();
	int h = r.Height();

	toolBar.GetWindowRect(&t);
	y += t.Height();
	h -= t.Height();

	tabber.MoveWindow(x, y, w - 360, h);
	tabber2.MoveWindow(x + w - 360, y, 360, h);

	CRect rc;
	tabber.GetWindowRect(&rc);
	ScreenToClient(&rc);

	m_filterCombo.MoveWindow(rc.right - 120, rc.top + 30, 110, 250);

	m_filterCombo.ShowWindow(SW_SHOW);
	m_filterCombo.BringWindowToTop();
}

void MainFrame::setRuntime(void* mod, void* env) {
	consts_tree.reset((Environ*)env);
	globals_tree.reset((Module*)mod, (Environ*)env);
	locals_tree.reset((Environ*)env);
	profiler.reset();
	profiler.clearSamples();
	profiler_panel.clear();
	flame_graph_panel.setProfiler(&profiler);
	flame_graph_panel.refresh();
}

void MainFrame::showCurStmt() {
	if(!cur_file) return;

	SourceFile* t = sourceFile(cur_file);

	int row = (cur_pos >> 16) & 0xffff, col = cur_pos & 0xffff;
	t->highLight(row, col);

	globals_tree.refresh();
	locals_tree.refresh();
}

void MainFrame::debugRun() {
	setState(RUNNING);
}

void MainFrame::debugStop() {
	step_level = locals_tree.size();
	setState(STOPPED);
	showCurStmt();
}

bool MainFrame::debugStmt(int pos, const char* file) {
	cur_pos = pos;
	cur_file = file;

	if(shouldRun()) return true;

	::PostMessage(0, WM_STOP, 0, 0);
	return false;
}

void MainFrame::debugEnter(void* frame, void* env, const char* func) {
	profiler.enter(func);
	call_stack.push_back(func);

	locals_tree.pushFrame(frame, env, func);

	if(locals_tree.size() > 1) return;

	globals_tree.refresh();
	locals_tree.refresh();

	setState(RUNNING);
}

void MainFrame::debugLeave() {
	profiler.leave();
	if(!call_stack.empty()) call_stack.pop_back();

	locals_tree.popFrame();
}

std::string MainFrame::buildCrashReport(const char* msg)const {
	std::string s = msg ? msg : "";
	s += "\r\n";

	if(cur_file) {
		int row = (cur_pos >> 16) & 0xffff, col = cur_pos & 0xffff;
		s += "\r\nLocation: ";
		s += cur_file;
		s += " (line ";
		s += std::to_string(row + 1);
		s += ", col ";
		s += std::to_string(col + 1);
		s += ")\r\n";
	}

	if(!call_stack.empty()) {
		s += "\r\nCall stack (innermost first):\r\n";
		for(int i = (int)call_stack.size() - 1; i >= 0; --i) {
			s += "  ";
			s += call_stack[i];
			s += "\r\n";
		}
	}

	return s;
}

void MainFrame::debugMsg(const char* msg, bool serious) {
	if(serious) {
		std::string report = buildCrashReport(msg);
		::MessageBoxW(0, UTF8::convertToUtf16(report).c_str(), MultiLang::runtime_error, MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
		showCurStmt();
		profiler.resyncStack();
		call_stack.clear();
	}
	else {
		::MessageBoxW(0, UTF8::convertToUtf16(msg).c_str(), MultiLang::runtime_message, MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
	}
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

	AddLogEntry(severity, displayText);
}

void MainFrame::AddLogEntry(ELogSeverity severity, const std::string& text) {
	m_logEntries.push_back({ severity, text });
	RefreshLogDisplay();
}

void MainFrame::RefreshLogDisplay() {
	debug_log.SetSel(0, -1);
	debug_log.ReplaceSel("");

	CHARFORMAT cf = {};
	cf.cbSize = sizeof(cf);
	cf.dwMask = CFM_COLOR;

	for (const auto& entry : m_logEntries) {

		if (m_currentFilter == 1 && entry.severity != LOG_INFO) continue;
		if (m_currentFilter == 2 && entry.severity != LOG_WARNING) continue;
		if (m_currentFilter == 3 && entry.severity != LOG_ERROR) continue;

		switch (entry.severity) {
		case LOG_WARNING: cf.crTextColor = RGB(255, 200, 0); break;
		case LOG_ERROR:   cf.crTextColor = RGB(255, 0, 0); break;
		default:          cf.crTextColor = prefs.rgb_default; break;
		}

		debug_log.SetSel(-1, -1);
		debug_log.SetSelectionCharFormat(cf);
		debug_log.ReplaceSel(entry.text.c_str());
		debug_log.ReplaceSel("\r\n");
	}

	debug_log.SetSel(-1, -1);
	debug_log.SendMessage(EM_SCROLLCARET);

	m_filterCombo.BringWindowToTop();
}

void MainFrame::OnFilterSelChange() {
	m_currentFilter = m_filterCombo.GetCurSel();
	RefreshLogDisplay();
}

void MainFrame::debugSys(void* m) {
	if(!m) return;

	int tag = *(int*)m;
	if(tag == DBGSYS_MEMSTATS) {
		DbgSysMemStats* s = (DbgSysMemStats*)m;
		last_obj_cnt = s->objCnt;
		last_unrel_cnt = s->unrelObjCnt;
		last_str_cnt = s->stringCnt;
		last_working_set_bytes = s->workingSetBytes;
	}
}

void MainFrame::cmdStop() {
	::PostMessage(0, WM_STOP, 0, 0);
}

void MainFrame::cmdRun() {
	step_level = -1;
	::PostMessage(0, WM_RUN, 0, 0);
}

void MainFrame::cmdEnd() {
	::PostMessage(0, WM_END, 0, 0);
	setState(ENDING);
}

void MainFrame::cmdStepOver() {
	::PostMessage(0, WM_RUN, 0, 0);
}

void MainFrame::cmdStepInto() {
	step_level = locals_tree.size() + 1;
	::PostMessage(0, WM_RUN, 0, 0);
}

void MainFrame::cmdStepOut() {
	step_level = locals_tree.size() - 1;
	::PostMessage(0, WM_RUN, 0, 0);
}

SourceFile* MainFrame::sourceFile(const char* file) {
	if(!file) file = MultiLang::debugger_unknown;

	std::map<const char*, SourceFile*>::const_iterator it = files.find(file);

	if(it != files.end()) {
		tabber.setCurrent(file_tabs[file]);
		return it->second;
	}

	//crete new source file
	SourceFile* t = new SourceFile();

	it = files.insert(std::make_pair(file, t)).first;

	int tab = files.size() + 1;

	t->Create(
		WS_CHILD | WS_HSCROLL | WS_VSCROLL |
		ES_NOHIDESEL | ES_MULTILINE | ES_AUTOHSCROLL | ES_AUTOVSCROLL,
		CRect(0, 0, 0, 0), &tabber, 1);

	if(FILE* f = fopen(file, "rb")) {
		fseek(f, 0, SEEK_END);
		int sz = ftell(f);
		fseek(f, 0, SEEK_SET);
		char* buf = new char[sz + 1];
		fread(buf, sz, 1, f);
		buf[sz] = 0;
		t->ReplaceSel(buf);
		delete[] buf;
		fclose(f);
	}

	file_tabs.insert(std::make_pair(file, tab));

	if(const char* p = strrchr(file, '/')) file = p + 1;
	if(const char* p = strrchr(file, '\\')) file = p + 1;
	tabber.insert(tab, t, file);

	tabber.setCurrent(tab);

	return t;
}

void MainFrame::updateCmdUI(CCmdUI* ui) {
	if(state != RUNNING && state != STOPPED) {
		ui->Enable(false);
		return;
	}
	switch(ui->m_nID) {
		case ID_STOP:
			ui->Enable(shouldRun());
			break;
		case ID_RUN:
		case ID_STEPOVER:
		case ID_STEPINTO:
		case ID_STEPOUT:
			ui->Enable(!shouldRun());
			break;
		case ID_END:
			ui->Enable(true);
			break;
	}
}

void MainFrame::OnWindowPosChanging(WINDOWPOS* pos) {
	RECT rect;
	SystemParametersInfo(SPI_GETWORKAREA, 0, &rect, 0);

	pos->x = rect.left;
	pos->cx = rect.right - pos->x;
	pos->cy = rect.bottom - pos->y;
}

void MainFrame::OnTimer(UINT_PTR id) {
	if (id != PROFILER_TIMER_ID) return;
	if (state == RUNNING || state == STOPPED) {
		profiler.sampleStack();
		profiler_panel.refresh(profiler, last_obj_cnt, last_unrel_cnt, last_str_cnt, last_working_set_bytes);
		if (flame_graph_panel.IsWindowVisible()) {
			flame_graph_panel.refresh();
		}
	}
}

void MainFrame::OnKeyDown(UINT nChar, UINT nRepCnt, UINT nFlags) {
	if (nChar == PROFILER_CLEAR_HOTKEY) {
		cmdProfileReset();
	}
	CFrameWnd::OnKeyDown(nChar, nRepCnt, nFlags);
}

void MainFrame::cmdProfileToggle() {
	profiler.enabled = !profiler.enabled;
	if (profiler.enabled) {
		profiler.reset();
		profiler.clearSamples();
		flame_graph_panel.refresh();
	}
}

void MainFrame::cmdProfileReset() {
	profiler.reset();
	profiler.clearSamples();
	profiler_panel.clear();
	flame_graph_panel.refresh();
}

