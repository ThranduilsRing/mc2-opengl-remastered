// dynamic_decal_ring.cpp
// Runtime terrain impact decal ring buffer.
// See dynamic_decal_ring.h for design notes.
//
// Gate: MC2_DYNAMIC_DECALS (env var; default OFF).
// Off behavior: spawn() is a no-op; gatherToDecalBatch() pushes nothing.
//
// Dependencies (all already in mclib):
//   terrain.h    -> land (getTerrainElevation)
//   gameos.hpp   -> gos_PushDecal, WorldOverlayVert
//
// The gos texture handle is resolved by the caller (crater.cpp) at spawn
// time so this file has no dependency on crater.h or legacy texture manager.

#include "dynamic_decal_ring.h"

#include <cstdlib>
#include <cmath>
#include <cstring>

#ifndef TERRAIN_H
#include "terrain.h"
#endif

#include "terrain_runtime.h"   // TERRAIN-RUNTIME-CONSUMER-DECALS-1

#include "gameos.hpp"  // gos_PushDecal, WorldOverlayVert
#include "Gates/gate.h"

// ─────────────────────────────────────────────────────────────────────────────
// Internal state
// ─────────────────────────────────────────────────────────────────────────────

namespace DynDecal {

namespace {

// Gate check — evaluated once at first use via static bool.
bool isEnabled()
{
    static int cached = -1;
    if (cached < 0) {
        cached = mc2::gate::get<"MC2_DYNAMIC_DECALS">();
    }
    return cached == 1;
}

static Slot  s_slots[kCapacity];
static bool  s_init = false;

// Default lifetime (ms) when caller passes 0.
static constexpr uint16_t kDefaultLifetimeMs = 6000;

// Fade starts when age reaches this fraction of lifetime.
static constexpr float kFadeStartFraction = 0.80f;

void ensureInit()
{
    if (!s_init) {
        memset(s_slots, 0, sizeof(s_slots));
        s_init = true;
    }
}

// Next write position (ring-buffer wraparound; newest overwrites oldest).
static int s_writeHead = 0;

// Evaluate alpha [0,1] for a slot given its current age/lifetime.
float slotAlpha(const Slot& sl)
{
    if (sl.lifetimeMs == 0) return 0.0f;
    float t = static_cast<float>(sl.ageMs) / static_cast<float>(sl.lifetimeMs);
    if (t >= kFadeStartFraction) {
        float fadeT = (t - kFadeStartFraction) / (1.0f - kFadeStartFraction);
        return 1.0f - fadeT;  // linear 1 -> 0
    }
    return 1.0f;
}

// Pack BGRA uint from floats [0,1].
// Layout: bits [31:24]=A [23:16]=R [15:8]=G [7:0]=B (little-endian BGRA).
unsigned int packBGRA(float r, float g, float b, float a)
{
    unsigned int ib = static_cast<unsigned int>((b * 255.0f + 0.5f));
    unsigned int ig = static_cast<unsigned int>((g * 255.0f + 0.5f));
    unsigned int ir = static_cast<unsigned int>((r * 255.0f + 0.5f));
    unsigned int ia = static_cast<unsigned int>((a * 255.0f + 0.5f));
    return (ia << 24) | (ir << 16) | (ig << 8) | ib;
}

// Elevate a world XY position to terrain height.
void elevate(float x, float y, float& z)
{
    if (!land) return;
    glm::vec3 p;
    p.x = x; p.y = y; p.z = 0.0f;
    z = TerrainRuntime::decalElevation(p);
}

// Push a decal quad as two triangles via gos_PushDecal.
void pushQuad(const Slot& sl, unsigned int argb)
{
    float cosR = cosf(sl.rotation);
    float sinR = sinf(sl.rotation);

    // Four corners in local XY, then rotate into world XY.
    float localX[4] = { -sl.radius,  sl.radius,  sl.radius, -sl.radius };
    float localY[4] = { -sl.radius, -sl.radius,  sl.radius,  sl.radius };

    float wx[4], wy[4], wz[4];
    for (int i = 0; i < 4; ++i) {
        float lx = localX[i], ly = localY[i];
        wx[i] = sl.wx + cosR * lx - sinR * ly;
        wy[i] = sl.wy + sinR * lx + cosR * ly;
        wz[i] = sl.wz;
        elevate(wx[i], wy[i], wz[i]);
    }

    // UV: map quad corners to [0,1].
    float us[4] = { 0.0f, 1.0f, 1.0f, 0.0f };
    float vs[4] = { 1.0f, 1.0f, 0.0f, 0.0f };

    // Fog: 1.0 = clear (no fog attenuation).
    const float fog = 1.0f;

    // Triangle 0: corners 0,1,2
    WorldOverlayVert t0[3];
    t0[0] = { wx[0], wy[0], wz[0], us[0], vs[0], fog, argb };
    t0[1] = { wx[1], wy[1], wz[1], us[1], vs[1], fog, argb };
    t0[2] = { wx[2], wy[2], wz[2], us[2], vs[2], fog, argb };
    gos_PushDecal(t0, sl.texHandle);

    // Triangle 1: corners 0,2,3
    WorldOverlayVert t1[3];
    t1[0] = { wx[0], wy[0], wz[0], us[0], vs[0], fog, argb };
    t1[1] = { wx[2], wy[2], wz[2], us[2], vs[2], fog, argb };
    t1[2] = { wx[3], wy[3], wz[3], us[3], vs[3], fog, argb };
    gos_PushDecal(t1, sl.texHandle);
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

void spawn(const glm::vec3& worldPos, float radius,
           float rotationRad, unsigned int texHandle, uint16_t lifetimeMs)
{
    if (!isEnabled()) return;
    if (!texHandle)   return;  // no valid texture, skip
    ensureInit();

    Slot& sl       = s_slots[s_writeHead];
    sl.wx          = worldPos.x;
    sl.wy          = worldPos.y;
    sl.wz          = worldPos.z;
    sl.radius      = radius;
    sl.rotation    = rotationRad;
    sl.texHandle   = texHandle;
    sl.lifetimeMs  = (lifetimeMs > 0) ? lifetimeMs : kDefaultLifetimeMs;
    sl.ageMs       = 0;

    s_writeHead = (s_writeHead + 1) % kCapacity;
}

void gatherToDecalBatch(float frameDeltaSec)
{
    if (!isEnabled()) return;
    if (!s_init)      return;  // nothing spawned yet

    uint16_t deltaMs = static_cast<uint16_t>((frameDeltaSec * 1000.0f + 0.5f));
    if (deltaMs == 0) deltaMs = 1;  // advance at least 1 ms per frame

    for (int i = 0; i < kCapacity; ++i) {
        Slot& sl = s_slots[i];
        if (sl.lifetimeMs == 0) continue;  // empty slot

        // Advance age first.
        int newAge = static_cast<int>(sl.ageMs) + deltaMs;
        if (newAge >= static_cast<int>(sl.lifetimeMs)) {
            // Slot expired — mark empty.
            sl.lifetimeMs = 0;
            sl.ageMs      = 0;
            continue;
        }
        sl.ageMs = static_cast<uint16_t>(newAge);

        float alpha = slotAlpha(sl);
        if (alpha <= 0.0f) continue;

        // Full white RGB, computed alpha.
        unsigned int argb = packBGRA(1.0f, 1.0f, 1.0f, alpha);

        pushQuad(sl, argb);
    }
}

int liveCount()
{
    if (!s_init) return 0;
    int count = 0;
    for (int i = 0; i < kCapacity; ++i) {
        if (s_slots[i].lifetimeMs > 0) ++count;
    }
    return count;
}

int snapshotLiveSlots(Slot* outSlots, float* outAlpha, int maxOut)
{
    // Pure read; never mutates ring state. Safe to call from a parallel consumer
    // even when MC2_DYNAMIC_DECALS is off (returns 0 since nothing was ever spawned).
    if (!outSlots || maxOut <= 0) return 0;
    if (!s_init) return 0;
    int n = 0;
    for (int i = 0; i < kCapacity && n < maxOut; ++i) {
        const Slot& sl = s_slots[i];
        if (sl.lifetimeMs == 0) continue;   // empty
        float a = slotAlpha(sl);
        if (a <= 0.0f) continue;            // fully faded
        outSlots[n] = sl;
        if (outAlpha) outAlpha[n] = a;
        ++n;
    }
    return n;
}

} // namespace DynDecal
