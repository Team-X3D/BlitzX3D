#include "std.h"
#include "camera.h"

extern sdlScene* sdl_scene;

Camera::Camera() {
	setZoom(1);
	setRange(1, 1000);
	setViewport(0, 0, 0, 0);
	setClsColor(Vector(), 1.0f);
	setClsMode(true, true);
	setProjMode(PROJ_PERSP);
	setFogRange(1, 1000);
	setFogColor(Vector());
	setFogDensity(1.0f);
	setFogMode(sdlScene::FOG_NONE);
}

void Camera::setZoom(float z) {
	zoom = z;
	local_valid = false;
}

void Camera::setRange(float n, float f) {
	frustum_nr = n; frustum_fr = f;
	local_valid = false;
}

void Camera::setViewport(int x, int y, int w, int h) {
	vp_x = x; vp_y = y; vp_w = w; vp_h = h;
	local_valid = false;
}

void Camera::setClsColor(const Vector& v, float alpha) {
	cls_color = v;
	cls_alpha = alpha;
}

void Camera::setClsMode(bool c, bool z) {
	cls_argb = c; cls_z = z;
}

void Camera::setProjMode(int mode) {
	proj_mode = mode;
}

void Camera::setFogColor(const Vector& v) {
	fog_color = v;
}

void Camera::setFogRange(float nr, float fr) {
	fog_nr = nr; fog_fr = fr;
}

void Camera::setFogDensity(float den) {
	fog_den = den;
}

void Camera::setFogMode(int mode) {
	fog_mode = mode;
}

void Camera::setCullMode(int mode) {
	cull_mode = mode;
}

void Camera::setDepthBias(float bias, float slope) {
	depth_bias = bias;
	slope_bias = slope;
}

void Camera::setReverseZ(int enable) {
	reverse_z = enable;
}

void Camera::setColorWrite(int enable) {
	color_write = enable;
}

const Frustum& Camera::getFrustum()const {
	if (!local_valid) {
		float ar = (float)vp_h / vp_w;
		frustum_w = frustum_nr * 2 / zoom;
		frustum_h = frustum_nr * 2 / zoom * ar;
		new(&local_frustum) Frustum(frustum_nr, frustum_fr, frustum_w, frustum_h);
		local_valid = true;
	}
	return local_frustum;
}

float Camera::getFrustumNear()const {
	return frustum_nr;
}

float Camera::getFrustumFar()const {
	return frustum_fr;
}

float Camera::getFrustumWidth()const {
	getFrustum(); return frustum_w;
}

float Camera::getFrustumHeight()const {
	getFrustum(); return frustum_h;
}

float Camera::getFogNear()const {
	return fog_nr;
}

float Camera::getFogFar()const {
	return fog_fr;
}

void Camera::getViewport(int* x, int* y, int* w, int* h)const {
	*x = vp_x; *y = vp_y; *w = vp_w; *h = vp_h;
}

bool Camera::beginRenderFrame() {
	if (!proj_mode) return false;
	getFrustum();
	sdl_scene->setViewport(vp_x, vp_y, vp_w, vp_h);
	sdl_scene->clear(&(cls_color.x), cls_alpha, 1, cls_argb, cls_z);
	if (proj_mode == PROJ_ORTHO) {
		sdl_scene->setOrthoProj(frustum_nr, frustum_fr, frustum_w, frustum_h);
	}
	else {
		sdl_scene->setPerspProj(frustum_nr, frustum_fr, frustum_w, frustum_h);
	}
	sdl_scene->setFogRange(fog_nr, fog_fr);
	sdl_scene->setFogDensity(fog_den);
	sdl_scene->setFogColor((float*)&fog_color.x);
	sdl_scene->setFogMode(fog_mode);
	if (cull_mode >= 0) sdl_scene->setCullMode(cull_mode);
	sdl_scene->setDepthBias(depth_bias, slope_bias);
	sdl_scene->setReverseZ(reverse_z != 0);
	sdl_scene->setColorWrite(color_write != 0);
	return true;
}