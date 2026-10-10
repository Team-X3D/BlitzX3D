#ifndef LIGHT_H
#define LIGHT_H

#include "geom.h"
#include "object.h"
#include "../sdlruntime/sdllight.h"

class World;

class Light : public Object {
public:
	Light(int type);
	~Light();

	Light* getLight() { return this; }

	void setRange(float r);
	void setColor(const Vector& v);
	void setConeAngles(float inner, float outer);

	bool beginRender(float tween);

	sdlLight* getGxLight()const { return light; }

private:
	friend class World;
	sdlLight* light;
};

#endif