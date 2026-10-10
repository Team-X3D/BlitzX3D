#include "std.h"
#include "bbsys.h"

bool debug;
sdlRuntime* sdl_runtime;
const char* errorfunc = "";
const char* errorlog = "";

static const int BB_RELEASE_STACK_CAP = 64;
static thread_local const char* bbReleaseStack[BB_RELEASE_STACK_CAP];
static thread_local int bbReleaseStackDepth = 0;
static thread_local int bbReleaseStmtPos = 0;
static thread_local const char* bbReleaseStmtFile = nullptr;

void _bbReleaseEnter(const char* func) {
	if (bbReleaseStackDepth < BB_RELEASE_STACK_CAP) {
		bbReleaseStack[bbReleaseStackDepth++] = func;
	}
}

void _bbReleaseLeave() {
	if (bbReleaseStackDepth > 0) {
		--bbReleaseStackDepth;
	}
}

void _bbReleaseStmt(int pos, const char* file) {
	bbReleaseStmtPos = pos;
	bbReleaseStmtFile = file;
}

void bbReleaseReset() {
	bbReleaseStackDepth = 0;
	bbReleaseStmtPos = 0;
	bbReleaseStmtFile = nullptr;
}

int bbReleaseDepth() {
	return bbReleaseStackDepth;
}

const char* bbReleaseFuncAt(int depthIndex) {
	if (depthIndex < 0 || depthIndex >= bbReleaseStackDepth) return nullptr;
	return bbReleaseStack[depthIndex];
}

int bbReleasePos() {
	return bbReleaseStmtPos;
}

const char* bbReleaseFile() {
	return bbReleaseStmtFile;
}

std::string bbReleaseCrashReport(const char* msg) {
	std::string s = msg ? msg : "";
	s += "\r\n";
	if (bbReleaseStmtFile && bbReleaseStmtFile[0]) {
		int row = (bbReleaseStmtPos >> 16) & 0xffff, col = bbReleaseStmtPos & 0xffff;
		s += "\r\nLocation: ";
		s += bbReleaseStmtFile;
		s += " (line ";
		s += std::to_string(row + 1);
		s += ", col ";
		s += std::to_string(col + 1);
		s += ")\r\n";
	}
	if (bbReleaseStackDepth > 0) {
		s += "\r\nCall stack (innermost first):\r\n";
		for (int i = bbReleaseStackDepth - 1; i >= 0; --i) {
			const char* f = bbReleaseStack[i];
			if (!f || !f[0]) continue;
			s += "  ";
			s += f;
			s += "\r\n";
		}
	}
	return s;
}