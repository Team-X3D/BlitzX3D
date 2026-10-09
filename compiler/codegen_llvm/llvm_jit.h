#ifndef LLVM_JIT_H
#define LLVM_JIT_H

#include <string>

typedef bool (_cdecl *LLVMSymResolver)(const char* name, int* addr, void* ctx);

void* _cdecl llvm_load_module(const std::string& ir, LLVMSymResolver resolver, void* ctx, std::string& err);

#endif
