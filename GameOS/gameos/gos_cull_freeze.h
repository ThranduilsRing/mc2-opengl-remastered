// gos_cull_freeze.h -- debug "freeze culling camera" (ImGui toggle in the GPU Passes window; no env gate).
//
// While frozen, every CULL consumer sees the camera as it was when the toggle was switched on, while the view camera keeps moving, so
// false culls (things dropped that are on screen) become visible at the screen edges. Consumers:
//   - CPU frustum planes: Camera::cacheFrustumPlanes (terrain block/superchunk cull, object block gate)
//   - actor/mech/prop compute cull MVP: gpu_cull_compute.cpp
//   - HZB prop cull push constant: vulkan_post_hzb_native.cpp (the pyramid itself is built from the LIVE depth, so occlusion is only
//     approximate while frozen; frustum culling is exact)
// Picking, projection, LOD choice and every draw MVP stay on the live camera.
// Main thread only. Header-only so mclib and GameOS share one state.
#pragma once

#include <cstring>

namespace mc2cullfreeze {

struct State {
    bool  frozen = false;
    bool  mvpValid = false;
    bool  planesValid = false;
    float mvp[16] = {};
    float planes[6][4] = {};
};
inline State& state() { static State s; return s; }

inline bool frozen() { return state().frozen; }
inline void set(bool on) {
    State& s = state();
    s.frozen = on;
    s.mvpValid = s.planesValid = false;   // re-snapshot at the next consumer call
}

// Live 16-float MVP in, cull MVP out (the first call after the toggle captures the live one).
inline const float* cullMvp(const float* live) {
    State& s = state();
    if (!s.frozen || !live) return live;
    if (!s.mvpValid) { std::memcpy(s.mvp, live, sizeof(s.mvp)); s.mvpValid = true; }
    return s.mvp;
}

// Fills `planes` (6x4) with the frozen planes; `live` already holds this frame's planes. Returns true when it overwrote them.
inline bool cullPlanes(float (*planes)[4]) {
    State& s = state();
    if (!s.frozen) return false;
    if (!s.planesValid) { std::memcpy(s.planes, planes, sizeof(s.planes)); s.planesValid = true; }
    std::memcpy(planes, s.planes, sizeof(s.planes));
    return true;
}

}  // namespace mc2cullfreeze
