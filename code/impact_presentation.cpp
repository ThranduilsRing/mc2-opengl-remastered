#include "tick_commit.h"
// code/impact_presentation.cpp -- simulation impacts -> impact decal pool -> Vulkan box-decal pass.
// See impact_presentation.h and docs/impact-damage-design.md.

#include "impact_presentation.h"
#include "sim_overlap.h"

#include "../RenderCore/ImpactDecals.h"
#include "Gates/gate.h"
#include "Gates/render_options.h"
#include "bldng.h"
#include "../mclib/camera.h"
#include "../RenderCore/axis_convert.h"
#include "gameobj.h"
#include "objmgr.h"
#include "../mclib/cmponent.h"
#include "../mclib/terrain.h"
#include "../mclib/timing.h"
#include "../mclib/weaponfx.h"

#ifdef MC2_VULKAN_ISLAND
#include "../GameOS/gameos/vulkan_impact_decals.h"
#endif

#include <tracy/Tracy.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <optional>
#include <string_view>

namespace impact_presentation {
namespace {

// A pending impact: the raw sim facts. Material and anchor are resolved on the presentation side.
impact::EventQueue g_queue;
impact::EventQueue g_tickQueue;
bool g_commitQueued = false;
void pushImpact(const impact::Event& event) {
    if (!mc2::tick::collecting()) { g_queue.push(event); return; }
    g_tickQueue.push(event);
    if (!g_commitQueued) {
        g_commitQueued = true;
        mc2::tick::defer(mc2::tick::Queue::Impact, [] {
            g_tickQueue.drain([](const impact::Event& e) { g_queue.push(e); });
            g_commitQueued = false;
        });
    }
}
impact::Pool g_pool;
impact::GpuDecal g_gpu[impact::kMaxDecals];

bool containsNoCase(std::string_view hay, std::string_view needle) {
    if (needle.size() > hay.size()) return false;
    for (size_t p = 0; p + needle.size() <= hay.size(); ++p) {
        size_t i = 0;
        while (i < needle.size() && std::tolower(static_cast<unsigned char>(hay[p + i])) == needle[i]) ++i;
        if (i == needle.size()) return true;
    }
    return false;
}

// Tier-1 material of a building/prop: the kit/content-pipeline per-part material class will replace this name heuristic
// (impact::materialFromContentClass) once it reaches the runtime; until then sheet-metal structures are recognised by name.
impact::Material buildingMaterial(GameObject* obj) {
    static const char* kMetal[] = {"quonset", "hangar", "tank", "silo", "tower", "pipe", "fence", "container", "crate",
                                   "turret", "gate", "dropship", "generator", "radar", "antenna", "truck", "train",
                                   "car", "derrick", "pump", "refinery", "crane", "bridge", "light", "dish", "shed"};
    const char* name = obj->getName();
    if (!name) return impact::Material::Concrete;
    for (const char* k : kMetal) if (containsNoCase(name, k)) return impact::Material::SheetMetal;
    return impact::Material::Concrete;
}

impact::Material terrainMaterial(const float pos[3], bool& water) {
    water = false;
    if (!land) return impact::Material::Soil;
    int r = 0, c = 0;
    const glm::vec3 p(pos[0], pos[1], pos[2]);
    land->worldToTile(p, r, c);
    const int t = land->getTerrain(r, c);
    water = impact::terrainTypeIsWater(t);
    return impact::materialFromTerrainType(t);
}

// Read-only anchor lookup: position and yaw of a live object; invalid when it is gone or (for buildings) destroyed, so
// its decals die with it and the destroyed-state art is not painted with the intact building's scars.
impact::AnchorPose resolveAnchor(impact::AnchorKind kind, uint32_t id) {
    impact::AnchorPose p;
    p.valid = false;
    if (kind == impact::AnchorKind::World) { p.valid = true; return p; }
    if (!ObjectManager || id == 0) return p;
    GameObjectPtr obj = ObjectManager->getByWatchID(id);
    if (!obj || !obj->getExists()) return p;
    if (kind == impact::AnchorKind::Static && obj->isDestroyed()) return p;
    const glm::vec3& pos = obj->getPosition();
    p.pos[0] = pos.x; p.pos[1] = pos.y; p.pos[2] = pos.z;
    p.yawDeg = kind == impact::AnchorKind::Mover ? obj->getRotation() : 0.0f;
    p.valid = true;
    return p;
}

int quality() {
    // Avoid added decal overdraw and its normal/depth reads on the shared-memory presets.
    if (mc2::options::lowCost()) return 0;
    return mc2::options::current().impactDecals;
}

impact::Event fixtureEvent(impact::WeaponClass w, const glm::vec3& hit, const glm::vec3& dir, uint32_t anchor) {
    impact::Event e;
    e.tick = static_cast<uint32_t>(turn);
    e.time = scenarioTime;
    e.pos[0] = hit.x; e.pos[1] = hit.y; e.pos[2] = hit.z;
    e.dir[0] = dir.x; e.dir[1] = dir.y; e.dir[2] = dir.z;
    e.anchorId = anchor;
    e.anchorKind = anchor ? impact::AnchorKind::Static : impact::AnchorKind::World;
    e.weapon = w;
    e.magnitude = 10.0f;
    return e;
}

// MC2_FX_FORCE_SPAWN=impact (test fixture, default off): every 1.5 s, a ballistic / laser / missile volley on the facing
// wall of the nearest sheet-metal and the nearest concrete building to the camera focus, and the same three classes on the
// ground in front of the first one. Goes through the same queue and resolution as a real sim impact.
void fixtureVolley() {
    static const bool s_on = mc2::gate::is<"MC2_FX_FORCE_SPAWN", "impact">();
    static float s_next = 0.0f;
    static glm::vec3 s_lastFocus(1e9f);
    if (!s_on || !eye || !ObjectManager || scenarioTime < 4.0f) return;
    const glm::vec3 focus = eye->getPosition();
    const float moved = (focus.x - s_lastFocus.x) * (focus.x - s_lastFocus.x) + (focus.y - s_lastFocus.y) * (focus.y - s_lastFocus.y);
    if (moved >= 150.0f * 150.0f) {   // a camera cut: fire shortly after, once the camera origin has caught up
        s_lastFocus = focus;
        s_next = scenarioTime + 0.25f;
        return;
    }
    if (scenarioTime < s_next) return;
    s_next = scenarioTime + 1.5f;
    // The camera origin is held in the render axis order (y = elevation); bring it back to MC2 space.
    const glm::vec3 camO = renderToMc2(eye->getCameraOrigin());
    GameObject* best[2] = {nullptr, nullptr};   // [0] sheet metal, [1] concrete
    float bestD[2] = {1e30f, 1e30f};
    for (long i = 0; i < ObjectManager->getNumBuildings(); ++i) {
        GameObject* b = ObjectManager->getBuilding(i);
        if (!b || !b->getExists() || b->isDestroyed() || b->getExtentRadius() < 8.0f) continue;
        const glm::vec3 p = b->getPosition();
        const float d = (p.x - focus.x) * (p.x - focus.x) + (p.y - focus.y) * (p.y - focus.y);
        const int k = buildingMaterial(b) == impact::Material::SheetMetal ? 0 : 1;
        if (d < bestD[k] && d < 900.0f * 900.0f) { bestD[k] = d; best[k] = b; }
    }
    static int s_logs = 0;
    bool groundDone = false;
    for (GameObject* b : best) {
        if (!b) continue;
        const glm::vec3 bp = b->getPosition();
        const float r = std::clamp(b->getExtentRadius(), 10.0f, 60.0f);
        glm::vec3 dir(bp.x - camO.x, bp.y - camO.y, 0.0f);
        const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
        if (len < 1e-3f) continue;
        dir /= len;
        const glm::vec3 side(-dir.y, dir.x, 0.0f), up(0.0f, 0.0f, 1.0f);
        const glm::vec3 shot = glm::normalize(dir - up * 0.15f);
        const uint32_t wid = static_cast<uint32_t>(b->getWatchID());
        g_queue.push(fixtureEvent(impact::WeaponClass::Ballistic, bp - side * (r * 0.5f) + up * (r * 0.45f), shot, wid));
        g_queue.push(fixtureEvent(impact::WeaponClass::Laser, bp + up * (r * 0.6f), shot, wid));
        g_queue.push(fixtureEvent(impact::WeaponClass::Missile, bp + side * (r * 0.5f) + up * (r * 0.35f), shot, wid));
        if (!groundDone) {
            groundDone = true;
            // A row on open ground between the camera and the building: missile, laser, ballistic.
            const glm::vec3 ground = glm::normalize(dir - up * 0.6f);
            auto onGround = [&](glm::vec3 p) { if (land) p.z = land->getTerrainElevation(p); return p; };
            g_queue.push(fixtureEvent(impact::WeaponClass::Missile, onGround(bp - dir * (r + 45.0f)), ground, 0));
            g_queue.push(fixtureEvent(impact::WeaponClass::Laser, onGround(bp - dir * (r + 95.0f) + side * 10.0f), ground, 0));
            g_queue.push(fixtureEvent(impact::WeaponClass::Ballistic, onGround(bp - dir * (r + 130.0f) - side * 15.0f), ground, 0));
        }
        if (s_logs < 4) {
            ++s_logs;
            std::fprintf(stderr, "[IMPACT_FIXTURE] volley sample=%d/4 building=%s wid=%u pos=%.0f,%.0f,%.0f extent=%.0f material=%s\n", s_logs,
                         b->getName() ? b->getName() : "?", wid, bp.x, bp.y, bp.z, r,
                         buildingMaterial(b) == impact::Material::SheetMetal ? "sheet_metal" : "concrete");
        }
    }
}

}  // namespace

void onWeaponImpact(long masterId, bool isBeam, bool areaEffect, float damage,
                    const glm::vec3& from, const glm::vec3& hit, unsigned long targetWID) {
    int form = 1;
    bool artillery = false, inferno = false;
    if (MasterComponent::masterList && masterId >= 0 && masterId < MasterComponent::numComponents) {
        MasterComponent& mc = MasterComponent::masterList[masterId];
        form = static_cast<int>(mc.getForm()) - static_cast<int>(COMPONENT_FORM_WEAPON_ENERGY);
        artillery = mc.getWeaponArtillery();
        inferno = mc.getWeaponInferno();
    }
    impact::Event e;
    e.tick = static_cast<uint32_t>(turn);
    e.time = scenarioTime;
    e.pos[0] = hit.x; e.pos[1] = hit.y; e.pos[2] = hit.z;
    glm::vec3 d = hit - from;
    const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    if (len > 1e-3f) { e.dir[0] = d.x / len; e.dir[1] = d.y / len; e.dir[2] = d.z / len; }
    e.anchorId = static_cast<uint32_t>(targetWID);
    e.anchorKind = targetWID ? impact::AnchorKind::Static : impact::AnchorKind::World;   // refined on the presentation side
    e.weapon = impact::classifyWeapon(form, isBeam, areaEffect, artillery, inferno);
    e.magnitude = damage;
    pushImpact(e);
}

void onExplosion(long effectId, const glm::vec3& pos, float radius) {
    impact::WeaponClass w;
    switch (effectId) {
    case LARGE_AIRSTRIKE_ID: case SMALL_AIRSTRIKE_ID: w = impact::WeaponClass::Artillery; break;
    case MINE_EXPLOSION_ID: case BUILDING_EXPLOSION_ID: case VEHICLE_EXPLOSION_ID: case SPLASH_VEHICLE_EXPLOSION_ID:
    case MECH_EXPLOSION_ID: case TURRET_EXPLOSION_ID: case DFA_BOOM_EFFECT:
        w = impact::WeaponClass::Splash; break;
    default: return;   // bolt impacts, smoke, critical hits: no scar of their own
    }
    impact::Event e;
    e.tick = static_cast<uint32_t>(turn);
    e.time = scenarioTime;
    e.pos[0] = pos.x; e.pos[1] = pos.y; e.pos[2] = pos.z;
    e.weapon = w;
    e.magnitude = std::max(radius, effectId == LARGE_AIRSTRIKE_ID ? 80.0f : 30.0f);
    pushImpact(e);
}

namespace {
uint32_t g_builtCount = 0;   // extract() -> frame()
uint32_t g_builtHot = 0;
}  // namespace

void reset() {
    g_queue.clear();
    g_pool.clear();
    g_builtCount = 0;
    g_builtHot = 0;
#ifdef MC2_VULKAN_ISLAND
    gos_SetImpactDecalFrame(nullptr, 0, 0);
#endif
}

// G7b K3: everything that reads the sim (queue drain, anchor poses, object classes) runs here, at the render extract.
void extract() {
    mc2::overlap::assertSimQuiescent("impact_presentation::extract");
    ZoneScopedN("Impact.Presentation");
    // A new mission (or a load) replaces the object manager and restarts the clock: its decals and pending events belong to
    // the old world.
    static const void* s_world = nullptr;
    static float s_lastTime = 0.0f;
    if (ObjectManager != s_world || scenarioTime + 0.5f < s_lastTime) { reset(); s_world = ObjectManager; }
    s_lastTime = scenarioTime;
    const int q = quality();
    g_pool.setCapacity(impact::capacityForQuality(q));
    g_pool.beginFrame();
    fixtureVolley();
    g_queue.drain([&](impact::Event e) {
        if (q <= 0) return;
        // Resolve what was hit: movers carry armour and follow their root; buildings and props keep their scars until
        // destroyed; everything else is ground.
        GameObjectPtr obj = (e.anchorId && ObjectManager) ? ObjectManager->getByWatchID(e.anchorId) : nullptr;
        if (obj && obj->isMover()) {
            if (q < 2) return;   // low quality keeps unit scars off
            e.anchorKind = impact::AnchorKind::Mover;
            e.material = impact::Material::Armor;
        } else if (obj && obj->getObjectClass() != TREE && obj->getObjectClass() != TREEBUILDING) {
            e.anchorKind = impact::AnchorKind::Static;
            e.material = buildingMaterial(obj);
            e.extent = obj->getExtentRadius();
        } else {
            e.anchorKind = impact::AnchorKind::World;
            e.anchorId = 0;
            bool water = false;
            e.material = terrainMaterial(e.pos, water);
            if (water) return;
        }
        const impact::AnchorPose pose = resolveAnchor(e.anchorKind, e.anchorId);
        if (!pose.valid) return;
        const impact::Recipe recipe = impact::recipeFor(e.weapon, e.material, e.magnitude);
        const std::optional<uint32_t> slot = g_pool.spawn(e, recipe, pose);
        static int s_logs = 0;   // receipt of the first spawns: what the library chose for what the sim reported
        if (s_logs < 16) {
            ++s_logs;
            std::fprintf(stderr, "[IMPACT_DECAL] spawn sample=%d/16 tick=%u weapon=%d material=%d anchor=%d id=%u pos=%.0f,%.0f,%.0f tile=%d/%d radius=%.1f slot=%d\n", s_logs,
                         e.tick, static_cast<int>(e.weapon), static_cast<int>(e.material), static_cast<int>(e.anchorKind), e.anchorId, e.pos[0], e.pos[1], e.pos[2],
                         static_cast<int>(recipe.tileHit), static_cast<int>(recipe.tileGround), recipe.radius, slot ? static_cast<int>(*slot) : -1);
        }
    });
    g_pool.update(scenarioTime, resolveAnchor);
    uint32_t hot = 0;
    g_builtCount = g_pool.build(scenarioTime, resolveAnchor, g_gpu, impact::kMaxDecals, hot);
    g_builtHot = hot;
}

void frame() {
#ifdef MC2_VULKAN_ISLAND
    gos_SetImpactDecalFrame(g_gpu, g_builtCount, g_builtHot);
#endif
}

}  // namespace impact_presentation
