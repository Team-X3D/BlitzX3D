#ifndef SDLGRAPHICS_H
#define SDLGRAPHICS_H

#include <set>
#include <string>
#include <d3d9.h>

#include "ddutil.h"

#include "sdlfont.h"
#include "sdlcanvas.h"
#include "sdlscene.h"
#include "sdlmesh.h"
#include "sdlmovie.h"

class sdlRuntime;
class sdlEffect;
struct DecodedImage;

class sdlGraphics {
public:
	IDirect3DDevice9Ex* dir3dDev;
	IDirect3DSurface9* frontBuffer;
	IDirect3DSurface9* backBuffer;
	IDirect3D9Ex* dir3d;

	D3DFORMAT           zbuffFmt;
	D3DPRESENT_PARAMETERS present_params;
	
	bool mask565 = false;
	bool running_on_wine = false;

	sdlGraphics(sdlRuntime* runtime, IDirect3DDevice9Ex* device, IDirect3DSurface9* front, IDirect3DSurface9* back, bool d3d, int w = 0, int h = 0);
	~sdlGraphics();

	bool restore();

	sdlRuntime* runtime;
	bool d3dSceneOpen = false;
	bool ensureD3DBegun();
	void endD3DScene();
	//std::set<std::set<std::any>*> custom_set;

	void applyAntialiasChange();
	void resolveAntialias();
	bool antialiasActive() const { return antialias_msaa; }

private:
	void refreshAntialiasCanvas(int w, int h, D3DFORMAT fmt);
	bool ensureResolveScratch(int w, int h, D3DFORMAT fmt);
	void releaseResolveScratch();
	IDirect3DSurface9* resolve_scratch = nullptr;
	int resolve_scratch_w = 0, resolve_scratch_h = 0;
	D3DFORMAT resolve_scratch_fmt = D3DFMT_UNKNOWN;

	sdlCanvas* front_canvas, * back_canvas;
	sdlCanvas* antialias_canvas = nullptr;
	bool antialias_msaa = false;
	bool applied_antialias_request = false;
	sdlFont* def_font;
	bool gfx_lost;
	unsigned short gammaRamp[3][256];
	sdlMesh* dummy_mesh;
	std::string lastEffectError;

	bool copySceneToTexture(sdlCanvas* dest, int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh);

	std::set<sdlFont*> font_set;
	std::set<sdlCanvas*> canvas_set;
	std::set<sdlMesh*> mesh_set;
	std::set<sdlScene*> scene_set;
	std::set<sdlMovie*> movie_set;
	std::set<std::string> font_res;
	std::set<sdlEffect*> effect_set;

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
	sdlEffect* createEffect(const std::string& filename);
	sdlEffect* verifyEffect(sdlEffect* e);
	void freeEffect(sdlEffect* e);
	void clearEffects();
	const std::string& getLastEffectError() const { return lastEffectError; }

	//MANIPULATORS
	void vwait();
	void flip(bool vwait);
	bool changeDisplayMode(int width, int height, bool fullscreen, bool borderless = false);
	bool setDarkMode(bool dark_mode);

	//SPECIAL!
	void copy(sdlCanvas* dest, int dx, int dy, int dw, int dh, sdlCanvas* src, int sx, int sy, int sw, int sh);

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

	sdlCanvas* getFrontCanvas()const;
	sdlCanvas* getBackCanvas()const;
	sdlFont* getDefaultFont()const;

	//OBJECTS
	sdlCanvas* createCanvas(int width, int height, int flags);
	sdlCanvas* loadCanvas(const std::string& file, int flags);
	sdlCanvas* createCanvasFromImage(const DecodedImage* img, int flags, bool keepPixels = true);
	sdlCanvas* verifyCanvas(sdlCanvas* canvas);
	void freeCanvas(sdlCanvas* canvas);

	sdlMovie* openMovie(const std::string& file, int flags);
	sdlMovie* verifyMovie(sdlMovie* movie);
	void closeMovie(sdlMovie* movie);

	sdlFont* loadFont(std::string font, int height, bool bold = false, bool italic = false, bool underlined = false);
	sdlFont* verifyFont(sdlFont* font);
	void freeFont(sdlFont* font);

	sdlScene* createScene(int flags);
	sdlScene* verifyScene(sdlScene* scene);
	void freeScene(sdlScene* scene);
	bool presentSceneSDL(struct SDL_GPUDevice* dev, struct SDL_Window* win);
	bool presentSceneWithCanvas(struct SDL_GPUDevice* dev, struct SDL_Window* win, sdlCanvas* canvas);
	void setActiveCanvas(sdlCanvas* canvas);

	void adoptCanvas(sdlCanvas* c);

	sdlMesh* createMesh(int max_verts, int max_tris, int flags);
	sdlMesh* verifyMesh(sdlMesh* mesh);
	void freeMesh(sdlMesh* mesh);

	//GPU SKINNING
	bool skinningSupported();
	bool ensureSkinningShader();
	IDirect3DVertexShader9* getSkinningShader()const { return skin_vshader; }

	bool ensureCopyScratch(int w, int h, D3DFORMAT fmt);
	void releaseCopyScratchCanvas();

private:
	IDirect3DVertexShader9* skin_vshader;
	IDirect3DVertexDeclaration9* skin_decl;
	bool skin_shader_load_failed;
	int skin_caps_checked;   //-1 unknown, 0 unsupported, 1 supported

	sdlCanvas* copy_scratch;
	int copy_scratch_w, copy_scratch_h;
	D3DFORMAT copy_scratch_fmt;
};

#endif