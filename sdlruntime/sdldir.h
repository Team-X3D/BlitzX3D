#ifndef SDLDIR_H
#define SDLDIR_H

#include <string>
#include <Windows.h>

class sdlDir {
public:
	sdlDir(HANDLE h, const WIN32_FIND_DATA& f);
	~sdlDir();

private:
	HANDLE handle;
	WIN32_FIND_DATA findData;

	/***** GX INTERFACE *****/
public:
	std::string getNextFile();
};

#endif