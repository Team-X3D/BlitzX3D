#include "std.h"
#include "linker.h"
#include "image_util.h"
#include <cstdlib>

class BBModule : public Module {
public:
	BBModule();
	~BBModule();

	void* link(Module* libs);
	bool createExe(const char* exe_file, const char* dll_file, bool laa);

	int getPC();

	void emit(int byte);
	void emitw(int word);
	void emitd(int dword);
	void emitx(void* mem, int sz);
	bool addSymbol(const char* sym, int pc);
	bool addReloc(const char* dest_sym, int pc, bool pcrel);

	bool findSymbol(const char* sym, int* pc);

private:
	char* data;
	int data_sz, pc;
	bool linked;

	std::map<std::string, int> symbols;
	std::map<int, std::string> rel_relocs, abs_relocs;

	bool findSym(const std::string& t, Module* libs, int* n) {
		if(findSymbol(t.c_str(), n)) return true;
		if(libs->findSymbol(t.c_str(), n)) return true;
		std::string err = "Symbol '" + t + "' not found";
		MessageBox(GetDesktopWindow(), err.c_str(), "Blitz Linker Error", MB_TOPMOST | MB_SETFOREGROUND);
		return false;
	}

	void ensure(int n) {
		if(pc + n <= data_sz) return;
		data_sz = data_sz / 2 + data_sz;
		if(data_sz < pc + n) data_sz = pc + n;
		char* old_data = data;
		data = new char[data_sz];
		memcpy(data, old_data, pc);
		delete[] old_data;
	}
};

BBModule::BBModule() :data(0), data_sz(0), pc(0), linked(false) {
}

BBModule::~BBModule() {
	if(linked) VirtualFree(data, 0, MEM_RELEASE);
	else delete[] data;
}

void* BBModule::link(Module* libs) {

	if(linked) return data;

	int dest;
	std::map<int, std::string>::iterator it;

	char* p = (char*)VirtualAlloc(0, pc, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	memcpy(p, data, pc);
	delete[] data;
	data = p;

	linked = true;

	for(it = rel_relocs.begin(); it != rel_relocs.end(); ++it) {
		if(!findSym(it->second, libs, &dest)) return 0;
		int* p = (int*)(data + it->first); *p += (dest - (int)p);
	}

	for(it = abs_relocs.begin(); it != abs_relocs.end(); ++it) {
		if(!findSym(it->second, libs, &dest)) return 0;
		int* p = (int*)(data + it->first); *p += dest;
	}

	return data;
}

int BBModule::getPC() {
	return pc;
}

void BBModule::emit(int byte) {
	ensure(1); data[pc++] = byte;
}

void BBModule::emitw(int word) {
	ensure(2); *(short*)(data + pc) = word; pc += 2;
}

void BBModule::emitd(int dword) {
	ensure(4); *(int*)(data + pc) = dword; pc += 4;
}

void BBModule::emitx(void* mem, int sz) {
	ensure(sz); memcpy(data + pc, mem, sz); pc += sz;
}

bool BBModule::addSymbol(const char* sym, int pc) {
	std::string t(sym);
	if(symbols.find(t) != symbols.end()) return false;
	symbols[t] = pc; return true;
}

bool BBModule::addReloc(const char* dest_sym, int pc, bool pcrel) {
	std::map<int, std::string>& rel = pcrel ? rel_relocs : abs_relocs;
	if(rel.find(pc) != rel.end()) return false;
	rel[pc] = std::string(dest_sym); return true;
}

bool BBModule::findSymbol(const char* sym, int* pc) {
	std::string t = std::string(sym);
	std::map<std::string, int>::iterator it = symbols.find(t);
	if(it == symbols.end()) return false;
	*pc = it->second + (int)data;
	return true;
}

int Linker::version() {
	return VERSION;
}

Module* Linker::createModule() {
	return new BBModule();
}

void Linker::deleteModule(Module* mod) {
	delete mod;
}

Linker* _cdecl linkerGetLinker() {
	static Linker linker; return &linker;
}

bool BBModule::createExe(const char* exe_file, const char* dll_file, bool laa) {
	//find proc address of bbWinMain
	HMODULE hmod = LoadLibrary(dll_file); if(!hmod) return false;
	int proc = (int)GetProcAddress(hmod, "_bbWinMain@0");
	int entry = proc - (int)hmod;
	FreeLibrary(hmod);
	if (!proc) return false;

	if (!CopyFile(dll_file, exe_file, false)) return false;
	if (!openImage(exe_file)) return false;
	makeExe(entry, laa);

	//create module
	//code size: code...
	//num_syms:  name,val...
	//num_rels:  name,val...
	//num_abss:  name,val...
	//
	qstreambuf buf;
	std::iostream out(&buf);

	std::map<std::string, int>::iterator it;
	std::map<int, std::string>::iterator rit;

	//write the code
	int sz = pc; out.write((char*)&sz, 4); out.write(data, pc);

	//write symbols
	std::vector<std::pair<std::string, int>> allSymbols;
	for (auto& kv : symbols) {
		allSymbols.emplace_back(kv.first, kv.second);
	}

	int totalCount = (int)allSymbols.size();
	out.write((char*)&totalCount, 4);

	for (auto& pair : allSymbols) {
		std::string t = pair.first + '\0';
		out.write(t.data(), t.size());
		int val = pair.second;
		out.write((char*)&val, 4);
	}

	//write relative relocs
	sz = rel_relocs.size(); out.write((char*)&sz, 4);
	for(rit = rel_relocs.begin(); rit != rel_relocs.end(); ++rit) {
		std::string t = rit->second + '\0';
		out.write(t.data(), t.size());
		sz = rit->first; out.write((char*)&sz, 4);
	}

	//write absolute relocs
	sz = abs_relocs.size(); out.write((char*)&sz, 4);
	for(rit = abs_relocs.begin(); rit != abs_relocs.end(); ++rit) {
		std::string t = rit->second + '\0';
		out.write(t.data(), t.size());
		sz = rit->first; out.write((char*)&sz, 4);
	}

	size_t bufSize = buf.size();

	replaceRsrc(10, 1111, 1033, buf.data(), (int)bufSize);
	closeImage();

	return true;
}