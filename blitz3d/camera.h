#ifndef CAMERA_H
#define CAMERA_H

#include "model.h"
#include "frustum.h"
#include "mirror.h"

class Camera : public Object {
public:
	enum {
		PROJ_NONE = 0, PROJ_PERSP = 1, PROJ_ORTHO = 2
	};

	Camera();
	Camera* getCamera() { return this; }

	//called by user
	void setZoom(float z);
	void setRange(float nr, float fr);
	void setViewport(int x, int y, int w, int h);
	void setClsColor(const Vector& v, float alpha = 1.0f);
	void setClsMode(bool cls_argb, bool cls_z);
	void setProjMode(int mode);
	void setFogColor(const Vector& v);
	void setFogRange(float nr, float fr);
	void setFogDensity(float den);
	void setFogMode(int mode);
	void setCullMode(int mode);
	void setDepthBias(float bias, float slope);
	void setReverseZ(int enable);
	void setColorWrite(int enable);

	//called by world
	bool beginRenderFrame();

	//Camera frustum...
	float getFrustumNear()const;
	float getFrustumFar()const;
	float getFrustumWidth()const;
	float getFrustumHeight()const;
	const Frustum& getFrustum()const;
	void  getViewport(int* x, int* y, int* w, int* h)const;
	int   getProjMode()const { return proj_mode; }
	float getFogNear()const;
	float getFogFar()const;

private:
	float zoom;
	int vp_x, vp_y, vp_w, vp_h;
	Vector cls_color;
	float cls_alpha;
	bool cls_argb, cls_z;
	int proj_mode;
	Vector fog_color;
	float fog_nr = 1.0f, fog_fr = 1000.0f, fog_den = 1.0f;
	int fog_mode;
	float frustum_nr, frustum_fr;
	int cull_mode = -1;
	float depth_bias = 0, slope_bias = 0;
	int reverse_z = 0, color_write = 1;
	mutable float frustum_w, frustum_h;
	mutable Frustum local_frustum;
	mutable bool local_valid;
};

#endif