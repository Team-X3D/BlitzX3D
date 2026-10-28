#include "../std.h"
#include "llvm_obj.h"

#include "../../linker/linker.h"

#include "llvm-c/Core.h"
#include "llvm-c/IRReader.h"
#include "llvm-c/Target.h"
#include "llvm-c/TargetMachine.h"

#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace {

const char* LLVM_TRIPLE = "i386-pc-windows-msvc";

void initLLVM() {
	static bool inited = false;
	if (inited) return;
	LLVMInitializeX86TargetInfo();
	LLVMInitializeX86Target();
	LLVMInitializeX86TargetMC();
	LLVMInitializeX86AsmPrinter();
	LLVMInitializeX86AsmParser();
	inited = true;
}

std::string takeError(char* msg) {
	std::string s = msg ? msg : "unknown LLVM error";
	if (msg) LLVMDisposeMessage(msg);
	return s;
}

std::string demangle(const std::string& name) {
	std::string s = name;

	size_t at = s.rfind('@');
	if (at != std::string::npos && at + 1 < s.size()) {
		bool digits = true;
		for (size_t k = at + 1; k < s.size(); ++k) {
			if (!isdigit((unsigned char)s[k])) { digits = false; break; }
		}
		if (digits) s = s.substr(0, at);
	}
	if (!s.empty() && s[0] == '_') s = s.substr(1);
	return s;
}

bool emitObject(const std::string& ir, std::string& out, std::string& err) {
	initLLVM();

	LLVMContextRef ctx = LLVMContextCreate();
	LLVMMemoryBufferRef buf = LLVMCreateMemoryBufferWithMemoryRangeCopy(ir.data(), ir.size(), "bb");
	LLVMModuleRef mod = 0;
	char* perr = 0;
	if (LLVMParseIRInContext(ctx, buf, &mod, &perr)) {
		err = takeError(perr);
		LLVMContextDispose(ctx);
		return false;
	}

	LLVMTargetRef target = 0;
	char* terr = 0;
	if (LLVMGetTargetFromTriple(LLVM_TRIPLE, &target, &terr)) {
		err = takeError(terr);
		LLVMDisposeModule(mod);
		LLVMContextDispose(ctx);
		return false;
	}

	LLVMTargetMachineRef tm = LLVMCreateTargetMachine(target, LLVM_TRIPLE, "pentium4", "",
		LLVMCodeGenLevelDefault, LLVMRelocStatic, LLVMCodeModelSmall);
	if (!tm) {
		err = "unable to create LLVM target machine";
		LLVMDisposeModule(mod);
		LLVMContextDispose(ctx);
		return false;
	}

	LLVMSetTarget(mod, LLVM_TRIPLE);
	LLVMTargetDataRef dl = LLVMCreateTargetDataLayout(tm);
	char* dls = LLVMCopyStringRepOfTargetData(dl);
	LLVMSetDataLayout(mod, dls);
	LLVMDisposeMessage(dls);
	LLVMDisposeTargetData(dl);

	for (LLVMValueRef f = LLVMGetFirstFunction(mod); f; f = LLVMGetNextFunction(f)) {
		if (LLVMIsDeclaration(f)) continue;
		// runtime.dll has no __chkstk, so prevent LLVM from raping the stack!
		LLVMAddTargetDependentFunctionAttr(f, "no-stack-arg-probe", "");
	}

	LLVMMemoryBufferRef obj = 0;
	char* emsg = 0;
	if (LLVMTargetMachineEmitToMemoryBuffer(tm, mod, LLVMObjectFile, &emsg, &obj)) {
		err = takeError(emsg);
		LLVMDisposeTargetMachine(tm);
		LLVMDisposeModule(mod);
		LLVMContextDispose(ctx);
		return false;
	}

	out.assign(LLVMGetBufferStart(obj), LLVMGetBufferSize(obj));

	LLVMDisposeMemoryBuffer(obj);
	LLVMDisposeTargetMachine(tm);
	LLVMDisposeModule(mod);
	LLVMContextDispose(ctx);
	return true;
}

struct Symbol {
	std::string name;
	int section;
	uint32_t value;
	Symbol() :section(0), value(0) {}
};

struct Section {
	int index;
	int blobBase;
	int blobSize;
	int align;
	bool included;
};

bool rangeOk(size_t off, size_t len, size_t total) {
	return off <= total && len <= total - off;
}

std::string symbolName(const IMAGE_SYMBOL* sym, const char* strTable, size_t strSize) {
	if (sym->N.Name.Short == 0) {
		size_t off = sym->N.Name.Long;
		if (off >= 4 && off < strSize) {
			const char* s = strTable + off;
			size_t n = 0;
			while (off + n < strSize && s[n]) ++n;
			return std::string(s, n);
		}
		return "";
	}
	char tmp[9];
	memcpy(tmp, sym->N.ShortName, 8);
	tmp[8] = 0;
	return std::string(tmp);
}

int sectionAlign(uint32_t characteristics) {
	uint32_t a = (characteristics >> 20) & 0xf;
	if (a >= 1 && a <= 14) return 1 << (a - 1);
	return 16;
}

int alignUp(int n, int a) {
	if (a <= 1) return n;
	return (n + a - 1) / a * a;
}

} // namespace :tm:

bool _cdecl llvm_emit_object(const std::string& ir, std::string& obj, std::string& err) {
	return emitObject(ir, obj, err);
}

bool _cdecl llvm_object_to_module(const std::string& obj, Module* mod, std::string& err) {
	err.clear();
	if (!mod) { err = "no output module"; return false; }
	if (obj.size() < sizeof(IMAGE_FILE_HEADER)) { err = "object file is too small"; return false; }

	const IMAGE_FILE_HEADER* fh = (const IMAGE_FILE_HEADER*)obj.data();
	if (fh->Machine != IMAGE_FILE_MACHINE_I386) { err = "object file is not 32-bit x86"; return false; }

	size_t headerEnd = sizeof(IMAGE_FILE_HEADER) + fh->SizeOfOptionalHeader;
	if (!rangeOk(headerEnd, (size_t)fh->NumberOfSections * sizeof(IMAGE_SECTION_HEADER), obj.size())) {
		err = "object file has a corrupt section table";
		return false;
	}
	const IMAGE_SECTION_HEADER* secs = (const IMAGE_SECTION_HEADER*)(obj.data() + headerEnd);

	size_t symTableOff = fh->PointerToSymbolTable;
	size_t symCount = fh->NumberOfSymbols;
	if (symCount && !rangeOk(symTableOff, symCount * sizeof(IMAGE_SYMBOL), obj.size())) {
		err = "object file has a corrupt symbol table";
		return false;
	}

	const char* strTable = 0;
	size_t strSize = 0;
	if (symCount) {
		size_t strOff = symTableOff + symCount * sizeof(IMAGE_SYMBOL);
		if (rangeOk(strOff, 4, obj.size())) {
			uint32_t sz = 0;
			memcpy(&sz, obj.data() + strOff, 4);
			if (sz >= 4 && rangeOk(strOff, sz, obj.size())) {
				strTable = obj.data() + strOff;
				strSize = sz;
			}
		}
	}

	std::vector<Symbol> symbols(symCount);
	for (size_t i = 0; i < symCount; ) {
		const IMAGE_SYMBOL* sym = (const IMAGE_SYMBOL*)(obj.data() + symTableOff + i * sizeof(IMAGE_SYMBOL));
		symbols[i].name = symbolName(sym, strTable, strSize);
		symbols[i].section = sym->SectionNumber;
		symbols[i].value = sym->Value;

		size_t aux = sym->NumberOfAuxSymbols;
		for (size_t k = 1; k <= aux && i + k < symCount; ++k) {
			symbols[i + k].name.clear();
			symbols[i + k].section = 0;
			symbols[i + k].value = 0;
		}
		i += 1 + aux;
	}

	std::vector<Section> sections(fh->NumberOfSections);
	int pos = 5;
	for (int i = 0; i < fh->NumberOfSections; ++i) {
		const IMAGE_SECTION_HEADER& h = secs[i];
		uint32_t c = h.Characteristics;

		Section s;
		s.index = i;
		s.included = false;
		s.align = sectionAlign(c);
		s.blobSize = (int)h.SizeOfRawData;
		if (s.blobSize == 0) s.blobSize = (int)h.Misc.VirtualSize;

		bool discard = (c & (IMAGE_SCN_LNK_INFO | IMAGE_SCN_LNK_REMOVE | IMAGE_SCN_MEM_DISCARDABLE)) != 0;
		bool data = (c & (IMAGE_SCN_CNT_CODE | IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_CNT_UNINITIALIZED_DATA)) != 0;
		if (!discard && data && s.blobSize > 0) {
			pos = alignUp(pos, s.align);
			s.included = true;
			s.blobBase = pos;
			pos += s.blobSize;
		}
		else {
			s.blobBase = 0;
		}
		sections[i] = s;
	}

	if (pos <= 5 || pos > INT_MAX) { err = "object file contains no usable data"; return false; }

	std::string blob((size_t)pos, '\0');
	for (size_t i = 0; i < sections.size(); ++i) {
		Section& s = sections[i];
		if (!s.included) continue;
		const IMAGE_SECTION_HEADER& h = secs[i];
		size_t copy = h.SizeOfRawData;
		if (copy > (size_t)s.blobSize) copy = (size_t)s.blobSize;
		if (copy && rangeOk(h.PointerToRawData, copy, obj.size())) {
			memcpy(&blob[s.blobBase], obj.data() + h.PointerToRawData, copy);
		}
	}

	int mainOffset = -1;
	for (size_t i = 0; i < symbols.size(); ++i) {
		const Symbol& sym = symbols[i];
		if (sym.name.empty() || sym.section <= 0 || sym.section > fh->NumberOfSections) continue;
		const Section& s = sections[sym.section - 1];
		if (!s.included) continue;
		if (demangle(sym.name) == "__MAIN") mainOffset = s.blobBase + (int)sym.value;
	}
	if (mainOffset < 0) { err = "object file has no __MAIN function"; return false; }

	// __MAIN is rarely at offset 0 if ever, and runtime is always at bolb base
	blob[0] = (char)0xE9;
	int32_t disp = mainOffset - 5;
	memcpy(&blob[1], &disp, 4);
	bool wroteSymbols = false;
	std::set<std::string> added;
	for (size_t i = 0; i < symbols.size(); ++i) {
		const Symbol& sym = symbols[i];
		if (sym.name.empty() || sym.section <= 0 || sym.section > fh->NumberOfSections) continue;
		const Section& s = sections[sym.section - 1];
		if (!s.included) continue;

		std::string name = demangle(sym.name);
		if (name.empty() || added.count(name)) continue;

		int off = s.blobBase + (int)sym.value;
		if (off < 0 || off >= (int)blob.size()) continue;

		added.insert(name);
		mod->addSymbol(name.c_str(), off);
		wroteSymbols = true;
	}
	if (!wroteSymbols) { err = "object file has no usable symbols"; return false; }

	bool wroteRelocs = false;
	for (size_t i = 0; i < sections.size(); ++i) {
		Section& s = sections[i];
		if (!s.included) continue;
		const IMAGE_SECTION_HEADER& h = secs[i];

		size_t count = h.NumberOfRelocations;
		size_t relOff = h.PointerToRelocations;
		if (h.Characteristics & IMAGE_SCN_LNK_NRELOC_OVFL) {
			if (!rangeOk(relOff, sizeof(IMAGE_RELOCATION), obj.size())) {
				err = "object file has a corrupt relocation table";
				return false;
			}
			const IMAGE_RELOCATION* first = (const IMAGE_RELOCATION*)(obj.data() + relOff);
			count = first->VirtualAddress;
			relOff += sizeof(IMAGE_RELOCATION);
		}
		if (!count) continue;
		if (!rangeOk(relOff, count * sizeof(IMAGE_RELOCATION), obj.size())) {
			err = "object file has a corrupt relocation table";
			return false;
		}
		size_t avail = (obj.size() - relOff) / sizeof(IMAGE_RELOCATION);
		if (count > avail) count = avail;

		for (size_t k = 0; k < count; ++k) {
			const IMAGE_RELOCATION* rel = (const IMAGE_RELOCATION*)(obj.data() + relOff + k * sizeof(IMAGE_RELOCATION));
			if (rel->SymbolTableIndex >= symbols.size()) {
				err = "object file has a relocation with a bad symbol index";
				return false;
			}

			int type = (int)rel->Type;
			if (type == IMAGE_REL_I386_ABSOLUTE) continue;
			if (type != IMAGE_REL_I386_REL32 && type != IMAGE_REL_I386_DIR32) {
				err = "unsupported 32-bit x86 relocation type " + std::to_string(type);
				return false;
			}

			size_t field = (size_t)s.blobBase + rel->VirtualAddress;
			if (!rangeOk(field, 4, blob.size())) {
				err = "object file has a relocation outside of its data";
				return false;
			}

			bool pcrel = type == IMAGE_REL_I386_REL32;
			if (pcrel) {
				int32_t addend = 0;
				memcpy(&addend, &blob[field], 4);
				addend -= 4;
				memcpy(&blob[field], &addend, 4);
			}

			const Symbol& target = symbols[rel->SymbolTableIndex];
			std::string name = demangle(target.name);
			if (name.empty()) {
				err = "object file has a relocation to an unnamed symbol";
				return false;
			}
			if (target.section > 0 && target.section <= fh->NumberOfSections) {
				const Section& ts = sections[target.section - 1];
				if (!ts.included) {
					err = "object file has a relocation to a discarded section";
					return false;
				}
			}

			mod->addReloc(name.c_str(), (int)field, pcrel);
			wroteRelocs = true;
		}
	}
	(void)wroteRelocs;

	mod->emitx(&blob[0], (int)blob.size());
	return true;
}
