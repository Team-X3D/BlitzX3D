#include "std.h"
#include "gxscene.h"
#include "gxgraphics.h"
#include "gxruntime.h"
#include "gxeffect.h"
#include "gxmesh.h"
#include "sdlgpu/sdl_gpu_texture.h"
#include "sdlgpu/sdl_gpu_context.h"
#include "sdlgpu/sdl_gpu_mesh.h"
#include "sdlgpu/sdl_gpu_pipeline.h"
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_video.h>

static bool can_wb;
static int  hw_tex_stages, tex_stages;
static gxScene* t_lastD3DScene = nullptr;
static float BLACK[] = { 0,0,0 };
static float WHITE[] = { 1,1,1 };
static float GRAY[] = { .5f,.5f,.5f };
static D3DMATRIX sphere_mat, nullmatrix;

void gxScene::setRS(int n, int t) {
	if(d3d_rs[n] == t) return;
	dir3dDev->SetRenderState((D3DRENDERSTATETYPE)n, t);
	d3d_rs[n] = t;
}

void gxScene::setTSS(int n, int s, int t) {
	if(d3d_tss[n][s] == t) return;
	dir3dDev->SetTextureStageState(n, (D3DTEXTURESTAGESTATETYPE)s, t);
	d3d_tss[n][s] = t;
}

void gxScene::setSamp(int n, int s, int t) {
	if (d3d_samp[n][s] == t) return;
	dir3dDev->SetSamplerState(n, (D3DSAMPLERSTATETYPE)s, t);
	d3d_samp[n][s] = t;
}

void gxScene::setTex(int n, IDirect3DBaseTexture9* t) {
	if (d3d_tex[n] == t) return;
	dir3dDev->SetTexture(n, t);
	d3d_tex[n] = t;
}

static int computeAlphaRef(const gxScene::RenderState& rs) {
	if (rs.fx & gxScene::FX_VERTEXALPHA) return 0;
	int base = 128;
	for (int k = 0; k < gxScene::MAX_TEXTURES; ++k) {
		const gxScene::RenderState::TexState& ts = rs.tex_states[k];
		if (ts.canvas && (ts.canvas->getFlags() & gxCanvas::CANVAS_TEX_MASK)) {
			base = 200;
			break;
		}
	}
	return (int)(base * rs.alpha);
}

static uint64_t computeRenderStateKey(const gxScene::RenderState& rs) {
	uint64_t key = 0;

	key ^= (uint64_t)rs.blend;
	key ^= (uint64_t)rs.fx << 8;
	key ^= (uint64_t)(rs.alpha * 255.0f) << 16;
	key ^= (uint64_t)(rs.shininess * 255.0f) << 24;

	uint32_t r_bits, g_bits, b_bits;
	memcpy(&r_bits, &rs.color[0], sizeof(float));
	memcpy(&g_bits, &rs.color[1], sizeof(float));
	memcpy(&b_bits, &rs.color[2], sizeof(float));
	key ^= (uint64_t)r_bits << 40;
	key ^= (uint64_t)g_bits >> 8;
	key ^= (uint64_t)b_bits << 20;

	if (rs.effect) key ^= (uint64_t)(uintptr_t)rs.effect << 32;

	for (int i = 0; i < gxScene::MAX_TEXTURES; ++i) {
		const auto& ts = rs.tex_states[i];
		if (!ts.canvas) continue;

		uint64_t ptr = (uint64_t)(uintptr_t)ts.canvas;
		key ^= (ptr << (i * 8)) ^ (ptr >> (64 - i * 8));

		key ^= (uint64_t)ts.blend << (i * 4 + 32);
		key ^= (uint64_t)ts.flags << (i * 4 + 40);

		key ^= (uint64_t)ts.bumpEnvMat[0][0] << (i * 3);
		key ^= (uint64_t)ts.bumpEnvMat[0][1] << (i * 3 + 1);
		key ^= (uint64_t)ts.bumpEnvMat[1][0] << (i * 3 + 2);
		key ^= (uint64_t)ts.bumpEnvMat[1][1] << (i * 3 + 3);
		key ^= (uint64_t)ts.bumpEnvScale << (i * 3 + 4);
		key ^= (uint64_t)ts.bumpEnvOffset << (i * 3 + 5);

		if (ts.matrix) {
			const float* m = &ts.matrix->elements[0][0];
			for (int j = 0; j < 12; ++j) {
				uint32_t bits = *reinterpret_cast<const uint32_t*>(m + j);
				key ^= (uint64_t)bits << (j % 32);
				key ^= (uint64_t)bits >> (32 - (j % 32));
			}
		}
	}
	return key;
}

gxScene::gxScene(gxGraphics* g, gxCanvas* t) :
	graphics(g), target(t), dir3dDev(g->dir3dDev),
	n_texs(0), tris_drawn(0), lastStateKey(0),
	textureLodBias(0), textureAnisotropic(0) {

	currentEffect = nullptr;
	D3DXMatrixIdentity(&currentWorld);
	D3DXMatrixIdentity(&currentView);
	D3DXMatrixIdentity(&currentProj);
	eyePos[0] = eyePos[1] = eyePos[2] = 0;

	memset(d3d_rs, 0x55, sizeof(d3d_rs));
	memset(d3d_tss, 0x55, sizeof(d3d_tss));
	memset(d3d_samp, 0x55, sizeof(d3d_samp));
	memset(d3d_tex, 0x55, sizeof(d3d_tex));
	memset(&lastRenderState, 0, sizeof(lastRenderState));
	lastRenderStateValid = false;

	//nomalize normals
	setRS(D3DRS_NORMALIZENORMALS, TRUE);

	//vertex coloring
	setRS(D3DRS_COLORVERTEX, FALSE);
	setRS(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1);
	setRS(D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_COLOR1);
	setRS(D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL);
	setRS(D3DRS_SPECULARMATERIALSOURCE, D3DMCS_MATERIAL);

	//Alpha test
	setRS(D3DRS_ALPHATESTENABLE, FALSE);
	setRS(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
	setRS(D3DRS_ALPHAREF, 128);

	//source/dest blending modes
	setRS(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
	setRS(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

	//suss out caps
	can_wb = false;
	hw_tex_stages = 1;
	caps_level = 100;
	max_lights = 8;

	D3DCAPS9 caps8;
	if (SUCCEEDED(dir3dDev->GetDeviceCaps(&caps8))) {
		DWORD rasterCaps = caps8.RasterCaps;

		//texture stages
		hw_tex_stages = caps8.MaxSimultaneousTextures;
		max_lights = caps8.MaxActiveLights;

		//depth format must be 16-bit for safe Wbuffer use
		if ((rasterCaps & D3DPRASTERCAPS_WBUFFER) && graphics->zbuffFmt == D3DFMT_D16)
			can_wb = true;

		//fog mode
		if ((rasterCaps & D3DPRASTERCAPS_FOGTABLE) && (rasterCaps & D3DPRASTERCAPS_WFOG)) {
			setRS(D3DRS_FOGVERTEXMODE, D3DFOG_NONE);
			setRS(D3DRS_FOGTABLEMODE, D3DFOG_LINEAR);
		}
		else {
			setRS(D3DRS_FOGTABLEMODE, D3DFOG_NONE);
			setRS(D3DRS_FOGVERTEXMODE, D3DFOG_LINEAR);
		}

		//cube maps
		if (caps8.TextureCaps & D3DPTEXTURECAPS_CUBEMAP)
			caps_level = 110;
	}
	tex_stages = hw_tex_stages;

	//default texture states
	for(int n = 0; n < hw_tex_stages; ++n) {
		setTSS(n, D3DTSS_COLORARG1, D3DTA_TEXTURE);
		setTSS(n, D3DTSS_COLORARG2, D3DTA_CURRENT);
		setTSS(n, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
		setTSS(n, D3DTSS_ALPHAARG2, D3DTA_CURRENT);
		setSamp(n, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
		setSamp(n, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
		setSamp(n, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
	}
	setHWMultiTex(true);

	//globals
	sphere_mat._11 = .5f;  sphere_mat._22 = -.5f; sphere_mat._33 = .5f;
	sphere_mat._41 = .5f;  sphere_mat._42 = .5f;  sphere_mat._43 = .5f;
	nullmatrix._11 = nullmatrix._22 = nullmatrix._33 = nullmatrix._44 = 1;

	//set null renderstate
	memset(&material, 0, sizeof(material));
	shininess = 0; blend = BLEND_REPLACE; fx = 0;
	for(int k = 0; k < MAX_TEXTURES; ++k) memset(&texstate[k], 0, sizeof(texstate[k]));

	wbuffer = can_wb;
	dither = false; setDither(true);
	antialias = false;
	setRS(D3DRS_MULTISAMPLEANTIALIAS, FALSE);
	wireframe = true; setWireframe(false);
	flipped = true; setFlippedTris(false);
	ambient = ~0; setAmbient(GRAY);
	ambient2 = ~0; setAmbient2(BLACK);
	fogcolor = ~0; setFogColor(BLACK);
	fogrange_nr = fogrange_fr = 0; setFogRange(1, 1000);
	fog_density = 0.0; setFogDensity(1.0);
	fogmode = FOG_LINEAR; setFogMode(FOG_NONE);
	zmode = -1; setZMode(ZMODE_NORMAL);
	memset(&projmatrix, 0, sizeof(projmatrix));
	ortho_proj = true; frustum_nr = frustum_fr = frustum_w = frustum_h = 0;
	setPerspProj(1, 1000, 1, 1);

	memset(&viewport, 0, sizeof(viewport));
	viewport.MaxZ = 1.0f;
	setViewport(0, 0, target->getWidth(), target->getHeight());

	viewmatrix = nullmatrix; setViewMatrix(0);
	worldmatrix = nullmatrix; setWorldMatrix(0);

	//set default renderstate
	blend = fx = ~0; shininess = 1;
	RenderState state; memset(&state, 0, sizeof(state));
	state.color[0] = state.color[1] = state.color[2] = state.alpha = 1;
	state.blend = BLEND_REPLACE;
	setRenderState(state);
}

gxScene::~gxScene() {
	if (t_lastD3DScene == this) t_lastD3DScene = nullptr;
	while(_allLights.size()) freeLight(*_allLights.begin());
	if (graphics && graphics->runtime && graphics->runtime->sdlGpu) {
		sdlgpu::ReleaseSceneTargets(graphics->runtime->sdlGpu, gpuFrame);
	}
}

void gxScene::setEffect(gxEffect* e) {
	currentEffect = e;
}

gxEffect* gxScene::getEffect() const {
	return currentEffect;
}

void gxScene::setTexState(int n, const TexState& state, bool tex_blend) {

	int flags = state.canvas->getFlags();
	int tc_index = state.flags & TEX_COORDS2 ? 1 : 0;

	//set canvas
	setTex(n, state.canvas->getTexSurface());

	//set addressing modes
	setSamp(n, D3DSAMP_ADDRESSU, (flags & gxCanvas::CANVAS_TEX_CLAMPU) ? D3DTADDRESS_CLAMP : D3DTADDRESS_WRAP);
	setSamp(n, D3DSAMP_ADDRESSV, (flags & gxCanvas::CANVAS_TEX_CLAMPV) ? D3DTADDRESS_CLAMP : D3DTADDRESS_WRAP);

	switch(flags & (
		gxCanvas::CANVAS_TEX_POINT |
		gxCanvas::CANVAS_TEX_BILINEAR |
		gxCanvas::CANVAS_TEX_NOFILTER |
		gxCanvas::CANVAS_TEX_ANISOTROPIC)) {
		case gxCanvas::CANVAS_TEX_POINT:
			setSamp(n, D3DSAMP_MINFILTER, D3DTEXF_POINT);
			setSamp(n, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
			setSamp(n, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
			break;
		case gxCanvas::CANVAS_TEX_BILINEAR:
			setSamp(n, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
			setSamp(n, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
			setSamp(n, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
			break;
		case gxCanvas::CANVAS_TEX_NOFILTER:
			setSamp(n, D3DSAMP_MINFILTER, D3DTEXF_POINT);
			setSamp(n, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
			setSamp(n, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
			break;
		case gxCanvas::CANVAS_TEX_ANISOTROPIC:
			setSamp(n, D3DSAMP_MINFILTER, D3DTEXF_ANISOTROPIC);
			setSamp(n, D3DSAMP_MAGFILTER, D3DTEXF_ANISOTROPIC);
			setSamp(n, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
			setSamp(n, D3DSAMP_MAXANISOTROPY, (textureAnisotropic > 0) ? textureAnisotropic : 1);
			break;
		default:
			setSamp(n, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
			setSamp(n, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
			setSamp(n, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
	}

	//texgen
	switch(flags & (
		gxCanvas::CANVAS_TEX_SPHERE |
		gxCanvas::CANVAS_TEX_CUBE)) {

		case gxCanvas::CANVAS_TEX_SPHERE:
			setTSS(n, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACENORMAL);//|tc_index );
			setTSS(n, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
			dir3dDev->SetTransform((D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + n), &sphere_mat);
			break;
		case gxCanvas::CANVAS_TEX_CUBE:
			switch(state.canvas->cubeMode() & 3) {
				case gxCanvas::CUBEMODE_NORMAL:
					setTSS(n, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACENORMAL);//|tc_index );
					break;
				case gxCanvas::CUBEMODE_POSITION:
					setTSS(n, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEPOSITION);//|tc_index );
					break;
				default:
					setTSS(n, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR);//|tc_index );
					break;
			}
			if(state.canvas->cubeMode() & 4) {
				setTSS(n, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
			}
			else {
				setTSS(n, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT3);//COUNT4|D3DTTFF_PROJECTED );
				dir3dDev->SetTransform((D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + n), &inv_viewmatrix);
			}
			break;
		default:
			setTSS(n, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_PASSTHRU | tc_index);
			if(state.mat_valid) {
				setTSS(n, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
				dir3dDev->SetTransform((D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + n), (D3DMATRIX*)&state.matrix);
			}
			else {
				setTSS(n, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
			}
	}

	if(!tex_blend) return;

	//blending
	switch(state.blend) {
		case BLEND_ALPHA:
			setTSS(n, D3DTSS_COLOROP, D3DTOP_BLENDTEXTUREALPHA);
			break;
		case BLEND_MULTIPLY:
			setTSS(n, D3DTSS_COLOROP, D3DTOP_MODULATE);
			break;
		case BLEND_ADD:
			setTSS(n, D3DTSS_COLOROP, D3DTOP_ADD);
			break;
		case BLEND_DOT3:
			setTSS(n, D3DTSS_COLOROP, D3DTOP_DOTPRODUCT3);
			break;
		case BLEND_MULTIPLY2:
			setTSS(n, D3DTSS_COLOROP, D3DTOP_MODULATE2X);
			break;
		case BLEND_BUMPENVMAP:
			setTSS(n, D3DTSS_COLOROP, D3DTOP_BUMPENVMAP);
			break;
	}

	float m00 = *(float*)&state.bumpEnvMat[0][0];
	float m01 = *(float*)&state.bumpEnvMat[0][1];
	float m10 = *(float*)&state.bumpEnvMat[1][0];
	float m11 = *(float*)&state.bumpEnvMat[1][1];

	if (bumpNormalize && state.canvas) {
		float w = (float)state.canvas->getWidth();
		float h = (float)state.canvas->getHeight();
		if (w > 0.0f) { m00 *= w; m01 *= w; }
		if (h > 0.0f) { m10 *= h; m11 *= h; }
	}

	setTSS(n, D3DTSS_BUMPENVMAT00, *(DWORD*)&m00);
	setTSS(n, D3DTSS_BUMPENVMAT01, *(DWORD*)&m01);
	setTSS(n, D3DTSS_BUMPENVMAT10, *(DWORD*)&m10);
	setTSS(n, D3DTSS_BUMPENVMAT11, *(DWORD*)&m11);
	setTSS(n, D3DTSS_BUMPENVLSCALE, state.bumpEnvScale);
	setTSS(n, D3DTSS_BUMPENVLOFFSET, state.bumpEnvOffset);
	setTSS(n, D3DTSS_ALPHAOP, (flags & gxCanvas::CANVAS_TEX_ALPHA) ? D3DTOP_MODULATE : D3DTOP_SELECTARG2);
}

int  gxScene::hwTexUnits() {
	return tex_stages;
}

int  gxScene::gfxDriverCaps3D() {
	return caps_level;
}

void gxScene::setZMode() {
	switch(zmode) {
		case ZMODE_NORMAL:
			setRS(D3DRS_ZENABLE, D3DZB_TRUE);
			setRS(D3DRS_ZWRITEENABLE, true);
			break;
		case ZMODE_DISABLE:
			setRS(D3DRS_ZENABLE, D3DZB_FALSE);
			setRS(D3DRS_ZWRITEENABLE, false);
			break;
		case ZMODE_CMPONLY:
			setRS(D3DRS_ZENABLE, D3DZB_TRUE);
			setRS(D3DRS_ZWRITEENABLE, false);
			break;
	}
}

void gxScene::setLights() {
	if(fx & FX_FULLBRIGHT) {
		//no lights on
		for(int n = 0; n < _curLights.size(); ++n) dir3dDev->LightEnable(n, false);
	}
	else if(fx & FX_CONDLIGHT) {
		//some lights on
		for(int n = 0; n < _curLights.size(); ++n) {
			gxLight* light = _curLights[n];
			bool enable = light->d3d_light.Type != D3DLIGHT_DIRECTIONAL;
			dir3dDev->LightEnable(n, enable);
		}
	}
	else {
		//all lights on
		for(int n = 0; n < _curLights.size(); ++n) dir3dDev->LightEnable(n, true);
	}
}

void gxScene::setAmbient() {
	int n = (fx & FX_FULLBRIGHT) ? 0xffffff : ((fx & FX_CONDLIGHT) ? ambient2 : ambient);
	setRS(D3DRS_AMBIENT, n);
}

void gxScene::setFogMode() {
	if(!!(fx & FX_NOFOG)) {
		setRS(D3DRS_FOGENABLE, false);
		return;
	}
	switch(fogmode) {
		case FOG_NONE:
			setRS(D3DRS_FOGENABLE, false);
			break;
		case FOG_EXP:
			setRS(D3DRS_FOGENABLE, true);
			setRS(D3DRS_FOGTABLEMODE, D3DFOG_EXP);
			break;
		case FOG_EXP2:
			setRS(D3DRS_FOGENABLE, true);
			setRS(D3DRS_FOGTABLEMODE, D3DFOG_EXP2);
			break;
		case FOG_LINEAR:
			setRS(D3DRS_FOGENABLE, true);
			setRS(D3DRS_FOGTABLEMODE, D3DFOG_LINEAR);
			break;
	}
}

void gxScene::setTriCull() {
	if(fx & FX_DOUBLESIDED) {
		setRS(D3DRS_CULLMODE, D3DCULL_NONE);
	}
	else if(flipped) {
		setRS(D3DRS_CULLMODE, D3DCULL_CW);
	}
	else {
		setRS(D3DRS_CULLMODE, D3DCULL_CCW);
	}
}

void gxScene::setHWMultiTex(bool e) {
	for(int n = 0; n < 8; ++n) {
		setTSS(n, D3DTSS_COLOROP, D3DTOP_DISABLE);
		setTSS(n, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
		setTex(n, nullptr);
	}
	for(int k = 0; k < MAX_TEXTURES; ++k) {
		memset(&texstate[k], 0, sizeof(texstate[k]));
	}
	tex_stages = e ? hw_tex_stages : 1;
	n_texs = 0;
}

void gxScene::setWBuffer(bool n) {
	if(n == wbuffer || !can_wb) return;
	wbuffer = n; setZMode();
}

void gxScene::setDither(bool n) {
	if(n == dither) return;
	dither = n; setRS(D3DRS_DITHERENABLE, dither ? true : false);
}

void gxScene::setAntialias(bool n) {
	antialias = n;
	if (graphics && graphics->runtime) {
		graphics->runtime->setAntialiasRequest(n);
	}
	if (dir3dDev) {
		setRS(D3DRS_MULTISAMPLEANTIALIAS, n ? TRUE : FALSE);
	}
}

void gxScene::setWireframe(bool n) {
	if(n == wireframe) return;
	wireframe = n;
}

void gxScene::setFlippedTris(bool n) {
	if(n == flipped) return;
	flipped = n; setTriCull();
}

void gxScene::setAmbient(const float rgb[]) {
	int n = (int(rgb[0] * 255.0f) << 16) | (int(rgb[1] * 255.0f) << 8) | int(rgb[2] * 255.0f);
	ambient = n; setAmbient();
}

void gxScene::setAmbient2(const float rgb[]) {
	int n = (int(rgb[0] * 255.0f) << 16) | (int(rgb[1] * 255.0f) << 8) | int(rgb[2] * 255.0f);
	ambient2 = n; setAmbient();
}

void gxScene::setViewport(int x, int y, int w, int h) {
	if (x == (int)viewport.X && y == (int)viewport.Y && w == (int)viewport.Width && h == (int)viewport.Height) return;
	viewport.X = x; viewport.Y = y; viewport.Width = w; viewport.Height = h;
	dir3dDev->SetViewport(&viewport);
}

void gxScene::setOrthoProj(float nr, float fr, float w, float h) {
	if(ortho_proj && nr == frustum_nr && fr == frustum_fr && w == frustum_w && h == frustum_h) return;
	frustum_nr = nr; frustum_fr = fr; frustum_w = w; frustum_h = h; ortho_proj = true;
	float W = 2 / w;
	float H = 2 / h;
	float Q = 1 / (fr - nr);
	projmatrix._11 = W;
	projmatrix._22 = H;
	projmatrix._33 = Q;
	projmatrix._34 = 0;
	projmatrix._43 = -Q * nr;
	projmatrix._44 = 1;
	currentProj = projmatrix;
	dir3dDev->SetTransform(D3DTS_PROJECTION, &projmatrix);
}

void gxScene::setPerspProj(float nr, float fr, float w, float h) {
	if(!ortho_proj && nr == frustum_nr && fr == frustum_fr && w == frustum_w && h == frustum_h) return;
	frustum_nr = nr; frustum_fr = fr; frustum_w = w; frustum_h = h; ortho_proj = false;
	float W = 2 * nr / w;
	float H = 2 * nr / h;
	float Q = fr / (fr - nr);
	projmatrix._11 = W;
	projmatrix._22 = H;
	projmatrix._33 = Q;
	projmatrix._34 = 1;
	projmatrix._43 = -Q * nr;
	projmatrix._44 = 0;
	currentProj = projmatrix;
	dir3dDev->SetTransform(D3DTS_PROJECTION, &projmatrix);
}

void gxScene::setFogColor(const float rgb[3]) {
	int n = (int(rgb[0] * 255.0f) << 16) | (int(rgb[1] * 255.0f) << 8) | int(rgb[2] * 255.0f);
	if(n == fogcolor) return;
	fogcolor = n; setRS(D3DRS_FOGCOLOR, fogcolor);
}

void gxScene::setFogRange(float nr, float fr) {
	if(nr == fogrange_nr && fr == fogrange_fr) return;
	fogrange_nr = nr; fogrange_fr = fr;
	setRS(D3DRS_FOGSTART, *(DWORD*)&fogrange_nr);
	setRS(D3DRS_FOGEND, *(DWORD*)&fogrange_fr);
}

void gxScene::setFogDensity(float den) {
	if(den == fog_density) return;
	fog_density = den;
	setRS(D3DRS_FOGDENSITY, *(DWORD*)&fog_density);
}

void gxScene::setFogMode(int n) {
	if(n == fogmode) return;
	fogmode = n; setFogMode();
}

void gxScene::setZMode(int n) {
	if(n == zmode) return;
	zmode = n; setZMode();
}

void gxScene::setViewMatrix(const Matrix* m) {
	D3DMATRIX prev = viewmatrix;

	if (m) {
		memcpy(&viewmatrix._11, m->elements[0], 12);
		memcpy(&viewmatrix._21, m->elements[1], 12);
		memcpy(&viewmatrix._31, m->elements[2], 12);
		memcpy(&viewmatrix._41, m->elements[3], 12);
		currentView = viewmatrix;
		inv_viewmatrix._11 = viewmatrix._11; inv_viewmatrix._21 = viewmatrix._12; inv_viewmatrix._31 = viewmatrix._13;
		inv_viewmatrix._12 = viewmatrix._21; inv_viewmatrix._22 = viewmatrix._22; inv_viewmatrix._32 = viewmatrix._23;
		inv_viewmatrix._13 = viewmatrix._31; inv_viewmatrix._23 = viewmatrix._32; inv_viewmatrix._33 = viewmatrix._33;
		inv_viewmatrix._44 = viewmatrix._44;
	}
	else {
		D3DXMatrixIdentity(&currentView);
		viewmatrix = inv_viewmatrix = nullmatrix;
	}

	if (memcmp(&viewmatrix, &prev, sizeof(D3DMATRIX)) == 0) return;
	dir3dDev->SetTransform(D3DTS_VIEW, &viewmatrix);
}

void gxScene::setEyePosition(const float pos[3]) {
	eyePos[0] = pos[0]; eyePos[1] = pos[1]; eyePos[2] = pos[2];
}

void gxScene::setWorldMatrix(const Matrix* m) {
	D3DMATRIX prev = worldmatrix;

	if (m) {
		memcpy(&currentWorld._11, m->elements[0], 12);
		memcpy(&currentWorld._21, m->elements[1], 12);
		memcpy(&currentWorld._31, m->elements[2], 12);
		memcpy(&currentWorld._41, m->elements[3], 12);
		worldmatrix = currentWorld;
	}
	else {
		D3DXMatrixIdentity(&currentWorld);
		worldmatrix = nullmatrix;
	}

	if (memcmp(&worldmatrix, &prev, sizeof(D3DMATRIX)) == 0) return;
	dir3dDev->SetTransform(D3DTS_WORLD, &worldmatrix);
}

void gxScene::setRenderState(const RenderState& rs) {
	setEffect(rs.effect);

	int fxChanged = rs.fx ^ fx;
	fx = rs.fx;

	setLights();
	setAmbient();

	if (lastRenderStateValid && memcmp(&rs, &lastRenderState, sizeof(rs)) == 0) {
		setFogMode();
		return;
	}

	bool setmat = false;
	if (memcmp(rs.color, &material.Diffuse.r, 12)) {
		memcpy(&material.Diffuse.r, rs.color, 12);
		memcpy(&material.Ambient.r, rs.color, 12);
		setmat = true;
	}
	if (rs.alpha != material.Diffuse.a) {
		material.Diffuse.a = rs.alpha;
		if (rs.fx & FX_ALPHATEST) {
			setRS(D3DRS_ALPHAREF, computeAlphaRef(rs));
		}
		setmat = true;
	}
	if (rs.shininess != shininess) {
		shininess = rs.shininess;
		float t = shininess > 0 ? (shininess < 1 ? shininess : 1.f) : 0;
		material.Specular.r = material.Specular.g = material.Specular.b = t;
		material.Power = shininess * 128.f;
		setRS(D3DRS_SPECULARENABLE, shininess > 0 ? TRUE : FALSE);
		setmat = true;
	}
	if(rs.blend != blend) {
		blend = rs.blend;
		switch(blend) {
			case BLEND_REPLACE:
				setRS(D3DRS_ALPHABLENDENABLE, false);
				break;
			case BLEND_ALPHA:
				setRS(D3DRS_ALPHABLENDENABLE, true);
				setRS(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
				setRS(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
				break;
			case BLEND_MULTIPLY:
				setRS(D3DRS_ALPHABLENDENABLE, true);
				setRS(D3DRS_SRCBLEND, D3DBLEND_DESTCOLOR);
				setRS(D3DRS_DESTBLEND, D3DBLEND_ZERO);
				break;
			case BLEND_ADD:
				setRS(D3DRS_ALPHABLENDENABLE, true);
				setRS(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
				setRS(D3DRS_DESTBLEND, D3DBLEND_ONE);
				break;
		}
	}
	if(fxChanged) {
		if(fxChanged & FX_VERTEXCOLOR) {
			setRS(D3DRS_COLORVERTEX, fx & FX_VERTEXCOLOR ? true : false);
		}
		if(fxChanged & FX_FLATSHADED) {
			setRS(D3DRS_SHADEMODE, fx & FX_FLATSHADED ? D3DSHADE_FLAT : D3DSHADE_GOURAUD);
		}
		if(fxChanged & FX_NOFOG) {
			setFogMode();
		}
		if(fxChanged & FX_DOUBLESIDED) {
			setTriCull();
		}
		if(!wireframe && fxChanged & FX_WIREFRAME) {
			setRS(D3DRS_FILLMODE, fx & FX_WIREFRAME ? D3DFILL_WIREFRAME : D3DFILL_SOLID);
		}
		if(fxChanged & (FX_EMISSIVE | FX_VERTEXCOLOR)) {
			bool vc = fx & (FX_VERTEXCOLOR | FX_EMISSIVE);
			setRS(D3DRS_COLORVERTEX, vc ? true : false);
			bool emissive = fx & FX_EMISSIVE;
			bool vcolor = fx & FX_VERTEXCOLOR;
			setRS(D3DRS_DIFFUSEMATERIALSOURCE, vcolor ? D3DMCS_COLOR1 : D3DMCS_MATERIAL);
			setRS(D3DRS_AMBIENTMATERIALSOURCE, vcolor ? D3DMCS_COLOR1 : D3DMCS_MATERIAL);
			setRS(D3DRS_EMISSIVEMATERIALSOURCE, emissive ? D3DMCS_COLOR1 : D3DMCS_MATERIAL);
		}
		if(fx & FX_ALPHATEST) {
			setRS(D3DRS_ALPHAREF, computeAlphaRef(rs));
			setRS(D3DRS_ALPHATESTENABLE, true);
		}
		else if(fxChanged & FX_ALPHATEST) {
			setRS(D3DRS_ALPHATESTENABLE, false);
		}
	}
	setFogMode();
	if(setmat) {
		dir3dDev->SetMaterial(&material);
	}

	n_texs = 0;
	TexState* hw = texstate;
	for(int k = 0; k < MAX_TEXTURES; ++k) {
		const RenderState::TexState& ts = rs.tex_states[k];
		if(!ts.canvas || !ts.blend) continue;
		bool settex = false;
		ts.canvas->getTexSurface();	//force mipmap rebuild
		if(ts.canvas != hw->canvas) { hw->canvas = ts.canvas; settex = true; }
		if(ts.blend != hw->blend) { hw->blend = ts.blend; settex = true; }
		if(ts.flags != hw->flags) { hw->flags = ts.flags; settex = true; }
		if(ts.bumpEnvMat[0][0] != hw->bumpEnvMat[0][0]) { hw->bumpEnvMat[0][0] = ts.bumpEnvMat[0][0]; settex = true; }
		if(ts.bumpEnvMat[1][0] != hw->bumpEnvMat[1][0]) { hw->bumpEnvMat[1][0] = ts.bumpEnvMat[1][0]; settex = true; }
		if(ts.bumpEnvMat[0][1] != hw->bumpEnvMat[0][1]) { hw->bumpEnvMat[0][1] = ts.bumpEnvMat[0][1]; settex = true; }
		if(ts.bumpEnvMat[1][1] != hw->bumpEnvMat[1][1]) { hw->bumpEnvMat[1][1] = ts.bumpEnvMat[1][1]; settex = true; }
		if(ts.bumpEnvScale != hw->bumpEnvScale) { hw->bumpEnvScale = ts.bumpEnvScale; settex = true; }
		if(ts.bumpEnvOffset != hw->bumpEnvOffset) { hw->bumpEnvOffset = ts.bumpEnvOffset; settex = true; }
		if(ts.matrix || hw->mat_valid) {
			if(ts.matrix) {
				memcpy(&hw->matrix._11, ts.matrix->elements[0], 12);
				memcpy(&hw->matrix._21, ts.matrix->elements[1], 12);
				memcpy(&hw->matrix._31, ts.matrix->elements[2], 12);
				memcpy(&hw->matrix._41, ts.matrix->elements[3], 12);
				hw->mat_valid = true;
			}
			else {
				hw->mat_valid = false;
			}
			settex = true;
		}
		if(settex && n_texs < tex_stages) {
			setTexState(n_texs, *hw, true);
		}
		++hw; ++n_texs;
	}
	for(int s = n_texs; s < tex_stages; ++s) {
		if(texstate[s].canvas) {
			texstate[s].canvas = 0;
			setTSS(s, D3DTSS_COLOROP, D3DTOP_DISABLE);
			setTSS(s, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
			setTex(s, nullptr);
		}
	}
	lastRenderState = rs;
	lastRenderStateValid = true;
}

void gxScene::invalidateD3DCaches() {
	memset(d3d_rs, 0x55, sizeof(d3d_rs));
	memset(d3d_tss, 0x55, sizeof(d3d_tss));
	memset(d3d_samp, 0x55, sizeof(d3d_samp));
	memset(d3d_tex, 0x55, sizeof(d3d_tex));
}

bool gxScene::begin(const std::vector<gxLight*>& lights) {
	bool sdl = graphics && graphics->runtime && graphics->runtime->sdlGpu;
	if (sdl) {
		if (t_lastD3DScene != this) { invalidateD3DCaches(); t_lastD3DScene = this; }
	}
	else {
		if (!graphics->ensureD3DBegun()) return false;
		invalidateD3DCaches();
	}

	lastRenderStateValid = false;
	blend = fx = ~0;
	shininess = -1;

	dir3dDev->SetRenderState(D3DRS_LIGHTING, TRUE);

	//clear textures!
	int n;
	for(n = 0; n < tex_stages; ++n) {
		texstate[n].canvas = 0;
		setTSS(n, D3DTSS_COLOROP, D3DTOP_DISABLE);
		setTSS(n, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
		setTex(n, nullptr);
		setSamp(n, D3DSAMP_MIPMAPLODBIAS, textureLodBias);
	}

	//set light states
	_curLights.clear();
	for(n = 0; n < max_lights; ++n) {
		if(n < lights.size()) {
			_curLights.push_back(lights[n]);
			dir3dDev->SetLight(n, &_curLights[n]->d3d_light);
		}
		else {
			dir3dDev->LightEnable(n, false);
		}
	}
	setLights();

	IDirect3DSurface9* depthSurf = depthTarget ? depthTarget->z_surf : target->z_surf;
	if (depthSurf) {
		dir3dDev->SetRenderTarget(0, target->surf);
		dir3dDev->SetDepthStencilSurface(depthSurf);
	}

	dir3dDev->SetViewport(&viewport);

	D3DRECT clearRect = {(LONG)viewport.X, (LONG)viewport.Y, (LONG)(viewport.X + viewport.Width), (LONG)(viewport.Y + viewport.Height) };

	dir3dDev->Clear(0, NULL, D3DCLEAR_ZBUFFER, 0, 1.0f, 0);

	setRS(D3DRS_FILLMODE, wireframe ? D3DFILL_WIREFRAME : D3DFILL_SOLID);
	setRS(D3DRS_MULTISAMPLEANTIALIAS, antialias ? TRUE : FALSE);

	gpuOnlyFrame = true;
	if (graphics && graphics->runtime && graphics->runtime->sdlGpu && graphics->runtime->sdlWindow) {
		if (graphics->runtime->vwaitPending) {
			graphics->runtime->vwaitPending = false;
			sdlgpu::SetVSync((SDL_GPUDevice*)graphics->runtime->sdlGpu,
				(SDL_Window*)graphics->runtime->sdlWindow, graphics->runtime->vwaitValue);
		}
		graphics->runtime->sceneBeganSinceFlip = true;
		sdlgpu::BeginSceneFrame(gpuFrame, (SDL_GPUDevice*)graphics->runtime->sdlGpu, (SDL_Window*)graphics->runtime->sdlWindow);
	}

	return true;
}

void gxScene::clear(const float rgb[3], float alpha, float z, bool clear_argb, bool clear_z) {
	if(!clear_argb && !clear_z) return;
	int flags = (clear_argb ? D3DCLEAR_TARGET : 0) | (clear_z ? D3DCLEAR_ZBUFFER : 0);
	unsigned argb = (int(alpha * 255.0f) << 24) | (int(rgb[0] * 255.0f) << 16) | (int(rgb[1] * 255.0f) << 8) | int(rgb[2] * 255.0f);
	if (clear_argb && gpuFrame.ready() && target) {
		argb = target->getClsColor();
	}
	dir3dDev->Clear(0, 0, flags, argb, z, 0);
	if (gpuFrame.ready()) {
		sdlgpu::BeginScenePass(gpuFrame, (int)viewport.X, (int)viewport.Y,
			(int)viewport.Width, (int)viewport.Height,
			rgb[0], rgb[1], rgb[2], clear_argb, clear_z);
	}
}

void gxScene::render(gxMesh* mesh, int first_vert, int vert_cnt, int first_tri, int tri_cnt) {
	bool drewGpu = false;
	if (gpuFrame.ready() && mesh && !mesh->isSkinned() && mesh->getGpuMirror()) {
		bool skipGpu = false;
		if (graphics && graphics->runtime && graphics->runtime->sdlWindow) {
			SDL_Window* sdlWin = graphics->runtime->sdlWindow;
			SDL_WindowFlags wf = SDL_GetWindowFlags(sdlWin);
			if (wf & SDL_WINDOW_MINIMIZED) skipGpu = true;
			if (wf & SDL_WINDOW_HIDDEN) skipGpu = true;
		}
		if (gpuFrame.dev && SDL_GetGPUShaderFormats(gpuFrame.dev) == SDL_GPU_SHADERFORMAT_INVALID) skipGpu = true;
		if (!skipGpu && !gpuFrame.active()) {
			if (!sdlgpu::BeginScenePass(gpuFrame, (int)viewport.X, (int)viewport.Y,
				(int)viewport.Width, (int)viewport.Height, 0, 0, 0, false, false)) skipGpu = true;
		}
		if (!skipGpu) {
			sdlgpu::MeshUniforms uniforms;
			computeGpuMeshUniforms(uniforms);
			SDL_GPUDevice* dev = gpuFrame.dev ? gpuFrame.dev : (graphics && graphics->runtime ? (SDL_GPUDevice*)graphics->runtime->sdlGpu : nullptr);
			if (dev) {
				sdlgpu::MeshDrawParams p;
				fillGpuDrawParams(p, dev);
				sdlgpu::RenderSceneMesh(gpuFrame, mesh->getGpuMirror(), uniforms, first_vert, vert_cnt, first_tri, tri_cnt, p);
				drewGpu = true;
			}
		}
	}
	if (!drewGpu) gpuOnlyFrame = false;

	if (currentEffect) {
		gpuOnlyFrame = false;
		UINT passes;
		if (currentEffect->begin(&passes)) {
			currentEffect->setAutoMatrices(currentWorld, currentView, currentProj);
			for (UINT p = 0; p < passes; ++p) {
				if (currentEffect->beginPass(p)) {
					mesh->render(first_vert, vert_cnt, first_tri, tri_cnt, false);
					currentEffect->endPass();
				}
			}
			currentEffect->end();
		}
		tris_drawn += tri_cnt;
		return;
	}

	bool extraTex = (n_texs > tex_stages);
	if (extraTex) gpuOnlyFrame = false;
	mesh->render(first_vert, vert_cnt, first_tri, tri_cnt, drewGpu && !extraTex);
	tris_drawn += tri_cnt;
	if(n_texs <= tex_stages) return;

	setTSS(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	setTSS(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
	if(tex_stages > 1) {
		setTSS(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
		setTSS(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
	}

	setRS(D3DRS_LIGHTING, false);
	setRS(D3DRS_ALPHABLENDENABLE, true);

	for(int k = tex_stages; k < n_texs; ++k) {
		const TexState& state = texstate[k];
		switch(state.blend) {
			case BLEND_ALPHA:
				setRS(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
				setRS(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
				break;
			case BLEND_MULTIPLY:case BLEND_DOT3:
				setRS(D3DRS_SRCBLEND, D3DBLEND_DESTCOLOR);
				setRS(D3DRS_DESTBLEND, D3DBLEND_ZERO);
				break;
			case BLEND_ADD:
				setRS(D3DRS_SRCBLEND, D3DBLEND_ONE);
				setRS(D3DRS_DESTBLEND, D3DBLEND_ONE);
				break;
		}
		setTexState(0, state, false);
		mesh->render(first_vert, vert_cnt, first_tri, tri_cnt);
		tris_drawn += tri_cnt;
	}

	setRS(D3DRS_ALPHABLENDENABLE, false);
	setRS(D3DRS_LIGHTING, true);
	if(tex_stages > 1) setTexState(1, texstate[1], true);
	setTexState(0, texstate[0], true);
}

void gxScene::computeGpuMVP(float out[16]) const {
	D3DXMATRIX mvp;
	D3DXMatrixMultiply(&mvp, &currentWorld, &currentView);
	D3DXMatrixMultiply(&mvp, &mvp, &currentProj);
	memcpy(out, &mvp, 64);
}

void gxScene::computeGpuWorld(float out[16]) const {
	memcpy(out, &currentWorld, 64);
}

void gxScene::computeGpuMeshUniforms(sdlgpu::MeshUniforms& u) const {
	computeGpuMVP(u.mvp);
	computeGpuWorld(u.world);

	unsigned amb = (fx & FX_FULLBRIGHT) ? 0xffffff : ((fx & FX_CONDLIGHT) ? ambient2 : ambient);
	u.ambient[0] = ((amb >> 16) & 0xff) / 255.0f;
	u.ambient[1] = ((amb >> 8) & 0xff) / 255.0f;
	u.ambient[2] = (amb & 0xff) / 255.0f;
	u.ambient[3] = 0.0f;

	u.matDiffuse[0] = material.Diffuse.r; u.matDiffuse[1] = material.Diffuse.g;
	u.matDiffuse[2] = material.Diffuse.b; u.matDiffuse[3] = material.Diffuse.a;
	u.matAmbient[0] = material.Ambient.r; u.matAmbient[1] = material.Ambient.g;
	u.matAmbient[2] = material.Ambient.b; u.matAmbient[3] = 1.0f;
	u.matEmissive[0] = material.Emissive.r; u.matEmissive[1] = material.Emissive.g;
	u.matEmissive[2] = material.Emissive.b; u.matEmissive[3] = 1.0f;
	u.matSpec[0] = material.Specular.r; u.matSpec[1] = material.Specular.g;
	u.matSpec[2] = material.Specular.b; u.matSpec[3] = material.Power;
	u.matSrc[0] = (fx & FX_VERTEXCOLOR) ? 1.0f : 0.0f;
	u.matSrc[1] = (fx & FX_VERTEXCOLOR) ? 1.0f : 0.0f;
	u.matSrc[2] = (fx & FX_EMISSIVE) ? 1.0f : 0.0f;
	u.matSrc[3] = 0.0f;

	u.fogColor[0] = ((fogcolor >> 16) & 0xff) / 255.0f;
	u.fogColor[1] = ((fogcolor >> 8) & 0xff) / 255.0f;
	u.fogColor[2] = (fogcolor & 0xff) / 255.0f;
	u.fogColor[3] = 1.0f;
	u.fogParams[0] = fogrange_nr; u.fogParams[1] = fogrange_fr;
	u.fogParams[2] = fog_density; u.fogParams[3] = 0.0f;

	u.eyePos[0] = eyePos[0]; u.eyePos[1] = eyePos[1]; u.eyePos[2] = eyePos[2];
	u.eyePos[3] = 0.0f;

	u.flags[0] = (fx & FX_VERTEXCOLOR) ? 1.0f : 0.0f;
	u.flags[1] = (fx & FX_FULLBRIGHT) ? 1.0f : 0.0f;
	u.flags[2] = 0.0f; u.flags[3] = 0.5f;

	u.lightCount = 0;
	if (!(fx & FX_FULLBRIGHT)) {
		for (gxLight* light : _curLights) {
			if (!light) continue;
			if (u.lightCount >= sdlgpu::kGpuMaxLights) break;
			const D3DLIGHT9& L = light->d3d_light;
			if ((fx & FX_CONDLIGHT) && L.Type == D3DLIGHT_DIRECTIONAL) continue;
			int i = u.lightCount;
			if (L.Type == D3DLIGHT_DIRECTIONAL) {
				float dx = -L.Direction.x, dy = -L.Direction.y, dz = -L.Direction.z;
				float len = sqrtf(dx * dx + dy * dy + dz * dz);
				if (len > 1e-6f) { dx /= len; dy /= len; dz /= len; }
				else { dx = 0.0f; dy = 0.0f; dz = -1.0f; }
				u.lightPos[i][0] = dx; u.lightPos[i][1] = dy; u.lightPos[i][2] = dz;
			}
			else {
				u.lightPos[i][0] = L.Position.x; u.lightPos[i][1] = L.Position.y; u.lightPos[i][2] = L.Position.z;
				u.lightAtten[i][0] = L.Attenuation0; u.lightAtten[i][1] = L.Attenuation1;
				u.lightAtten[i][2] = L.Attenuation2; u.lightAtten[i][3] = L.Range;
				float dx = L.Direction.x, dy = L.Direction.y, dz = L.Direction.z;
				float len = sqrtf(dx * dx + dy * dy + dz * dz);
				if (len > 1e-6f) { dx /= len; dy /= len; dz /= len; }
				else { dx = 0.0f; dy = 0.0f; dz = 1.0f; }
				u.lightSpotDir[i][0] = dx; u.lightSpotDir[i][1] = dy; u.lightSpotDir[i][2] = dz;
				u.lightSpotDir[i][3] = L.Falloff;
				u.lightSpotPrm[i][0] = L.Theta; u.lightSpotPrm[i][1] = L.Phi;
				u.lightSpotPrm[i][2] = 0.0f; u.lightSpotPrm[i][3] = 0.0f;
			}
		u.lightPos[i][3] = (float)L.Type;
		u.lightColor[i][0] = L.Diffuse.r; u.lightColor[i][1] = L.Diffuse.g; u.lightColor[i][2] = L.Diffuse.b;
		u.lightColor[i][3] = 1.0f;
		u.lightSpec[i][0] = L.Specular.r; u.lightSpec[i][1] = L.Specular.g; u.lightSpec[i][2] = L.Specular.b;
		u.lightSpec[i][3] = 1.0f;
		u.lightAmb[i][0] = L.Ambient.r; u.lightAmb[i][1] = L.Ambient.g; u.lightAmb[i][2] = L.Ambient.b;
		u.lightAmb[i][3] = 1.0f;
		++u.lightCount;
		}
	}
	if (!(fx & FX_NOFOG) && fogmode != FOG_NONE) u.fogParams[3] = (float)fogmode;
	if (fx & FX_ALPHATEST) {
		if (fx & FX_VERTEXALPHA) { u.flags[2] = 0.0f; }
		else {
			int base = 128;
			if (n_texs > 0 && texstate[0].canvas && (texstate[0].canvas->getFlags() & gxCanvas::CANVAS_TEX_MASK)) base = 200;
			u.flags[2] = 1.0f;
			u.flags[3] = (float)(base * material.Diffuse.a) / 255.0f;
		}
	}
}

void gxScene::setSkinShaderConstants() {
	IDirect3DDevice9* dev = dir3dDev;

	D3DXMATRIX viewProj;
	D3DXMatrixMultiply(&viewProj, &currentView, &currentProj);
	D3DXMatrixTranspose(&viewProj, &viewProj); 
	dev->SetVertexShaderConstantF(192, (const float*)&viewProj, 4);

	static const int SKIN_MAX_LIGHTS = 8;
	float lpos[8][4], ldiff[8][4], latten[8][4], ldir[8][4];
	int n = (int)_curLights.size();
	if (n > SKIN_MAX_LIGHTS) n = SKIN_MAX_LIGHTS;

	int active = 0;
	for (int i = 0; i < n; ++i) {
		gxLight* light = _curLights[i];
		bool enabled;
		if (fx & FX_FULLBRIGHT) enabled = false;
		else if (fx & FX_CONDLIGHT) enabled = (light->d3d_light.Type != D3DLIGHT_DIRECTIONAL);
		else enabled = true;
		if (!enabled) continue;

		const D3DLIGHT9& L = light->d3d_light;
		float* p = lpos[active];
		float* d = ldiff[active];
		float* a = latten[active];
		float* dir = ldir[active];

		if (L.Type == D3DLIGHT_DIRECTIONAL) {
			p[0] = -L.Direction.x; p[1] = -L.Direction.y; p[2] = -L.Direction.z; p[3] = 0;
		}
		else {
			p[0] = L.Position.x; p[1] = L.Position.y; p[2] = L.Position.z;
			p[3] = (L.Type == D3DLIGHT_SPOT) ? 2.0f : 1.0f;
		}
		d[0] = L.Diffuse.r; d[1] = L.Diffuse.g; d[2] = L.Diffuse.b; d[3] = 0;
		a[0] = L.Attenuation1; a[1] = cosf(L.Theta * 0.5f); a[2] = cosf(L.Phi * 0.5f); a[3] = L.Falloff;
		dir[0] = L.Direction.x; dir[1] = L.Direction.y; dir[2] = L.Direction.z; dir[3] = 0;

		++active;
	}
	if (active > 0) {
		dev->SetVertexShaderConstantF(196, (const float*)lpos, active);
		dev->SetVertexShaderConstantF(204, (const float*)ldiff, active);
		dev->SetVertexShaderConstantF(212, (const float*)latten, active);
		dev->SetVertexShaderConstantF(220, (const float*)ldir, active);
	}

	unsigned amb = (fx & FX_FULLBRIGHT) ? 0xffffff : ((fx & FX_CONDLIGHT) ? ambient2 : ambient);
	float ambf[4] = {
		((amb >> 16) & 0xff) / 255.0f,
		((amb >> 8) & 0xff) / 255.0f,
		(amb & 0xff) / 255.0f,
		(float)active
	};
	dev->SetVertexShaderConstantF(228, ambf, 1);

	float matDiffuse[4] = { material.Diffuse.r, material.Diffuse.g, material.Diffuse.b, material.Diffuse.a };
	dev->SetVertexShaderConstantF(229, matDiffuse, 1);

	float matSpecular[4] = { material.Specular.r, material.Specular.g, material.Specular.b, material.Power };
	dev->SetVertexShaderConstantF(230, matSpecular, 1);

	float flags = 0;
	if (fx & FX_VERTEXCOLOR) flags += 1.0f;
	if (shininess > 0) flags += 2.0f;
	float eye[4] = { eyePos[0], eyePos[1], eyePos[2], flags };
	dev->SetVertexShaderConstantF(231, eye, 1);
}

void gxScene::fillGpuDrawParams(sdlgpu::MeshDrawParams& p, SDL_GPUDevice* dev) {
	p.tex = nullptr; p.tex1 = nullptr;
	p.stage1[0] = p.stage1[1] = p.stage1[2] = p.stage1[3] = 0.0f;
	p.boneBuf = nullptr;
	p.blend = blend; p.zMode = zmode;
	p.cull = SDL_GPU_CULLMODE_BACK;
	if (fx & FX_DOUBLESIDED) p.cull = SDL_GPU_CULLMODE_NONE;
	else if (flipped) p.cull = SDL_GPU_CULLMODE_FRONT;
	if (n_texs > 0 && texstate[0].canvas) {
		p.tex = sdlgpu::GetCanvasTexture(dev, texstate[0].canvas);
	}
	if (n_texs > 1 && texstate[1].canvas && texstate[1].blend &&
		texstate[1].blend != BLEND_BUMPENVMAP && !texstate[1].mat_valid) {
		SDL_GPUTexture* t1 = sdlgpu::GetCanvasTexture(dev, texstate[1].canvas);
		if (t1) {
			p.tex1 = t1;
			p.stage1[0] = (float)texstate[1].blend;
			p.stage1[1] = (texstate[1].flags & TEX_COORDS2) ? 1.0f : 0.0f;
			p.stage1[2] = 1.0f;
			p.stage1[3] = (texstate[1].canvas->getFlags() & gxCanvas::CANVAS_TEX_ALPHA) ? 1.0f : 0.0f;
		}
	}
}

void gxScene::computeGpuSkinnedUniforms(sdlgpu::MeshUniforms& u) const {
	computeGpuMeshUniforms(u);
	D3DXMATRIX vp;
	D3DXMatrixMultiply(&vp, &currentView, &currentProj);
	memcpy(u.mvp, &vp, 64);
	D3DXMATRIX ident;
	D3DXMatrixIdentity(&ident);
	memcpy(u.world, &ident, 64);
}

void gxScene::renderSkinned(gxMesh* mesh, int first_vert, int vert_cnt, int first_tri, int tri_cnt, const float* bone_data, int bone_cnt) {
	bool drewGpu = false;
	if (!currentEffect && gpuFrame.ready() && mesh && mesh->isSkinned() && mesh->getGpuMirror() && bone_data && bone_cnt > 0) {
		bool skipGpu = false;
		if (graphics && graphics->runtime && graphics->runtime->sdlWindow) {
			SDL_Window* sdlWin = graphics->runtime->sdlWindow;
			SDL_WindowFlags wf = SDL_GetWindowFlags(sdlWin);
			if (wf & SDL_WINDOW_MINIMIZED) skipGpu = true;
			if (wf & SDL_WINDOW_HIDDEN) skipGpu = true;
		}
		if (gpuFrame.dev && SDL_GetGPUShaderFormats(gpuFrame.dev) == SDL_GPU_SHADERFORMAT_INVALID) skipGpu = true;
		if (!skipGpu && !gpuFrame.active()) {
			if (!sdlgpu::BeginScenePass(gpuFrame, (int)viewport.X, (int)viewport.Y,
				(int)viewport.Width, (int)viewport.Height, 0, 0, 0, false, false)) skipGpu = true;
		}
		if (!skipGpu) {
			SDL_GPUDevice* dev = gpuFrame.dev ? gpuFrame.dev : (graphics && graphics->runtime ? (SDL_GPUDevice*)graphics->runtime->sdlGpu : nullptr);
			SDL_GPUBuffer* bones = nullptr;
			if (dev && sdlgpu::UploadBones(dev, bone_data, (unsigned)bone_cnt))
				bones = sdlgpu::EnsureBoneBuffer(dev);
			if (bones) {
				sdlgpu::MeshUniforms uniforms;
				computeGpuSkinnedUniforms(uniforms);
				sdlgpu::MeshDrawParams p;
				fillGpuDrawParams(p, dev);
				p.boneBuf = bones;
				sdlgpu::RenderSceneMesh(gpuFrame, mesh->getGpuMirror(), uniforms, first_vert, vert_cnt, first_tri, tri_cnt, p);
				drewGpu = true;
			}
		}
	}
	if (drewGpu) { tris_drawn += tri_cnt; return; }
	gpuOnlyFrame = false;
	setSkinShaderConstants();
	mesh->renderSkinned(first_vert, vert_cnt, first_tri, tri_cnt, bone_data, bone_cnt);
	tris_drawn += tri_cnt;
}

void gxScene::end() {
	for (int s = 1; s < tex_stages; ++s) {
		if (texstate[s].canvas) {
			texstate[s].canvas = 0;
		}
		setTSS(s, D3DTSS_COLOROP, D3DTOP_DISABLE);
		setTSS(s, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
		setTex(s, nullptr);
	}
	n_texs = n_texs > 1 ? 1 : n_texs;
	lastRenderStateValid = false;
	if (graphics) graphics->endD3DScene();
	RECT r = { (LONG)viewport.X, (LONG)viewport.Y, (LONG)(viewport.X + viewport.Width), (LONG)(viewport.Y + viewport.Height) };
	if (graphics && graphics->runtime && graphics->runtime->sdlGpu && gpuOnlyFrame && gpuFrame.drew3D) target->damageScene(r);
	else target->damageD3D(r);
	sdlgpu::EndSceneFrame(gpuFrame);
}

bool gxScene::hasGpuImage() const {
	return gpuFrame.drew3D;
}

bool gxScene::presentGpuFrame(struct SDL_GPUDevice* dev, struct SDL_Window* win) {
	if (!hasGpuImage()) return false;
	return sdlgpu::PresentSceneFrame((SDL_GPUDevice*)dev, (SDL_Window*)win, gpuFrame);
}

bool gxScene::presentGpuFrameWithCanvas(struct SDL_GPUDevice* dev, struct SDL_Window* win, gxCanvas* canvas) {
	return sdlgpu::PresentSceneWithCanvas((SDL_GPUDevice*)dev, (SDL_Window*)win, gpuFrame, canvas);
}

gxLight* gxScene::createLight(int flags) {
	gxLight* l = new gxLight(this, flags);
	_allLights.insert(l);
	return l;
}

void gxScene::freeLight(gxLight* l) {
	_allLights.erase(l);
}

int gxScene::getTrianglesDrawn()const {
	return tris_drawn;
}