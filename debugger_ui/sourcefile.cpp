#include "stdafx.h"
#include "sourcefile.h"
#include "prefs.h"

#include "blitzlang.h"

#include "../imgui/imgui.h"

#include <fstream>
#include <sstream>

SourceFile::SourceFile() :loaded(false) {
}

SourceFile::~SourceFile() {
}

void SourceFile::load(const char* file) {
	path = file;
	name = file;
	if (const char* p = strrchr(file, '/')) name = p + 1;
	if (const char* p = strrchr(file, '\\')) name = p + 1;

	editor.SetReadOnly(true);
	editor.SetLanguageDefinition(makeBlitzLangDef(builtinBlitzKeywords(), {}, {}));
	editor.SetShowWhitespaces(false);
	editor.SetColorizerEnable(true);

	TextEditor::Palette pal = editor.GetPalette();
	auto col = [](const int* rgb) -> ImU32 {
		return IM_COL32(rgb[0], rgb[1], rgb[2], 255);
	};
	pal[(int)TextEditor::PaletteIndex::Background] = col(prefs.rgb_bkgrnd);
	pal[(int)TextEditor::PaletteIndex::String] = col(prefs.rgb_string);
	pal[(int)TextEditor::PaletteIndex::Identifier] = col(prefs.rgb_ident);
	pal[(int)TextEditor::PaletteIndex::KnownIdentifier] = IM_COL32(150, 255, 200, 255);
	pal[(int)TextEditor::PaletteIndex::PreprocIdentifier] = IM_COL32(255, 200, 120, 255);
	pal[(int)TextEditor::PaletteIndex::Keyword] = col(prefs.rgb_keyword);
	pal[(int)TextEditor::PaletteIndex::Comment] = col(prefs.rgb_comment);
	pal[(int)TextEditor::PaletteIndex::MultiLineComment] = col(prefs.rgb_comment);
	pal[(int)TextEditor::PaletteIndex::Number] = col(prefs.rgb_digit);
	pal[(int)TextEditor::PaletteIndex::Default] = col(prefs.rgb_default);
	pal[(int)TextEditor::PaletteIndex::Cursor] = IM_COL32(220, 220, 220, 255);
	pal[(int)TextEditor::PaletteIndex::Selection] = IM_COL32(60, 90, 120, 120);
	pal[(int)TextEditor::PaletteIndex::LineNumber] = IM_COL32(120, 120, 120, 200);
	pal[(int)TextEditor::PaletteIndex::ExecutionLineFill] = IM_COL32(255, 140, 0, 55);
	pal[(int)TextEditor::PaletteIndex::ExecutionLineEdge] = IM_COL32(255, 175, 40, 255);
	editor.SetPalette(pal);

	std::stringstream ss;
	if (FILE* f = fopen(file, "rb")) {
		fseek(f, 0, SEEK_END);
		int sz = (int)ftell(f);
		fseek(f, 0, SEEK_SET);
		std::vector<char> buf(sz);
		fread(buf.data(), sz, 1, f);
		fclose(f);
		ss.write(buf.data(), sz);
	}

	editor.SetText(ss.str());
	editor.SetExecutionLine(-1);
	editor.SetCursorPosition(TextEditor::Coordinates(0, 0));
	loaded = true;
}

void SourceFile::highLight(int row, int col) {
	if (!loaded) return;

	int end = col;
	std::vector<std::string> lines = editor.GetTextLines();
	if (row >= 0 && row < (int)lines.size()) {
		const std::string& line = lines[row];
		bool quote = false;
		while (end < (int)line.size()) {
			char c = line[end];
			if (c == '\"') quote = !quote;
			if (!quote && (c == ':' || !isprint((unsigned char)c))) break;
			++end;
		}
	}

	editor.SetCursorPosition(TextEditor::Coordinates(row, end));
	editor.SetSelection(TextEditor::Coordinates(row, col), TextEditor::Coordinates(row, end));
	editor.SetExecutionLine(row);
	editor.EnsureCursorVisible();
}

void SourceFile::render(const char* tabId) {
	if (!loaded) return;
	ImVec2 avail = ImGui::GetContentRegionAvail();
	editor.Render(tabId, avail, false);
}
