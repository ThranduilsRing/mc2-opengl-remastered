// RenderCore/StaticPropOrigin.h -- the one reader of a static-prop instance's world position.
//
// GpuStaticPropInstance::modelMatrix is a memcpy of the Stuff 4x3 matrix (row-vector convention): the
// translation sits at [3], [7], [11] (x = east, y = north, z = elevation). Indices [12..14] are always 0
// and must never be read for a position (a doctest and the static_prop_family contract row keep that so).
// The camera (ViewUniforms::cameraWorldPos) is in render space, so distance tests use the render-space origin.
#ifndef RENDERCORE_STATICPROPORIGIN_H
#define RENDERCORE_STATICPROPORIGIN_H

#include "axis_convert.h"

struct StaticPropOrigin {
    float x, y, z;
};

// Stuff-space translation of an instance matrix (16 floats, translation at [3],[7],[11]).
inline StaticPropOrigin staticPropOriginStuff(const float* m) { return {m[3], m[7], m[11]}; }

// Render-space origin: (x,y,z)stuff -> (-x, z, y). Use this against cameraWorldPos.
inline StaticPropOrigin staticPropOriginRender(const float* m)
{
    const StaticPropOrigin s = staticPropOriginStuff(m);
    StaticPropOrigin r;
    mc2ToRender(r, s);
    return r;
}

#endif
