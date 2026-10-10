#include "std.h"
#include "sdllight.h"
#include "sdlscene.h"
#include "sdlgraphics.h"

sdlLight::sdlLight(sdlScene* s, int type) :
    scene(s) {

    memset(&d3d_light, 0, sizeof(d3d_light));

    switch (type) {
    case LIGHT_POINT:
        d3d_light.Type = D3DLIGHT_POINT;
        break;
    case LIGHT_SPOT:
        d3d_light.Type = D3DLIGHT_SPOT;
        break;
    default:
        d3d_light.Type = D3DLIGHT_DIRECTIONAL;
    }

    d3d_light.Diffuse.a = 1;
    d3d_light.Diffuse.r = d3d_light.Diffuse.g = d3d_light.Diffuse.b = 1;
    d3d_light.Specular.r = d3d_light.Specular.g = d3d_light.Specular.b = 1;
    d3d_light.Range = FLT_MAX;
    d3d_light.Theta = 0;
    d3d_light.Phi = HALFPI;
    d3d_light.Falloff = 1;
    d3d_light.Direction.z = 1;
    setRange(1000);
}

sdlLight::~sdlLight() {
}

void sdlLight::setRange(float r) {
    d3d_light.Attenuation1 = 1.0f / r;
}

void sdlLight::setPosition(const float pos[3]) {
    d3d_light.Position.x = pos[0];
    d3d_light.Position.y = pos[1];
    d3d_light.Position.z = pos[2];
}

void sdlLight::setDirection(const float dir[3]) {
    d3d_light.Direction.x = dir[0];
    d3d_light.Direction.y = dir[1];
    d3d_light.Direction.z = dir[2];
}

void sdlLight::setConeAngles(float inner, float outer) {
    d3d_light.Theta = inner;
    d3d_light.Phi = outer;
}