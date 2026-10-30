#pragma once
// CPU view/projection record published by the view registry and frame packets.
// Native renderers consume frame-feed matrices rather than a shared driver view UBO.
// Row-major storage; the GL suffix identifies the existing clip-space convention.

#include <cstddef>   // offsetof
#include <cstdint>   // uint32_t

namespace RenderCore {

// 208-byte per-frame view/projection record.
// std430-compatible; same layout in std140 for these types.
struct alignas(16) ViewUniforms {
    float worldToClipGL[16];   //   0 — world → GL clip (row-major, GL_FALSE upload)
    float worldToViewGL[16];   //  64 — world → view   (row-major, GL_FALSE upload)
    float cameraWorldPos[4];   // 128 — world-space camera position (w unused)
    float previousWorldToClipGL[16]; // 144 — previous main-view projection, same convention
};

static_assert(sizeof(ViewUniforms) == 208,
              "ViewUniforms must be 208 bytes for clean std430 stride");
static_assert(offsetof(ViewUniforms, worldToClipGL) ==   0, "worldToClipGL offset");
static_assert(offsetof(ViewUniforms, worldToViewGL) ==  64, "worldToViewGL offset");
static_assert(offsetof(ViewUniforms, cameraWorldPos) == 128, "cameraWorldPos offset");
static_assert(offsetof(ViewUniforms, previousWorldToClipGL) == 144, "previousWorldToClipGL offset");

} // namespace RenderCore
