#ifndef GX_D3DXMATH_H
#define GX_D3DXMATH_H

#ifndef GX_USE_LEGACY_D3DX
#define GX_USE_LEGACY_D3DX 0
#endif

#if GX_USE_LEGACY_D3DX
#include <d3dx9.h>
#else
#include <d3d9.h>
#include <cstring>

struct D3DXMATRIX : public D3DMATRIX {
	D3DXMATRIX() {}
	D3DXMATRIX(const D3DXMATRIX& o) { std::memcpy(&_11, &o._11, sizeof(D3DMATRIX)); }
	D3DXMATRIX(const D3DMATRIX& o) { std::memcpy(&_11, &o, sizeof(D3DMATRIX)); }
	D3DXMATRIX(float a11, float a12, float a13, float a14,
		float a21, float a22, float a23, float a24,
		float a31, float a32, float a33, float a34,
		float a41, float a42, float a43, float a44) {
		float v[16] = { a11, a12, a13, a14, a21, a22, a23, a24, a31, a32, a33, a34, a41, a42, a43, a44 };
		std::memcpy(&_11, v, sizeof(v));
	}
	D3DXMATRIX& operator=(const D3DXMATRIX& o) { std::memcpy(&_11, &o._11, sizeof(D3DMATRIX)); return *this; }
	D3DXMATRIX& operator=(const D3DMATRIX& o) { std::memcpy(&_11, &o, sizeof(D3DMATRIX)); return *this; }
};

inline void D3DXMatrixIdentity(D3DXMATRIX* o) {
	for (int i = 0; i < 4; ++i)
		for (int j = 0; j < 4; ++j)
			o->m[i][j] = (i == j) ? 1.0f : 0.0f;
}

inline void D3DXMatrixMultiply(D3DXMATRIX* o, const D3DXMATRIX* a, const D3DXMATRIX* b) {
	D3DXMATRIX r;
	for (int i = 0; i < 4; ++i)
		for (int j = 0; j < 4; ++j)
			r.m[i][j] = a->m[i][0] * b->m[0][j] + a->m[i][1] * b->m[1][j] + a->m[i][2] * b->m[2][j] + a->m[i][3] * b->m[3][j];
	*o = r;
}

inline void D3DXMatrixTranspose(D3DXMATRIX* o, const D3DXMATRIX* a) {
	D3DXMATRIX r;
	for (int i = 0; i < 4; ++i)
		for (int j = 0; j < 4; ++j)
			r.m[i][j] = a->m[j][i];
	*o = r;
}

inline void D3DXMatrixOrthoOffCenterLH(D3DXMATRIX* o, float l, float r, float b, float t, float zn, float zf) {
	D3DXMatrixIdentity(o);
	o->m[0][0] = 2.0f / (r - l);
	o->m[1][1] = 2.0f / (t - b);
	o->m[2][2] = 1.0f / (zf - zn);
	o->m[3][0] = (l + r) / (l - r);
	o->m[3][1] = (t + b) / (b - t);
	o->m[3][2] = zn / (zn - zf);
}

inline D3DXMATRIX operator*(const D3DXMATRIX& a, const D3DXMATRIX& b) {
	D3DXMATRIX r;
	D3DXMatrixMultiply(&r, &a, &b);
	return r;
}
#endif

#endif
