#ifndef PREFS_H
#define PREFS_H

#include <string>
#include <vector>

struct Prefs {
	struct CompileOptions {
		bool noautodecl = false;
		bool experimental = false;
	};

	bool prg_debug = true;
	bool prg_nolaa = false;
	bool prg_noautodecl = false;
	bool prg_experimental = false;
	bool prg_dumpasm = false;
	bool prg_quiet = true;
	bool prg_veryquiet = false;
	bool prg_dumpkeys = false;
	std::string prg_lastbuild;
	std::string cmd_line;

	int win_x = 0, win_y = 0, win_w = 800, win_h = 600;
	bool win_maximized = false;
	bool win_notoolbar = false;

	std::string font_editor = "consolas";
	int font_editor_height = 14;
	std::string font_tabs = "consolas";
	int font_tabs_height = 10;
	std::string font_debug = "consolas";
	int font_debug_height = 14;

	int rgb_bkgrnd[3] = { 34, 85, 136 };
	int rgb_string[3] = { 0, 255, 102 };
	int rgb_ident[3] = { 255, 255, 255 };
	int rgb_keyword[3] = { 170, 255, 255 };
	int rgb_comment[3] = { 255, 238, 0 };
	int rgb_digit[3] = { 51, 255, 221 };
	int rgb_default[3] = { 238, 238, 238 };
	int rgb_known[3] = { 150, 255, 200 };
	int rgb_preproc[3] = { 255, 200, 120 };
	int rgb_global[3] = { 196, 160, 255 };
	int rgb_const[3] = { 235, 205, 255 };
	int rgb_cursor[3] = { 255, 255, 255 };
	int rgb_selection[3] = { 255, 200, 80 };

	int edit_tabs = 4;
	int edit_backup = 2;
	bool edit_autocomplete = true;
	bool noBackup = false;
	std::string img_toolbar = "toolbar.bmp";

	std::string theme = "Classic Dark";
	int ui_rounding = 0;
	float ui_alpha = 1.0f;

	std::string configDir;

	CompileOptions projectOptions;
	bool projectOptionsActive = false;

	std::vector<std::string> recentFiles;
	std::string ignore_version_update;

	std::string homeDir;

	void open();
	void close();

	bool loadProjectOptions(const std::string& path);
	bool saveProjectOptions(const std::string& path) const;
	void clearProjectOptions();
	std::string compileOptionsXml() const;
};

extern Prefs prefs;

#endif
