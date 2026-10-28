// G7b K3: RenderClock + RenderList/RenderView (G1-lite RenderProxy).
//
// Built once per frame at ExtractRenderSnapshot (main thread, no tick in flight), in today's render order: terrain
// blocks, movers, other. Object render paths read their view instead of the live sim object, so the frame's object
// draws may overlap the next tick. `obj` stays valid until the next commit (tick frees are deferred, tick_commit.h);
// render only touches its presentation-owned members (appearance, gosFX effects), which only main writes.
#pragma once

#include <cstdint>
#include <vector>
#include "math/glm_config.h"

class GameObject;

namespace mc2::render_view {

struct RenderClock {
    int32_t turn = 0;
    float scenarioTime = 0.0f;
    float dt = 0.0f;           // render frame length
    uint32_t frame = 0;        // g_mc2FrameCounter
    float alpha = 1.0f;        // interpolation weight of the presented poses
};

// RenderView::flags bits.
inline constexpr uint32_t Visible = 1u << 0;       // static objects: appearance->canBeSeen(); bolts: drawn at all
inline constexpr uint32_t Bars = 1u << 1;          // set the bar color/status before the draw
inline constexpr uint32_t FireIcon = 1u << 2;      // movers: hold-fire icon
inline constexpr uint32_t HitTarget = 1u << 3;     // bolts
inline constexpr uint32_t Timer = 1u << 4;         // artillery: draw the countdown
inline constexpr uint32_t SeenAtExtract = 1u << 5; // artillery: appearance->canBeSeen() when windowsVisible was stamped

// What a mover's render() decided from the sim (contact status vs home, team, isOnGui, status).
enum class Mode : uint8_t { None, Full, Sensor, SensorText, Fade, Own };

struct RenderView {
    GameObject* obj = nullptr;
    glm::vec3 position{0.0f};  // sim position
    glm::vec3 aux[3]{};        // bolts: laser tip, resolved hit point, raw target point
    uint32_t watchId = 0;
    uint32_t flags = 0;
    uint32_t barColor = 0;
    float barStatus = 0.0f;
    float value[2]{};          // bolts: effect heading, owner rotation; artillery: time to impact
    int32_t nameId = 0;        // appearance object-name id, or the sensor text id (Mode::SensorText)
    uint8_t objClass = 0;
    Mode mode = Mode::None;
    bool has(uint32_t f) const { return (flags & f) != 0; }
};

struct RenderList {
    std::vector<RenderView> terrain, movers, other;
    void clear() { terrain.clear(); movers.clear(); other.clear(); }
};

RenderClock& clock();
RenderList& list();

}  // namespace mc2::render_view
