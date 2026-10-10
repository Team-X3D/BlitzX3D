#include "std.h"
#include "bbbank.h"
#include "bbstream.h"
#include "../MultiLang/MultiLang.h"

struct bbBank {
	char* data;
	int size, capacity;

	bbBank(int sz) :size(sz) {
		capacity = (size + 15) & ~15;
		data = new char[capacity]();
	}
	virtual ~bbBank() {
		delete[] data;
	}
	void resize(int n) {
		if(n > size) {
			if(n > capacity) {
				capacity = capacity * 3 / 2;
				if(n > capacity) capacity = n;
				capacity = (capacity + 15) & ~15;
				char* p = new char[capacity];
				memcpy(p, data, size);
				delete[] data;
				data = p;
			}
			else memset(data + size, 0, n - size);
		}
		size = n;
	}
};

static std::unordered_set<bbBank*> bank_set;

static inline bool validBank(bbBank* b, const char* function) {
	if (!bank_set.count(b)) {
		ErrorLog(function, MultiLang::bank_not_exist);
		return false;
	}
	return true;
}

static inline bool validRange(bbBank* b, const char* function, int offset, int size) {
	if (!validBank(b, function)) return false;
	if (offset < 0 || size < 0 || offset > b->size - size) {
		ErrorLog(function, MultiLang::offset_out_of_range);
		return false;
	}
	return true;
}

int bbVerifyBank(bbBank* b) {
	return (bool)bank_set.count(b);
}

bbBank* bbCreateBank(int size) {
	if (size < 0) {
		ErrorLog("CreateBank", MultiLang::illegal_buffer_size);
		size = 0;
	}
	bbBank* b = new bbBank(size);
	bank_set.insert(b);
	return b;
}

void bbFreeBank(bbBank* b) {
	if(bank_set.erase(b)) delete b;
}

int bbBankSize(bbBank* b) {
	if (!validBank(b, "BankSize")) return 0;
	return b->size;
}

int bbBankPointer(bbBank* b) {
	if (!validBank(b, "BankPointer")) return 0;
	return (int)b->data;
}

void  bbResizeBank(bbBank* b, int size) {
	if (!validBank(b, "ResizeBank")) return;
	if (size < 0) {
		ErrorLog("ResizeBank", MultiLang::illegal_buffer_size);
		return;
	}
	b->resize(size);
}

void  bbCopyBank(bbBank* src, int src_p, bbBank* dest, int dest_p, int count) {
	if (!validBank(src, "CopyBank")) return;
	if (!validBank(dest, "CopyBank")) return;
	if (count < 0 || src_p < 0 || dest_p < 0 ||
		src_p > src->size - count || dest_p > dest->size - count) {
		ErrorLog("CopyBank", MultiLang::offset_out_of_range);
		return;
	}
	memmove(dest->data + dest_p, src->data + src_p, count);
}

int  bbPeekByte(bbBank* b, int offset) {
	if (!validRange(b, "PeekByte", offset, 1)) return 0;
	return *(unsigned char*)(b->data + offset);
}

int  bbPeekShort(bbBank* b, int offset) {
	if (!validRange(b, "PeekShort", offset, 2)) return 0;
	return *(unsigned short*)(b->data + offset);
}

int  bbPeekInt(bbBank* b, int offset) {
	if (!validRange(b, "PeekInt", offset, 4)) return 0;
	return *(int*)(b->data + offset);
}

float  bbPeekFloat(bbBank* b, int offset) {
	if (!validRange(b, "PeekFloat", offset, 4)) return 0;
	return *(float*)(b->data + offset);
}

BBStr* bbPeekString(bbBank* b, int offset) {
	if (!validRange(b, "PeekString", offset, 4)) return new BBStr();
	int length = *(int*)(b->data + offset);
	if (length < 0 || length > b->size - offset - 4) {
		ErrorLog("PeekString", MultiLang::offset_out_of_range);
		return new BBStr();
	}
	return new BBStr(b->data + offset + 4, length);
}

void  bbPokeByte(bbBank* b, int offset, int value) {
	if (!validRange(b, "PokeByte", offset, 1)) return;
	*(char*)(b->data + offset) = value;
}

void  bbPokeShort(bbBank* b, int offset, int value) {
	if (!validRange(b, "PokeShort", offset, 2)) return;
	*(unsigned short*)(b->data + offset) = value;
}

void  bbPokeInt(bbBank* b, int offset, int value) {
	if (!validRange(b, "PokeInt", offset, 4)) return;
	*(int*)(b->data + offset) = value;
}

void  bbPokeFloat(bbBank* b, int offset, float value) {
	if (!validRange(b, "PokeFloat", offset, 4)) return;
	*(float*)(b->data + offset) = value;
}

int bbPokeString(bbBank* b, int offset, BBStr* str) {
	int length = str->length();
	if (length < 0 || !validRange(b, "PokeString", offset, length + 4)) {
		delete str;
		return 0;
	}
	*(int*)(b->data + offset) = length;
	memcpy_s(b->data + offset + 4, length, str->data(), length);
	delete str;
	return offset + 4 + length;
}

int bbBankStringSize(BBStr* str) {
	int length = str->length() + 4;
	delete str;
	return length;
}

int bbPtrPeekInt(int addr) {
	return *(int*)addr;
}

float bbPtrPeekFloat(int addr) {
	return *(float*)addr;
}

void bbPtrPokeInt(int addr, int value) {
	*(int*)addr = value;
}

void bbPtrPokeFloat(int addr, float value) {
	*(float*)addr = value;
}

int   bbReadBytes(bbBank* b, bbStream* s, int offset, int count) {
	if (!validBank(b, "ReadBytes")) return 0;
	if (count < 0 || offset < 0 || offset > b->size - count) {
		ErrorLog("ReadBytes", MultiLang::offset_out_of_range);
		return 0;
	}
	debugStream(s, "ReadBytes");
	return s->read(b->data + offset, count);
}

int   bbWriteBytes(bbBank* b, bbStream* s, int offset, int count) {
	if (!validBank(b, "WriteBytes")) return 0;
	if (count < 0 || offset < 0 || offset > b->size - count) {
		ErrorLog("WriteBytes", MultiLang::offset_out_of_range);
		return 0;
	}
	debugStream(s, "WriteBytes");
	return s->write(b->data + offset, count);
}

int  bbCallDLL(BBStr* dll, BBStr* fun, bbBank* in, bbBank* out) {
	if (in && !validBank(in, "CallDLL")) { delete dll; delete fun; return 0; }
	if (out && !validBank(out, "CallDLL")) { delete dll; delete fun; return 0; }
	int t = sdl_runtime->callDll(*dll, *fun,
		in ? in->data : 0, in ? in->size : 0,
		out ? out->data : 0, out ? out->size : 0);
	delete dll; delete fun;
	return t;
}

bool bank_create() {
	return true;
}

bool bank_destroy() {
	while(bank_set.size()) bbFreeBank(*bank_set.begin());
	return true;
}

void bank_link(void(*rtSym)(const char*, void*)) {
	rtSym("%VerifyBank%bank", bbVerifyBank);
	rtSym("%CreateBank%size=0", bbCreateBank);
	rtSym("FreeBank%bank", bbFreeBank);
	rtSym("%BankSize%bank", bbBankSize);
	rtSym("%BankPointer%bank", bbBankPointer);
	rtSym("ResizeBank%bank%size", bbResizeBank);
	rtSym("CopyBank%src_bank%src_offset%dest_bank%dest_offset%count", bbCopyBank);
	rtSym("%PeekByte%bank%offset", bbPeekByte);
	rtSym("%PeekShort%bank%offset", bbPeekShort);
	rtSym("%PeekInt%bank%offset", bbPeekInt);
	rtSym("#PeekFloat%bank%offset", bbPeekFloat);
	rtSym("$PeekString%bank%offset", bbPeekString);
	rtSym("PokeByte%bank%offset%value", bbPokeByte);
	rtSym("PokeShort%bank%offset%value", bbPokeShort);
	rtSym("PokeInt%bank%offset%value", bbPokeInt);
	rtSym("PokeFloat%bank%offset#value", bbPokeFloat);
	rtSym("%PokeString%bank%offset$value", bbPokeString);
	rtSym("%BankStringSize$str", bbBankStringSize);
	rtSym("%ReadBytes%bank%file%offset%count", bbReadBytes);
	rtSym("%WriteBytes%bank%file%offset%count", bbWriteBytes);
	rtSym("%CallDLL$dll_name$func_name%in_bank=0%out_bank=0", bbCallDLL);
	rtSym("%PtrPeekInt%addr", bbPtrPeekInt);
	rtSym("%PtrPeekFloat%addr", bbPtrPeekFloat);
	rtSym("PtrPokeInt%addr%value", bbPtrPokeInt);
	rtSym("PtrPokeFloat%addr#value", bbPtrPokeFloat);
}