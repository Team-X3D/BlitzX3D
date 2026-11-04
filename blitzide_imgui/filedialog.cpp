#include "filedialog.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <commdlg.h>
#else
#include <unistd.h>
#endif

static std::vector<std::string> splitFilter(const char* filter) {
	std::vector<std::string> parts;
	if (!filter || !*filter) filter = "All files (*.*)|*.*";
	std::string cur;
	for (const char* p = filter;; ++p) {
		if (*p == '|' || *p == '\0') {
			parts.push_back(cur);
			cur.clear();
			if (*p == '\0') break;
		}
		else {
			cur.push_back(*p);
		}
	}
	return parts;
}

#if defined(_WIN32)
static std::string winFilterString(const char* filter) {
	std::string out;
	for (const std::string& part : splitFilter(filter)) {
		out += part;
		out.push_back('\0');
	}
	out.push_back('\0');
	return out;
}

static std::string defExtFromFilter(const char* filter) {
	auto parts = splitFilter(filter);
	for (size_t i = 1; i < parts.size(); i += 2) {
		const std::string& pat = parts[i];
		size_t dot = pat.find("*.");
		if (dot != std::string::npos) {
			size_t end = pat.find(';', dot);
			if (end == std::string::npos) end = pat.size();
			return pat.substr(dot + 2, end - dot - 2);
		}
	}
	return "";
}
#else
static std::string zenityFilterArgs(const char* filter) {
	auto parts = splitFilter(filter);
	std::string out;
	for (size_t i = 0; i + 1 < parts.size(); i += 2) {
		out += " --file-filter='";
		out += parts[i];
		out += " | ";
		for (char c : parts[i + 1]) out += (c == ';') ? ' ' : c;
		out += "'";
	}
	return out;
}
#endif

bool fileOpenDialog(std::string& path, const char* filter) {
#if defined(_WIN32)
	OPENFILENAMEA ofn = { sizeof(ofn) };
	std::vector<char> buf(32768, 0);
	std::string f = winFilterString(filter);
	ofn.lpstrFilter = f.c_str();
	ofn.lpstrFile = buf.data();
	ofn.nMaxFile = (DWORD)buf.size();
	ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
	if (!GetOpenFileNameA(&ofn)) return false;
	path = buf.data();
	return true;
#else
	std::string cmd = "zenity --file-selection" + zenityFilterArgs(filter) + " 2>/dev/null";
	std::string out;
	FILE* p = popen(cmd.c_str(), "r");
	if (!p) return false;
	char buf[4096];
	size_t n = fread(buf, 1, sizeof(buf) - 1, p);
	buf[n] = 0;
	int rc = pclose(p);
	if (rc != 0 || n == 0) return false;
	std::string s(buf);
	while (s.size() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
	if (s.empty()) return false;
	path = s;
	return true;
#endif
}

bool fileSaveDialog(std::string& path, const char* defaultName, const char* filter) {
#if defined(_WIN32)
	OPENFILENAMEA ofn = { sizeof(ofn) };
	std::vector<char> buf(32768, 0);
	if (defaultName) strncpy_s(buf.data(), buf.size(), defaultName, _TRUNCATE);
	std::string f = winFilterString(filter);
	std::string defExt = defExtFromFilter(filter);
	ofn.lpstrFilter = f.c_str();
	ofn.lpstrDefExt = defExt.empty() ? NULL : defExt.c_str();
	ofn.lpstrFile = buf.data();
	ofn.nMaxFile = (DWORD)buf.size();
	ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
	if (!GetSaveFileNameA(&ofn)) return false;
	path = buf.data();
	return true;
#else
	std::string cmd = "zenity --file-selection --save --confirm-overwrite" + zenityFilterArgs(filter) + " 2>/dev/null";
	if (defaultName && *defaultName) {
		cmd += " --filename='" + std::string(defaultName) + "'";
	}
	std::string out;
	FILE* p = popen(cmd.c_str(), "r");
	if (!p) return false;
	char buf[4096];
	size_t n = fread(buf, 1, sizeof(buf) - 1, p);
	buf[n] = 0;
	int rc = pclose(p);
	if (rc != 0 || n == 0) return false;
	std::string s(buf);
	while (s.size() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
	if (s.empty()) return false;
	path = s;
	return true;
#endif
}
