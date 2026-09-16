#ifndef GXGRAPHICS_H
#define GXGRAPHICS_H

#include <ft2build.h>
#include FT_FREETYPE_H

#include <set>
#include <string>
#include <d3d9.h>

#include "ddutil.h"

#include "gxfont.h"
#include "gxcanvas.h"
#include "gxscene.h"
#include "gxmesh.h"
#include "gxmovie.h"

class gxRuntime;
class gxEffect;
struct DecodedImage;

class gxGraphics {
public:
	IDirect3DDevice9Ex* dir3dDev;
	IDirect3DSurface9* frontBuffer;
	IDirect3DSurface9* backBuffer;
	IDirect3D9Ex* dir3d;

	D3DFORMAT           zbuffFmt;
	D3DPRESENT_PARAMETERS present_params;
	
	FT_Library ftLibrary;

	bool running_on_wine;

	gxGraphics(gxRuntime* runtime, IDirect3DDevice9Ex* device, IDirect3DSurface9* front, IDirect3DSurface9* back, bool d3d, int w = 0, int h = 0);
	~gxGraphics();

	bool restore();

	gxRuntime* runtime;
	bool d3dSceneOpen = false;
	bool ensureD3DBegun();
	void endD3DScene();
	//std::set<std::set<std::any>*> custom_set;

private:

	gxCanvas* front_canvas, * back_canvas;
	gxFont* def_font;
	bool gfx_lost;
	gxMesh* dummy_mesh;
	std::string lastEffectError;

	bool copySceneToTexture(gxCanvas* dest, int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh);

	std::set<gxFont*> font_set;
	std::set<gxCanvas*> canvas_set;
	std::set<gxMesh*> mesh_set;
	std::set<gxScene*> scene_set;
	std::set<gxMovie*> movie_set;
	std::set<std::string> font_res;
	std::set<gxEffect*> effect_set;

	// DDGAMMARAMP _gammaRamp;
	// IDirectDrawGammaControl* _gamma;

	/***** GX INTERFACE *****/
public:
	enum {
		GRAPHICS_WINDOWED = 1,	//windowed mode
		GRAPHICS_SCALED = 2,		//scaled window
		GRAPHICS_3D = 4,			//3d mode! Hurrah!
		GRAPHICS_AUTOSUSPEND = 8,	//suspend graphics when app suspended
		GRAPHICS_BORDERLESS = 16
	};

	enum DeviceState {
		DEVICE_OK,
		DEVICE_LOST,
		DEVICE_NEEDS_RESET
	};

	DeviceState getDeviceState();

	// i wonder what this is for
	gxEffect* createEffect(const std::string& filename);
	gxEffect* verifyEffect(gxEffect* e);
	void freeEffect(gxEffect* e);
	void clearEffects();
	const std::string& getLastEffectError() const { return lastEffectError; }

	//MANIPULATORS
	void vwait();
	void flip(bool vwait);
	bool changeDisplayMode(int width, int height, bool fullscreen, bool borderless = false);
	bool setDarkMode(bool dark_mode);

	//SPECIAL!
	void copy(gxCanvas* dest, int dx, int dy, int dw, int dh, gxCanvas* src, int sx, int sy, int sw, int sh);

	//NEW! Gamma control!
	void setGamma(int r, int g, int b, float dr, float dg, float db);
	void getGamma(int r, int g, int b, float* dr, float* dg, float* db);
	void updateGamma(bool calibrate);

	//ACCESSORS
	int getWidth()const;
	int getHeight()const;
	int getDepth()const;
	int getScanLine()const;
	int getAvailVidmem()const;
	int getTotalVidmem()const;

	gxCanvas* getFrontCanvas()const;
	gxCanvas* getBackCanvas()const;
	gxFont* getDefaultFont()const;

	//OBJECTS
	gxCanvas* createCanvas(int width, int height, int flags);
	gxCanvas* loadCanvas(const std::string& file, int flags);
	gxCanvas* createCanvasFromImage(const DecodedImage* img, int flags, bool keepPixels = true);
	gxCanvas* verifyCanvas(gxCanvas* canvas);
	void freeCanvas(gxCanvas* canvas);

	gxMovie* openMovie(const std::string& file, int flags);
	gxMovie* verifyMovie(gxMovie* movie);
	void closeMovie(gxMovie* movie);

	gxFont* loadFont(std::string font, int height, bool bold = false, bool italic = false, bool underlined = false);
	gxFont* verifyFont(gxFont* font);
	void freeFont(gxFont* font);

	gxScene* createScene(int flags);
	gxScene* verifyScene(gxScene* scene);
	void freeScene(gxScene* scene);
	bool presentSceneSDL(struct SDL_GPUDevice* dev, struct SDL_Window* win);
	bool presentSceneWithCanvas(struct SDL_GPUDevice* dev, struct SDL_Window* win, gxCanvas* canvas);
	void setActiveCanvas(gxCanvas* canvas);

	void adoptCanvas(gxCanvas* c);

	gxMesh* createMesh(int max_verts, int max_tris, int flags);
	gxMesh* verifyMesh(gxMesh* mesh);
	void freeMesh(gxMesh* mesh);

	//GPU SKINNING
	bool skinningSupported();
	bool ensureSkinningShader();
	IDirect3DVertexShader9* getSkinningShader()const { return skin_vshader; }

private:
	IDirect3DVertexShader9* skin_vshader;
	IDirect3DVertexDeclaration9* skin_decl;
	bool skin_shader_load_failed;
	int skin_caps_checked;   //-1 unknown, 0 unsupported, 1 supported
};

#endif