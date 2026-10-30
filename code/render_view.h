// G7b K3: RenderClock + RenderList/RenderView (G1-lite RenderProxy).
//
// Built once per frame at ExtractRenderSnapshot (main thread, no tick in flight), in today's render order: terrain
// blocks, movers, other. Object render paths read their view instead of the live sim object, so the frame's object
// draws may overlap the next tick. `obj` stays valid until the next commit (tick frees are deferred, tick_commit.h);
// render only touches its presentation-owned members (appearance, gosFX effects), which only main writes.
#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <memory>
#include "math/glm_config.h"

class GameObject;
class BldgAppearance;
namespace gosFX { class Effect; }

namespace mc2::render_view {

struct RenderClock {
    int32_t turn = 0;
    float scenarioTime = 0.0f;
    float dt = 0.0f;           // render frame length
    uint32_t frame = 0;        // g_mc2FrameCounter
    uint32_t missionResult = 0;
    bool missionActive = false; // Mission::active (the tick's mission script can end the mission mid-draw)
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
    // The tick can clear/reuse owner slots; releaseEffect retains these payloads until the join.
    // Bolts: body, muzzle, hit, miss, water miss; carnage: body.
    gosFX::Effect* effects[5]{};
    glm::vec3 position{0.0f};  // sim position
    glm::vec3 aux[3]{};        // bolts: laser tip, resolved hit point, raw target point
    uint32_t watchId = 0;
    uint32_t generation = 0;   // obj->generation at extract; the draw skips the view when the slot was re-inited
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

// G7b K4: copied at the render extract; pointers are identity keys only.
struct HudMover {
    const GameObject* object = nullptr;
    const void* pilot = nullptr;
    glm::vec3 position{};
    std::string name, pilotName;
    float health = 0, sensorRange = 0;
    uint16_t groups = 0;
    bool active = false, friendly = false, own = false, neutral = false;
    bool selected = false, onGui = false, shutdown = false, canJump = false;
    bool messagePlaying = false, sensorBroken = true, detected = false, enemyMarker = false, moving = false;
    bool weaponBands[3]{};
    const char* orderStatus = "IDLE";
    std::vector<glm::vec3> waypoints, overviewPath;
    std::vector<std::shared_ptr<BldgAppearance>> waypointMarkers;
    glm::vec3 waypointStart{};
    bool waypointFirst = false;
};
struct HudSensor { glm::vec3 position{}; float range = 0; bool tower = false; };
struct HudObjective {
    const void* object = nullptr;
    glm::vec3 position{};
    std::string markerText, description;
    bool visible = false, active = false, hidden = false, primary = false;
    int marker = 0, status = 0;
    long points = 0, color = 0;
};
struct HudBlip { glm::vec3 position{}; unsigned long color = 0, ring = 0, range = 0; bool selected = false; };
struct HudVtol { void* appearance = nullptr; void* dust = nullptr; void* beam = nullptr; glm::vec3 position{}; };
struct HudSnapshot {
    std::vector<HudMover> movers;
    std::vector<size_t> roster;
    std::vector<HudSensor> sensors, coverage;
    std::vector<HudObjective> objectives;
    std::vector<HudBlip> blips;
    std::vector<HudVtol> vtols;
    bool home = false, ended = false;
    int objectiveStatus = 0, objectiveCount = 0;
    long resourcePoints = 0;
    float timeLimit = 0;
    const HudMover& mover(const GameObject* object) const {
        for (const auto& row : movers) if (row.object == object) return row;
        static const HudMover empty;
        return empty;
    }
    const HudObjective& objective(const void* object) const {
        for (const auto& row : objectives) if (row.object == object) return row;
        static const HudObjective empty;
        return empty;
    }
};
HudSnapshot& hud();
void drawHudWaypoints(const HudMover& row);
void drawHudOverviewPath(const HudMover& row, unsigned long color);

RenderClock& clock();
RenderList& list();

}  // namespace mc2::render_view
