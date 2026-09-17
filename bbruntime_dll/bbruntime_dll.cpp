#pragma warning( disable:4786 )

#include "bbruntime_dll.h"
#include "../debugger/debugger.h"

#include <map>
#include <eh.h>
#include <float.h>

#include "../bbruntime/bbruntime.h"

#include "../gxruntime/gxutf8.h"

#include "../MultiLang/MultiLang.h"
#include "../bbruntime/bbsys.h"

#include <cstring>
#include <cstdio>
#include <ctime>
#include <DbgHelp.h>
#pragma comment(lib, "Dbghelp.lib")
#pragma comment(lib, "Shell32.lib")
#include <shellapi.h>
#include <commctrl.h>
#include "../linker/cryptseed.h"

static void writeMiniDump(EXCEPTION_POINTERS* pExp) {
	char path[MAX_PATH];
	GetModuleFileNameA(NULL, path, MAX_PATH);
	char* slash = strrchr(path, '\\');
	if (slash) *slash = '\0';
	else path[0] = '\0';
	strcat_s(path, MAX_PATH, "\\blitz_crash.dmp");

	HANDLE hFile = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE) return;

	MINIDUMP_EXCEPTION_INFORMATION mdei;
	mdei.ThreadId = GetCurrentThreadId();
	mdei.ExceptionPointers = pExp;
	mdei.ClientPointers = FALSE;

	MINIDUMP_TYPE dumpType = (MINIDUMP_TYPE)(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithDataSegs);

	MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, dumpType, pExp ? &mdei : NULL, NULL, NULL);
	CloseHandle(hFile);
}

static void writeCrashLog(const char* fmt, ...) {
	char path[MAX_PATH];
	GetModuleFileNameA(NULL, path, MAX_PATH);
	char* slash = strrchr(path, '\\');
	if (slash) *slash = '\0';
	else path[0] = '\0';
	strcat_s(path, MAX_PATH, "\\blitz_crash.log");

	HANDLE hFile = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE) return;

	SetFilePointer(hFile, 0, NULL, FILE_END); // append

	char buffer[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);

	DWORD written;
	WriteFile(hFile, buffer, (DWORD)strlen(buffer), &written, NULL);
	WriteFile(hFile, "\r\n", 2, &written, NULL);
	CloseHandle(hFile);
}

class DummyDebugger : public Debugger {
public:
	virtual void debugRun() {}
	virtual void debugStop() {}// bbruntime_panic(0); }
	virtual bool debugStmt(int srcpos, const char* file) { return true; }
	virtual void debugEnter(void* frame, void* env, const char* func) {}
	virtual void debugLeave() {}
	virtual void debugLog(const char* msg) {}
	virtual void debugMsg(const char* e, bool serious) {
		if (serious) {
			std::string full = bbReleaseCrashReport(e);
			writeCrashLog("=== BLITZ RUNTIME ERROR ===");
			writeCrashLog("%s", full.c_str());
			writeCrashLog("---");
			MessageBoxW(gx_runtime->hwnd, UTF8::convertToUtf16(full).c_str(), MultiLang::runtime_error, MB_APPLMODAL);
		}
	}
	virtual void debugSys(void* msg) {}
	virtual void internalLog(const char* msg) {}
};

static HINSTANCE hinst;
static std::map<const char*, void*> syms;
std::map<const char*, void*>::iterator sym_it;
static gxRuntime* gx_runtime;
static void* module_pc = nullptr;

inline const char* getCharPtr(std::string str) {
	char* cha = new char[str.size() + 1];
	memcpy(cha, str.c_str(), str.size() + 1);
	const char* p = cha;
	return p;
}

inline std::string replace_all(const std::string& string, const std::string& pattern, const std::string& newpat) {
	std::string str = string;
	const uint32_t nsize = newpat.size();
	const uint32_t psize = pattern.size();

	for (uint32_t pos = str.find(pattern, 0); pos != std::string::npos; pos = str.find(pattern, pos + nsize))
	{
		str.replace(pos, psize, newpat);
	}
	return str;
}

void throw_mav() {
	if (ErrorMessagePool::memoryAccessViolation == 0) {
		RTEX(MultiLang::memory_access_violation);
	}
	else {
		std::string s = "";
		for (int i = 0; i < ErrorMessagePool::size; i++) {
			if (!ErrorMessagePool::memoryAccessViolation[i].empty()) {
				s = s + ErrorMessagePool::memoryAccessViolation[i] + "\n";
			}
		}
		if (ErrorMessagePool::hasMacro) {
			std::string caught = "unknown location";
			if (errorfunc && errorlog) {
				caught = std::format("{0}: {1}", errorfunc, errorlog);
			}
			s = replace_all(s, "_CaughtError_", caught);
			s = replace_all(s, "_AvailPhys_", to_string(gx_runtime->getAvailPhys()));
			s = replace_all(s, "_AvailVirtual_", to_string(gx_runtime->getAvailVirtual()));
		}
		RTEX(UTF8::convertToAnsi(s).c_str());
	}
}

static void rtSym(const char* sym, void* pc) {
	syms[sym] = pc;
	std::string symSpare = sym;
	if (sym[0] == '%' || sym[0] == '$' || sym[0] == '#') symSpare = symSpare.insert(1, "Blitz_");
	else symSpare = symSpare.insert(0, "Blitz_");
	syms[getCharPtr(symSpare)] = pc;
}

static void killer() {
	ExitProcess(-1);
}

static void _cdecl seTranslator(unsigned int u, EXCEPTION_POINTERS* pExp) {
	if (pExp && pExp->ExceptionRecord) {
		writeMiniDump(pExp);

		EXCEPTION_RECORD* rec = pExp->ExceptionRecord;
		void* crashAddr = rec->ExceptionAddress;

		DWORD offset = 0;
		if (module_pc && crashAddr >= module_pc) {
			offset = (DWORD)((char*)crashAddr - (char*)module_pc);
		}

		DWORD accessAddr = 0;
		DWORD accessType = 0; // 0 = read 1 = write
		if (rec->NumberParameters >= 2) {
			accessType = (DWORD)rec->ExceptionInformation[0];
			accessAddr = (DWORD)rec->ExceptionInformation[1];
		}
		const char* typeStr = (accessType == 1) ? "WRITE" : "READ";

		time_t now = time(nullptr);
		char timeBuf[64];
		ctime_s(timeBuf, sizeof(timeBuf), &now);
		char* nl = strchr(timeBuf, '\n');
		if (nl) *nl = '\0';

		writeCrashLog("=== CRASH LOG ===");
		writeCrashLog("Time: %s", timeBuf);
		writeCrashLog("Exception Code: 0x%08X", rec->ExceptionCode);
		writeCrashLog("Instruction Address: 0x%p (Offset in module: 0x%X)", crashAddr, offset);
		writeCrashLog("Access Violation: %s at 0x%p", typeStr, (void*)accessAddr);

		unsigned char instr[16];
		SIZE_T bytesRead;
		if (ReadProcessMemory(GetCurrentProcess(), crashAddr, instr, 16, &bytesRead)) {
			char hex[64] = { 0 };
			for (SIZE_T i = 0; i < bytesRead; i++) {
				char part[8];
				sprintf_s(part, sizeof(part), "%02X ", instr[i]);
				strcat_s(hex, sizeof(hex), part);
			}
			writeCrashLog("Instruction bytes: %s", hex);
		}
		if (const char* bfile = bbReleaseFile()) {
			if (bfile[0]) {
				int pos = bbReleasePos();
				int row = (pos >> 16) & 0xffff, col = pos & 0xffff;
				writeCrashLog("Blitz location: %s (line %d, col %d)", bfile, row + 1, col + 1);
			}
		}
		if (bbReleaseDepth() > 0) {
			writeCrashLog("Blitz call stack (innermost first):");
			for (int i = bbReleaseDepth() - 1; i >= 0; --i) {
				const char* f = bbReleaseFuncAt(i);
				if (!f || !f[0]) continue;
				writeCrashLog("  %s", f);
			}
		}
		writeCrashLog("---");

		char logPath[MAX_PATH];
		GetModuleFileNameA(NULL, logPath, MAX_PATH);
		char* slash = strrchr(logPath, '\\');
		if (slash) *slash = '\0';
		else logPath[0] = '\0';
		strcat_s(logPath, MAX_PATH, "\\blitz_crash.log");

		char msg[512];
		sprintf_s(msg, sizeof(msg), "A crash has occurred.\n\nThe log and dump files have been saved to:\n%s\n\n" "Please send these files to the developers.", logPath);
		MessageBoxA(NULL, msg, "Blitz Runtime Error", MB_OK | MB_ICONERROR);
	}

	switch (u) {
	case EXCEPTION_INT_DIVIDE_BY_ZERO:
		bbruntime_panic(MultiLang::integer_divide_zero);
		break;
	case EXCEPTION_ACCESS_VIOLATION:
		throw_mav();
		break;
	case EXCEPTION_ILLEGAL_INSTRUCTION:
		bbruntime_panic(MultiLang::illegal_instruction);
		break;
	case EXCEPTION_STACK_OVERFLOW:
		bbruntime_panic(MultiLang::stack_overflow);
		break;
	case EXCEPTION_INT_OVERFLOW:
		bbruntime_panic(MultiLang::integer_overflow);
		break;
	case EXCEPTION_FLT_OVERFLOW:
		bbruntime_panic(MultiLang::float_overflow);
		break;
	case EXCEPTION_FLT_DIVIDE_BY_ZERO:
		bbruntime_panic(MultiLang::float_divide_zero);
		break;
	default:
		bbruntime_panic(MultiLang::unknown_runtime_exception);
		break;
	}
}

int Runtime::version() {
	return VERSION;
}

const char* Runtime::nextSym() {
	if(!syms.size()) {
		bbruntime_link(rtSym);
		sym_it = syms.begin();
	}
	if(sym_it == syms.end()) {
		syms.clear(); return 0;
	}
	return (sym_it++)->first;
}

int Runtime::symValue(const char* sym) {
	std::map<const char*, void*>::iterator it = syms.find(sym);
	if(it != syms.end()) return (int)it->second;
	return -1;
}

void Runtime::startup(HINSTANCE h) {
	hinst = h;
	module_pc = (void*)h;
}

void Runtime::shutdown() {
	trackmem(false);
	syms.clear();
}

void Runtime::execute(void (*pc)(), const char* args, Debugger* dbg) {

	bool debug = !!dbg;

	static DummyDebugger dummydebug;

	if(!dbg) dbg = &dummydebug;

	trackmem(true);

#ifndef _DEBUG
	_se_translator_function old_trans = _set_se_translator(seTranslator);
	_control87(_RC_NEAR | _PC_24 | _EM_INVALID | _EM_ZERODIVIDE | _EM_OVERFLOW | _EM_UNDERFLOW | _EM_INEXACT | _EM_DENORMAL, 0xfffff);
#endif

	//strip spaces from ends of args...
	std::string params = args;
	while(params.size() && params[0] == ' ') params = params.substr(1);
	while(params.size() && params[params.size() - 1] == ' ') params = params.substr(0, params.size() - 1);

	//Fix the issue of NTF Mod clipping outside monitor boundaries in "fullscreen" mode when you have the system scale set to
	//something different than 100%.
	SetProcessDPIAware();

	if(gx_runtime = gxRuntime::openRuntime(hinst, params, dbg)) {
		bbruntime_run(gx_runtime, pc, debug);

		gxRuntime* t = gx_runtime;
		gx_runtime = 0;
		gxRuntime::closeRuntime(t);
	}

#ifndef _DEBUG
	_control87(_CW_DEFAULT, 0xfffff);
	_set_se_translator(old_trans);
#endif
}

void Runtime::asyncStop() {
	if(gx_runtime) gx_runtime->asyncStop();
}

void Runtime::asyncRun() {
	if(gx_runtime) gx_runtime->asyncRun();
}

void Runtime::asyncEnd() {
	if(gx_runtime) gx_runtime->asyncEnd();
}

void Runtime::checkmem(std::streambuf* buf) {
	std::ostream out(buf);
	::checkmem(out);
}

Runtime* _cdecl runtimeGetRuntime() {
	static Runtime runtime;
	return &runtime;
}

/********************** BUTT UGLY DLL->EXE HOOK! *************************/

static std::map<std::string, int> module_syms;
static std::map<std::string, int> runtime_syms;
static Runtime* runtime;

static void fail() {
	MessageBox(0, MultiLang::unable_run_module, 0, 0);
	ExitProcess(-1);
}

struct Sym {
	std::string name;
	int value;
};

static Sym getSym(void** p) {
	Sym sym;
	char* t = (char*)*p;
	while(char c = *t++) sym.name += c;
	sym.value = *(int*)t + (int)module_pc;
	*p = t + 4; return sym;
}

static int findSym(const std::string& t) {
	std::map<std::string, int>::iterator it;

	it = module_syms.find(t);
	if(it != module_syms.end()) return it->second;
	it = runtime_syms.find(t);
	if(it != runtime_syms.end()) return it->second;

	std::string err = std::format(MultiLang::cant_find_symbol, t);
	MessageBox(0, err.c_str(), 0, 0);
	ExitProcess(0);
	return 0;
}

static void* findSectionData(const char* sectionName, DWORD* pSize) {
	HMODULE hMod = GetModuleHandle(nullptr);
	PIMAGE_DOS_HEADER pDos = (PIMAGE_DOS_HEADER)hMod;
	if (pDos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
	PIMAGE_NT_HEADERS pNt = (PIMAGE_NT_HEADERS)((BYTE*)pDos + pDos->e_lfanew);
	if (pNt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
	PIMAGE_SECTION_HEADER pSection = IMAGE_FIRST_SECTION(pNt);
	for (int i = 0; i < pNt->FileHeader.NumberOfSections; ++i) {
		if (memcmp(pSection->Name, sectionName, 8) == 0) {
			*pSize = pSection->Misc.VirtualSize;
			return (BYTE*)hMod + pSection->VirtualAddress;
		}
		pSection++;
	}
	return nullptr;
}

static void link() {

	while (const char* sc = runtime->nextSym()) {
		std::string t(sc);

		if(t[0] == '_') {
			runtime_syms["_" + t] = runtime->symValue(sc);
			continue;
		}

		if(t[0] == '!') t = t.substr(1);

		if(!isalnum(t[0])) t = t.substr(1);

		for(int k = 0; k < t.size(); ++k) {
			if(isalnum(t[k]) || t[k] == '_') continue;
			t = t.substr(0, k); break;
		}

		runtime_syms["_f" + tolower(t)] = runtime->symValue(sc);
	}

	DWORD sectionSize = 0;
	void* pSectionData = findSectionData(".b3dmod", &sectionSize);
	void* p = nullptr;
	size_t dataSize = 0;

	if (pSectionData && sectionSize >= 8) {
		char* dataPtr = (char*)pSectionData;
		uint32_t salt = *(uint32_t*)dataPtr;  dataPtr += 4;
		uint32_t storedKey = *(uint32_t*)dataPtr;  dataPtr += 4;
		uint32_t key = storedKey ^ b3dMixKey(RUNTIME_KEY_SEED, salt);
		p = dataPtr;
		dataSize = sectionSize - 8;
		uint32_t* pData = (uint32_t*)p;
		for (size_t i = 0; i < dataSize / 4; ++i) {
			pData[i] ^= key;
		}
		for (size_t i = (dataSize / 4) * 4; i < dataSize; ++i) {
			((char*)p)[i] ^= (char)(key >> ((i % 4) * 8));
		}
	}
	else {
		HRSRC hres = FindResource(0, MAKEINTRESOURCE(1111), RT_RCDATA);
		if (!hres) fail();
		HGLOBAL hglo = LoadResource(0, hres);
		if (!hglo) fail();
		p = LockResource(hglo);
		if (!p) fail();
		dataSize = SizeofResource(0, hres);
	}

	int sz = *(int*)p; p = (int*)p + 1;

	//replace malloc for service pack 2 Data Execution Prevention (DEP).
	module_pc = VirtualAlloc(0, sz, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!module_pc) fail();
	memcpy(module_pc, p, sz);
	p = (char*)p + sz;

	int k, cnt;

	cnt = *(int*)p; p = (int*)p + 1;
	for(k = 0; k < cnt; ++k) {
		Sym sym = getSym(&p);
		if (sym.value < (int)module_pc || sym.value >= (int)module_pc + sz) fail();
		if (module_syms.find(sym.name) == module_syms.end())
			module_syms[sym.name] = sym.value;
	}

	cnt = *(int*)p; p = (int*)p + 1;
	for(k = 0; k < cnt; ++k) {
		Sym sym = getSym(&p);
		int* pp = (int*)sym.value;
		int dest = findSym(sym.name);
		*pp += dest - (int)pp;
	}

	cnt = *(int*)p; p = (int*)p + 1;
	for(k = 0; k < cnt; ++k) {
		Sym sym = getSym(&p);
		int* pp = (int*)sym.value;
		int dest = findSym(sym.name);
		*pp += dest;
	}

	runtime_syms.clear();
	module_syms.clear();
}

extern "C" _declspec(dllexport) int _stdcall bbWinMain();
extern "C" BOOL _stdcall _DllMainCRTStartup(HANDLE, DWORD, LPVOID);


static const char* DX9_DOWNLOAD_URL = "https://www.microsoft.com/en-us/download/details.aspx?id=35";

static bool isD3DX943Missing() {
	return false;
}

static HRESULT CALLBACK DX9TaskDialogCallback(HWND, UINT msg, WPARAM wParam, LPARAM, LONG_PTR) {
	if (msg == TDN_HYPERLINK_CLICKED && wParam) {
		ShellExecuteW(NULL, L"open", (LPCWSTR)wParam, NULL, NULL, SW_SHOWNORMAL);
		return S_OK;
	}
	return S_FALSE;
}

static void showDX9MissingPopup() {
	HMODULE hComCtl = LoadLibraryW(L"ComCtl32.dll");
	if (hComCtl) {
		typedef HRESULT(WINAPI* TaskDialogIndirectFunc)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
		TaskDialogIndirectFunc pTDI = (TaskDialogIndirectFunc)GetProcAddress(hComCtl, "TaskDialogIndirect");
		if (pTDI) {
			TASKDIALOGCONFIG cfg = { 0 };
			cfg.cbSize = sizeof(cfg);
			cfg.hwndParent = NULL;
			cfg.hInstance = GetModuleHandle(NULL);
			cfg.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
			cfg.dwCommonButtons = TDCBF_OK_BUTTON;
			cfg.pszWindowTitle = L"Missing DirectX 9 components";
			cfg.pszMainIcon = TD_ERROR_ICON;
			cfg.pszMainInstruction = L"d3dx9_43.dll was not found.";
			cfg.pszContent = L"Blitz3D requires the DirectX 9 (June 2010) runtime.\nDownload the <a href=\"https://www.microsoft.com/en-us/download/details.aspx?id=35\">DirectX End-User Runtime</a> from Microsoft and try again.";
			cfg.pfCallback = DX9TaskDialogCallback;
			if (SUCCEEDED(pTDI(&cfg, NULL, NULL, NULL))) {
				FreeLibrary(hComCtl);
				return;
			}
		}
		FreeLibrary(hComCtl);
	}
	int r = MessageBoxA(0,
		"d3dx9_43.dll was not found.\n\nBlitz3D requires the DirectX 9 (June 2010) runtime.\nPress Yes to open the download page for the DirectX End-User Runtime.",
		"Missing DirectX 9 components",
		MB_YESNO | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
	if (r == IDYES) {
		ShellExecuteA(NULL, "open", DX9_DOWNLOAD_URL, NULL, NULL, SW_SHOWNORMAL);
	}
}

static void showDX9MissingAndExit() {
	showDX9MissingPopup();
	ExitProcess(-1);
}

bool WINAPI DllMain(HANDLE module, DWORD reason, void* reserved) {
	return TRUE;
}

int __stdcall bbWinMain() {

	if (isD3DX943Missing()) {
		showDX9MissingAndExit();
	}

	HINSTANCE inst = GetModuleHandle(0);

	_DllMainCRTStartup(inst, DLL_PROCESS_ATTACH, 0);

#ifdef BETA
	int ver = VERSION & 0x7fff;
	string t = std::format(MultiLang::created_with_beta, itoa(ver / 100), itoa(ver % 100));
	MessageBox(GetDesktopWindow(), t.c_str(), MultiLang::blitz3d_message, MB_OK);
#endif

	runtime = runtimeGetRuntime();
	runtime->startup(inst);

	link();

	//get cmd_line and params
	std::string cmd = GetCommandLine(), params;
	while(cmd.size() && cmd[0] == ' ') cmd = cmd.substr(1);
	if(cmd.find('\"') == 0) {
		int n = cmd.find('\"', 1);
		if(n != std::string::npos) {
			params = cmd.substr(n + 1);
			cmd = cmd.substr(1, n - 1);
		}
	}
	else {
		int n = cmd.find(' ');
		if(n != std::string::npos) {
			params = cmd.substr(n + 1);
			cmd = cmd.substr(0, n);
		}
	}

	runtime->execute((void(*)())module_pc, params.c_str(), 0);
	runtime->shutdown();

	_DllMainCRTStartup(inst, DLL_PROCESS_DETACH, 0);

	ExitProcess(0);
	return 0;
}