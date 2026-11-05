// RenderCore/ImpactDecalAtlas.h -- procedural authoring of the impact decal atlas (2D-array layers, one per impact::Tile).
//
// WHAT: paints every response tile from a height field plus albedo/emissive/char/coverage masks, then derives the tangent-space
// normal from the height. Two RGBA8 layer stacks come out:
//   surface:  R = albedo multiplier * 0.5 (0.5 = unchanged, up to 2x for bare metal), G = emissive mask, B = char tint amount,
//             A = coverage
//   normal:   RG = tangent normal xy * 0.5 + 0.5, B = height * 0.5 + 0.5, A = roughness (reserved for the lighting lane)
// WHY: a portable library (weapon class x material) instead of per-building damage art; procedural so it ships with no
// asset dependency, mods cannot break it, and the result is deterministic (the unit tier hashes it). Clean-room: authored from
// the shapes of real damage (punctures, melt grooves, breaches, craters), no third-party textures.
#pragma once

#include "ImpactDecals.h"
#include "../3rdparty/fastnoiselite/FastNoiseLite.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <vector>

namespace impact::atlas {

inline constexpr uint32_t kSize = 256;   // per-layer edge; 8 layers x 2 stacks x 256 KB, plus mips

namespace detail {
inline float smooth(float t) { return t * t * (3.0f - 2.0f * t); }
inline float vnoise(float x, float y, uint32_t salt) {
    // Keep cubic value interpolation, unit lattice and [0,1] masks; only the hash pattern changes.
    static thread_local FastNoiseLite noise = [] {
        FastNoiseLite n;
        n.SetNoiseType(FastNoiseLite::NoiseType_Value);
        n.SetFrequency(1.0f);
        return n;
    }();
    noise.SetSeed(std::bit_cast<int32_t>(salt));
    return noise.GetNoise(x, y) * 0.5f + 0.5f;
}
inline float fbm(float x, float y, uint32_t salt, int oct = 4) {
    float s = 0.0f, amp = 0.5f, f = 1.0f;
    for (int i = 0; i < oct; ++i) { s += amp * vnoise(x * f, y * f, salt + static_cast<uint32_t>(i) * 977u); f *= 2.03f; amp *= 0.5f; }
    return s;   // ~[0,1]
}
inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
inline float sstep(float e0, float e1, float v) { return smooth(clamp01((v - e0) / (e1 - e0))); }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

// One texel of a response: everything is a function of uv in [-1,1]^2.
struct Texel { float height = 0.0f, albedo = 1.0f, emissive = 0.0f, charAmt = 0.0f, coverage = 0.0f; };

inline float angleNoise(float theta, float freq, uint32_t salt) {
    // Periodic noise around a circle: sample value noise on a ring so theta=-pi and pi agree.
    return vnoise(std::cos(theta) * freq + 7.0f, std::sin(theta) * freq + 3.0f, salt);
}

// Puncture: hole, petals bent toward the exit side, bright bare-metal ring, soot halo.
inline void hole(Texel& t, float u, float v, float cx, float cy, float r, uint32_t salt) {
    const float dx = u - cx, dy = v - cy, d = std::sqrt(dx * dx + dy * dy), th = std::atan2(dy, dx);
    const float rr = r * (0.85f + 0.35f * angleNoise(th, 2.5f, salt));
    if (d < rr) { t.height = -1.0f; t.albedo = 0.03f; t.coverage = 1.0f; t.emissive = std::max(t.emissive, 0.35f * sstep(rr * 0.6f, rr, d)); return; }
    const float petals = 0.5f + 0.5f * std::cos(th * 7.0f + 6.0f * angleNoise(th, 1.5f, salt + 5u));
    const float ring = 1.0f - sstep(rr, rr * 1.7f, d);
    t.height = std::max(t.height, 0.75f * ring * (0.4f + 0.6f * petals));
    t.albedo = std::min(t.albedo, lerp(1.0f, 0.55f, 1.0f - sstep(rr, rr * 2.6f, d)));
    if (d < rr * 1.35f) t.albedo = std::max(t.albedo, 1.45f - 0.4f * petals);     // bare metal lip
    t.emissive = std::max(t.emissive, 1.0f - sstep(rr, rr * 1.25f, d));
    t.charAmt = std::max(t.charAmt, 1.0f - sstep(rr * 1.2f, rr * 2.4f, d));
    t.coverage = std::max(t.coverage, 1.0f - sstep(rr * 1.6f, rr * 2.8f, d));
}

inline Texel paint(Tile tile, float u, float v) {
    Texel t;
    const float d = std::sqrt(u * u + v * v), th = std::atan2(v, u);
    const uint32_t salt = static_cast<uint32_t>(tile) * 1013u + 17u;
    const float n = fbm(u * 3.0f + 11.0f, v * 3.0f + 5.0f, salt);
    const float nf = fbm(u * 9.0f, v * 9.0f, salt + 101u);
    switch (tile) {
    case Tile::BulletMetal:
        hole(t, u, v, 0.0f, 0.0f, 0.17f, salt);
        hole(t, u, v, 0.42f, -0.25f, 0.11f, salt + 1u);
        hole(t, u, v, -0.33f, 0.34f, 0.09f, salt + 2u);
        hole(t, u, v, -0.38f, -0.42f, 0.06f, salt + 3u);
        if (t.albedo > 0.05f) t.coverage *= 0.75f + 0.25f * n;   // ragged soot, solid holes
        break;
    case Tile::BulletConcrete: {
        const float rr = 0.42f * (0.8f + 0.4f * angleNoise(th, 2.0f, salt));
        const float cracks = std::pow(std::fabs(std::cos(th * 3.5f + 4.0f * n)), 40.0f) * (1.0f - sstep(0.3f, 0.9f, d));
        if (d < rr) {
            const float q = d / rr;
            t.height = -0.9f * (1.0f - q * q) + 0.25f * (nf - 0.5f);
            t.albedo = (nf > 0.58f ? 0.35f : 1.15f) * lerp(0.35f, 1.0f, q);   // fresh aggregate, dark pits, dark core
            t.coverage = 1.0f;
        } else {
            t.albedo = lerp(0.7f, 1.0f, sstep(rr, 0.9f, d + 0.2f * (n - 0.5f)));   // powder-burn halo
            t.coverage = 1.0f - sstep(rr, 0.92f, d + 0.15f * n);
        }
        t.albedo = lerp(t.albedo, 0.45f, cracks);
        t.height -= 0.4f * cracks;
        t.coverage = std::max(t.coverage, cracks);
        break;
    }
    case Tile::LaserMetal:
    case Tile::LaserConcrete: {
        const bool metal = tile == Tile::LaserMetal;
        const float along = std::fabs(u);
        const float wobble = 0.05f * (vnoise(u * 6.0f, 1.0f, salt) - 0.5f);
        const float halfW = (metal ? 0.2f : 0.15f) * std::sqrt(std::max(0.0f, 1.0f - along * along * 1.1f)) + 0.02f;
        const float dv = std::fabs(v - wobble);
        const float groove = 1.0f - sstep(halfW * 0.6f, halfW, dv);
        const float lip = sstep(halfW * 0.7f, halfW, dv) * (1.0f - sstep(halfW, halfW * 1.6f, dv));
        const float band = 1.0f - sstep(halfW, halfW * 3.2f + 0.08f * n, dv);
        t.height = -0.7f * groove + (metal ? 0.55f : 0.25f) * lip;
        const float beads = nf > 0.6f ? 1.0f : 0.0f;
        t.albedo = metal ? lerp(lerp(1.0f, 0.62f, band), lerp(0.3f, 1.25f, beads), groove)
                         : lerp(lerp(1.0f, 0.55f, band), 0.22f + 0.15f * nf, groove);
        t.emissive = groove * (1.0f - sstep(halfW * 0.2f, halfW * 0.9f, dv)) * (0.7f + 0.3f * nf);
        t.charAmt = band;
        if (!metal) {
            const float cross = std::pow(std::fabs(std::sin(u * 21.0f + 3.0f * n)), 30.0f) * band;
            t.albedo = lerp(t.albedo, 0.4f, cross); t.height -= 0.3f * cross;
        }
        t.coverage = band * (1.0f - sstep(0.8f, 1.0f, along));
        break;
    }
    case Tile::BlastMetal: {
        const float rr = 0.3f * (0.8f + 0.45f * angleNoise(th, 3.0f, salt));
        const float petals = 0.5f + 0.5f * std::cos(th * 9.0f + 5.0f * angleNoise(th, 2.0f, salt + 9u));
        if (d < rr) { t.height = -1.0f; t.albedo = 0.02f; t.coverage = 1.0f; t.emissive = 0.8f * sstep(rr * 0.8f, rr, d); break; }
        const float petalZone = 1.0f - sstep(rr, rr + 0.25f * (0.5f + petals), d);
        t.height = 0.85f * petalZone * (0.3f + 0.7f * petals);
        const float soot = 1.0f - sstep(rr, 0.95f, d + 0.25f * (n - 0.5f));
        t.albedo = lerp(1.0f, 0.3f + 0.25f * nf, soot);
        if (petalZone > 0.5f) t.albedo = lerp(t.albedo, 1.25f, 0.5f * petals);     // torn bright edges
        t.emissive = 1.0f - sstep(rr, rr + 0.07f, d);
        t.charAmt = soot;
        t.coverage = 1.0f - sstep(0.7f, 0.98f, d + 0.2f * (n - 0.5f));
        break;
    }
    case Tile::BlastGround: {
        const float rr = 0.5f * (0.9f + 0.2f * angleNoise(th, 2.0f, salt));
        float h;
        if (d < rr) { const float q = d / rr; h = -0.95f * (1.0f - q * q); }
        else h = 0.55f * (1.0f - sstep(rr, rr + 0.25f, d)) * sstep(rr - 0.02f, rr + 0.04f, d);
        const float rays = std::pow(0.5f + 0.5f * std::cos(th * 13.0f + 6.0f * angleNoise(th, 2.5f, salt + 3u)), 6.0f) *
                           (1.0f - sstep(rr, 0.98f, d));
        t.height = h + 0.2f * (nf - 0.5f) * (d < rr + 0.2f ? 1.0f : 0.3f);
        const float soot = 1.0f - sstep(0.15f, 0.9f, d + 0.2f * (n - 0.5f));
        t.albedo = lerp(1.0f, 0.22f + 0.2f * nf, soot);
        t.albedo = lerp(t.albedo, 0.5f, 0.6f * rays);
        t.emissive = 1.0f - sstep(0.0f, 0.22f, d);
        t.charAmt = soot;
        t.coverage = std::max(1.0f - sstep(0.6f, 0.97f, d + 0.15f * (n - 0.5f)), rays);
        break;
    }
    case Tile::PpcScorch: {
        float lines = 0.0f;
        for (int k = 0; k < 3; ++k) {
            const float f = 5.0f + 2.0f * static_cast<float>(k);
            const float w = std::fabs(std::sin(th * f + 7.0f * fbm(d * 2.5f, static_cast<float>(k), salt + 50u)));
            lines = std::max(lines, std::pow(1.0f - w, 28.0f) * (1.0f - sstep(0.25f + 0.2f * static_cast<float>(k), 0.95f, d)));
        }
        const float core = 1.0f - sstep(0.08f, 0.32f, d + 0.1f * (n - 0.5f));
        t.height = -0.25f * core - 0.15f * lines;
        t.albedo = lerp(lerp(1.0f, 0.7f, 1.0f - sstep(0.2f, 0.85f, d)), 0.25f, std::max(core, lines));
        t.emissive = std::max(lines, 0.8f * core);
        t.charAmt = std::max(core, 0.5f * lines);
        t.coverage = std::max(1.0f - sstep(0.55f, 0.95f, d + 0.2f * (n - 0.5f)), lines);
        break;
    }
    case Tile::SplashScorch:
    default: {
        const float blot = 1.0f - sstep(0.1f, 0.95f, d + 0.35f * (n - 0.5f));
        t.albedo = lerp(1.0f, 0.3f + 0.25f * nf, blot);
        t.charAmt = blot;
        t.coverage = blot;
        break;
    }
    }
    // Every tile fades to nothing before its border so the box edge never shows.
    t.coverage *= 1.0f - sstep(0.88f, 0.985f, std::max(std::fabs(u), std::fabs(v)));
    t.coverage = clamp01(t.coverage);
    return t;
}

inline uint8_t u8(float v) { return static_cast<uint8_t>(std::lround(clamp01(v) * 255.0f)); }
}  // namespace detail

struct Layers {
    std::vector<uint8_t> surface;   // kTileCount layers of kSize^2 RGBA8
    std::vector<uint8_t> normal;
};

// Deterministic within a build: integer hashing and no state, so every call returns the same bytes (presentation only, so
// cross-CRT differences in the transcendental functions are harmless).
inline Layers generate() {
    Layers out;
    const size_t layerBytes = static_cast<size_t>(kSize) * kSize * 4;
    out.surface.resize(layerBytes * kTileCount);
    out.normal.resize(layerBytes * kTileCount);
    std::vector<float> height(static_cast<size_t>(kSize) * kSize);
    for (uint32_t layer = 0; layer < kTileCount; ++layer) {
        const Tile tile = static_cast<Tile>(layer);
        uint8_t* s = out.surface.data() + layer * layerBytes;
        uint8_t* nm = out.normal.data() + layer * layerBytes;
        for (uint32_t y = 0; y < kSize; ++y) {
            for (uint32_t x = 0; x < kSize; ++x) {
                const float u = (static_cast<float>(x) + 0.5f) / kSize * 2.0f - 1.0f;
                const float v = (static_cast<float>(y) + 0.5f) / kSize * 2.0f - 1.0f;
                const detail::Texel t = detail::paint(tile, u, v);
                const size_t i = (static_cast<size_t>(y) * kSize + x);
                height[i] = t.height * t.coverage;
                s[i * 4 + 0] = detail::u8(t.albedo * 0.5f);
                s[i * 4 + 1] = detail::u8(t.emissive * t.coverage);
                s[i * 4 + 2] = detail::u8(t.charAmt);
                s[i * 4 + 3] = detail::u8(t.coverage);
            }
        }
        // Tangent normal from the height field; the gain is 'how deep' a unit height reads across one tile.
        const float gain = 6.0f;
        for (uint32_t y = 0; y < kSize; ++y) {
            for (uint32_t x = 0; x < kSize; ++x) {
                auto H = [&](int xx, int yy) {
                    xx = xx < 0 ? 0 : (xx >= static_cast<int>(kSize) ? static_cast<int>(kSize) - 1 : xx);
                    yy = yy < 0 ? 0 : (yy >= static_cast<int>(kSize) ? static_cast<int>(kSize) - 1 : yy);
                    return height[static_cast<size_t>(yy) * kSize + static_cast<size_t>(xx)];
                };
                const float dhx = (H(static_cast<int>(x) + 1, static_cast<int>(y)) - H(static_cast<int>(x) - 1, static_cast<int>(y))) * 0.5f * gain;
                const float dhy = (H(static_cast<int>(x), static_cast<int>(y) + 1) - H(static_cast<int>(x), static_cast<int>(y) - 1)) * 0.5f * gain;
                float nx = -dhx, ny = -dhy, nz = 1.0f;
                const float l = std::sqrt(nx * nx + ny * ny + nz * nz);
                nx /= l; ny /= l;
                const size_t i = static_cast<size_t>(y) * kSize + x;
                nm[i * 4 + 0] = detail::u8(nx * 0.5f + 0.5f);
                nm[i * 4 + 1] = detail::u8(ny * 0.5f + 0.5f);
                nm[i * 4 + 2] = detail::u8(height[i] * 0.5f + 0.5f);
                nm[i * 4 + 3] = detail::u8(0.6f);
            }
        }
    }
    return out;
}

}  // namespace impact::atlas
