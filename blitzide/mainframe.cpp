#include "stdafx.h"
#include "resource.h"
#include "mainframe.h"
#include "about.h"
#include "blitzide.h"
#include "libs.h"
#include "update.h"
#include "../procutil.h"

#include <mmsystem.h>

#include <fstream>
#include <sstream>
#include <cstring>

IMPLEMENT_DYNAMIC(MainFrame, CFrameWnd)
BEGIN_MESSAGE_MAP(MainFrame, CFrameWnd)
	ON_WM_CREATE()
	ON_WM_DROPFILES()
	ON_WM_CLOSE()
	ON_WM_DESTROY()
	ON_WM_ERASEBKGND()
	ON_WM_SIZE()
	ON_WM_ACTIVATE()

	ON_COMMAND(ID_NEW, fileNew)
	ON_COMMAND(ID_OPEN, fileOpen)
	ON_COMMAND(ID_SAVE, fileSave)
	ON_COMMAND(ID_SAVEAS, fileSaveAs)
	ON_COMMAND(ID_SAVEALL, fileSaveAll)
	ON_COMMAND(ID_PRINT, filePrint)
	ON_COMMAND(ID_CLOSE, fileClose)
	ON_COMMAND(ID_CLOSEALL, fileCloseAll)
	ON_COMMAND(ID_EXIT, fileExit)
	ON_COMMAND_RANGE(333, 343, fileRecent)
	ON_COMMAND(ID_CUT, editCut)
	ON_COMMAND(ID_COPY, editCopy)
	ON_COMMAND(ID_PASTE, editPaste)
	ON_COMMAND(ID_SELECTALL, editSelectAll)
	ON_COMMAND(ID_FIND, editFind)
	ON_COMMAND(ID_FINDNEXT, editFindNext)
	ON_COMMAND(ID_REPLACE, editReplace)
	ON_COMMAND(ID_CTRLTAB, ctrlTab)
	ON_COMMAND(ID_CTRLSHIFTTAB, ctrlShiftTab)
	ON_COMMAND(ID_ESCAPE, escape)
	ON_COMMAND(ID_BACKUP, switchBackup)
	ON_COMMAND(ID_QUICKHELP, quick_Help)

	ON_COMMAND(ID_EXECUTE, programExecute)
	ON_COMMAND(ID_REEXECUTE, programReExecute)
	ON_COMMAND(ID_COMPILE, programCompile)
	ON_COMMAND(ID_PUBLISH, programPublish)
	ON_COMMAND(ID_COMMANDLINE, programCommandLine)
	ON_COMMAND(ID_PROGRAM_PREPROCESSOR, programPreprocess)
	ON_COMMAND(ID_DEBUG, programDebug)
	ON_COMMAND(ID_NLAA, programNoLAA)

	ON_COMMAND(ID_HOME, helpHome)
	ON_COMMAND(ID_BACK, helpBack)
	ON_COMMAND(ID_FORWARD, helpForward)
	ON_COMMAND(ID_ABOUT, helpAbout)

	ON_UPDATE_COMMAND_UI(ID_NEW, updateCmdUI)
	ON_UPDATE_COMMAND_UI(ID_OPEN, updateCmdUI)
	ON_UPDATE_COMMAND_UI(ID_SAVE, updateCmdUI)
	ON_UPDATE_COMMAND_UI(ID_SAVEAS, updateCmdUI)
	ON_UPDATE_COMMAND_UI(ID_SAVEALL, updateCmdUI)
	ON_UPDATE_COMMAND_UI(ID_PRINT, updateCmdUI)
	ON_UPDATE_COMMAND_UI(ID_CLOSE, updateCmdUI)
	ON_UPDATE_COMMAND_UI(ID_CLOSEALL, updateCmdUI)
	ON_UPDATE_COMMAND_UI(ID_CUT, updateCmdUI)
	ON_UPDATE_COMMAND_UI(ID_COPY, updateCmdUI)
	ON_UPDATE_COMMAND_UI(ID_PASTE, updateCmdUI)
	ON_UPDATE_COMMAND_UI(ID_SELECTALL, updateCmdUI)
	ON_UPDATE_COMMAND_UI(ID_FIND, updateCmdUI)
	ON_UPDATE_COMMAND_UI(ID_FINDNEXT, updateCmdUI)
	ON_UPDATE_COMMAND_UI(ID_REPLACE, updateCmdUI)
	ON_UPDATE_COMMAND_UI(ID_EXECUTE, updateCmdUI)
	ON_UPDATE_COMMAND_UI(ID_REEXECUTE, updateCmdUI)
	ON_UPDATE_COMMAND_UI(ID_COMPILE, updateCmdUI)
	ON_UPDATE_COMMAND_UI(ID_PUBLISH, updateCmdUI)
	ON_UPDATE_COMMAND_UI(ID_COMMANDLINE, updateCmdUI)
	ON_UPDATE_COMMAND_UI(ID_PROGRAM_PREPROCESSOR, updateCmdUI)
	ON_UPDATE_COMMAND_UI(ID_DEBUG, updateCmdUI)
	ON_UPDATE_COMMAND_UI(ID_NLAA, updateCmdUI)
	ON_UPDATE_COMMAND_UI(ID_HOME, updateCmdUI)
	ON_UPDATE_COMMAND_UI(ID_BACK, updateCmdUI)
	ON_UPDATE_COMMAND_UI(ID_FORWARD, updateCmdUI)
	ON_UPDATE_COMMAND_UI(ID_ESCAPE, updateCmdUI)
	ON_UPDATE_COMMAND_UI(ID_BACKUP, updateCmdUI)
	ON_UPDATE_COMMAND_UI(ID_QUICKHELP, updateCmdUI)
	ON_UPDATE_COMMAND_UI_RANGE(333, 343, updateCmdUIRange)
END_MESSAGE_MAP()

enum {
	TAB_INVALID, TAB_EDITOR, TAB_HTMLHELP, TAB_DEBUGLOG
};

static std::string getFile(const std::string& f) {
	int n;
	std::string t = f;
	n = t.rfind('/'); if (n != std::string::npos) t = t.substr(n + 1);
	n = t.rfind('\\'); if (n != std::string::npos) t = t.substr(n + 1);
	return t;
}

static std::string getPath(const std::string& f) {
	int n;
	std::string t = f;
	n = t.rfind('/'); if (n != std::string::npos) t = t.substr(0, n);
	n = t.rfind('\\'); if (n != std::string::npos) t = t.substr(0, n);
	return t;
}

static bool ApplyIconToExe(const std::string& exePath, const std::string& icoPath) {

#pragma pack(push, 2)
	typedef struct {
		WORD idReserved;
		WORD idType;
		WORD idCount;
	} ICONDIR;

	typedef struct {
		BYTE bWidth;
		BYTE bHeight;
		BYTE bColorCount;
		BYTE bReserved;
		WORD wPlanes;
		WORD wBitCount;
		DWORD dwBytesInRes;
		DWORD dwImageOffset;
	} ICONDIRENTRY;

	typedef struct {
		WORD idReserved;
		WORD idType;
		WORD idCount;
	} GRPICONDIR;

	typedef struct {
		BYTE  bWidth;
		BYTE  bHeight;
		BYTE  bColorCount;
		BYTE  bReserved;
		WORD  wPlanes;
		WORD  wBitCount;
		DWORD dwBytesInRes;
		WORD  nID;
	} GRPICONDIRENTRY;
#pragma pack(pop)

	HANDLE hFile = CreateFileA(icoPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE) return false;

	ICONDIR dir;
	DWORD bytesRead;
	if (!ReadFile(hFile, &dir, sizeof(dir), &bytesRead, NULL) || bytesRead != sizeof(dir)) {
		CloseHandle(hFile);
		return false;
	}
	if (dir.idReserved != 0 || dir.idType != 1 || dir.idCount == 0) {
		CloseHandle(hFile);
		return false;
	}

	std::vector<ICONDIRENTRY> entries(dir.idCount);
	DWORD entrySize = sizeof(ICONDIRENTRY) * dir.idCount;
	if (!ReadFile(hFile, entries.data(), entrySize, &bytesRead, NULL) || bytesRead != entrySize) {
		CloseHandle(hFile);
		return false;
	}

	struct ImageData {
		std::vector<BYTE> data;
	};

	std::vector<ImageData> images(dir.idCount);
	for (WORD i = 0; i < dir.idCount; ++i) {
		if (SetFilePointer(hFile, entries[i].dwImageOffset, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER) {
			CloseHandle(hFile);
			return false;
		}

		images[i].data.resize(entries[i].dwBytesInRes);
		if (!ReadFile(hFile, images[i].data.data(), entries[i].dwBytesInRes, &bytesRead, NULL) || bytesRead != entries[i].dwBytesInRes) {
			CloseHandle(hFile);
			return false;
		}
	}
	CloseHandle(hFile);

	HANDLE hUpdate = BeginUpdateResourceA(exePath.c_str(), FALSE);
	if (!hUpdate) return false;

	bool success = true;

	for (WORD i = 0; i < dir.idCount; ++i) {
		WORD iconId = i + 1;
		UpdateResourceA(hUpdate, RT_ICON, MAKEINTRESOURCEA(iconId), MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED), images[i].data.data(), (DWORD)images[i].data.size());
	}

	if (success)
	{
		GRPICONDIR grpDir;

		grpDir.idReserved = 0;
		grpDir.idType = 1;
		grpDir.idCount = dir.idCount;

		std::vector<BYTE> groupData(sizeof(GRPICONDIR) + sizeof(GRPICONDIRENTRY) * dir.idCount);

		memcpy(groupData.data(), &grpDir, sizeof(GRPICONDIR));

		GRPICONDIRENTRY* grpEntries = (GRPICONDIRENTRY*) (groupData.data() + sizeof(GRPICONDIR));

		for (WORD i = 0; i < dir.idCount; ++i)
		{
			grpEntries[i].bWidth = entries[i].bWidth;

			grpEntries[i].bHeight = entries[i].bHeight;

			grpEntries[i].bColorCount = entries[i].bColorCount;

			grpEntries[i].bReserved = entries[i].bReserved;

			grpEntries[i].wPlanes = entries[i].wPlanes;

			grpEntries[i].wBitCount = entries[i].wBitCount;

			grpEntries[i].dwBytesInRes = entries[i].dwBytesInRes;

			grpEntries[i].nID = i + 1;
		}

		if (!UpdateResourceA(hUpdate, RT_GROUP_ICON, MAKEINTRESOURCEA(1), MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL), groupData.data(), (DWORD)groupData.size()))
		{
			success = false;
		}
	}

	if (!EndUpdateResourceA(hUpdate, !success)) {
		return false;
	}

	return success;
}

MainFrame::MainFrame() :exit_flag(false) {
}

int MainFrame::OnCreate(LPCREATESTRUCT lpCreateStruct) {
	CFrameWnd::OnCreate(lpCreateStruct);

	this->DragAcceptFiles(TRUE);

	HICON hIcon = LoadIcon(AfxGetInstanceHandle(), MAKEINTRESOURCE(IDI_MAIN_ICON));
	SetIcon(hIcon, FALSE);

	static HBITMAP toolbmp;
	static SIZE imgsz, butsz;
	static UINT toolbuts[] = {
		ID_NEW,ID_OPEN,ID_SAVE,ID_CLOSE,ID_SEPARATOR,
		ID_CUT,ID_COPY,ID_PASTE,ID_SEPARATOR,
		ID_FIND,ID_SEPARATOR,
		ID_EXECUTE,ID_SEPARATOR,
		ID_HOME,ID_BACK,ID_FORWARD };
	static int toolcnt = sizeof(toolbuts) / sizeof(UINT);

	if (!toolbmp) {
		BITMAP bm;
		std::string t = prefs.homeDir + "/cfg/ide_toolbar.bmp";
		toolbmp = (HBITMAP)LoadImage(0, t.c_str(), IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE | LR_LOADMAP3DCOLORS);
		if (!toolbmp)
		{
			AfxMessageBox("Toolbar failed to load!");
			ExitProcess(0);
		}
		GetObject(toolbmp, sizeof(bm), &bm);
		int n = 0;
		for (int k = 0; k < toolcnt; ++k) if (toolbuts[k] != ID_SEPARATOR) ++n;
		imgsz.cx = bm.bmWidth / n; imgsz.cy = bm.bmHeight;
		butsz.cx = imgsz.cx + 7; butsz.cy = imgsz.cy + 6;
	}
	toolBar.CreateEx(this, TBSTYLE_FLAT, WS_CHILD | WS_VISIBLE | CBRS_TOP | CBRS_TOOLTIPS);
	toolBar.SetBitmap(toolbmp);
	toolBar.SetSizes(butsz, imgsz);
	toolBar.SetButtons(toolbuts, toolcnt);

	int style;
	style = WS_CHILD | WS_VISIBLE | CBRS_ALIGN_BOTTOM;
	statusBar.CreateEx(this, 0, style);
	UINT IDS[] = { ID_STATUSTEXT,ID_COLROWTEXT };
	statusBar.SetIndicators(IDS, 2);
	statusBar.SetPaneInfo(0, ID_STATUSTEXT, SBPS_NOBORDERS | SBPS_STRETCH, 0);
	statusBar.SetPaneInfo(1, ID_COLROWTEXT, 0, 128);
	statusBar.SetPaneText(0, ""); statusBar.SetPaneText(1, "");

	tabber.Create(WS_VISIBLE | WS_CHILD | TCS_HOTTRACK, CRect(0, 0, 0, 0), this, 1);
	tabber.SetFont(&prefs.tabsFont);
	tabber.setListener(this);

	prefs.win_notoolbar = !prefs.win_notoolbar;
	escape();

	//create recent file list
	CMenu menu;
	menu.CreatePopupMenu();
	for (int k = 0; k < prefs.recentFiles.size(); ++k) {
		menu.InsertMenu(k, MF_BYPOSITION | MF_ENABLED, 333 + k, prefs.recentFiles[k].c_str());
	}
	CMenu* file = GetMenu()->GetSubMenu(0);
	file->InsertMenu(12, MF_BYPOSITION | MF_ENABLED | MF_POPUP, (UINT)menu.m_hMenu, "&Recent Files");
	menu.Detach();

	helpHome();

	trackmem(true);

	if (blitzIDE.m_lpCmdLine[0]) {
		std::string t = std::string(blitzIDE.m_lpCmdLine);
		if (t[0] == '\"') t = t.substr(1, t.size() - 2);
		open(t);
	}
	else {
		SetCurrentDirectory((prefs.homeDir + "/samples").c_str());
	}

	checkForUpdate();

	return 0;
}

void MainFrame::OnDropFiles(HDROP hDropInfo) {
	UINT nFiles = DragQueryFile(hDropInfo, 0xFFFFFFFF, NULL, 0);
	for (UINT i = 0; i < nFiles; ++i)
	{
		UINT len = DragQueryFile(hDropInfo, i, NULL, 0);
		if (!len) continue;
		std::vector<TCHAR> buf(len + 1, 0);
		if (DragQueryFile(hDropInfo, i, buf.data(), len + 1) != len) continue;
		std::string fileName(buf.data(), len);
		std::filesystem::path extension = std::filesystem::path(fileName).extension();
		if (extension == ".htm" || extension == ".html")
		{
			HtmlHelp* h = findHelp();
			h->Navigate(fileName.c_str());
		} else if (extension == ".bb" || extension == ".txt")
			open(fileName);
		else
			ShellExecute(nullptr, "open", fileName.c_str(), "", "", SW_SHOW);
	}
}

void MainFrame::OnDestroy() {
	trackmem(false);
	WINDOWPLACEMENT wp = { sizeof(wp) };
	if (GetWindowPlacement(&wp)) {
		prefs.win_rect = wp.rcNormalPosition;
		if (wp.showCmd == SW_SHOWMAXIMIZED) {
			prefs.win_maximized = true;
		}
		else {
			prefs.win_maximized = false;
		}
	}
}

void MainFrame::setTitle(const std::string& s)
{
	SetWindowText(("BlitzX3D - " + s).c_str());
	return;
}

void MainFrame::OnClose() {
	bool exit_flag = true;
	for (int k = tabber.size() - 1; k >= 0; --k) {
		exit_flag = close(k);
		if (!exit_flag) break;
	}
	if (exit_flag) DestroyWindow();
}

BOOL MainFrame::OnEraseBkgnd(CDC* dc) {
	return true;
}

void MainFrame::OnSize(UINT type, int sw, int sh) {
	CFrameWnd::OnSize(type, sw, sh);

	CRect r, t; GetClientRect(&r);
	int x = r.left, y = r.top, w = r.Width(), h = r.Height();

	if (!prefs.win_notoolbar) {
		statusBar.GetWindowRect(&t); h -= t.Height();
		toolBar.GetWindowRect(&t); y += t.Height(); h -= t.Height();
	}

	tabber.MoveWindow(x, y, w, h);
}

static const char* bbFilter =

"Blitz Basic files (.bb)|*.bb|"

"All files|*.*||";

Editor* MainFrame::getEditor() {
	return getEditor(tabber.getCurrent());
}

Editor* MainFrame::getEditor(int n) {
	std::map<CWnd*, Editor*>::iterator it = editors.find(tabber.getTabWnd(n));
	return it == editors.end() ? 0 : it->second;
}

HtmlHelp* MainFrame::getHelp(int n) {
	std::map<CWnd*, HtmlHelp*>::iterator it = helps.find(tabber.getTabWnd(n));
	return it == helps.end() ? 0 : it->second;
}

HtmlHelp* MainFrame::getHelp() {
	return getHelp(tabber.getCurrent());
}

HtmlHelp* MainFrame::findHelp() {
	int n;
	HtmlHelp* h;
	for (n = 0; n < tabber.size(); ++n) {
		if (h = getHelp(n)) break;
	}
	if (n == tabber.size()) {
		h = new HtmlHelp(this);
		h->Create(0, "Help", WS_CHILD | WS_BORDER, CRect(0, 0, 0, 0), &tabber, 1);
		helps[h] = h;
		tabber.insert(n, h, "Help");
	}
	tabber.setCurrent(n);
	return h;
}

void MainFrame::cursorMoved(Editor* editor) {
	if (editor != getEditor()) return;
	int row, col;
	editor->getCursor(&row, &col);
	char mod = editor->getModified() ? '*' : ' ';
	char str[64]; sprintf(str, "Row: %i Col: %i %c", row, col, mod);
	statusBar.SetPaneText(1, str);
}

void MainFrame::currentSet(Tabber* tabber, int index) {
	if (Editor* e = getEditor()) {
		std::string t = e->getName();
		if (!t.size()) t = "<untitled>";
		setTitle(t);
		cursorMoved(e);
	}
	else if (HtmlHelp* h = getHelp()) {
		setTitle(h->getTitle());
		statusBar.SetPaneText(1, "");
	}
	else {
		setTitle("no file");
		statusBar.SetPaneText(1, "");
	}
}

void MainFrame::helpOpen(HtmlHelp* help, const std::string& file) {
	open(file);
}

void MainFrame::helpTitleChange(HtmlHelp* help, const std::string& title) {
	if (HtmlHelp* h = getHelp()) setTitle(h->getTitle());
}

void MainFrame::insertRecent(const std::string& file) {
	//check recent files
	if (CMenu* list = GetMenu()->GetSubMenu(0)->GetSubMenu(12)) {
		std::vector<std::string>::iterator it;
		std::vector<std::string>& f = prefs.recentFiles;
		for (it = f.begin(); it != f.end(); ++it) {
			if (tolower(*it) == tolower(file)) break;
		}
		if (it != f.end()) {
			//move to top
			std::string t = *it;
			f.erase(it);
			f.insert(f.begin(), t);
		}
		else {
			//insert
			if (f.size() == 10) {
				f.pop_back();
				list->RemoveMenu(9, MF_BYPOSITION);
			}
			f.insert(f.begin(), file);
			list->InsertMenu(0, MF_BYPOSITION | MF_ENABLED, 333, file.c_str());
		}
		//renumber menu items
		for (int k = 0; k < f.size(); ++k) {
			list->ModifyMenu(k, MF_BYPOSITION | MF_ENABLED, 333 + k, f[k].c_str());
		}
	}
}

void MainFrame::newed(const std::string& t) {

	CRect r(0, 0, 0, 0);

	Editor* e = new Editor(this);
	e->Create(0, "Editor", WS_CHILD, r, &tabber, 1);
	e->setName(t);
	editors[e] = e;
	int n = tabber.size();
	std::string s = t.size() ? getFile(t) : "<untitled>";
	tabber.insert(n, e, s.c_str());
	tabber.setCurrent(n);
}

bool MainFrame::open(const std::string& f) {

	std::string file = f, filter = "*.bb";

	int n = f.find("*.");
	if (n != std::string::npos) {
		file = f.substr(0, n);
		filter = f.substr(n);
	}

	int attr = GetFileAttributes(file.c_str()); if (attr == -1) attr = 0;
	if (!file.size() || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
		CFileDialog fd(true, "bb", filter.c_str(), OFN_NOCHANGEDIR | OFN_FILEMUSTEXIST, bbFilter);
		char* i_dir = strdup(file.c_str());
		fd.m_ofn.lpstrInitialDir = i_dir;
		fd.m_ofn.lpstrTitle = "Open Blitz Basic File...";
		int nn = fd.DoModal(); 
		free(i_dir);
		if (nn == IDCANCEL) return false;
		file = fd.GetPathName();
	}
	else
	{
		std::error_code ec;
		std::filesystem::path abs = std::filesystem::absolute(file, ec);
		if (!ec) file = abs.lexically_normal().string();
		else file = f;
	}

	//is file already open?
	int k;
	for (k = 0; k < tabber.size(); ++k) {
		if (Editor* e = getEditor(k)) {
			if (tolower(e->getName()) == tolower(file)) {
				tabber.setCurrent(k);
				return true;
			}
		}
	}
	//open new file
	std::ifstream in(file.c_str());
	if (!in.good()) {
		std::string e = "Error reading file \"" + f + "\"";
		AfxMessageBox(e.c_str(), MB_ICONWARNING);
		return false;
	}
	newed(file);
	tabber.UpdateWindow();
	Editor* e = getEditor();
	e->setText(in);
	e->setName(file);
	e->setModified(false);
	insertRecent(file);
	cursorMoved(e);
	return true;
}

bool MainFrame::close(int n) {
	if (Editor* e = getEditor(n)) {
		if (e->getModified()) {
			tabber.setCurrent(n);
			std::string t = "File " + e->getName() + " has been modified!\nSave changes before closing?";
			int rt = AfxMessageBox(t.c_str(), MB_YESNOCANCEL | MB_ICONWARNING);
			if (rt == IDYES) return save(n);
			if (rt == IDCANCEL) return false;
		}
		tabber.remove(n);
		e->DestroyWindow();
		editors.erase(e);
		delete e;
	}
	else if (HtmlHelp* h = getHelp(n)) {
	}
	return true;
}

bool MainFrame::save(int n) {
	Editor* e = getEditor(n);
	if (!e) return true;
	std::string t = e->getName();
	if (!t.size()) {
		tabber.setCurrent(n);
		int df = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER | OFN_OVERWRITEPROMPT;
		CFileDialog fd(false, "bb", "*.bb", df, bbFilter);
		fd.m_ofn.lpstrTitle = "Save Blitz Basic program as...";
		if (fd.DoModal() == IDCANCEL) return false;
		t = fd.GetPathName();
		tabber.setTabText(n, getFile(t));
		e->setName(t);
		insertRecent(t);
	}
	//Do backups!
	if (!prefs.noBackup) {
		if (prefs.edit_backup) {
			for (int k = prefs.edit_backup; k > 1; --k) {
				CopyFile((t + "_bak" + itoa(k - 1)).c_str(), (t + "_bak" + itoa(k)).c_str(), false);
			}
			CopyFile(t.c_str(), (t + "_bak1").c_str(), false);
		}
	}
	std::ofstream out(t.c_str(), std::ios_base::binary | std::ios_base::out | std::ios_base::trunc);
	if (!out.good())
	{
		std::string e = "Error writing file \"" + t + "\"";
		AfxMessageBox(e.c_str(), MB_ICONERROR);
		return false;
	}
	e->getText(out);
	out.close();
	e->setModified(false);
	cursorMoved(e);
	return true;
}

void MainFrame::fileNew() {
	newed("");
}

void MainFrame::fileOpen() {
	open("");
}

void MainFrame::fileSave() {
	save(tabber.getCurrent());
}

void MainFrame::fileSaveAs() {
	if (Editor* e = getEditor()) {
		std::string t = e->getName();
		e->setName("");
		if (!save(tabber.getCurrent())) e->setName(t);
		currentSet(&tabber, tabber.getCurrent());
	}
}

void MainFrame::fileSaveAll() {
	for (int k = tabber.size() - 1; k >= 0; --k) {
		if (!save(k)) return;
	}
}

void MainFrame::filePrint() {
	if (Editor* e = getEditor()) e->print();
}

void MainFrame::fileClose() {
	close(tabber.getCurrent());
}

void MainFrame::fileCloseAll() {
	for (int k = tabber.size() - 1; k >= 0; --k) {
		if (!close(k)) return;
	}
}

void MainFrame::fileExit() {
	PostMessage(WM_CLOSE);
}

void MainFrame::fileRecent(UINT id) {
	id -= 333;
	if (id < 0 || id >= prefs.recentFiles.size()) return;
	open(prefs.recentFiles[id]);
}

void MainFrame::editCut() {
	if (Editor* e = getEditor()) e->cut();
}

void MainFrame::editCopy() {
	if (Editor* e = getEditor()) e->copy();
}

void MainFrame::editPaste() {
	if (Editor* e = getEditor()) e->paste();
}

void MainFrame::editSelectAll() {
	if (Editor* e = getEditor()) e->selectAll();
}

void MainFrame::editFind() {
	if (Editor* e = getEditor()) e->find();
}

void MainFrame::editFindNext() {
	if (Editor* e = getEditor()) e->findNext(true);
}

void MainFrame::editReplace() {
	if (Editor* e = getEditor()) e->replace();
}

static HANDLE startProc(const std::vector<std::string>& args) {
	HANDLE rd, wr;

	SECURITY_ATTRIBUTES sa = { sizeof(sa),0,true };

	if (CreatePipe(&rd, &wr, &sa, 0)) {
		STARTUPINFO si = { sizeof(si) };
		si.dwFlags = STARTF_USESTDHANDLES;
		si.hStdOutput = si.hStdError = wr;
		PROCESS_INFORMATION pi = { 0 };
		std::string cmdline = buildWindowsCommandLine(args);
		std::vector<char> mutableCmd(cmdline.begin(), cmdline.end());
		mutableCmd.push_back('\0');
		if (CreateProcess(0, mutableCmd.data(), 0, 0, true, DETACHED_PROCESS, 0, 0, &si, &pi)) {
			CloseHandle(pi.hProcess);
			CloseHandle(pi.hThread);
			CloseHandle(wr);
			return rd;
		}
		CloseHandle(rd);
		CloseHandle(wr);
	}
	return 0;
}

class PDialog : public CDialog {
public:
	void OnOk() {}
	void OnCancel() {}
};

void MainFrame::compile(const std::vector<std::string>& args) {

	CDialog compiling;
	compiling.Create(IDD_COMPILING);

	CProgressCtrl* cp = (CProgressCtrl*)compiling.GetDlgItem(IDC_COMPILEPROGRESS);
	cp->SetStep(20);

	putenv("blitzide=1");

	HANDLE rd = startProc(args);

	if (!rd) {
		putenv("blitzide");
		compiling.DestroyWindow();
		AfxMessageBox("Error launching compiler", MB_ICONWARNING | MB_OK);
		return;
	}

	std::string line, err;

	//OK....here we go!
	for (;;) {
		char buff;
		unsigned long sz;
		int n = ReadFile(rd, &buff, 1, &sz, 0);

		if (n && !sz) break;	//EOF!
		if (!n && GetLastError() == ERROR_BROKEN_PIPE) break;	//PROC END!
		if (!n) { err = "Internal Error"; break; }

		if (buff == '\r') continue;
		if (buff != '\n') { line += buff; continue; }
		if (!line.size()) continue;

		if (line[0] == '\"') {
			err = line;
			int n = line.find("\"", 1);
			if (n == std::string::npos) break;
			if (++n == line.size() || line[n] != ':') break;
			std::string file = line.substr(1, n - 2); line = line.substr(n + 1);
			n = line.find(':'); if (!n || n == std::string::npos) break;
			int row1 = atoi(line.substr(0, n)); line = line.substr(n + 1);
			n = line.find(':'); if (!n || n == std::string::npos) break;
			int col1 = atoi(line.substr(0, n)); line = line.substr(n + 1);
			n = line.find(':'); if (!n || n == std::string::npos) break;
			int row2 = atoi(line.substr(0, n)); line = line.substr(n + 1);
			n = line.find(':'); if (!n || n == std::string::npos) break;
			int col2 = atoi(line.substr(0, n)); line = line.substr(n + 1);

			int pos = ((row1 - 1) << 16) | (col1 - 1);

			if (!open(file)) return;
			if (Editor* e = getEditor()) e->setCursor(pos);

			err = line;
			break;
		}
		else if (line.starts_with("[Message] ")) {
		}
		else if (line.find("...") != line.size() - 3) {
			err = line; break;
		}

		if (line.find("Executing") == 0) {
			//OK, we're running!
			break;
		}

		cp->StepIt();
		line = "";
	}

	compiling.DestroyWindow();

	CloseHandle(rd);
	putenv("blitzide");

	if (!err.size()) return;

	AfxMessageBox(err.c_str(), MB_ICONWARNING | MB_OK);
}

void MainFrame::build(bool exec, bool publish) {
	Editor* e = getEditor();
	if (!e) return;

	std::string src_file = e->getName();

	for (int k = tabber.size() - 1; k >= 0; --k) {
		if (Editor* e = getEditor(k)) {
			if (e->getModified() && e->getName().size()) {
				if (!save(k)) return;
			}
		}
	}

	std::vector<std::string> args;
	args.push_back(prefs.homeDir + "/bin/blitzcc");
	args.push_back("-q");

	if (prefs.prg_preprocess) args.push_back("-p");
	if (prefs.prg_debug) args.push_back("-d");
	if (prefs.prg_nolaa) args.push_back("-nlaa");

	if (publish) {
		std::string exe = src_file;
		if (exe.size()) {
			int n = exe.find('.');
			if (n != std::string::npos) exe = exe.substr(0, n);
		}
		else {
			exe = "untitled";
		}

		static const char* exeFilter = "Executable files (*.exe)|*.exe||";
		int t = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
		CFileDialog fd(false, "exe", exe.c_str(), t, exeFilter);
		fd.m_ofn.lpstrTitle = "Select executable filename";
		fd.m_ofn.lpstrInitialDir = "./";
		if (fd.DoModal() == IDCANCEL) return;

		std::string outExePath = (LPCTSTR)fd.GetPathName();
		args.push_back("-o");
		args.push_back(outExePath);

		std::string iconPath;
		CFileDialog iconDlg(TRUE, "ico", NULL, OFN_FILEMUSTEXIST | OFN_HIDEREADONLY, "Icon files (*.ico)|*.ico||");
		iconDlg.m_ofn.lpstrTitle = "Select an icon to embed (optional)";
		if (iconDlg.DoModal() == IDOK) {
			iconPath = iconDlg.GetPathName();
		}

		args.push_back(src_file);
		if (!prefs.cmd_line.empty()) {
			std::vector<std::string> extra = splitCommandLine(prefs.cmd_line);
			args.insert(args.end(), extra.begin(), extra.end());
		}
		compile(args);

		if (!iconPath.empty()) {
			if (!ApplyIconToExe(outExePath, iconPath)) {
				AfxMessageBox("Warning: Could not apply the selected icon to the executable.", MB_ICONWARNING);
			}
		}
		return;
	}
	else if (!exec) {
		args.push_back("-c");
	}

	std::string src = src_file;

	//if the file doesnt exist, create a temporary file.
	if (!src.size())
	{
		src = prefs.homeDir + "\\temp\\tmp.bb";
		checkAndCreateDir(prefs.homeDir + "\\temp");

		int om = std::ios_base::binary | std::ios_base::out | std::ios_base::trunc;
		std::ofstream out(src.c_str(), om);
		if (!out.good())
		{
			std::string e = "Error writing file \"" + src + "\"";
			AfxMessageBox(e.c_str(), MB_ICONERROR);
			return;
		}
		e->getText(out);
		out.close();
		e->setName(src);
	}
	else {
		prefs.prg_lastbuild = e->getName();
	}

	args.push_back(src);
	if (!prefs.cmd_line.empty()) {
		std::vector<std::string> extra = splitCommandLine(prefs.cmd_line);
		args.insert(args.end(), extra.begin(), extra.end());
	}
	compile(args);

	if (!src_file.size()) e->setName("");
}

void MainFrame::programExecute() {
	build(true, false);
}

void MainFrame::programReExecute() {
	if (prefs.prg_lastbuild.size() && open(prefs.prg_lastbuild)) {
		build(true, false);
	}
}

void MainFrame::programCompile() {
	build(false, false);
}

void MainFrame::programPublish() {
	Editor* e = getEditor(); if (!e) return;
	if (prefs.prg_debug) {
		std::string t =
			"You currently have the debugging feature enabled!\n\n"
			"This will result in slower executables - continue anyway?";
		if (MessageBox(t.c_str(), 0, MB_OKCANCEL) == IDCANCEL) return;
	}
	build(false, true);
}

struct CmdLineDialog : public CDialog {
	CmdLineDialog() :CDialog(IDD_COMMANDLINE) {
	}

	BOOL OnInitDialog() {
		CDialog::OnInitDialog();
		SetDlgItemText(IDC_CMDLINE, prefs.cmd_line.c_str());
		return TRUE;
	}

	void OnOK() {
		CString c_str;
		GetDlgItemText(IDC_CMDLINE, c_str);
		prefs.cmd_line = c_str;
		CDialog::OnOK();
	}
};

void MainFrame::programCommandLine() {
	CmdLineDialog d;
	d.DoModal();
}

void MainFrame::programPreprocess() {
	prefs.prg_preprocess = !prefs.prg_preprocess;
}

void MainFrame::programDebug() {
	prefs.prg_debug = !prefs.prg_debug;
}

void MainFrame::programNoLAA() {
	prefs.prg_nolaa = !prefs.prg_nolaa;
}

void MainFrame::helpHome() {
	HtmlHelp* h = findHelp();
	std::string t;
	t = "index.html";
	h->Navigate((prefs.homeDir + "/help/" + t).c_str());
}

void MainFrame::helpAutodoc() {
	HtmlHelp* h = findHelp();
	h->Navigate((prefs.homeDir + "/help/autodoc.html").c_str());
}

void MainFrame::helpBack() {
	if (HtmlHelp* h = findHelp()) h->GoBack();
}

void MainFrame::helpForward() {
	if (HtmlHelp* h = findHelp()) h->GoForward();
}

void MainFrame::helpAbout() {
	aboutBlitz();
}

void MainFrame::ctrlTab() {
	int n = tabber.getCurrent() + 1;
	if (n >= tabber.size()) n -= tabber.size();
	tabber.setCurrent(n);
}

void MainFrame::ctrlShiftTab() {
	int n = tabber.getCurrent() - 1;
	if (n < 0) n += tabber.size();
	tabber.setCurrent(n);
}

void MainFrame::escape() {
	if (!prefs.win_notoolbar) {
		toolBar.ShowWindow(SW_HIDE);
		statusBar.ShowWindow(SW_HIDE);
		prefs.win_notoolbar = true;
	}
	else {
		toolBar.ShowWindow(SW_SHOW);
		statusBar.ShowWindow(SW_SHOW);
		prefs.win_notoolbar = false;
	}
	RECT r; GetClientRect(&r);
	int w = r.right - r.left, h = r.bottom - r.top;
	int n = prefs.win_maximized ? SIZE_MAXIMIZED : SIZE_RESTORED;
	PostMessage(WM_SIZE, n, (h << 16) | w);
}

void MainFrame::switchBackup() {
	prefs.noBackup = !prefs.noBackup;
}

void MainFrame::updateCmdUIRange(CCmdUI* ui) {
	int n = ui->m_nID - 333;
	if (n >= 0 && n < prefs.recentFiles.size()) {
		ui->Enable(true);
	}
	else {
		ui->Enable(false);
	}
}

void MainFrame::updateCmdUI(CCmdUI* ui) {
	int k;

	Editor* e = getEditor();

	switch (ui->m_nID) {
	case ID_NEW:case ID_OPEN:case ID_HOME:
		ui->Enable(true);
		break;
	case ID_PROGRAM_PREPROCESSOR:
		ui->SetCheck(prefs.prg_preprocess); ui->Enable(true);
		break;
	case ID_DEBUG:
		ui->SetCheck(prefs.prg_debug); ui->Enable(true);
		break;
	case ID_NLAA:
		ui->SetCheck(prefs.prg_nolaa); ui->Enable(true);
		break;
	case ID_CLOSE:case ID_PRINT:
		ui->Enable(!!e);
		break;
	case ID_CLOSEALL:
		for (k = 0; k < tabber.size() && !getEditor(k); ++k) {}
		ui->Enable(k < tabber.size());
		break;
	case ID_CUT:case ID_COPY:
		if (!!e) {
			ui->Enable(e->canCutCopy());
		}
		else ui->Enable(false);
		break;
	case ID_PASTE:
		if (!!e) {
			ui->Enable(e->canPaste());
		}
		else ui->Enable(false);
		break;
	case ID_SELECTALL:case ID_QUICKHELP:
	case ID_FIND:case ID_FINDNEXT:case ID_REPLACE:
	case ID_EXECUTE:case ID_COMPILE:case ID_PUBLISH:
		ui->Enable(!!e);
		break;
	case ID_REEXECUTE:
		ui->Enable(prefs.prg_lastbuild.size());
		break;
	case ID_COMMANDLINE:
		ui->Enable(true);
		break;
	case ID_SAVE:case ID_SAVEAS:
		ui->Enable(!!e);
		break;
	case ID_SAVEALL:
		for (k = 0; k < tabber.size() && !getEditor(k); ++k) {}
		ui->Enable(k < tabber.size());
		break;
	case ID_BACK:case ID_FORWARD:
		ui->Enable(getHelp() != 0);
		break;
	case ID_ESCAPE:
		ui->SetCheck(!prefs.win_notoolbar);
		break;
	case ID_BACKUP:
		ui->SetCheck(!prefs.noBackup);
		break;
	default:
		ui->Enable(false);
	}
}

static std::string commandURL(const std::string& t) {

	static const char* dirs[] = {
		"help\\commands\\2d_commands\\",
		"help\\commands\\3d_commands\\",
		0
	};

	const char** dir_p = dirs;

	while (const char* dir = *dir_p++) {
		WIN32_FIND_DATA fd;
		std::string path = prefs.homeDir + "/" + dir + t + ".htm";
		HANDLE h = FindFirstFile(path.c_str(), &fd);
		if (h == INVALID_HANDLE_VALUE) continue;
		FindClose(h);
		return path;
	}
	return "";
}

void MainFrame::quick_Help() {
	if (Editor* e = getEditor()) {
		//look for keyword at cursor...
		std::string t = e->getKeyword(); if (!t.size()) return;
		if (t.substr(0, 6) == "Blitz_") t = t.substr(6, t.size());
		statusBar.SetPaneText(0, quickHelp(t).c_str());
		if (t != last_quick_help) {
			last_quick_help = t;
			return;
		}
		std::string url = commandURL(t);
		if (!url.size()) {
			std::string ex = "Unable to open help file for \"" + t + "\"";
			AfxMessageBox(ex.c_str(), MB_ICONWARNING);
			return;
		}
		if (HtmlHelp* h = findHelp()) {
			h->Navigate(url.c_str(), 0, 0);
		}
	}
}

void MainFrame::OnActivate(UINT state, CWnd* other, BOOL min) {
	CFrameWnd::OnActivate(state, other, min);

	if (Editor* e = getEditor()) {
		if (state != WA_ACTIVE && state != WA_CLICKACTIVE) return;
		e->SetFocus();
	}
}

void MainFrame::checkAndCreateDir(std::string dir)
{
	CreateDirectory(dir.c_str(), 0);
}