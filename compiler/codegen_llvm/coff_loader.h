#ifndef COFF_LOADER_H
#define COFF_LOADER_H

#include <string>

typedef bool (*LLVMSymResolver)(const char* name, int* addr, void* ctx);

void* llvm_load_module(const std::string& ir, LLVMSymResolver resolver, void* ctx, std::string& err);

#endif
