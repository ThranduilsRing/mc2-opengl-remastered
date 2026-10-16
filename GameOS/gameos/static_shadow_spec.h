// static_shadow_spec.h - the engine's world-fixed static sun shadow map, as CPU data.
//
// WHAT: the light matrix, map size, building depth bias and build generation of the static shadow map the engine builds once per map (and again on a
// building-destroy rebake).
// WHY: a backend that renders the map itself (the Vulkan shadow island) needs the same inputs GL renders with, without touching GL objects.
#pragma once

#include <cstdint>

struct StaticShadowSpec {
    float    lightSpace[16] = {};   // column-major, clip z in [0, 1]
    int      size = 0;              // square map edge in texels
    float    biasFactor = 0.0f;     // building casters' slope bias (GL glPolygonOffset factor); terrain has none
    float    biasUnits = 0.0f;      // building casters' constant bias (GL glPolygonOffset units)
    uint64_t generation = 0;        // bumps on every (re)build of the light matrix
};

// False while the engine has no static light matrix for the current map (shadows off, or the map's first build not reached yet).
bool gos_GetStaticShadowSpec(StaticShadowSpec* out);
