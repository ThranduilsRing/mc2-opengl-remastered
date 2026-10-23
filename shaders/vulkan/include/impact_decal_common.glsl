// Shared interface of the impact box-decal pass (GameOS/gameos/vulkan_impact_decals.cpp): the per-decal record
// (RenderCore/ImpactDecals.h GpuDecal, 8 x vec4) and the frame block. MC2 world space throughout.
struct ImpactDecal { vec4 v[8]; };
layout(std430, set = 0, binding = 5) readonly buffer Decals { ImpactDecal d[]; } decals;
layout(std140, set = 0, binding = 4) uniform ImpactFrame {
    mat4 viewProj;
    mat4 invViewProj;
    vec4 eye;        // homogeneous camera point
    vec4 lightDir;   // toward the sun; w = ambient share of the relight
    vec4 screen;     // w, h, 1/w, 1/h
} F;
