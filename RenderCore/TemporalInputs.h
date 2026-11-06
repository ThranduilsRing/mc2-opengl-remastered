#pragma once
// Temporal reconstruction inputs (docs/plans/2026-11-06-temporal-aa.md, slice B).
// Pure math plus one consumer counter. With no consumer registered every function here
// returns the neutral value (jitter 0, mip bias log2(renderScale)), so the image is unchanged.
#include <atomic>
#include <cmath>
#include <cstdint>

namespace RenderCore::temporal {

// Number of consumers (FSR resolve, debug views that need jittered input) that want jitter.
// Slice B has none; slice C's provider registers here.
inline std::atomic<int>& consumerCount() {
    static std::atomic<int> n{0};
    return n;
}
inline bool consumerActive() { return consumerCount().load(std::memory_order_relaxed) > 0; }

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
    jitterPixels(frame, phaseCount(renderW, displayW), px);
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
