// RenderCore/StaticPropOrigin.h -- the one reader of a static-prop instance's world position.
//
// GpuStaticPropInstance::modelMatrix is a memcpy of the Stuff 4x3 matrix (row-vector convention): the
// translation sits at [3], [7], [11]. BldgAppearance builds it from xlatPosition = (-x, elevation, y)
// (mclib/bdactor.cpp, mclib/bdactor_static.cpp), so it is ALREADY render space -- the same space as
// ViewUniforms::cameraWorldPos (cameraOriginGL) and worldToClipGL. Indices [12..14] are always 0 and must
// never be read for a position (a doctest and the static_prop_family contract row keep that so).
#ifndef RENDERCORE_STATICPROPORIGIN_H
#define RENDERCORE_STATICPROPORIGIN_H

struct StaticPropOrigin {
    float x, y, z;
};

// Raw translation of an instance matrix (16 floats, translation at [3],[7],[11]); a stable per-instance key.
inline StaticPropOrigin staticPropOriginStuff(const float* m) { return {m[3], m[7], m[11]}; }

// Render-space origin for distance tests against cameraWorldPos: the translation as stored (no axis swap).
inline StaticPropOrigin staticPropOriginRender(const float* m) { return staticPropOriginStuff(m); }

#endif
