#pragma once
namespace mc2depth {
#define MC2_DEPTH_CONST constexpr
#include "../shaders/include/terrain_depth_bias.hglsl"
constexpr float WATER_DEPTH_FUDGE = WATER_DEPTH_FUDGE_FAST_REL;
// Relative depth pushes under reverse-Z n/d. Near cancels out.
constexpr float worldPush(float layer, float distance, float /*near*/) {
    return distance * layer / (1.0f + layer);
}
static_assert(VEG_DEPTH_BIAS_REL < OVERLAY_DEPTH_BIAS_REL &&
    OVERLAY_DEPTH_BIAS_REL < OBJECT_DEPTH_BIAS_REL &&
    OBJECT_DEPTH_BIAS_REL < WATER_DEPTH_FUDGE_FAST_REL);
}
