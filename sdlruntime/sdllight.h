#ifndef SDLLIGHT_H
#define SDLLIGHT_H

#include <cstring>
#include <d3d9.h>

class sdlScene;

class sdlLight {
public:
	sdlLight(sdlScene* scene, int type);
	~sdlLight();

	D3DLIGHT9 d3d_light;

private:
	sdlScene* scene;

	/***** GX INTERFACE *****/
public:
	enum {
		LIGHT_DISTANT = 1, LIGHT_POINT = 2, LIGHT_SPOT = 3
	};
	void setRange(float range);

	void setColor(const float rgb[3]) {
		memcpy(&d3d_light.Diffuse, rgb, sizeof(float) * 3);
		d3d_light.Diffuse.a = 1.0f;
	}

	void setPosition(const float pos[3]);
	void setDirection(const float dir[3]);
	void setConeAngles(float inner, float outer);

	void getColor(float rgb[3]) {
		memcpy(rgb, &d3d_light.Diffuse, sizeof(float) * 3);
	}
};

#endif