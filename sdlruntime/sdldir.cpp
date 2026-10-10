#include "std.h"
#include "sdldir.h"

sdlDir::sdlDir(HANDLE h, const WIN32_FIND_DATA& f) :handle(h), findData(f) {
}

sdlDir::~sdlDir() {
	if(handle != INVALID_HANDLE_VALUE) FindClose(handle);
}

std::string sdlDir::getNextFile() {
	if(handle == INVALID_HANDLE_VALUE) return "";
	std::string t = findData.cFileName;
	if(!FindNextFile(handle, &findData)) {
		FindClose(handle);
		handle = INVALID_HANDLE_VALUE;
	}
	return t;
}