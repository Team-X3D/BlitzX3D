#ifndef BBSYS_H
#define BBSYS_H

#include "basic.h"
#include "../gxruntime/gxruntime.h"

#include <string>

extern bool debug;
extern gxRuntime* gx_runtime;
extern const char* errorfunc;
extern const char* errorlog;

bool angel_is_executing();

void _bbReleaseEnter(const char* func);
void _bbReleaseLeave();
void _bbReleaseStmt(int pos, const char* file);
void bbReleaseReset();
std::string bbReleaseCrashReport(const char* msg);
int bbReleaseDepth();
const char* bbReleaseFuncAt(int depthIndex);
int bbReleasePos();
const char* bbReleaseFile();

struct bbEx {
	const char* err;
	bbEx(const char* e) : err(e) {
		if (e && !angel_is_executing()) gx_runtime->debugError(e);
	}
};

#define RTEX( _X_ ) throw bbEx( _X_ );

#endif