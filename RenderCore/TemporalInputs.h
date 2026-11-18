#pragma once
// Temporal reconstruction inputs (jitter, mip bias, motion-vector algebra).
// Pure math plus one consumer counter. With no consumer registered every function here
// returns the neutral value (jitter 0, mip bias log2(renderScale)), so the image is unchanged.
#include <atomic>
#include <cstddef>
#include <cmath>
#include <cstdint>

namespace RenderCore::temporal {

// Number of consumers (FSR resolve, debug views that need jittered input) that want jitter.
// Present registers here while an IUpscalerProvider resolve is active.
inline std::atomic<int>& consumerCount() {
    static std::atomic<int> n{0};
    return n;
}
inline bool consumerActive() { return consumerCount().load(std::memory_order_relaxed) > 0; }
// Jitter phase count the active provider asked for (IUpscalerProvider::jitterPhaseCount); 0 = the default formula.
inline std::atomic<uint32_t>& providerPhases() {
    static std::atomic<uint32_t> n{0};
    return n;
}

// Projection the scene camera last built (mclib Camera::setOrthogonal publishes it for the main camera; the upscaler
// glue copies it into UpscalerFrame). Distances positive in world units, fovY in radians, reverse-Z, finite far plane.
struct Projection {
    float nearZ = 1.0f, farZ = 20000.0f, fovY = 0.6f;
    bool perspective = true, reverseZ = true, infiniteFar = false;
    uint8_t padding = 0;
};
static_assert(sizeof(Projection) == 16 && offsetof(Projection, padding) == 15);
// Camera publication and the upscaler read need a coherent snapshot, not lock-free
// progress. Retain atomic load/store semantics; all 16 bytes are explicit members.
inline std::atomic<Projection>& projection() {
    static std::atomic<Projection> p{Projection{}};
    return p;
}

// Halton(base) in (0,1).
inline float halton(uint32_t index, uint32_t base) {
    float f = 1.0f, r = 0.0f;
    while (index > 0) {
        f /= static_cast<float>(base);
        r += f * static_cast<float>(index % base);
        index /= base;
    }
    return r;
}

// FSR guidance: phase count = 8 * (display / render)^2 (at least 8).
inline uint32_t phaseCount(uint32_t renderW, uint32_t displayW) {
    const float s = renderW ? static_cast<float>(displayW) / static_cast<float>(renderW) : 1.0f;
    const uint32_t n = static_cast<uint32_t>(std::ceil(8.0f * s * s));
    return n < 8u ? 8u : n;
}

// Sub-pixel offset in pixels, each component in [-0.5, 0.5), Halton(2,3) over `phases`.
inline void jitterPixels(uint32_t frame, uint32_t phases, float out[2]) {
    const uint32_t i = (phases ? frame % phases : 0u) + 1u;
    out[0] = halton(i, 2) - 0.5f;
    out[1] = halton(i, 3) - 0.5f;
}

// Viewport offset (pixels, +x right, +y down) the scene feeds apply for a render frame at 1:1 render/display.
// Zero unless a consumer is active, so every feed is unchanged with the provider off.
inline void sceneJitterPixels(uint32_t frame, float out[2]) {
    out[0] = out[1] = 0.0f;
    const uint32_t provider = providerPhases().load(std::memory_order_relaxed);
    if (consumerActive()) jitterPixels(frame, provider ? provider : phaseCount(1, 1), out);
}

// Clip-space offset for a render extent: 2 * jitterPx / extent (y follows the GL clip convention).
inline void jitterClip(const float px[2], uint32_t renderW, uint32_t renderH, float out[2]) {
    out[0] = renderW ? 2.0f * px[0] / static_cast<float>(renderW) : 0.0f;
    out[1] = renderH ? 2.0f * px[1] / static_cast<float>(renderH) : 0.0f;
}

// Jitter published for a frame: zero unless a consumer is active.
inline void frameJitterClip(uint32_t frame, uint32_t renderW, uint32_t renderH,
                            uint32_t displayW, float out[2]) {
    out[0] = out[1] = 0.0f;
    if (!consumerActive()) return;
    float px[2];
    const uint32_t provider = providerPhases().load(std::memory_order_relaxed);
    jitterPixels(frame, provider ? provider : phaseCount(renderW, displayW), px);
    jitterClip(px, renderW, renderH, out);
}

// Applies the offset to a row-major GL world->clip matrix (clip.xy += J * clip.w, i.e. rows 0/1
// gain J * row 3). Render-feed use only; the shared camera and CPU picking never see it.
inline void applyJitterRowMajor(float m[16], const float j[2]) {
    for (int c = 0; c < 4; ++c) {
        m[0 * 4 + c] += j[0] * m[3 * 4 + c];
        m[1 * 4 + c] += j[1] * m[3 * 4 + c];
    }
}

// Texture LOD bias. Neutral path is the existing log2(renderScale); a temporal provider flips to
// log2(render / display) - 1 together with its resolve (never alone).
inline float mipBias(float renderScale, bool providerResolve) {
    const float base = std::log2(renderScale);
    return providerResolve ? base - 1.0f : base;
}

// Effective material bias as the resolve state changes (FSR active / AA toggled / provider failure). `observe` is true
// when the bias differs from the last one seen (the first observation counts), so a sampler built at `last` is stale.
struct MipBiasState {
    float last = 0.0f;
    bool seen = false;
    bool observe(float now) {
        const bool changed = !seen || now != last;
        last = now; seen = true;
        return changed;
    }
};

// Unjittered motion of a pixel, mirrors motion_vectors.frag (unit-tested against the shader math).
// uv in [0,1]; depth reverse-Z; reproj = previousClip * inverse(currentClip), column-major.
inline void motionVector(const float reproj[16], const float uv[2], float depth,
                         const float jClip[2], float out[2]) {
    const float x = (uv[0] * 2.0f - 1.0f) - jClip[0];
    const float y = (uv[1] * 2.0f - 1.0f) - jClip[1];
    const float v[4] = {x, y, depth, 1.0f};
    float o[4];
    for (int r = 0; r < 4; ++r)
        o[r] = reproj[0 * 4 + r] * v[0] + reproj[1 * 4 + r] * v[1] + reproj[2 * 4 + r] * v[2] + reproj[3 * 4 + r] * v[3];
    out[0] = out[1] = 0.0f;
    if (o[3] <= 0.0f || depth == 0.0f) return;
    out[0] = 0.5f * o[0] / o[3] + 0.5f - (uv[0] - 0.5f * jClip[0]);
    out[1] = 0.5f * o[1] / o[3] + 0.5f - (uv[1] - 0.5f * jClip[1]);
}

}  // namespace RenderCore::temporal
