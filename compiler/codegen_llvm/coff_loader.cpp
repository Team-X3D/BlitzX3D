#include "../std.h"
#include "coff_loader.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>

namespace {

struct CoffHeader {
	unsigned short machine;
	unsigned short numSections;
	unsigned int timestamp;
	unsigned int symTable;
	unsigned int numSymbols;
	unsigned short optHeaderSize;
	unsigned short characteristics;
};

struct CoffSection {
	char name[8];
	unsigned int virtualSize;
	unsigned int virtualAddress;
	unsigned int rawSize;
	unsigned int rawPtr;
	unsigned int relocPtr;
	unsigned int linePtr;
	unsigned short numRelocs;
	unsigned short numLines;
	unsigned int characteristics;
};

struct CoffSymbol {
	char name[8];
	unsigned int value;
	short sectionNumber;
	unsigned short type;
	unsigned char storageClass;
	unsigned char numAux;
};

struct CoffReloc {
	unsigned int virtualAddress;
	unsigned int symbolIndex;
	unsigned short type;
};

static bool readFile(const std::string& path, std::vector<unsigned char>& out) {
	FILE* f = fopen(path.c_str(), "rb");
	if (!f) return false;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n < 0) { fclose(f); return false; }
	out.resize(n);
	if (n) {
		size_t got = fread(out.data(), 1, n, f);
		if (got != (size_t)n) { fclose(f); return false; }
	}
	fclose(f);
	return true;
}

static bool writeFile(const std::string& path, const std::string& data) {
	FILE* f = fopen(path.c_str(), "wb");
	if (!f) return false;
	size_t n = fwrite(data.data(), 1, data.size(), f);
	fclose(f);
	return n == data.size();
}

static std::string readFileString(const std::string& path) {
	std::vector<unsigned char> b;
	if (!readFile(path, b)) return "";
	return std::string(b.begin(), b.end());
}

static bool fileExists(const std::string& path) {
	DWORD a = GetFileAttributesA(path.c_str());
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::string tempDir() {
	char buf[MAX_PATH];
	DWORD n = GetTempPathA(MAX_PATH, buf);
	if (!n || n >= MAX_PATH) return ".";
	std::string s(buf);
	while (s.size() && (s[s.size() - 1] == '\\' || s[s.size() - 1] == '/')) s.erase(s.size() - 1);
	return s;
}

static std::string exeDir() {
	char buf[MAX_PATH];
	DWORD n = GetModuleFileNameA(0, buf, MAX_PATH);
	if (!n || n >= MAX_PATH) return "";
	std::string s(buf, n);
	size_t p = s.find_last_of('\\');
	if (p == std::string::npos) p = s.find_last_of('/');
	if (p == std::string::npos) return "";
	return s.substr(0, p);
}

// this is pretty crappy! 
// actually baking it into the engine isn't possible since the 32bit compiler can't load 64bit LLVM
static std::string findClang(std::string& err) {
	const char* env = getenv("BB_LLVM_BIN");
	if (env && *env) {
		std::string p(env);
		if (p.size() && p[p.size() - 1] != '\\' && p[p.size() - 1] != '/') p += "\\";
		p += "clang.exe";
		if (fileExists(p)) return p;
	}

	const char* full = getenv("BB_CLANG");
	if (full && *full && fileExists(full)) return full;

	std::string dir = exeDir();
	if (!dir.empty()) {
		std::string local = dir + "\\clang.exe";
		if (fileExists(local)) return local;
	}

	char found[MAX_PATH];
	if (SearchPathA(0, "clang.exe", 0, MAX_PATH, found, 0)) return found;

	err = "clang.exe not found";
	return "";
}

static bool runClang(const std::string& clang, const std::string& args, const std::string& logPath, std::string& err) {
	SECURITY_ATTRIBUTES sa;
	sa.nLength = sizeof(sa);
	sa.lpSecurityDescriptor = 0;
	sa.bInheritHandle = TRUE;

	HANDLE hLog = CreateFileA(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
	if (hLog == INVALID_HANDLE_VALUE) { err = "cannot create compiler log"; return false; }

	HANDLE hNul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, 0);

	STARTUPINFOA si;
	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = hNul;
	si.hStdOutput = hLog;
	si.hStdError = hLog;

	PROCESS_INFORMATION pi;
	ZeroMemory(&pi, sizeof(pi));

	std::string cmd = "\"" + clang + "\" " + args;
	std::vector<char> cmdbuf(cmd.begin(), cmd.end());
	cmdbuf.push_back(0);

	BOOL ok = CreateProcessA(0, cmdbuf.data(), 0, 0, TRUE, CREATE_NO_WINDOW, 0, 0, &si, &pi);
	if (!ok) {
		CloseHandle(hLog);
		if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);
		err = "failed to launch clang";
		return false;
	}

	WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD code = 1;
	GetExitCodeProcess(pi.hProcess, &code);

	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	CloseHandle(hLog);
	if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);

	if (code != 0) {
		err = readFileString(logPath);
		if (err.empty()) err = "clang failed";
		return false;
	}
	return true;
}

// clang decorates the i386 names with a lead underscore and an stdcall @N suffix
// why x86 does this on everything is a mystery! (probably not, but i can't be arsed) 
// we can just strip it to match runtime names
static std::string stripStdcall(const std::string& n) {
	size_t at = n.rfind('@');
	if (at == std::string::npos || at + 1 >= n.size()) return n;
	for (size_t k = at + 1; k < n.size(); ++k) {
		if (!isdigit((unsigned char)n[k])) return n;
	}
	return n.substr(0, at);
}

static std::string demangle(const std::string& n) {
	std::string s = stripStdcall(n);
	if (!s.empty() && s[0] == '_') s = s.substr(1);
	return s;
}

static void addCandidate(std::vector<std::string>& c, const std::string& s) {
	if (s.empty()) return;
	for (size_t k = 0; k < c.size(); ++k) if (c[k] == s) return;
	c.push_back(s);
}

static std::vector<std::string> candidates(const std::string& raw) {
	std::vector<std::string> c;
	addCandidate(c, raw);
	std::string s = stripStdcall(raw);
	addCandidate(c, s);
	std::string t = s;
	while (!t.empty() && t[0] == '_') { t = t.substr(1); addCandidate(c, t); }
	std::string r = raw;
	while (!r.empty() && r[0] == '_') { r = r.substr(1); addCandidate(c, r); }
	return c;
}

struct LoadedObject {
	std::vector<unsigned char*> bases;
	unsigned int numSections;
};

static bool boundsOk(size_t total, unsigned int off, size_t len) {
	return (size_t)off + len <= total;
}

static int resolveSymbol(const std::vector<unsigned char>& data, size_t total, const CoffHeader& hdr,
	const LoadedObject& obj, const std::string& strTable, unsigned int symIndex,
	LLVMSymResolver resolver, void* ctx, std::string& err) {

	size_t symOff = (size_t)hdr.symTable + (size_t)symIndex * 18;
	if (!boundsOk(total, (unsigned int)symOff, 18)) { err = "bad symbol index"; return -1; }

	CoffSymbol sym;
	memcpy(&sym, data.data() + symOff, 18);

	if (sym.sectionNumber > 0 && sym.sectionNumber <= (short)obj.numSections) {
		return (int)(obj.bases[sym.sectionNumber - 1] + sym.value);
	}
	if (sym.sectionNumber == -1) return (int)sym.value;
	if (sym.sectionNumber != 0) { err = "unsupported symbol section"; return -1; }

	std::string name;
	if (sym.name[0] == 0 && sym.name[1] == 0 && sym.name[2] == 0 && sym.name[3] == 0) {
		unsigned int off = 0;
		memcpy(&off, sym.name + 4, 4);
		if (off >= strTable.size()) { err = "bad string table offset"; return -1; }
		name = strTable.c_str() + off;
	}
	else {
		name.assign(sym.name, strnlen(sym.name, 8));
	}

	std::vector<std::string> c = candidates(name);
	for (size_t k = 0; k < c.size(); ++k) {
		int pc = 0;
		if (resolver(c[k].c_str(), &pc, ctx)) return pc;
	}

	err = "unresolved external symbol '" + name + "'";
	return -1;
}

static void* loadObject(const std::vector<unsigned char>& data, LLVMSymResolver resolver, void* ctx, std::string& err) {
	size_t total = data.size();
	if (total < sizeof(CoffHeader)) { err = "object file too small"; return 0; }

	CoffHeader hdr;
	memcpy(&hdr, data.data(), sizeof(hdr));

	if (hdr.machine != 0x014c) { err = "object is not i386"; return 0; }
	if (hdr.numSections == 0) { err = "object has no sections"; return 0; }

	size_t secTableOff = sizeof(CoffHeader) + hdr.optHeaderSize;
	if (!boundsOk(total, (unsigned int)secTableOff, (size_t)hdr.numSections * 40)) { err = "bad section table"; return 0; }

	LoadedObject obj;
	obj.numSections = hdr.numSections;
	obj.bases.assign(hdr.numSections, 0);

	std::vector<CoffSection> sections(hdr.numSections);
	for (unsigned int k = 0; k < hdr.numSections; ++k) {
		memcpy(&sections[k], data.data() + secTableOff + (size_t)k * 40, 40);

		unsigned int sz = sections[k].virtualSize > sections[k].rawSize ? sections[k].virtualSize : sections[k].rawSize;
		if (sz == 0) sz = 1;

		void* p = VirtualAlloc(0, sz, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
		if (!p) { err = "VirtualAlloc failed"; return 0; }
		obj.bases[k] = (unsigned char*)p;

		if (sections[k].rawSize) {
			if (!boundsOk(total, sections[k].rawPtr, sections[k].rawSize)) { err = "bad section data"; return 0; }
			unsigned int copy = sections[k].rawSize < sz ? sections[k].rawSize : sz;
			memcpy(obj.bases[k], data.data() + sections[k].rawPtr, copy);
		}
	}

	std::string strTable;
	if (hdr.numSymbols) {
		size_t strOff = (size_t)hdr.symTable + (size_t)hdr.numSymbols * 18;
		if (boundsOk(total, (unsigned int)strOff, 4)) {
			unsigned int strSize = 0;
			memcpy(&strSize, data.data() + strOff, 4);
			if (strSize > 4 && boundsOk(total, (unsigned int)strOff, strSize)) {
				strTable.assign((const char*)data.data() + strOff, strSize);
			}
		}
	}

	for (unsigned int s = 0; s < hdr.numSections; ++s) {
		const CoffSection& sec = sections[s];
		if (!sec.numRelocs) continue;
		if (!boundsOk(total, sec.relocPtr, (size_t)sec.numRelocs * 10)) { err = "bad reloc table"; return 0; }

		for (unsigned int r = 0; r < sec.numRelocs; ++r) {
			CoffReloc rel;
			memcpy(&rel, data.data() + sec.relocPtr + (size_t)r * 10, 10);

			if (rel.type == IMAGE_REL_I386_ABSOLUTE) continue;

			int target = resolveSymbol(data, total, hdr, obj, strTable, rel.symbolIndex, resolver, ctx, err);
			if (target < 0) return 0; // https://www.youtube.com/watch?v=8-qZD6XHVCA&t=134s

			if (rel.virtualAddress + 4 > (sec.virtualSize > sec.rawSize ? sec.virtualSize : sec.rawSize)) {
				err = "reloc out of range";
				return 0;
			}

			unsigned int* loc = (unsigned int*)(obj.bases[s] + rel.virtualAddress);
			unsigned int p = (unsigned int)loc;

			switch (rel.type) {
			case IMAGE_REL_I386_DIR32:
			case IMAGE_REL_I386_DIR32NB:
				*loc += (unsigned int)target;
				break;
			case IMAGE_REL_I386_REL32:
				*loc += (unsigned int)target - (p + 4);
				break;
			default:
				err = "unsupported relocation type " + std::to_string(rel.type);
				return 0;
			}
		}
	}

	void* entry = 0;
	for (unsigned int k = 0; k < hdr.numSymbols; ++k) {
		size_t symOff = (size_t)hdr.symTable + (size_t)k * 18;
		if (!boundsOk(total, (unsigned int)symOff, 18)) break;
		CoffSymbol sym;
		memcpy(&sym, data.data() + symOff, 18);
		if (sym.sectionNumber <= 0 || sym.sectionNumber > (short)obj.numSections) continue;

		std::string name;
		if (sym.name[0] == 0 && sym.name[1] == 0 && sym.name[2] == 0 && sym.name[3] == 0) {
			unsigned int off = 0;
			memcpy(&off, sym.name + 4, 4);
			if (off < strTable.size()) name = strTable.c_str() + off;
		}
		else {
			name.assign(sym.name, strnlen(sym.name, 8));
		}

		if (demangle(name) == "__MAIN") {
			entry = obj.bases[sym.sectionNumber - 1] + sym.value;
			break;
		}
	}

	if (!entry) { err = "entry point __MAIN not found"; return 0; }
	return entry;
}

}

void* llvm_load_module(const std::string& ir, LLVMSymResolver resolver, void* ctx, std::string& err) {
	err.clear();

	std::string clang = findClang(err);
	if (clang.empty()) return 0;

	std::string dir = tempDir();
	std::string base = "bb_llvm_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(GetTickCount());
	std::string llPath = dir + "\\" + base + ".ll";
	std::string objPath = dir + "\\" + base + ".obj";
	std::string logPath = dir + "\\" + base + ".log";

	if (!writeFile(llPath, ir)) { err = "cannot write temporary IR file"; return 0; }

	std::string args = "-target i386-pc-windows-msvc -c \"" + llPath + "\" -o \"" + objPath + "\"";
	bool ok = runClang(clang, args, logPath, err);

	DeleteFileA(llPath.c_str());
	DeleteFileA(logPath.c_str());

	if (!ok) { DeleteFileA(objPath.c_str()); return 0; }

	std::vector<unsigned char> objData;
	if (!readFile(objPath, objData)) { err = "cannot read object file"; DeleteFileA(objPath.c_str()); return 0; }
	DeleteFileA(objPath.c_str());

	void* entry = loadObject(objData, resolver, ctx, err);
	return entry;
}
