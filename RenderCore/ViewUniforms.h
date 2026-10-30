#pragma once
// CPU view/projection record published by the view registry and frame packets.
// Native renderers consume frame-feed matrices rather than a shared driver view UBO.
// Row-major storage; the GL suffix identifies the existing clip-space convention.

#include <cstddef>   // offsetof
#include <cstdint>   // uint32_t

namespace RenderCore {

// 224-byte per-frame view/projection record.
// std430-compatible; same layout in std140 for these types.
struct alignas(16) ViewUniforms {
    float worldToClipGL[16];   //   0 — world → GL clip (row-major, GL_FALSE upload)
    float worldToViewGL[16];   //  64 — world → view   (row-major, GL_FALSE upload)
    float cameraWorldPos[4];   // 128 — world-space camera position (w unused)
    float previousWorldToClipGL[16]; // 144 — previous main-view projection, same convention
    float jitterNdc[4];        // 208 — current XY clip offset; zero for shipped rendering
};

// Render copies only: camera projection and picking retain their unjittered matrices.
inline float halton(uint32_t index, uint32_t base) {
    float value = 0.0f, weight = 1.0f;
    while (index) {
        weight /= float(base);
        value += weight * float(index % base);
        index /= base;
    }
    return value;
}
inline void applyViewJitter(float* matrix, const float* jitter) {
    if (jitter[0] == 0.0f && jitter[1] == 0.0f) return;
    for (int c = 0; c < 4; ++c) {
        matrix[c * 4] += jitter[0] * matrix[c * 4 + 3];
        matrix[c * 4 + 1] += jitter[1] * matrix[c * 4 + 3];
    }
}

static_assert(sizeof(ViewUniforms) == 224,
              "ViewUniforms must be 224 bytes for clean std430 stride");
static_assert(offsetof(ViewUniforms, worldToClipGL) ==   0, "worldToClipGL offset");
static_assert(offsetof(ViewUniforms, worldToViewGL) ==  64, "worldToViewGL offset");
static_assert(offsetof(ViewUniforms, cameraWorldPos) == 128, "cameraWorldPos offset");
static_assert(offsetof(ViewUniforms, previousWorldToClipGL) == 144, "previousWorldToClipGL offset");
static_assert(offsetof(ViewUniforms, jitterNdc) == 208, "jitterNdc offset");

} // namespace RenderCore
