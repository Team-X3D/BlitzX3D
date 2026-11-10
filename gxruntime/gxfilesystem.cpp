#include "std.h"
#include "gxfilesystem.h"

static std::set<gxDir*> dir_set;

gxFileSystem::gxFileSystem() {
	dir_set.clear();
}

gxFileSystem::~gxFileSystem() {
	while(dir_set.size()) closeDir(*dir_set.begin());
}

bool gxFileSystem::createDir(const std::string& dir) {
	return CreateDirectory(dir.c_str(), 0) ? true : false;
}

bool gxFileSystem::deleteDir(const std::string& dir) {
	return RemoveDirectory(dir.c_str()) ? true : false;
}

bool gxFileSystem::createFile(const std::string& file) {
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

bool gxFileSystem::deleteFile(const std::string& file) {
	return DeleteFile(file.c_str()) ? true : false;
}

bool gxFileSystem::copyFile(const std::string& src, const std::string& dest) {
	return CopyFile(src.c_str(), dest.c_str(), false) ? true : false;
}

bool gxFileSystem::renameFile(const std::string& src, const std::string& dest) {
	return MoveFile(src.c_str(), dest.c_str()) ? true : false;
}

bool gxFileSystem::setCurrentDir(const std::string& dir) {
	return SetCurrentDirectory(dir.c_str()) ? true : false;
}

std::string gxFileSystem::getCurrentDir()const {
	DWORD size = GetCurrentDirectoryA(0, 0);
	if(!size) return "";
	std::string t(size, '\0');
	DWORD n = GetCurrentDirectoryA(size, t.data());
	if(!n || n >= size) return "";
	t.resize(n);
	if(t.size() && t[t.size() - 1] != '\\') t += '\\';
	return t;
}

int gxFileSystem::getFileSize(const std::string& name)const {
	return std::filesystem::exists(name) ? std::filesystem::file_size(name) : 0;
}

int gxFileSystem::getFileType(const std::string& name)const {
	DWORD t = GetFileAttributes(name.c_str());
	return t == -1 ? FILE_TYPE_NONE :
		(t & FILE_ATTRIBUTE_DIRECTORY ? FILE_TYPE_DIR : FILE_TYPE_FILE);
}

gxDir* gxFileSystem::openDir(const std::string& name, int flags) {
	std::string t = name;
	if(t[t.size() - 1] == '\\') t += "*";
	else t += "\\*";
	WIN32_FIND_DATA f;
	HANDLE h = FindFirstFile(t.c_str(), &f);
	if(h != INVALID_HANDLE_VALUE) {
		gxDir* d = new gxDir(h, f);
		dir_set.insert(d);
		return d;
	}
	return 0;
}
gxDir* gxFileSystem::verifyDir(gxDir* d) {
	return dir_set.count(d) ? d : 0;
}

void gxFileSystem::closeDir(gxDir* d) {
	if(dir_set.erase(d)) delete d;
}