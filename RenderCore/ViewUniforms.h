#pragma once
// CPU view/projection record published by the view registry and frame packets.
// The layout is also exercised by the offline shader-schema fixture; native
// renderers consume frame-feed matrices rather than a shared driver view UBO.
// Row-major storage; the GL suffix identifies the existing clip-space convention.

#include <cstddef>   // offsetof
#include <cstdint>   // uint32_t

namespace RenderCore {

// 144-byte per-frame view/projection record.
// std430-compatible; same layout in std140 for these types.
struct alignas(16) ViewUniforms {
    float worldToClipGL[16];   //   0 — world → GL clip (row-major, GL_FALSE upload)
    float worldToViewGL[16];   //  64 — world → view   (row-major, GL_FALSE upload)
    float cameraWorldPos[4];   // 128 — world-space camera position (w unused)
};

static_assert(sizeof(ViewUniforms) == 144,
              "ViewUniforms must be 144 bytes for clean std430 stride");
static_assert(offsetof(ViewUniforms, worldToClipGL) ==   0, "worldToClipGL offset");
static_assert(offsetof(ViewUniforms, worldToViewGL) ==  64, "worldToViewGL offset");
static_assert(offsetof(ViewUniforms, cameraWorldPos) == 128, "cameraWorldPos offset");

// Reserved binding for the offline view_uniforms_contract shader fixture only.
// There is no production view UBO or driver binding.
constexpr uint32_t kViewUniformsBinding = 3;

} // namespace RenderCore
