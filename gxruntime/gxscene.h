#ifndef GXSCENE_H
#define GXSCENE_H

#include <map>
#include <d3d9.h>
#include <d3dx9.h>

#include "gxlight.h"
#include "gxeffect.h"
#include "sdlgpu/sdl_gpu_scene.h"

class gxCanvas;

class gxMesh;
class gxLight;
class gxGraphics;
class gxTexture;
class gxEffect;

class gxScene {
public:
	gxGraphics* graphics;
	IDirect3DDevice9Ex* dir3dDev;

	gxScene(gxGraphics* graphics, gxCanvas* target);
	~gxScene();


	/***** GX INTERFACE *****/
public:
	enum {
		MAX_TEXTURES = 8
	};
	enum {
		FX_FULLBRIGHT = 0x0001,
		FX_VERTEXCOLOR = 0x0002,
		FX_FLATSHADED = 0x0004,
		FX_NOFOG = 0x0008,
		FX_DOUBLESIDED = 0x0010,
		FX_VERTEXALPHA = 0x0020,
		FX_WIREFRAME = 0x0040,

		FX_ALPHATEST = 0x2000,
		FX_CONDLIGHT = 0x4000,
		FX_EMISSIVE = 0x8000
	};
	enum {
		BLEND_REPLACE = 0,
		BLEND_ALPHA = 1,
		BLEND_MULTIPLY = 2,
		BLEND_ADD = 3,
		BLEND_DOT3 = 4,
		BLEND_MULTIPLY2 = 5,
		BLEND_BUMPENVMAP = 6,
	};
	enum {
		ZMODE_NORMAL = 0,
		ZMODE_DISABLE = 1,
		ZMODE_CMPONLY = 2
	};
	enum {
		FOG_NONE = 0,
		FOG_LINEAR = 1,
		FOG_EXP = 2,
		FOG_EXP2 = 3,
	};
	enum {
		TEX_COORDS2 = 0x0001
	};
	struct Matrix {
		float elements[4][3];
	};
	struct RenderState {
		float color[3];
		float shininess, alpha;
		int blend, fx;
		struct TexState {
			gxCanvas* canvas;
			const Matrix* matrix;
			int blend, flags;
			DWORD bumpEnvMat[2][2];
			DWORD bumpEnvScale;
			DWORD bumpEnvOffset;
		}tex_states[MAX_TEXTURES];
		gxEffect* effect;
	};

	//state
	int  hwTexUnits();
	int  gfxDriverCaps3D();

	void setWBuffer(bool enable);
	void setHWMultiTex(bool enable);
	void setDither(bool enable);
	void setAntialias(bool enable);
	void setWireframe(bool enable);
	void setFlippedTris(bool enable);
	void setAmbient(const float rgb[]);
	void setAmbient2(const float rgb[]);
	void setFogColor(const float rgb[3]);
	void setFogRange(float nr, float fr);
	void setFogDensity(float den);
	void setFogMode(int mode);
	void setZMode(int mode);
	void setViewport(int x, int y, int w, int h);
	void setOrthoProj(float nr, float fr, float nr_w, float nr_h);
	void setPerspProj(float nr, float fr, float nr_w, float nr_h);
	void setViewMatrix(const Matrix* matrix);
	void setWorldMatrix(const Matrix* matrix);
	void setEyePosition(const float pos[3]);
	void setRenderState(const RenderState& state);
	void setEffect(gxEffect* effect);
	void setDepthTarget(gxCanvas* c) { depthTarget = c; }
	void setBumpNormalize(bool enable) { bumpNormalize = enable; }

	//rendering
	bool begin(const std::vector<gxLight*>& lights);
	void invalidateD3DCaches();
	void clear(const float rgb[3], float alpha, float z, bool clear_argb, bool clear_z);
	void render(gxMesh* mesh, int first_vert, int vert_cnt, int first_tri, int tri_cnt);
	void renderSkinned(gxMesh* mesh, int first_vert, int vert_cnt, int first_tri, int tri_cnt, const float* bone_data, int bone_cnt);
	void end();
	bool hasGpuImage() const;
	bool presentGpuFrame(struct SDL_GPUDevice* dev, struct SDL_Window* win);
	bool presentGpuFrameWithCanvas(struct SDL_GPUDevice* dev, struct SDL_Window* win, gxCanvas* canvas);
	bool blitFrameToTexture(struct SDL_GPUDevice* dev, gxCanvas* dest, int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh);

	//lighting
	gxLight* createLight(int flags);
	void freeLight(gxLight* l);

	//info
	int getTrianglesDrawn()const;
	gxEffect* getEffect() const;

	DWORD textureLodBias = 0;
	int textureAnisotropic = 0;

private:
	gxCanvas* target;
	gxCanvas* depthTarget = nullptr;
	bool wbuffer, dither, antialias, wireframe, flipped;
	unsigned ambient, ambient2, fogcolor;
	int caps_level, fogmode, zmode, max_lights;
	float fogrange_nr, fogrange_fr, fog_density;
	D3DVIEWPORT9 viewport;
	bool ortho_proj;
	float frustum_nr, frustum_fr, frustum_w, frustum_h;
	D3DMATRIX projmatrix, viewmatrix, worldmatrix;
	D3DMATRIX inv_viewmatrix;
	D3DMATERIAL9 material;
	float shininess;
	int blend, fx;
	struct TexState {
		gxCanvas* canvas;
		int blend, flags;
		DWORD bumpEnvMat[2][2];
		DWORD bumpEnvScale;
		DWORD bumpEnvOffset;
		D3DMATRIX matrix;
		bool mat_valid;
	};
	TexState texstate[MAX_TEXTURES];
	int n_texs, tris_drawn;

	gxEffect* currentEffect;
	D3DXMATRIX currentWorld, currentView, currentProj;
	float eyePos[3];

	sdlgpu::GpuSceneFrame gpuFrame;
	bool gpuOnlyFrame = true;
	bool gpuWinHidden = false;
	bool gpuShadersOk = false;

	bool bumpNormalize = false;
	float bumpUniformScale = 1.0f;

	std::set<gxLight*> _allLights;
	std::vector<gxLight*> _curLights;

	int d3d_rs[210];
	int d3d_tss[8][33];
	int d3d_samp[8][16];
	IDirect3DBaseTexture9* d3d_tex[8];

	RenderState lastRenderState;
	bool lastRenderStateValid;
	int lightModeCache = -1;

	uint64_t lastStateKey;

	void setRS(int n, int t);
	void setTSS(int n, int s, int t);
	void setSamp(int n, int s, int t);
	void setTex(int n, IDirect3DBaseTexture9* t);

	void setLights();
	void setZMode();
	void setAmbient();
	void setFogMode();
	void setTriCull();
	void setTexState(int index, const TexState& state, bool set_blend);
	void setEffectInternal(gxEffect* e);
	void setSkinShaderConstants();
	void computeGpuMVP(float out[16]) const;
	void computeGpuWorld(float out[16]) const;
	void computeGpuMeshUniforms(sdlgpu::MeshUniforms& u) const;
	void computeGpuSkinnedUniforms(sdlgpu::MeshUniforms& u) const;
	void fillGpuDrawParams(struct sdlgpu::MeshDrawParams& p, struct SDL_GPUDevice* dev);
	int fillGpuExtraStages(struct sdlgpu::MeshExtraStage* out, int maxOut, struct SDL_GPUDevice* dev);
	bool gpuTexGenOk() const;
};

#endif