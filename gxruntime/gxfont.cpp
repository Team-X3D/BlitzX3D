#include "std.h"
#include "gxfont.h"

#include "gxcanvas.h"
#include "gxgraphics.h"
#include "gxutf8.h"
#include "../bbruntime/bbsys.h"

#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <cmath>
#include <algorithm>
#include <freetype/ftsynth.h>

static unsigned long __cdecl streamRead(FT_Stream stream, unsigned long offset, unsigned char* buffer, unsigned long count) {
	std::ifstream* file = reinterpret_cast<std::ifstream*>(stream->descriptor.pointer);
	file->seekg(offset, std::ios::beg);
	file->read(reinterpret_cast<char*>(buffer), count);
	return static_cast<unsigned long>(file->gcount());
}

static void __cdecl streamClose(FT_Stream stream) {
	delete reinterpret_cast<std::ifstream*>(stream->descriptor.pointer);
	delete stream;
}

gxFont::gxFont(FT_Library ftLibrary, gxGraphics* gfx, const std::string& fn, int h, bool bold, bool italic, bool underlined) {
	graphics = gfx;
	filename = fn;
	height = h;
	this->bold = bold;
	this->italic = italic;
	this->underlined = underlined;
	smooth = true;

	FT_StreamRec* rec = new FT_StreamRec();
	std::ifstream* file = new std::ifstream(std::filesystem::path(UTF8::convertToUtf16(filename)), std::ios::binary);
	if (!file->is_open()) {
		delete file;
		delete rec;
		throw std::runtime_error("Failed to open font file!");
	}

	rec->base = nullptr;
	rec->size = 0x7FFFFFFF;
	rec->descriptor.pointer = file;
	rec->read = reinterpret_cast<FT_Stream_IoFunc>(streamRead);
	rec->close = reinterpret_cast<FT_Stream_CloseFunc>(streamClose);

	FT_Open_Args args{};
	args.flags = FT_OPEN_STREAM;
	args.stream = rec;

	if (FT_Open_Face(ftLibrary, &args, 0, &freeTypeFace)) {
		delete file;
		delete rec;
		throw std::runtime_error("Failed to load font file!");
	}

	FT_Set_Pixel_Sizes(freeTypeFace,
		0,
		height);

	glyphData.clear();
	atlases.clear();

	glyphHeight = height;
	renderAtlas('T');
	{
		std::unordered_map<int, GlyphData>::iterator it = glyphData.find('T');
		if (it != glyphData.end() && it->second.atlasIndex >= 0) {
			const GlyphData& gd = it->second;
			glyphHeight = gd.srcRect[3];
			glyphRenderOffset = -gd.drawOffset[1];
		}
	}

	tCanvasHeight = (glyphHeight * 40) / 10;
	glyphRenderBaseline = (glyphHeight * 3 / 10);
	glyphRenderOffset += glyphRenderBaseline;

	{
		int D = glyphRenderBaseline - glyphRenderOffset;
		int boxBot = glyphHeight + glyphRenderOffset;
		int needBot = boxBot;

		FT_Size_Metrics& m = freeTypeFace->size->metrics;
		int descentPx = (int)((-m.descender + 63) / 64);
		if (descentPx < 0) descentPx = 0;
		int need = D + descentPx + (bold ? 1 : 0);
		if (need > needBot) needBot = need;

		if (underlined) {
			float upos = -static_cast<float>(FT_MulFix(freeTypeFace->underline_position, m.y_scale)) / 64.0F;
			float uthick = std::max(1.0F, static_cast<float>(FT_MulFix(freeTypeFace->underline_thickness, m.y_scale)) / 64.0F);
			int needU = D + (int)std::ceil(upos + uthick) + 1;
			if (needU > needBot) needBot = needU;
		}

		if (needBot > boxBot) glyphRenderOffset += (needBot - boxBot);
	}

	tempCanvas = nullptr;
}

gxFont::~gxFont() {
	for(int i = 0; i < atlases.size(); i++) {
		graphics->freeCanvas(atlases[i]);
	}

	FT_Done_Face(freeTypeFace);
}

const int transparentPixel = 0x4A412A;
const int opaquePixel = 0xffffff;

void gxFont::renderAtlas(int chr) {
	bool needsNewAtlas = false;
	int startChr = chr - 1024;
	if (startChr < 0) startChr = 0;
	int endChr = startChr + 2048;

	uint8_t* buffer = nullptr;
	int x = -1, y = -1, maxHeight = -1;

	for (int i = startChr; i < endChr; i++) {
		auto it = glyphData.find(i);
		if (it == glyphData.end()) {
			long glyphIndex = FT_Get_Char_Index(freeTypeFace, i);
			if (glyphIndex != 0) {
				int loadFlags = smooth ? FT_LOAD_TARGET_NORMAL : FT_LOAD_TARGET_MONO;
				FT_Load_Glyph(freeTypeFace, (FT_UInt)glyphIndex, loadFlags);

				if (bold) FT_GlyphSlot_Embolden(freeTypeFace->glyph);
				if (italic) FT_GlyphSlot_Oblique(freeTypeFace->glyph);

				FT_Render_Mode renderMode = smooth ? FT_RENDER_MODE_NORMAL : FT_RENDER_MODE_MONO;
				FT_Render_Glyph(freeTypeFace->glyph, renderMode);

				unsigned char* glyphBuffer = freeTypeFace->glyph->bitmap.buffer;
				int glyphPitch = freeTypeFace->glyph->bitmap.pitch;
				int glyphWidth = freeTypeFace->glyph->bitmap.width;
				int glyphHeight = freeTypeFace->glyph->bitmap.rows;

				if (glyphWidth > 0 && glyphHeight > 0) {
					if (buffer == nullptr) {
						buffer = new uint8_t[atlasDims * atlasDims];
						memset(buffer, 0, atlasDims * atlasDims);
						x = 1; y = 1; maxHeight = 0;
					}

					if (x + glyphWidth + 1 > atlasDims - 1) {
						x = 1; y += maxHeight + 1; maxHeight = 0;
					}
					if (y + glyphHeight + 1 > atlasDims - 1) {
						needsNewAtlas = true;
						break;
					}
					if (glyphHeight > maxHeight) maxHeight = glyphHeight;

					if (smooth) {
						for (int row = 0; row < glyphHeight; ++row) {
							int destY = y + row;
							uint8_t* destRow = buffer + destY * atlasDims + x;
							uint8_t* srcRow = glyphBuffer + row * glyphPitch;
							memcpy(destRow, srcRow, glyphWidth);
						}
					}
					else {
						for (int row = 0; row < glyphHeight; ++row) {
							for (int col = 0; col < glyphWidth; ++col) {
								int byteIndex = (col / 8) + row * glyphPitch;
								int bitIndex = 7 - (col % 8);
								bool on = (glyphBuffer[byteIndex] & (1 << bitIndex)) != 0;
								buffer[(x + col) + (y + row) * atlasDims] = on ? 255 : 0;
							}
						}
					}

					GlyphData gd;
					gd.atlasIndex = (int)atlases.size();
					gd.horizontalAdvance = freeTypeFace->glyph->metrics.horiAdvance >> 6;
					gd.drawOffset[0] = -freeTypeFace->glyph->bitmap_left;
					gd.drawOffset[1] = freeTypeFace->glyph->bitmap_top - ((height * 10) / 14);
					gd.srcRect[0] = x;
					gd.srcRect[1] = y;
					gd.srcRect[2] = glyphWidth;
					gd.srcRect[3] = glyphHeight;

					if (glyphWidth > maxWidth) maxWidth = glyphWidth;
					x += glyphWidth + 1;
					glyphData.emplace(i, gd);
				}
				else {
					GlyphData gd;
					gd.atlasIndex = -1;
					gd.horizontalAdvance = freeTypeFace->glyph->metrics.horiAdvance >> 6;
					glyphData.emplace(i, gd);
				}
			}
			else {
				GlyphData gd;
				gd.atlasIndex = -1;
				gd.horizontalAdvance = freeTypeFace->glyph->metrics.horiAdvance >> 6;
				glyphData.emplace(i, gd);
			}
		}
	}

	if (buffer != nullptr) {
		gxCanvas* newAtlas = graphics->createCanvas(atlasDims, atlasDims, gxCanvas::CANVAS_TEXTURE | gxCanvas::CANVAS_TEX_ALPHA);
		newAtlas->backup();
		newAtlas->lock();
		for (int y = 0; y < atlasDims; ++y) {
			for (int x = 0; x < atlasDims; ++x) {
				uint8_t a = buffer[x + y * atlasDims];
				unsigned argb = (a << 24) | 0x00ffffff;
				newAtlas->setPixelFast(x, y, argb);
			}
		}
		newAtlas->unlock();
		newAtlas->setMask(0);
		newAtlas->backup();
		atlases.push_back(newAtlas);
		delete[] buffer;
	}

	if (needsNewAtlas) renderAtlas(chr);
}

void gxFont::render(gxCanvas* dest, unsigned color_argb, int x, int y, const std::string& text) {
	int baselineY = y - glyphRenderOffset + glyphRenderBaseline;
	int t_x = 0;

	for (int i = 0; i < (int)text.size(); ) {
		int codepointLen = UTF8::measureCharacter(text.c_str(), i);
		int chr = UTF8::decodeCharacter(text.c_str(), i);

		auto it = glyphData.find(chr);
		if (it == glyphData.end()) {
			renderAtlas(chr);
			it = glyphData.find(chr);
		}

		if (it != glyphData.end()) {
			const GlyphData& gd = it->second;
			if (gd.atlasIndex >= 0) {
				int dstX = x + t_x - gd.drawOffset[0];
				int dstY = baselineY - gd.drawOffset[1];

				gxCanvas* atlas = atlases[gd.atlasIndex];
				bool filter = smooth;
				dest->blitAlpha(dstX, dstY, atlas, gd.srcRect[0], gd.srcRect[1], gd.srcRect[2], gd.srcRect[3], color_argb, filter);
			}
			t_x += gd.horizontalAdvance;
		}
		i += codepointLen;
	}

	if (underlined) {
		int width = stringWidth(text);
		int uy = baselineY + static_cast<int>(getUnderlinePosition());
		int uh = max(1, static_cast<int>(getUnderlineThickness()));
		dest->rectBlend(x, uy, width, uh, color_argb);
	}
}

int gxFont::charWidth(int chr) {
	std::unordered_map<int, GlyphData>::iterator it = glyphData.find(chr);
	if(it == glyphData.end()) {
		renderAtlas(chr);
		it = glyphData.find(chr);
	}
	return it->second.srcRect[2];
}

int gxFont::charAdvance(int chr) {
	std::unordered_map<int, GlyphData>::iterator it = glyphData.find(chr);
	if(it == glyphData.end()) {
		renderAtlas(chr);
		it = glyphData.find(chr);
	}
	int adv = (it != glyphData.end()) ? it->second.horizontalAdvance : 0;
	if (adv == 0 && it != glyphData.end()) {
		OutputDebugStringA("Font advance is zero!\n");
	}
	return adv;
}

int gxFont::stringWidth(const std::string& text) {
	int width = 0;

	for(int i = 0; i < text.size();) {
		int codepointLen = UTF8::measureCharacter(text.c_str(), i);
		int chr = UTF8::decodeCharacter(text.c_str(), i);
		std::unordered_map<int, GlyphData>::iterator it = glyphData.find(chr);
		if(it == glyphData.end()) {
			renderAtlas(chr);
			it = glyphData.find(chr);
		}

		if(it != glyphData.end()) {
			width += it->second.horizontalAdvance;
		}
		i += codepointLen;
	}

	return width;
}

int gxFont::getWidth()const {
	return maxWidth;
}

int gxFont::getHeight()const {
	return glyphHeight;
}

int gxFont::getRenderOffset()const {
	return glyphRenderOffset;
}

int gxFont::getWidth(const std::string& text) {
	return stringWidth(text);
}

bool gxFont::isPrintable(int chr)const {
	return glyphData.find(chr) != glyphData.end();
}

float gxFont::getBaselinePosition() const
{
	return static_cast<float>(freeTypeFace->size->metrics.ascender) / 64.0F;
}

float gxFont::getUnderlinePosition()const
{
	return -static_cast<float>(FT_MulFix(freeTypeFace->underline_position, freeTypeFace->size->metrics.y_scale)) / 64.0F;
}

float gxFont::getUnderlineThickness()const
{
	return std::max(1.0F, static_cast<float>(FT_MulFix(freeTypeFace->underline_thickness, freeTypeFace->size->metrics.y_scale)) / 64.0F);
}