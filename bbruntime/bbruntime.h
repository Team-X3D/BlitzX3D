/*
Platform neutral runtime library.
To be statically linked with an appropriate sdlruntime driver.
*/

#ifndef BBRUNTIME_H
#define BBRUNTIME_H

//My friend Kat Purpy gave me this macro and I tweaked it a bit to fit B3D.
#define INIT(thing) if(!thing##_create()) {sue(#thing "_create() failed!"); return false;}

#include "../sdlruntime/sdlruntime.h"

void bbruntime_link(void (*rtSym)(const char* sym, void* pc));
const char* bbruntime_run(sdlRuntime* runtime, void (*pc)(), bool debug);
void bbruntime_panic(const wchar_t* err);

void bbSetExceptionHandler(void* handler);
void bbClearExceptionHandler();
bool bbCallExceptionHandler(const char* message);

class ErrorMessagePool {
public:
	static std::string* memoryAccessViolation;
	static int size;
	static bool hasMacro;
};

#endif