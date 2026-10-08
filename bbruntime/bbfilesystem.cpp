#include "std.h"
#include "bbfilesystem.h"
#include "bbstream.h"
#include <fstream>
#include "../MultiLang/MultiLang.h"
#include "unzip.h"

gxFileSystem* gx_filesys;

struct bbFile : public bbStream {
	std::filebuf* buf;
	bbFile(std::filebuf* f) :buf(f) {
	}
	~bbFile() {
		delete buf;
	}
	int read(char* buff, int size) {
		return buf->sgetn((char*)buff, size);
	}
	int write(const char* buff, int size) {
		return buf->sputn((char*)buff, size);
	}
	int avail() {
		return buf->in_avail();
	}
	int eof() {
		return buf->sgetc() == EOF;
	}
	void flush() {
		buf->pubsync();
	}
};

static std::set<bbFile*> file_set;

static inline void debugFile(bbFile* f, const char* function) {
	if (!file_set.count(f)) {
		ErrorLog(function, MultiLang::file_not_exist);
	}
}

static inline void debugDir(gxDir* d, const char* function) {
	if (!gx_filesys->verifyDir(d)) {
		// durhh...... only log fatal eerorrs in debug builds... i think i'm smarht!
		errorfunc = function;
		errorlog = MultiLang::directory_not_exist;
		RTEX(MultiLang::directory_not_exist);
	}
}

static bbFile* open(BBStr* f, int n) {
	std::string t = *f;
	std::filebuf* buf = new std::filebuf();
	if (buf->open(t.c_str(), n | std::ios_base::binary)) {
		bbFile* fl = new bbFile(buf);
		file_set.insert(fl);
		return fl;
	}
	delete buf;
	return 0;
}

bbFile* bbReadFile(BBStr* f) {
	return open(f, std::ios_base::in);
}

bbFile* bbWriteFile(BBStr* f) {
	return open(f, std::ios_base::out | std::ios_base::trunc);
}

bbFile* bbOpenFile(BBStr* f) {
	return open(f, std::ios_base::in | std::ios_base::out);
}

void bbCloseFile(bbFile* f) {
	debugFile(f, "CloseFile");
	file_set.erase(f);
	delete f;
}

int bbFilePos(bbFile* f) {
	return f->buf->pubseekoff(0, std::ios_base::cur);
}

int bbSeekFile(bbFile* f, int pos) {
	return f->buf->pubseekoff(pos, std::ios_base::beg);
}

gxDir* bbReadDir(BBStr* d) {
	std::string t = *d; delete d;
	return gx_filesys->openDir(t, 0);
}

void bbCloseDir(gxDir* d) {
	gx_filesys->closeDir(d);
}

BBStr* bbNextFile(gxDir* d) {
	debugDir(d, "NextFile");
	return new BBStr(d->getNextFile());
}

BBStr* bbCurrentDir() {
	return new BBStr(gx_filesys->getCurrentDir());
}

void bbChangeDir(BBStr* d) {
	gx_filesys->setCurrentDir(*d);
	delete d;
}

void bbCreateDir(BBStr* d) {
	gx_filesys->createDir(*d);
	delete d;
}

void bbDeleteDir(BBStr* d) {
	gx_filesys->deleteDir(*d);
	delete d;
}

inline bool IsDirectory(const char* pDir)
{
	char szCurPath[500];
	ZeroMemory(szCurPath, 500);
	sprintf_s(szCurPath, 500, "%s//*", pDir);
	WIN32_FIND_DATAA FindFileData;
	ZeroMemory(&FindFileData, sizeof(WIN32_FIND_DATAA));

	HANDLE hFile = FindFirstFileA(szCurPath, &FindFileData);

	if (hFile == INVALID_HANDLE_VALUE)
	{
		FindClose(hFile);
		return false;
	}
	else
	{
		FindClose(hFile);
		return true;
	}
}

void bbDeleteFolder(BBStr* d) {
	char szCurPath[MAX_PATH];
	_snprintf(szCurPath, MAX_PATH, "%s//*.*", d->c_str());
	WIN32_FIND_DATAA FindFileData;
	ZeroMemory(&FindFileData, sizeof(WIN32_FIND_DATAA));
	HANDLE hFile = FindFirstFileA(szCurPath, &FindFileData);
	bool IsFinded = true;
	while (IsFinded)
	{
		IsFinded = FindNextFile(hFile, &FindFileData);
		if (strcmp(FindFileData.cFileName, ".") && strcmp(FindFileData.cFileName, ".."))
		{
			std::string strFileName = "";
			strFileName = strFileName + d->c_str() + "//"s + FindFileData.cFileName;
			std::string strTemp;
			strTemp = strFileName;
			if (IsDirectory(strFileName.c_str()))
			{
				bbDeleteFolder(new BBStr(strTemp));
			}
			else
			{
				DeleteFile(strTemp.c_str());
			}
		}
	}
	FindClose(hFile);

	RemoveDirectory(d->c_str());
	delete d;
}

int bbFileType(BBStr* f) {
	std::string t = *f; delete f;
	int n = gx_filesys->getFileType(t);
	return n == gxFileSystem::FILE_TYPE_FILE ? 1 : (n == gxFileSystem::FILE_TYPE_DIR ? 2 : 0);
}

int	bbFileSize(BBStr* f) {
	std::string t = *f; delete f;
	return gx_filesys->getFileSize(t);
}

int	bbFileTime(BBStr* f) {
	std::string path = *f; delete f;
	WIN32_FILE_ATTRIBUTE_DATA data;
	if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data)) return 0;
	ULARGE_INTEGER t;
	t.LowPart = data.ftLastWriteTime.dwLowDateTime;
	t.HighPart = data.ftLastWriteTime.dwHighDateTime;
	return (int)((t.QuadPart - 116444736000000000ULL) / 10000000ULL);
}

BBStr* bbFileExtension(BBStr* f) {
	std::string t = *f; delete f;
	if (t.find_last_of(".") != std::string::npos) {
		return new BBStr(t.substr(t.find_last_of(".") + 1));
	}
	return new BBStr("");
}

void bbCopyFile(BBStr* f, BBStr* to) {
	std::string src = *f, dest = *to;
	delete f; delete to;
	gx_filesys->copyFile(src, dest);
}

void bbCreateFile(BBStr* f) {
	std::string fn = *f; delete f;
	gx_filesys->createFile(fn);
}

void bbDeleteFile(BBStr* f) {
	gx_filesys->deleteFile(*f);
	delete f;
}

void bbUnzip(BBStr* src, BBStr* dst, BBStr* password) {
	HZIP hz = OpenZip(src->c_str(), password->c_str());
	SetUnzipBaseDir(hz, dst->c_str());
	ZIPENTRY ze;
	GetZipItem(hz, -1, &ze);
	int numitems = ze.index; // no register :(
	for (int zi = 0; zi < numitems; zi++)
	{
		GetZipItem(hz, zi, &ze);
		UnzipItem(hz, zi, ze.name);
	}
	CloseZip(hz);
	delete src; delete dst; delete password;
}

BBStr* bbAbsolutePath(BBStr* path) {
	std::string file = path->c_str();
	delete path;
	return new BBStr(std::filesystem::absolute(file).generic_string());
}

bool filesystem_create() {
	if (gx_filesys = gx_runtime->openFileSystem(0)) {
		return true;
	}
	return false;
}

bool filesystem_destroy() {
	while (file_set.size()) bbCloseFile(*file_set.begin());
	gx_runtime->closeFileSystem(gx_filesys);
	return true;
}

void filesystem_link(void(*rtSym)(const char*, void*)) {
	rtSym("%OpenFile$filename", bbOpenFile);
	rtSym("%ReadFile$filename", bbReadFile);
	rtSym("%WriteFile$filename", bbWriteFile);
	rtSym("CloseFile%file_stream", bbCloseFile);
	rtSym("%FilePos%file_stream", bbFilePos);
	rtSym("%SeekFile%file_stream%pos", bbSeekFile);

	rtSym("%ReadDir$dirname", bbReadDir);
	rtSym("CloseDir%dir", bbCloseDir);
	rtSym("$NextFile%dir", bbNextFile);
	rtSym("$CurrentDir", bbCurrentDir);
	rtSym("ChangeDir$dir", bbChangeDir);
	rtSym("CreateDir$dir", bbCreateDir);
	rtSym("DeleteDir$dir", bbDeleteDir);
	rtSym("DeleteFolder$dir", bbDeleteFolder);

	rtSym("%FileSize$file", bbFileSize);
	rtSym("%FileTime$file", bbFileTime);
	rtSym("%FileType$file", bbFileType);
	rtSym("$FileExtension$file", bbFileExtension);
	rtSym("CopyFile$file$to", bbCopyFile);
	rtSym("DeleteFile$file", bbDeleteFile);
	rtSym("CreateFile$filename", bbCreateFile);
	rtSym("$AbsolutePath$path", bbAbsolutePath);

	rtSym("Unzip$src$dst$password=\"\"", bbUnzip);
}