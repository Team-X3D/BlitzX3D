#ifndef BBGRAPHICS_H
#define BBGRAPHICS_H

#include "bbsys.h"
#include "../sdlruntime/sdlgraphics.h"
#include "../blitz3d/texture.h"

extern sdlGraphics* sdl_graphics;
extern sdlCanvas* sdl_canvas;
extern sdlScene* sdl_scene;

class bbImage;

BBStr* bbGetTextureLoadError();

//general graphics functions
int		 bbCountGfxDrivers();
BBStr* bbGfxDriverName(int n);
void	 bbSetGfxDriver(int n);
int		 bbGfxModeExists(int w, int h, int d);
int		 bbCountGfxModes();
int		 bbGfxModeWidth(int n);
int		 bbGfxModeHeight(int n);
int		 bbGfxModeDepth(int n);
int		 bbGraphicsWidth();
int		 bbGraphicsHeight();
int		 bbGraphicsDepth();
int		 bbAvailVidMem();
int		 bbTotalVidMem();
float    bbDPIScaleX();
float    bbDPIScaleY();

//mode functions
void	 bbGraphics(int w, int h, int d, int mode);
sdlCanvas* bbFrontBuffer();
sdlCanvas* bbBackBuffer();
void	 bbEndGraphics();
int		 bbInFocus();
int		 bbGraphicsLost();
int		 bbScanLine();
void	 bbVWait(int n);
void	 bbFlip(int vwait);
int		 bbGetFPS();

//graphics buffer functions
void	 bbSetBuffer(sdlCanvas* buff);
void bbSetBufferDepth(sdlCanvas* buff, sdlCanvas* depthBuff);
sdlCanvas* bbGraphicsBuffer();
int		 bbLoadBuffer(sdlCanvas* surf, BBStr* str);
int		 bbSaveBuffer(sdlCanvas* surf, BBStr* str);

//fast read/write operations...
void	 bbLockBuffer(sdlCanvas* buff);
void	 bbUnlockBuffer(sdlCanvas* buff);
int  bbBufferWidth(sdlCanvas* buff);
int  bbBufferHeight(sdlCanvas* buff);
void bbDrawBufferRect(sdlCanvas* src, int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh);
int		 bbReadPixel(int x, int y, sdlCanvas* buff);
void	 bbWritePixel(int x, int y, int argb, sdlCanvas* buff);
int		 bbReadPixelFast(int x, int y, sdlCanvas* buff);
void	 bbWritePixelFast(int x, int y, int argb, sdlCanvas* buff);


//2d rendering functions
void	 bbOrigin(int x, int y);
void	 bbViewport(int x, int y, int w, int h);
void	 bbBeginScrollRect(int x, int y, int w, int h, int scroll_x = 0, int scroll_y = 0);
void	 bbEndScrollRect();
void	 bbColor(int r, int g, int b, int a = 255);
void	 bbClsColor(int r, int g, int b, int a = 255);
void	 bbCls();
void	 bbPlot(int x, int y);
void	 bbLine(int x1, int y1, int x2, int y2);
void	 bbRect(int x, int y, int w, int h, int solid);
void	 bbOval(int x, int y, int w, int h, int solid);
void	 bbText(int x, int y, BBStr* str, int xPos, int yPos);
void	 bbGetColor(int x, int y);
int		 bbColorRed();
int		 bbColorGreen();
int		 bbColorBlue();
int		 bbColorAlpha();
void bbSet2DEffect(sdlEffect* effect);
void bbClear2DEffect();
sdlEffect* bbGet2DEffect();

BBStr* bbConvertToANSI(BBStr* str);
BBStr* bbConvertToUTF8(BBStr* str);

//font functions
sdlFont* bbLoadFont(BBStr* name, int height, bool bold, bool italic, bool underlined);
void	 bbFreeFont(sdlFont* f);
void	 bbSetFont(sdlFont* f);
int		 bbFontWidth();
int		 bbFontHeight();
int		 bbStringWidth(BBStr* str);
int		 bbStringHeight(BBStr* str);

//image functions
bbImage* bbLoadImage(BBStr* s);
bbImage* bbLoadImageFlag(BBStr* s, int flags);
bbImage* bbCopyImage(bbImage* i);
bbImage* bbCreateImage(int w, int h, int n);
bbImage* bbCreateImageFlag(int w, int h, int n, int flags);
bbImage* bbLoadAnimImage(BBStr* s, int w, int h, int first, int cnt);
Texture* bbLoadAnimTextureGrid(BBStr* file, int flags, int columns, int rows, int first, int count);
void	 bbFreeImage(bbImage* i);
int		 bbSaveImage(bbImage* i, BBStr* filename, int frame);
void	 bbGrabImage(bbImage* i, int x, int y, int n);
sdlCanvas* bbImageBuffer(bbImage* i, int n);
void	 bbDrawImage(bbImage* i, int x, int y, int frame);
void	 bbDrawBlock(bbImage* i, int x, int y, int frame);
void	 bbTileImage(bbImage* i, int x, int y, int frame);
void	 bbTileBlock(bbImage* i, int x, int y, int frame);
void	 bbDrawImageRect(bbImage* i, int x, int y, int r_x, int r_y, int r_w, int r_h, int frame);
void bbDrawImageRectStretch(bbImage* i, int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh);
void	 bbDrawBlockRect(bbImage* i, int x, int y, int r_x, int r_y, int r_w, int r_h, int frame);
void	 bbMaskImage(bbImage* i, int r, int g, int b);
void	 bbHandleImage(bbImage* i, int x, int y);
void	 bbScaleImage(bbImage* i, float w, float h);
void	 bbScaleImageFast(bbImage* i, float xscale, float yscale);
void	 bbResizeImage(bbImage* i, float w, float h);
void	 bbResizeImageFast(bbImage* i, float w, float h);
void	 bbRotateImage(bbImage* i, float angle);
void	 bbTFormImage(bbImage* i, float a, float b, float c, float d);
void	 bbTFormFilter(int enable);
void	 bbAutoMidHandle(int enable);
void	 bbMidHandle(bbImage* i);
int		 bbImageWidth(bbImage* i);
int		 bbImageHeight(bbImage* i);
int		 bbImageXHandle(bbImage* i);
int		 bbImageYHandle(bbImage* i);
int		 bbImagesOverlap(bbImage* i1, int x1, int y1, bbImage* i2, int x2, int y2);
int		 bbImagesCollide(bbImage* i1, int x1, int y1, int f1, bbImage* i2, int x2, int y2, int f2);
int		 bbRectsOverlap(int x1, int y1, int w1, int h1, int x2, int y2, int w2, int h2);
int		 bbImageRectOverlap(bbImage* i, int x, int y, int r_x, int r_y, int r_w, int r_h);
int		 bbImageRectCollide(bbImage* i, int x, int y, int f, int r_x, int r_y, int r_w, int r_h);
int bbImageWidthUnscaled(bbImage* i);
int bbImageHeightUnscaled(bbImage* i);

//effects
BBStr* bbGetEffectError();

//simple print functions
void	 bbWrite(BBStr* str);
void	 bbPrint(BBStr* str);
BBStr* bbInput(BBStr* prompt);
void	 bbLocate(int x, int y);

#endif