// local_light_shadow.h -- LOCAL-LIGHTS-2: the shadow tiles of the nearest coned local lights (building searchlights).
//
// WHAT: a few coned pool lights get a 512^2 depth tile in one D16 atlas (kCols x kRows tiles). The pool producer
// (gos_terrain_lighting.cpp) picks the lights each frame and writes tile+1 into bits 4-7 of the pool record's lightType;
// the VK shadow island renders the tiles (static casters baked once per tile assignment, mechs every frame); every pool
// receiver (terrain, overlays, props, vegetation cards, mechs) samples the tile of a light that has one
// (light_pool_attenuation.hglsl lpa_localShadow).
// WHY no matrix buffer: the projection is a pure function of the pool record (position, packed aim + cone, far
// distance), so receivers rebuild it in the shader and no receiver needs a new buffer binding. lightMatrix() below and
// lpa_localShadow MUST stay in lockstep.
#pragma once
#include <cmath>
#include <cstdint>

namespace mc2localshadow {

inline constexpr uint32_t kCols = 4, kRows = 2, kTiles = kCols * kRows;
inline constexpr uint32_t kTileSide = 512;
inline constexpr uint32_t kAtlasW = kCols * kTileSide, kAtlasH = kRows * kTileSide;
// The lamps sit on (often inside) their building's geometry: casters closer than kNear along the aim are
// clipped so the housing does not shadow its own beam. Lockstep with lpa_localShadow.
inline constexpr float kNear = 12.0f;
inline constexpr float kFovMargin = 1.05f;

// One tile's light: the pool record fields the projection is built from. key identifies the light AND its
// projection, so a tile whose key is unchanged keeps its baked static casters.
struct Tile {
    uint32_t used = 0;
    float pos[3] = {0, 0, 0};       // MC2 world frame
    uint32_t packedType = 0;        // the pool record's lightType without the tile bits
    float farDistance = 0.0f;
    uint64_t key = 0;
};

inline void decodeAim(uint32_t packed, float out[3]) {
    float ex = static_cast<float>((packed >> 8) & 255u) * (2.0f / 255.0f) - 1.0f;
    float ey = static_cast<float>((packed >> 16) & 255u) * (2.0f / 255.0f) - 1.0f;
    float z = 1.0f - std::fabs(ex) - std::fabs(ey);
    if (z < 0.0f) {
        const float ox = ex, oy = ey;
        ex = (1.0f - std::fabs(oy)) * (ox >= 0.0f ? 1.0f : -1.0f);
        ey = (1.0f - std::fabs(ox)) * (oy >= 0.0f ? 1.0f : -1.0f);
    }
    const float l = std::sqrt(ex * ex + ey * ey + z * z);
    out[0] = ex / l; out[1] = ey / l; out[2] = z / l;
}

inline float tanHalf(uint32_t packed) {
    const float c = static_cast<float>(packed >> 24) * (1.0f / 255.0f);
    const float cc = c > 1e-3f ? c : 1e-3f;
    const float s2 = 1.0f - c * c;
    return std::sqrt(s2 > 1e-6f ? s2 : 1e-6f) / cc * kFovMargin;
}

// World (MC2 frame) -> clip of one tile, column-major (GL_FALSE upload, GLSL mat4 * vec4).
// clip = (x/t, y/t, A z + B, z) in the light's (right, up, forward) basis; depth A + B/z is in [0,1] over [kNear, far].
inline void lightMatrix(const Tile& tile, float m[16]) {
    float f[3];
    decodeAim(tile.packedType, f);
    float up[3] = {0.0f, 0.0f, 1.0f};
    if (std::fabs(f[2]) >= 0.95f) { up[0] = 1.0f; up[2] = 0.0f; }
    float r[3] = {f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0]};
    const float rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    r[0] /= rl; r[1] /= rl; r[2] /= rl;
    const float u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
    const float t = tanHalf(tile.packedType);
    const float F = tile.farDistance > kNear + 1.0f ? tile.farDistance : kNear + 1.0f;
    const float A = F / (F - kNear), B = -F * kNear / (F - kNear);
    const float* p = tile.pos;
    const float dr = r[0] * p[0] + r[1] * p[1] + r[2] * p[2];
    const float du = u[0] * p[0] + u[1] * p[1] + u[2] * p[2];
    const float df = f[0] * p[0] + f[1] * p[1] + f[2] * p[2];
    const float rows[4][4] = {
        {r[0] / t, r[1] / t, r[2] / t, -dr / t},
        {u[0] / t, u[1] / t, u[2] / t, -du / t},
        {A * f[0], A * f[1], A * f[2], -A * df + B},
        {f[0], f[1], f[2], -df}};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) m[c * 4 + row] = rows[row][c];
}

// ---- tile assignment (the pool producer selects; the shadow island reads) ----
inline Tile g_tiles[kTiles];

inline const Tile* currentTiles(uint32_t* count) {
    if (count) *count = kTiles;
    return g_tiles;
}

inline uint64_t tileKey(const float pos[3], uint32_t packed, float farDistance) {
    uint64_t h = 1469598103934665603ull;
    const auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ull; };
    for (int a = 0; a < 3; ++a) mix(static_cast<uint64_t>(static_cast<int64_t>(std::lround(pos[a] * 4.0f))));
    mix(packed & ~0xF0u);
    mix(static_cast<uint64_t>(std::lround(farDistance)));
    return h ? h : 1;
}

// Picks up to kTiles coned, non-mover pool lights nearest the camera (MC2 XY), keeps a light in the tile it held last frame
// (so its baked static casters stay valid) and writes tile+1 into bits 4-7 of the chosen records. `enabled` false clears
// every tile (no record then carries a tile). Light = gos_terrain_lighting::GpuTerrainLight.
template <class Light>
inline void selectTiles(Light* lights, uint32_t count, float camX, float camY, bool enabled) {
    struct Cand { uint32_t index; float d2; uint64_t key; int tile; };
    Cand best[kTiles];
    uint32_t nBest = 0;
    if (enabled) {
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t lt = lights[i].lightType;
            if ((lt & 0xFu) == 0u || (lt >> 24) == 0u || lights[i]._pad1 > 0.5f) continue;
            const float dx = lights[i].position[0] - camX, dy = lights[i].position[1] - camY;
            const Cand c{i, dx * dx + dy * dy, 0, -1};
            if (nBest < kTiles) { best[nBest++] = c; continue; }
            uint32_t worst = 0;
            for (uint32_t k = 1; k < nBest; ++k) if (best[k].d2 > best[worst].d2) worst = k;
            if (c.d2 < best[worst].d2) best[worst] = c;
        }
    }
    bool kept[kTiles] = {};
    for (uint32_t k = 0; k < nBest; ++k) {
        const Light& L = lights[best[k].index];
        best[k].key = tileKey(L.position, L.lightType, L.farDistance);
        for (uint32_t t = 0; t < kTiles; ++t)
            if (!kept[t] && g_tiles[t].used && g_tiles[t].key == best[k].key) { best[k].tile = static_cast<int>(t); kept[t] = true; break; }
    }
    for (uint32_t k = 0; k < nBest; ++k) {
        if (best[k].tile >= 0) continue;
        for (uint32_t t = 0; t < kTiles; ++t) {
            if (kept[t]) continue;
            const Light& L = lights[best[k].index];
            Tile& T = g_tiles[t];
            T.used = 1;
            T.pos[0] = L.position[0]; T.pos[1] = L.position[1]; T.pos[2] = L.position[2];
            T.packedType = L.lightType & ~0xF0u;
            T.farDistance = L.farDistance;
            T.key = best[k].key;
            best[k].tile = static_cast<int>(t);
            kept[t] = true;
            break;
        }
    }
    for (uint32_t t = 0; t < kTiles; ++t) if (!kept[t]) g_tiles[t].used = 0;
    for (uint32_t k = 0; k < nBest; ++k)
        if (best[k].tile >= 0)
            lights[best[k].index].lightType = (lights[best[k].index].lightType & ~0xF0u) | ((static_cast<uint32_t>(best[k].tile) + 1u) << 4);
}

} // namespace mc2localshadow
