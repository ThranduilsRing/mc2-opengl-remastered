// RenderCore/ImpactDecals.h -- weapon-class x surface-material impact decals: the pure presentation core.
//
// WHAT: the impact event the simulation emits when a shot lands, the library that turns (weapon class, surface material)
// into a decal recipe, the bounded pool that spawns, merges, fades and evicts decals, and the GPU record the Vulkan box-decal
// pass draws. Header-only, no engine or Vulkan dependency, so the unit tier tests exactly what the game runs.
// WHY: impacts are presentation. The simulation stays authoritative for hits, damage and destruction; it only pushes events
// into a queue that it never reads back, and nothing here touches a simulation RNG (variation comes from a stateless hash of
// the event). See docs/impact-damage-design.md.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace impact {

// What hit. Derived from the weapon's component form plus the bolt's beam/area flags (classifyWeapon).
enum class WeaponClass : uint8_t { Ballistic = 0, Laser, Ppc, Missile, Splash, Artillery, Count };

// What was hit. Tier 1 keys the response on these four; the kit and content-pipeline material classes map onto them
// (materialFromContentClass). Armor is mech/vehicle plating: sheet-metal response on a short-lived, mover-anchored decal.
enum class Material : uint8_t { Soil = 0, Concrete, SheetMetal, Armor, Count };

// Atlas layers (one 2D-array layer per response). The generator in ImpactDecalAtlas.h paints them in this order.
enum class Tile : uint8_t {
    BulletMetal = 0,   // puncture cluster: black holes, bent petals, bare-metal ring
    BulletConcrete,    // spall chip: lighter fresh aggregate, dark pits, radial cracks
    LaserMetal,        // melt groove with molten lips and a heat band
    LaserConcrete,     // glassed streak with cross cracks
    BlastMetal,        // torn breach: jagged hole, petals bent outward, heavy soot
    BlastGround,       // crater bowl, raised rim, ejecta rays, charred centre
    PpcScorch,         // discharge starburst with branching burn lines
    SplashScorch,      // soft soot blot
    Count
};
inline constexpr uint32_t kTileCount = static_cast<uint32_t>(Tile::Count);

// Which surfaces a decal may touch, matched in the shader against the scene normal target's class alpha
// (terrain writes 1.0, static props and buildings 0.1875..0.25, mechs/vehicles/legacy shapes 0).
inline constexpr uint8_t kAffectTerrain = 1, kAffectProp = 2, kAffectUnit = 4;

// What the decal is attached to. World decals sit on terrain; Static decals belong to a building/prop and die with it;
// Mover decals follow a mech or vehicle root every frame.
enum class AnchorKind : uint8_t { World = 0, Static, Mover };

inline constexpr uint32_t kMaxDecals = 256;        // hard cap of the pool and of the GPU instance buffer
inline constexpr uint32_t kMaxEventsPerFrame = 48; // queue cap; overflow drops the oldest (counted)
inline constexpr uint32_t kMaxSpawnsPerFrame = 32; // a burst beyond this in one frame is dropped (counted)

// Quality: 0 = off (potato / base game), 1 = low, 2 = full. The capacity per quality is the count budget.
inline constexpr uint32_t capacityForQuality(int quality) { return quality <= 0 ? 0u : quality == 1 ? 96u : kMaxDecals; }

// ---------------------------------------------------------------------------------------------------------------------
// The event. Positions and directions are in MC2 world space (x east, y north, z up); tick is the sim tick it came from.
struct Event {
    uint32_t    tick = 0;
    float       time = 0.0f;              // scenario time of the hit (seconds)
    float       pos[3] = {0, 0, 0};       // hit point
    float       dir[3] = {0, 0, -1};      // travel direction of the shot (normalized); straight down for explosions
    uint32_t    anchorId = 0;             // watch id of the target; 0 = world
    AnchorKind  anchorKind = AnchorKind::World;
    WeaponClass weapon = WeaponClass::Ballistic;
    Material    material = Material::Soil;
    float       magnitude = 0.0f;         // shot damage, or the explosion radius for Splash/Artillery
    float       extent = 0.0f;            // target extent radius (Static anchors), 0 otherwise
};

// Bounded FIFO the simulation pushes into and the presentation drains once per rendered frame. Overflow drops the OLDEST
// event: a burst keeps its newest impacts, which are the ones still on screen.
class EventQueue {
public:
    void push(const Event& e) {
        if (count_ == kMaxEventsPerFrame) { head_ = (head_ + 1) % kMaxEventsPerFrame; --count_; ++dropped_; }
        ring_[(head_ + count_) % kMaxEventsPerFrame] = e;
        ++count_;
    }
    template <class F> uint32_t drain(F&& f) {
        uint32_t n = 0;
        while (count_) { f(ring_[head_]); head_ = (head_ + 1) % kMaxEventsPerFrame; --count_; ++n; }
        return n;
    }
    void clear() { head_ = count_ = 0; }
    uint32_t size() const { return count_; }
    uint64_t dropped() const { return dropped_; }
private:
    std::array<Event, kMaxEventsPerFrame> ring_{};
    uint32_t head_ = 0, count_ = 0;
    uint64_t dropped_ = 0;
};

// ---------------------------------------------------------------------------------------------------------------------
// Classification. componentForm uses the MasterComponent ordinals relative to COMPONENT_FORM_WEAPON_ENERGY
// (0 energy, 1 ballistic, 2 missile); anything else is treated as ballistic.
inline WeaponClass classifyWeapon(int formFromEnergy, bool isBeam, bool areaEffect, bool artilleryFlag, bool infernoFlag) {
    if (artilleryFlag) return WeaponClass::Artillery;
    if (infernoFlag) return WeaponClass::Splash;
    switch (formFromEnergy) {
    case 0: return isBeam ? WeaponClass::Laser : WeaponClass::Ppc;
    case 2: return WeaponClass::Missile;
    default: return areaEffect ? WeaponClass::Missile : WeaponClass::Ballistic;
    }
}

// The content pipeline's per-part material class (mclib/model_override_registry.h on claude/content-pipeline:
// 1 dielectric, 2 painted metal, 3 bare metal, 4 rust). 0 = unknown -> the caller's fallback.
inline Material materialFromContentClass(uint32_t materialClass, Material fallback) {
    switch (materialClass) {
    case 1: return Material::Concrete;
    case 2: case 3: case 4: return Material::SheetMetal;
    default: return fallback;
    }
}

// Terrain type (mclib/dmapdata.h TerrainType ordinals) -> material. Concrete/cement are hard; water gets no decal.
inline bool terrainTypeIsWater(int t) { return t == 0 || t == 1; }
inline Material materialFromTerrainType(int t) {
    if (t == 10 || (t >= 13 && t <= 19)) return Material::Concrete;   // MC_CONCRETE_TYPE, MC_CEMENT2..8
    return Material::Soil;
}

// ---------------------------------------------------------------------------------------------------------------------
// The library: one recipe per (weapon, material). Sizes are MC2 world units (5 per metre). Lifetimes are scenario seconds.
struct Recipe {
    Tile    tileHit = Tile::SplashScorch;     // layer for pixels of the class that was hit (prop/unit), or terrain for World
    Tile    tileGround = Tile::SplashScorch;  // layer for terrain pixels inside the box when the hit was on an object
    float   radius = 4.0f;                    // half-size across the shot
    float   elongation = 1.0f;                // x half-size = radius * elongation (laser tracks)
    float   lifetime = 45.0f;
    float   glow = 0.0f;                      // initial emissive strength (HDR multiplier)
    float   glowTau = 0.0f;                   // cooling time constant (s); 0 = never glows
    float   glowColor[3] = {1.0f, 0.55f, 0.18f};   // emissive colour at full heat; it reddens as it cools
    float   normalStrength = 1.0f;            // deformation gain on the relit normal
    uint8_t affect = kAffectTerrain | kAffectProp;
};

inline Tile groundTileFor(WeaponClass w) {
    switch (w) {
    case WeaponClass::Ballistic: return Tile::BulletConcrete;
    case WeaponClass::Laser:     return Tile::LaserConcrete;
    case WeaponClass::Ppc:       return Tile::PpcScorch;
    case WeaponClass::Missile:
    case WeaponClass::Artillery: return Tile::BlastGround;
    default:                     return Tile::SplashScorch;
    }
}

inline Recipe recipeFor(WeaponClass w, Material m, float magnitude) {
    Recipe r;
    const bool metal = (m == Material::SheetMetal || m == Material::Armor);
    switch (w) {
    case WeaponClass::Ballistic:
        r.tileHit = metal ? Tile::BulletMetal : Tile::BulletConcrete;
        r.radius = metal ? 9.0f : (m == Material::Soil ? 10.0f : 9.0f);
        r.lifetime = 45.0f;
        if (metal) { r.glow = 1.5f; r.glowTau = 0.25f; }
        r.normalStrength = 1.2f;
        break;
    case WeaponClass::Laser:
        r.tileHit = metal ? Tile::LaserMetal : Tile::LaserConcrete;
        r.radius = 4.5f; r.elongation = metal ? 3.0f : 2.6f;
        r.lifetime = 50.0f;
        r.glow = metal ? 5.0f : 3.5f; r.glowTau = metal ? 1.6f : 1.0f;
        r.glowColor[0] = 1.0f; r.glowColor[1] = 0.62f; r.glowColor[2] = 0.25f;
        r.normalStrength = 1.0f;
        break;
    case WeaponClass::Ppc:
        r.tileHit = Tile::PpcScorch;
        r.radius = 14.0f;
        r.lifetime = 60.0f;
        r.glow = 3.5f; r.glowTau = 0.6f;
        r.glowColor[0] = 0.45f; r.glowColor[1] = 0.7f; r.glowColor[2] = 1.0f;
        r.normalStrength = 0.6f;
        break;
    case WeaponClass::Missile: {
        r.tileHit = metal ? Tile::BlastMetal : Tile::BlastGround;
        const float dmgScale = std::clamp(0.8f + magnitude / 25.0f, 0.8f, 1.3f);
        r.radius = (metal ? 15.0f : (m == Material::Soil ? 22.0f : 18.0f)) * dmgScale;
        r.lifetime = 90.0f;
        r.glow = 2.5f; r.glowTau = 0.45f;
        r.normalStrength = 1.4f;
        break;
    }
    case WeaponClass::Splash:
        r.tileHit = Tile::SplashScorch;
        r.radius = std::clamp(magnitude * 0.8f, 20.0f, 70.0f);
        r.lifetime = 75.0f;
        r.normalStrength = 0.0f;
        break;
    case WeaponClass::Artillery:
        r.tileHit = Tile::BlastGround;
        r.radius = std::clamp(magnitude * 0.6f, 40.0f, 80.0f);
        r.lifetime = 120.0f;
        r.glow = 2.0f; r.glowTau = 0.6f;
        r.normalStrength = 1.6f;
        break;
    default: break;
    }
    r.tileGround = (m == Material::Soil || m == Material::Concrete) ? r.tileHit : groundTileFor(w);
    if (m == Material::Armor) {          // mover plating: smaller, shorter, never paints the ground under a walking mech
        r.radius *= 0.6f;
        r.lifetime = std::min(r.lifetime, 25.0f);
        r.affect = kAffectUnit;
    }
    return r;
}

// Recipe affect by anchor: a world decal paints terrain and props inside its box (never units walking through it);
// a building decal paints the building and the ground at its foot; a mover decal paints only units.
inline uint8_t affectFor(AnchorKind k, const Recipe& r) {
    switch (k) {
    case AnchorKind::World:  return kAffectTerrain | kAffectProp;
    case AnchorKind::Static: return kAffectProp | kAffectTerrain;
    default:                 return static_cast<uint8_t>(r.affect & kAffectUnit ? r.affect : kAffectUnit);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Presentation variation: a stateless hash of the event (never a simulation RNG stream).
inline uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16; return x;
}
inline uint32_t presentationSeed(const Event& e) {
    uint32_t h = hash32(e.tick * 0x9E3779B9U ^ e.anchorId);
    for (float p : e.pos) h = hash32(h ^ static_cast<uint32_t>(static_cast<int32_t>(std::floor(p * 4.0f))));
    return hash32(h ^ static_cast<uint32_t>(e.weapon));
}
inline float seedUnit(uint32_t seed, uint32_t salt) { return static_cast<float>(hash32(seed ^ (salt * 0x632BE5ABU)) >> 8) * (1.0f / 16777216.0f); }

// ---------------------------------------------------------------------------------------------------------------------
// Anchor pose supplied by the caller each frame (and at spawn): MC2-space root position and yaw in degrees about +z.
struct AnchorPose {
    bool  valid = true;
    float pos[3] = {0, 0, 0};
    float yawDeg = 0.0f;
};

// GPU instance record (std430, 8 x vec4 = 128 bytes). Vectors are in MC2 world space, the space the terrain world-to-clip
// matrix takes and the scene depth reconstructs to.
//   v0 = centre.xyz, alpha            v4 = axisX / |axisX|^2, tileHit
//   v1 = axisX * halfX, heat          v5 = axisY / |axisY|^2, tileGround
//   v2 = axisY * halfY, normalGain    v6 = axisZ / |axisZ|^2, affect bits
//   v3 = axisZ * halfZ (projection axis into the surface), spare
//   v7 = glow colour at full heat (rgb), glow strength
struct GpuDecal { float v[8][4]; };
static_assert(sizeof(GpuDecal) == 128, "GpuDecal must match the std430 ImpactDecal record in shaders/vulkan/impact_decal.*");

struct PoolStats {
    uint64_t spawned = 0, merged = 0, evicted = 0, expired = 0, droppedBurst = 0, anchorLost = 0;
};

class Pool {
public:
    struct Decal {
        bool       live = false;
        AnchorKind anchorKind = AnchorKind::World;
        WeaponClass weapon = WeaponClass::Ballistic;
        uint32_t   anchorId = 0;
        uint32_t   seed = 0;
        Recipe     recipe;
        float      spawnTime = 0.0f;
        float      intensity = 1.0f;
        float      sizeScale = 1.0f;
        float      extent = 0.0f;
        float      local[3] = {0, 0, 0};   // hit point in the anchor frame at spawn (world space for World anchors)
        float      localDir[3] = {0, 0, -1};
    };

    void setCapacity(uint32_t c) {
        capacity_ = std::min(c, kMaxDecals);
        for (uint32_t i = capacity_; i < kMaxDecals; ++i) if (d_[i].live) { d_[i].live = false; ++stats_.evicted; }
    }
    uint32_t capacity() const { return capacity_; }
    void clear() { for (auto& d : d_) d.live = false; }
    void beginFrame() { spawnsThisFrame_ = 0; }

    uint32_t liveCount() const {
        uint32_t n = 0; for (uint32_t i = 0; i < capacity_; ++i) n += d_[i].live ? 1u : 0u; return n;
    }
    const PoolStats& stats() const { return stats_; }
    const Decal& at(uint32_t i) const { return d_[i]; }

    // Spawn or merge. Returns the slot used; empty when dropped (quality off, burst cap).
    std::optional<uint32_t> spawn(const Event& e, const Recipe& r, const AnchorPose& anchorAtSpawn) {
        if (capacity_ == 0) return std::nullopt;
        if (spawnsThisFrame_ >= kMaxSpawnsPerFrame) { ++stats_.droppedBurst; return std::nullopt; }
        ++spawnsThisFrame_;
        float local[3], ldir[3];
        toAnchorFrame(e, anchorAtSpawn, local, ldir);

        // Merge: sustained fire on one spot refreshes and grows the existing decal instead of stacking new ones.
        for (uint32_t i = 0; i < capacity_; ++i) {
            Decal& d = d_[i];
            if (!d.live || d.anchorId != e.anchorId || d.anchorKind != e.anchorKind || d.weapon != e.weapon) continue;
            const float dx = d.local[0] - local[0], dy = d.local[1] - local[1], dz = d.local[2] - local[2];
            const float mergeR = 0.5f * d.recipe.radius * d.sizeScale;
            if (dx * dx + dy * dy + dz * dz > mergeR * mergeR) continue;
            d.spawnTime = e.time;
            d.intensity = std::min(1.0f, d.intensity + 0.25f);
            d.sizeScale = std::min(1.6f, d.sizeScale * 1.12f);
            ++stats_.merged;
            return i;
        }

        uint32_t slot = capacity_;
        for (uint32_t i = 0; i < capacity_ && slot == capacity_; ++i) if (!d_[i].live) slot = i;
        if (slot == capacity_) {   // full: evict the decal with the least remaining life (fraction), lowest index on ties
            float best = 2.0f;
            for (uint32_t i = 0; i < capacity_; ++i) {
                const float rem = remainingFraction(d_[i], e.time);
                if (rem < best) { best = rem; slot = i; }
            }
            ++stats_.evicted;
        }
        Decal& d = d_[slot];
        d = Decal{};
        d.live = true;
        d.anchorKind = e.anchorKind; d.anchorId = e.anchorId; d.weapon = e.weapon;
        d.seed = presentationSeed(e);
        d.recipe = r; d.recipe.affect = affectFor(e.anchorKind, r);
        d.spawnTime = e.time;
        d.intensity = 0.8f + 0.2f * seedUnit(d.seed, 1);
        d.extent = e.extent;
        for (int k = 0; k < 3; ++k) { d.local[k] = local[k]; d.localDir[k] = ldir[k]; }
        ++stats_.spawned;
        return slot;
    }

    // Fade: full strength for the first 70% of the lifetime, then a smooth fall to zero.
    static float fadeAlpha(const Decal& d, float now) {
        const float age = std::max(0.0f, now - d.spawnTime);
        const float life = std::max(0.001f, d.recipe.lifetime);
        if (age >= life) return 0.0f;
        const float t = std::clamp((age - 0.7f * life) / (0.3f * life), 0.0f, 1.0f);
        return d.intensity * (1.0f - t * t * (3.0f - 2.0f * t));
    }
    // Heat: the glow cools exponentially from the moment of the (last merged) hit.
    static float heat(const Decal& d, float now) {
        if (d.recipe.glowTau <= 0.0f || d.recipe.glow <= 0.0f) return 0.0f;
        const float age = std::max(0.0f, now - d.spawnTime);
        return std::exp(-age / d.recipe.glowTau);
    }
    static constexpr float kHotThreshold = 0.01f;

    // Expire dead decals and the ones whose anchor is gone. resolve(anchorKind, anchorId) -> AnchorPose.
    template <class Resolve> void update(float now, Resolve&& resolve) {
        for (uint32_t i = 0; i < capacity_; ++i) {
            Decal& d = d_[i];
            if (!d.live) continue;
            if (fadeAlpha(d, now) <= 0.0f) { d.live = false; ++stats_.expired; continue; }
            if (d.anchorKind != AnchorKind::World && !resolve(d.anchorKind, d.anchorId).valid) { d.live = false; ++stats_.anchorLost; }
        }
    }

    // Build GPU records: hot (glowing) decals first so the additive pass draws the prefix [0, hotCount).
    template <class Resolve> uint32_t build(float now, Resolve&& resolve, GpuDecal* out, uint32_t maxOut, uint32_t& hotCount) const {
        uint32_t n = 0;
        hotCount = 0;
        for (int pass = 0; pass < 2; ++pass) {
            for (uint32_t i = 0; i < capacity_ && n < maxOut; ++i) {
                const Decal& d = d_[i];
                if (!d.live) continue;
                const bool hot = heat(d, now) * d.recipe.glow > kHotThreshold;
                if ((pass == 0) != hot) continue;
                AnchorPose pose = d.anchorKind == AnchorKind::World ? AnchorPose{} : resolve(d.anchorKind, d.anchorId);
                if (!pose.valid) continue;
                writeGpu(d, now, pose, out[n++]);
                if (hot) ++hotCount;
            }
        }
        return n;
    }

    // Public for the unit tier: the record of one decal.
    static void writeGpu(const Decal& d, float now, const AnchorPose& pose, GpuDecal& g) {
        float hit[3], dir[3];
        fromAnchorFrame(d, pose, hit, dir);
        const float* hitR = hit;
        float z[3] = {dir[0], dir[1], dir[2]};
        const Recipe& r = d.recipe;
        const float scale = d.sizeScale;
        float halfX = r.radius * r.elongation * scale, halfY = r.radius * scale, halfZ;
        float centre[3];
        const float up[3] = {0.0f, 0.0f, 1.0f};
        if (d.anchorKind == AnchorKind::World) {
            // Terrain: project straight down; the in-plane x axis follows the shot's horizontal heading (laser tracks run along it).
            const float hx = z[0], hy = z[1];
            const float hl = std::sqrt(hx * hx + hy * hy);
            z[0] = 0.0f; z[1] = 0.0f; z[2] = -1.0f;
            halfZ = std::max(r.radius, 6.0f) + 12.0f;
            for (int k = 0; k < 3; ++k) centre[k] = hitR[k];
            float x[3] = {1.0f, 0.0f, 0.0f};
            if (hl > 1e-3f) { x[0] = hx / hl; x[1] = hy / hl; }
            else { const float a = seedUnit(d.seed, 2) * 6.2831853f; x[0] = std::cos(a); x[1] = std::sin(a); }
            float y[3]; cross(z, x, y);
            finish(g, centre, x, y, z, halfX, halfY, halfZ);
        } else {
            // Objects project along the shot. A building's hit point may lie inside its hull, so its box reaches back toward the
            // shooter (35% of its depth) to find the facing wall; a mover's hit node is on the body.
            normalize(z);
            const bool building = d.anchorKind == AnchorKind::Static;
            const float reach = building ? std::max(r.radius * 1.5f, d.extent) : r.radius * 1.5f;
            halfZ = building ? std::clamp(reach, 8.0f, 60.0f) : std::max(reach, 4.0f);
            const float back = building ? halfZ * 0.35f : 0.0f;
            for (int k = 0; k < 3; ++k) centre[k] = hitR[k] - z[k] * back;
            orthoFrame(d, z, up, halfX, halfY, halfZ, centre, g);
        }
        const float h = heat(d, now);
        g.v[0][3] = fadeAlpha(d, now);
        g.v[1][3] = h;
        g.v[2][3] = r.normalStrength;
        g.v[3][3] = 0.0f;
        g.v[4][3] = static_cast<float>(r.tileHit);
        g.v[5][3] = static_cast<float>(r.tileGround);
        g.v[6][3] = static_cast<float>(r.affect);
        g.v[7][0] = r.glowColor[0]; g.v[7][1] = r.glowColor[1]; g.v[7][2] = r.glowColor[2];
        g.v[7][3] = r.glow;
    }

private:
    static float remainingFraction(const Decal& d, float now) {
        if (!d.live) return -1.0f;
        return std::clamp(1.0f - (now - d.spawnTime) / std::max(0.001f, d.recipe.lifetime), 0.0f, 1.0f);
    }
    static void cross(const float a[3], const float b[3], float o[3]) {
        o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0];
    }
    static void normalize(float v[3]) {
        const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (l < 1e-6f) { v[0] = 0; v[1] = 0; v[2] = -1; return; }
        v[0] /= l; v[1] /= l; v[2] /= l;
    }
    static void orthoFrame(const Decal& d, const float z[3], const float up[3], float halfX, float halfY, float halfZ,
                           const float centre[3], GpuDecal& g) {
        float x[3];
        const float zu = z[0] * up[0] + z[1] * up[1] + z[2] * up[2];
        if (std::fabs(zu) < 0.95f) cross(up, z, x); else { const float north[3] = {0, 1, 0}; cross(north, z, x); }
        normalize(x);
        float y[3]; cross(z, x, y);
        // In-plane variation of +-35 degrees (laser tracks stay roughly horizontal on walls).
        const float a = (seedUnit(d.seed, 3) - 0.5f) * 1.2217f;
        const float c = std::cos(a), s = std::sin(a);
        float xr[3], yr[3];
        for (int k = 0; k < 3; ++k) { xr[k] = c * x[k] + s * y[k]; yr[k] = -s * x[k] + c * y[k]; }
        finish(g, centre, xr, yr, z, halfX, halfY, halfZ);
    }
    static void finish(GpuDecal& g, const float c[3], const float x[3], const float y[3], const float z[3],
                       float hx, float hy, float hz) {
        for (int k = 0; k < 3; ++k) {
            g.v[0][k] = c[k];
            g.v[1][k] = x[k] * hx; g.v[2][k] = y[k] * hy; g.v[3][k] = z[k] * hz;
            g.v[4][k] = x[k] / hx; g.v[5][k] = y[k] / hy; g.v[6][k] = z[k] / hz;
        }
    }
    static void toAnchorFrame(const Event& e, const AnchorPose& p, float local[3], float ldir[3]) {
        if (e.anchorKind == AnchorKind::World || !p.valid) {
            for (int k = 0; k < 3; ++k) { local[k] = e.pos[k]; ldir[k] = e.dir[k]; }
            return;
        }
        const float a = -p.yawDeg * 0.017453292f, c = std::cos(a), s = std::sin(a);
        const float dx = e.pos[0] - p.pos[0], dy = e.pos[1] - p.pos[1];
        local[0] = c * dx - s * dy; local[1] = s * dx + c * dy; local[2] = e.pos[2] - p.pos[2];
        ldir[0] = c * e.dir[0] - s * e.dir[1]; ldir[1] = s * e.dir[0] + c * e.dir[1]; ldir[2] = e.dir[2];
    }
    static void fromAnchorFrame(const Decal& d, const AnchorPose& p, float hit[3], float dir[3]) {
        if (d.anchorKind == AnchorKind::World) {
            for (int k = 0; k < 3; ++k) { hit[k] = d.local[k]; dir[k] = d.localDir[k]; }
            return;
        }
        const float a = p.yawDeg * 0.017453292f, c = std::cos(a), s = std::sin(a);
        hit[0] = p.pos[0] + c * d.local[0] - s * d.local[1];
        hit[1] = p.pos[1] + s * d.local[0] + c * d.local[1];
        hit[2] = p.pos[2] + d.local[2];
        dir[0] = c * d.localDir[0] - s * d.localDir[1];
        dir[1] = s * d.localDir[0] + c * d.localDir[1];
        dir[2] = d.localDir[2];
    }

    std::array<Decal, kMaxDecals> d_{};
    uint32_t capacity_ = kMaxDecals;
    uint32_t spawnsThisFrame_ = 0;
    PoolStats stats_{};
};

}  // namespace impact
