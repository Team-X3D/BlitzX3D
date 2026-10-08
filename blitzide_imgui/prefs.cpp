#include "prefs.h"

#include "../theme.h"
#include "../inipp/inipp.h"

#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <fstream>
#include <sstream>
#include <filesystem>

#include <SDL3/SDL.h>
#if !defined(_WIN32)
#include <unistd.h>
#include <climits>
#endif

Prefs prefs;

static std::string getExeDir() {
	const char* base = SDL_GetBasePath();
	if (!base) return "";
	std::string result(base);
	return result + "blitzide_imgui.exe";
}

static std::string resolveHomeDir() {
	const char* p = std::getenv("blitzpath");
	if (p && *p) return p;

	std::string exe = getExeDir();
	if (exe.empty()) return "";
	std::filesystem::path ep(exe);
	std::filesystem::path home = ep.parent_path().parent_path();
	std::string h = home.string();
	if (h.empty()) return "";
	return h;
}

static std::string resolveConfigDir() {
#if defined(_WIN32)
	const char* ad = std::getenv("APPDATA");
	if (ad && *ad) {
		std::string dir = std::string(ad) + "/BlitzX3D";
		std::filesystem::create_directories(dir);
		return dir;
	}
	const char* ud = std::getenv("USERPROFILE");
	if (ud && *ud) {
		std::string dir = std::string(ud) + "/AppData/Roaming/BlitzX3D";
		std::filesystem::create_directories(dir);
		return dir;
	}
#else
	const char* h = std::getenv("HOME");
	if (h && *h) {
		std::string dir = std::string(h) + "/.blitzx3d";
		std::filesystem::create_directories(dir);
		return dir;
	}
#endif
	return prefs.homeDir + "/cfg";
}

static std::string recentPathKey(const std::string& path) {
	try {
		return std::filesystem::weakly_canonical(path).lexically_normal().string();
	}
	catch (...) {
		return path;
	}
}

static void parseColor(const std::string& s, int* rgb) {
	rgb[0] = rgb[1] = rgb[2] = 0;
	sscanf(s.c_str(), "%d %d %d", &rgb[0], &rgb[1], &rgb[2]);
}

static std::string colorToString(const int* rgb) {
	char b[64];
	sprintf(b, "%d %d %d", rgb[0], rgb[1], rgb[2]);
	return b;
}

static std::string boolToString(bool value) {
	return value ? "true" : "false";
}

static std::string xmlElement(const std::string& text, const std::string& tag) {
	size_t pos = text.find("<" + tag);
	if (pos == std::string::npos) return "";
	size_t end = text.find('>', pos);
	if (end == std::string::npos) return "";
	return text.substr(pos, end - pos + 1);
}

static std::string xmlAttr(const std::string& elem, const std::string& name) {
	std::string needle = name + "=\"";
	size_t pos = 0;
	while ((pos = elem.find(needle, pos)) != std::string::npos) {
		char prev = pos > 0 ? elem[pos - 1] : ' ';
		bool boundary = !(std::isalnum((unsigned char)prev) || prev == '_');
		size_t start = pos + needle.size();
		size_t end = elem.find('"', start);
		if (boundary && end != std::string::npos) return elem.substr(start, end - start);
		pos = start;
	}
	return "";
}

static bool parseBoolValue(const std::string& s, bool def) {
	std::string t;
	for (char c : s) t += (char)std::tolower((unsigned char)c);
	if (t == "true" || t == "1" || t == "yes") return true;
	if (t == "false" || t == "0" || t == "no") return false;
	return def;
}

static void migrateLegacyConfig() {
	if (prefs.homeDir.empty() || prefs.configDir == prefs.homeDir + "/cfg") return;
	for (const char* name : { "blitzide.ini", "imgui.ini", "themes.ini" }) {
		std::filesystem::path src = std::filesystem::path(prefs.homeDir) / "cfg" / name;
		std::filesystem::path dst = std::filesystem::path(prefs.configDir) / name;
		if (!std::filesystem::exists(src)) continue;
		if (std::filesystem::exists(dst)) continue;
		std::error_code ec;
		std::filesystem::copy_file(src, dst, std::filesystem::copy_options::none, ec);
	}
}

void Prefs::open() {
	homeDir = resolveHomeDir();
	configDir = resolveConfigDir();

	migrateLegacyConfig();

	if (homeDir.empty()) {
		std::fprintf(stderr, "blitzpath environment variable not found!\n");
		return;
	}

	std::ifstream in((configDir + "/blitzide.ini").c_str(), std::ios::in);
	if (!in.good()) return;
	in.seekg(0, std::ios::end);
	if (in.tellg() == 0) { in.close(); return; }
	in.seekg(0, std::ios::beg);

	inipp::Ini<char> ini;
	ini.parse(in);
	in.close();

	inipp::get_value(ini.sections["COMPILER"], "Debug", prg_debug);
	inipp::get_value(ini.sections["COMPILER"], "NoLAA", prg_nolaa);
	inipp::get_value(ini.sections["COMPILER"], "NoAutoDecl", prg_noautodecl);
	inipp::get_value(ini.sections["COMPILER"], "Experimental", prg_experimental);
	inipp::get_value(ini.sections["COMPILER"], "DumpAsm", prg_dumpasm);
	inipp::get_value(ini.sections["COMPILER"], "Quiet", prg_quiet);
	inipp::get_value(ini.sections["COMPILER"], "VeryQuiet", prg_veryquiet);
	inipp::get_value(ini.sections["COMPILER"], "DumpKeys", prg_dumpkeys);
	inipp::get_value(ini.sections["COMPILER"], "LastBuild", prg_lastbuild);
	inipp::get_value(ini.sections["COMPILER"], "CommandLine", cmd_line);

	bool maximized = false, notoolbar = false;
	inipp::get_value(ini.sections["WINDOW"], "Maximized", maximized);
	inipp::get_value(ini.sections["WINDOW"], "NoToolbar", notoolbar);
	win_maximized = maximized;
	win_notoolbar = notoolbar;
	std::string rect;
	inipp::get_value(ini.sections["WINDOW"], "WindowRect", rect);
	sscanf(rect.c_str(), "%d %d %d %d", &win_x, &win_y, &win_w, &win_h);

	inipp::get_value(ini.sections["FONTS"], "EditorFont", font_editor);
	inipp::get_value(ini.sections["FONTS"], "EditorFontSize", font_editor_height);
	inipp::get_value(ini.sections["FONTS"], "TabsFont", font_tabs);
	inipp::get_value(ini.sections["FONTS"], "TabsFontSize", font_tabs_height);
	inipp::get_value(ini.sections["FONTS"], "DebugFont", font_debug);
	inipp::get_value(ini.sections["FONTS"], "DebugFontSize", font_debug_height);

	std::string c;
	inipp::get_value(ini.sections["COLORS"], "Background", c); parseColor(c, rgb_bkgrnd);
	inipp::get_value(ini.sections["COLORS"], "String", c); parseColor(c, rgb_string);
	inipp::get_value(ini.sections["COLORS"], "Ident", c); parseColor(c, rgb_ident);
	inipp::get_value(ini.sections["COLORS"], "Keyword", c); parseColor(c, rgb_keyword);
	inipp::get_value(ini.sections["COLORS"], "Comment", c); parseColor(c, rgb_comment);
	inipp::get_value(ini.sections["COLORS"], "Digit", c); parseColor(c, rgb_digit);
	inipp::get_value(ini.sections["COLORS"], "Default", c); parseColor(c, rgb_default);
	if (inipp::get_value(ini.sections["COLORS"], "Known", c)) parseColor(c, rgb_known);
	if (inipp::get_value(ini.sections["COLORS"], "Preproc", c)) parseColor(c, rgb_preproc);
	if (inipp::get_value(ini.sections["COLORS"], "Global", c)) parseColor(c, rgb_global);
	if (inipp::get_value(ini.sections["COLORS"], "Const", c)) parseColor(c, rgb_const);
	if (inipp::get_value(ini.sections["COLORS"], "Cursor", c)) parseColor(c, rgb_cursor);
	if (inipp::get_value(ini.sections["COLORS"], "Selection", c)) parseColor(c, rgb_selection);

	inipp::get_value(ini.sections["EDITOR"], "TabSpaces", edit_tabs);
	inipp::get_value(ini.sections["EDITOR"], "BackupCount", edit_backup);
	inipp::get_value(ini.sections["EDITOR"], "ToolbarImage", img_toolbar);
	inipp::get_value(ini.sections["EDITOR"], "NoBackup", noBackup);
	inipp::get_value(ini.sections["EDITOR"], "AutoComplete", edit_autocomplete);

	inipp::get_value(ini.sections["UI"], "Theme", theme);
	inipp::get_value(ini.sections["UI"], "Rounding", ui_rounding);
	inipp::get_value(ini.sections["UI"], "Alpha", ui_alpha);

	inipp::get_value(ini.sections["UPDATE"], "IgnoreVersion", ignore_version_update);

	recentFiles.clear();
	std::string recentFile;
	for (int i = 1; i < 11; ++i) {
		recentFile.clear();
		inipp::get_value(ini.sections["RECENT_FILES"], "File" + std::to_string(i), recentFile);
		if (recentFile.empty()) continue;
		const std::string recentKey = recentPathKey(recentFile);
		bool dup = false;
		for (const auto& existing : recentFiles) {
			const std::string existingKey = recentPathKey(existing);
			if (existingKey.size() != recentKey.size()) continue;
			bool same = true;
			for (size_t c = 0; c < existingKey.size(); ++c)
				if (std::tolower((unsigned char)existingKey[c]) != std::tolower((unsigned char)recentKey[c])) { same = false; break; }
			if (same) { dup = true; break; }
		}
		if (!dup) recentFiles.push_back(recentKey);
	}

	themeLoadUserThemes(configDir + "/themes.ini");
}

void Prefs::close() {
	if (configDir.empty()) return;
	themeSaveUserThemes(configDir + "/themes.ini");
	std::fstream out((configDir + "/blitzide.ini").c_str(), std::ios::out | std::ios::trunc);
	if (!out.good()) return;

	inipp::Ini<char> ini;

	auto& compilerSection = ini.sections["COMPILER"];
	compilerSection.insert(std::make_pair("Debug", boolToString(prg_debug)));
	compilerSection.insert(std::make_pair("NoLAA", boolToString(prg_nolaa)));
	compilerSection.insert(std::make_pair("NoAutoDecl", boolToString(prg_noautodecl)));
	compilerSection.insert(std::make_pair("Experimental", boolToString(prg_experimental)));
	compilerSection.insert(std::make_pair("DumpAsm", boolToString(prg_dumpasm)));
	compilerSection.insert(std::make_pair("Quiet", boolToString(prg_quiet)));
	compilerSection.insert(std::make_pair("VeryQuiet", boolToString(prg_veryquiet)));
	compilerSection.insert(std::make_pair("DumpKeys", boolToString(prg_dumpkeys)));
	compilerSection.insert(std::make_pair("LastBuild", prg_lastbuild));
	compilerSection.insert(std::make_pair("CommandLine", cmd_line));

	auto& windowSection = ini.sections["WINDOW"];
	windowSection.insert(std::make_pair("Maximized", boolToString(win_maximized)));
	windowSection.insert(std::make_pair("NoToolbar", boolToString(win_notoolbar)));
	windowSection.insert(std::make_pair("WindowRect",
		std::to_string(win_x) + " " + std::to_string(win_y) + " " +
		std::to_string(win_w) + " " + std::to_string(win_h)));

	auto& fontsSection = ini.sections["FONTS"];
	fontsSection.insert(std::make_pair("EditorFont", font_editor));
	fontsSection.insert(std::make_pair("EditorFontSize", std::to_string(font_editor_height)));
	fontsSection.insert(std::make_pair("TabsFont", font_tabs));
	fontsSection.insert(std::make_pair("TabsFontSize", std::to_string(font_tabs_height)));
	fontsSection.insert(std::make_pair("DebugFont", font_debug));
	fontsSection.insert(std::make_pair("DebugFontSize", std::to_string(font_debug_height)));

	auto& colorsSection = ini.sections["COLORS"];
	colorsSection.insert(std::make_pair("Background", colorToString(rgb_bkgrnd)));
	colorsSection.insert(std::make_pair("String", colorToString(rgb_string)));
	colorsSection.insert(std::make_pair("Ident", colorToString(rgb_ident)));
	colorsSection.insert(std::make_pair("Keyword", colorToString(rgb_keyword)));
	colorsSection.insert(std::make_pair("Comment", colorToString(rgb_comment)));
	colorsSection.insert(std::make_pair("Digit", colorToString(rgb_digit)));
	colorsSection.insert(std::make_pair("Default", colorToString(rgb_default)));
	colorsSection.insert(std::make_pair("Known", colorToString(rgb_known)));
	colorsSection.insert(std::make_pair("Preproc", colorToString(rgb_preproc)));
	colorsSection.insert(std::make_pair("Global", colorToString(rgb_global)));
	colorsSection.insert(std::make_pair("Const", colorToString(rgb_const)));
	colorsSection.insert(std::make_pair("Cursor", colorToString(rgb_cursor)));
	colorsSection.insert(std::make_pair("Selection", colorToString(rgb_selection)));

	auto& editorSection = ini.sections["EDITOR"];
	editorSection.insert(std::make_pair("TabSpaces", std::to_string(edit_tabs)));
	editorSection.insert(std::make_pair("BackupCount", std::to_string(edit_backup)));
	editorSection.insert(std::make_pair("ToolbarImage", img_toolbar));
	editorSection.insert(std::make_pair("NoBackup", boolToString(noBackup)));
	editorSection.insert(std::make_pair("AutoComplete", boolToString(edit_autocomplete)));

	auto& uiSection = ini.sections["UI"];
	uiSection.insert(std::make_pair("Theme", theme));
	uiSection.insert(std::make_pair("Rounding", std::to_string(ui_rounding)));
	uiSection.insert(std::make_pair("Alpha", std::to_string(ui_alpha)));

	auto& updateSection = ini.sections["UPDATE"];
	updateSection.insert(std::make_pair("IgnoreVersion", ignore_version_update));

	auto& recentFilesSection = ini.sections["RECENT_FILES"];
	for (int i = 1; i < 11; ++i) {
		recentFilesSection.insert(std::make_pair("File" + std::to_string(i),
			i <= (int)recentFiles.size() ? recentFiles[i - 1] : ""));
	}

	ini.generate(out);
}

bool Prefs::loadProjectOptions(const std::string& path) {
	projectOptionsActive = false;
	projectOptions = CompileOptions{};
	if (path.empty()) return false;
	std::ifstream in(path, std::ios::binary);
	if (!in.good()) return false;
	std::stringstream ss;
	ss << in.rdbuf();
	in.close();
	std::string elem = xmlElement(ss.str(), "CompileOptions");
	if (elem.empty()) return false;
	projectOptions.noautodecl = parseBoolValue(xmlAttr(elem, "NoAutoDecl"), projectOptions.noautodecl);
	projectOptions.experimental = parseBoolValue(xmlAttr(elem, "Experimental"), projectOptions.experimental);
	projectOptionsActive = true;
	return true;
}

bool Prefs::saveProjectOptions(const std::string& path) const {
	if (path.empty()) return false;
	std::ifstream in(path, std::ios::binary);
	if (!in.good()) return false;
	std::stringstream ss;
	ss << in.rdbuf();
	in.close();
	std::string text = ss.str();

	std::string elem = "<CompileOptions " + compileOptionsXml() + "/>";
	size_t pos = text.find("<CompileOptions");
	if (pos != std::string::npos) {
		size_t end = text.find('>', pos);
		if (end == std::string::npos) return false;
		text.replace(pos, end - pos + 1, elem);
	}
	else {
		size_t close = text.rfind("</BlitzX3DProject>");
		if (close == std::string::npos) return false;
		text.insert(close, "  " + elem + "\r\n");
	}

	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out.good()) return false;
	out << text;
	out.close();
	return out.good();
}

void Prefs::clearProjectOptions() {
	projectOptionsActive = false;
	projectOptions = CompileOptions{};
}

std::string Prefs::compileOptionsXml() const {
	return std::string("NoAutoDecl=\"") + boolToString(projectOptions.noautodecl) +
		"\" Experimental=\"" + boolToString(projectOptions.experimental) + "\"";
}
