#include "std.h"
#include "sdlfilesystem.h"

static std::set<sdlDir*> dir_set;

sdlFileSystem::sdlFileSystem() {
	dir_set.clear();
}

sdlFileSystem::~sdlFileSystem() {
	while(dir_set.size()) closeDir(*dir_set.begin());
}

bool sdlFileSystem::createDir(const std::string& dir) {
	return CreateDirectory(dir.c_str(), 0) ? true : false;
}

bool sdlFileSystem::deleteDir(const std::string& dir) {
	return RemoveDirectory(dir.c_str()) ? true : false;
}

bool sdlFileSystem::createFile(const std::string& file) {
	HANDLE h = CreateFile(file.c_str(),
		GENERIC_ALL,
		0,
		0,
		CREATE_NEW,
		FILE_ATTRIBUTE_NORMAL,
		0);
	if(h) {
		CloseHandle(h);
		return true;
	}
	else return false;
}

bool sdlFileSystem::deleteFile(const std::string& file) {
	return DeleteFile(file.c_str()) ? true : false;
}

bool sdlFileSystem::copyFile(const std::string& src, const std::string& dest) {
	return CopyFile(src.c_str(), dest.c_str(), false) ? true : false;
}

bool sdlFileSystem::renameFile(const std::string& src, const std::string& dest) {
	return MoveFile(src.c_str(), dest.c_str()) ? true : false;
}

bool sdlFileSystem::setCurrentDir(const std::string& dir) {
	return SetCurrentDirectory(dir.c_str()) ? true : false;
}

std::string sdlFileSystem::getCurrentDir()const {
	char buff[MAX_PATH];
	if(!GetCurrentDirectory(MAX_PATH, buff)) return "";
	std::string t = buff; if(t.size() && t[t.size() - 1] != '\\') t += '\\';
	return t;
}

int sdlFileSystem::getFileSize(const std::string& name)const {
	return std::filesystem::exists(name) ? std::filesystem::file_size(name) : 0;
}

int sdlFileSystem::getFileType(const std::string& name)const {
	DWORD t = GetFileAttributes(name.c_str());
	return t == -1 ? FILE_TYPE_NONE :
		(t & FILE_ATTRIBUTE_DIRECTORY ? FILE_TYPE_DIR : FILE_TYPE_FILE);
}

sdlDir* sdlFileSystem::openDir(const std::string& name, int flags) {
	std::string t = name;
	if(t[t.size() - 1] == '\\') t += "*";
	else t += "\\*";
	WIN32_FIND_DATA f;
	HANDLE h = FindFirstFile(t.c_str(), &f);
	if(h != INVALID_HANDLE_VALUE) {
		sdlDir* d = new sdlDir(h, f);
		dir_set.insert(d);
		return d;
	}
	return 0;
}
sdlDir* sdlFileSystem::verifyDir(sdlDir* d) {
	return dir_set.count(d) ? d : 0;
}

void sdlFileSystem::closeDir(sdlDir* d) {
	if(dir_set.erase(d)) delete d;
}