#ifndef LLVM_OBJ_H
#define LLVM_OBJ_H

#include <string>

class Module;

bool _cdecl llvm_emit_object(const std::string& ir, std::string& obj, std::string& err);
bool _cdecl llvm_object_to_module(const std::string& obj, Module* mod, std::string& err);

#endif
