#ifndef GXFONT_H
#define GXFONT_H

#include "std.h"
#include <vector>
#include <map>
#include <unordered_map>

class gxCanvas;
class gxGraphics;
struct SDL_GPUDevice;
struct TTF_Font;

// typedef IDirectDrawSurface7 ddSurf;

class gxFont {
public:
	gxFont(gxGraphics* gfx, const std::string& fn, int h, bool bold = false, bool italic = false, bool underlined = false);
	~gxFont();

	void render(gxCanvas* dest, unsigned color_argb, int x, int y, const std::string& t);
	bool renderGPU(SDL_GPUDevice* dev, gxCanvas* dest, unsigned color_argb, int x, int y, const std::string& t);

	int charWidth(int c);
	int charAdvance(int c);
	int stringWidth(const std::string& text);
	void setSmooth(bool enable) { smooth = enable; }

	//ACCESSORS
	int getWidth()const;							//width of widest char
	int getHeight()const;							//height of font
	int getRenderOffset()const;
	int getWidth(const std::string& text);	    //width of string
	bool isPrintable(int chr)const;				//printable char?

	std::vector<gxCanvas*> atlases;

	gxCanvas* tempCanvas;

	enum {
		FONT_BOLD = 1,
		FONT_ITALIC = 2,
		FONT_UNDERLINE = 4 //TODO: remove? who actually wants this
	};

	bool bold;
	bool italic;
	bool underlined;
	bool smooth;
private:
	float getBaselinePosition()const;
	float getUnderlinePosition()const;
	float getUnderlineThickness()const;

	int maxWidth = 0;
	int glyphHeight = 0;
	int tCanvasHeight = 0;
	int glyphRenderBaseline = 0;
	int glyphRenderOffset = 0;

	struct GlyphData {
		int atlasIndex;
		int drawOffset[2];
		int horizontalAdvance;
		int srcRect[4];
	};

	const int atlasDims = 1024;
	void renderAtlas(int chr);
	int flags;

	int height;
	TTF_Font* font;
	std::unordered_map<int, GlyphData> glyphData;
	gxGraphics* graphics;
	std::string filename;
};

#endif