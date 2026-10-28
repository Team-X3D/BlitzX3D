#include "../std.h"
#include "llvm_jit.h"

#include "llvm-c/Core.h"
#include "llvm-c/Error.h"
#include "llvm-c/IRReader.h"
#include "llvm-c/LLJIT.h"
#include "llvm-c/Orc.h"
#include "llvm-c/Target.h"

namespace {

struct JitCtx {
	LLVMSymResolver resolver;
	void* user;
};

// the decorated i386 names have a lead underscore and an stdcall @N suffix
// why x86 does this on everything is a mystery! (probably not, but i can't be arsed) 
// we can just strip it to match runtime names
std::string demangle(const std::string& n) {
	std::string s = n;
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

std::string takeError(LLVMErrorRef e) {
	if (!e) return "";
	char* msg = LLVMGetErrorMessage(e);
	std::string s = msg ? msg : "unknown LLVM error";
	LLVMDisposeErrorMessage(msg);
	return s;
}

LLVMErrorRef resolveGenerator(LLVMOrcDefinitionGeneratorRef, void* ctx, LLVMOrcLookupStateRef*,
	LLVMOrcLookupKind, LLVMOrcJITDylibRef jd, LLVMOrcJITDylibLookupFlags,
	LLVMOrcCLookupSet set, size_t setSize) {

	JitCtx* c = (JitCtx*)ctx;
	std::vector<LLVMOrcCSymbolMapPair> pairs;
	for (size_t k = 0; k < setSize; ++k) {
		LLVMOrcSymbolStringPoolEntryRef sym = set[k].Name;
		const char* raw = LLVMOrcSymbolStringPoolEntryStr(sym);
		int addr = 0;
		if (!c->resolver(demangle(raw ? raw : "").c_str(), &addr, c->user)) continue;

		LLVMOrcCSymbolMapPair pair;
		pair.Name = sym;
		pair.Sym.Address = (LLVMOrcExecutorAddress)(uintptr_t)addr;
		pair.Sym.Flags.GenericFlags = (uint8_t)LLVMJITSymbolGenericFlagsExported;
		pair.Sym.Flags.TargetFlags = 0;
		pairs.push_back(pair);
	}
	if (pairs.empty()) return LLVMErrorSuccess;

	LLVMOrcMaterializationUnitRef mu = LLVMOrcAbsoluteSymbols(pairs.data(), pairs.size());
	return LLVMOrcJITDylibDefine(jd, mu);
}

}

void* _cdecl llvm_load_module(const std::string& ir, LLVMSymResolver resolver, void* ctx, std::string& err) {
	err.clear();

	static bool inited = false;
	if (!inited) {
		LLVMInitializeX86TargetInfo();
		LLVMInitializeX86Target();
		LLVMInitializeX86TargetMC();
		LLVMInitializeX86AsmPrinter();
		LLVMInitializeX86AsmParser();
		inited = true;
	}

	LLVMContextRef context = LLVMContextCreate();
	LLVMMemoryBufferRef buf = LLVMCreateMemoryBufferWithMemoryRangeCopy(ir.data(), ir.size(), "bb");
	LLVMModuleRef mod = 0;
	char* perr = 0;
	if (LLVMParseIRInContext(context, buf, &mod, &perr)) {
		err = perr ? perr : "failed to parse IR";
		if (perr) LLVMDisposeMessage(perr);
		LLVMContextDispose(context);
		return 0;
	}

	LLVMOrcLLJITRef jit = 0;
	LLVMErrorRef e = LLVMOrcCreateLLJIT(&jit, 0);
	if (e) {
		err = takeError(e);
		LLVMDisposeModule(mod);
		LLVMContextDispose(context);
		return 0;
	}

	LLVMSetTarget(mod, "i386-pc-windows-msvc");
	LLVMSetDataLayout(mod, LLVMOrcLLJITGetDataLayoutStr(jit));

	JitCtx* jc = new JitCtx();
	jc->resolver = resolver;
	jc->user = ctx;

	LLVMOrcJITDylibRef jd = LLVMOrcLLJITGetMainJITDylib(jit);
	LLVMOrcDefinitionGeneratorRef gen = LLVMOrcCreateCustomCAPIDefinitionGenerator(resolveGenerator, jc, 0);
	LLVMOrcJITDylibAddGenerator(jd, gen);

	LLVMOrcThreadSafeContextRef tsc = LLVMOrcCreateNewThreadSafeContextFromLLVMContext(context);
	LLVMOrcThreadSafeModuleRef tsm = LLVMOrcCreateNewThreadSafeModule(mod, tsc);
	e = LLVMOrcLLJITAddLLVMIRModule(jit, jd, tsm);
	if (e) { err = takeError(e); return 0; }

	LLVMOrcExecutorAddress addr = 0;
	e = LLVMOrcLLJITLookup(jit, &addr, "__MAIN");
	if (e) { err = takeError(e); return 0; }

	return (void*)(uintptr_t)addr;
}
