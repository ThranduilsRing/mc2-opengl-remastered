#pragma once
// CPU input contract for native Vulkan terrain lighting and shared light-pool bindings.

#include <cstdint>
#include <cstddef>   // offsetof (struct-layout static_asserts)

// Forward-declare TerrainQuad (full definition in mclib/quad.h:59).
class TerrainQuad;

namespace gos_terrain_lighting {

// ---------------------------------------------------------------------------
// SSBO struct layout (design doc Q6) — lockstep with terrain_lighting_shared.hglsl
// Extending any struct here MUST extend the GLSL struct too
// (memory/cpp_glsl_ubo_struct_lockstep.md).
// ---------------------------------------------------------------------------

// Flag bits for GpuTerrainVertexInput.flags (Phase 1: bits 0-3, Phase 2: bits 4-7)
inline constexpr uint32_t GPU_VERT_SHADOW          = 0x00000001u;  // pVertex->shadow != 0
inline constexpr uint32_t GPU_VERT_CALCFRAME_LIGHT = 0x00000002u;  // calcThisFrame & 1
inline constexpr uint32_t GPU_VERT_BASE_COLOR_LIT  = 0x00000004u;  // BaseVertexColor != 0
inline constexpr uint32_t GPU_VERT_RAIN_DAMPEN     = 0x00000008u;  // rainLightLevel < 1.0f
inline constexpr uint32_t GPU_VERT_WATER           = 0x00000010u;  // pVertex->water & 1
inline constexpr uint32_t GPU_VERT_WATER_ANIM_NEG  = 0x00000020u;  // water & 128
inline constexpr uint32_t GPU_VERT_WATER_ANIM_POS  = 0x00000040u;  // water & 64
inline constexpr uint32_t GPU_VERT_CALCFRAME_WATER = 0x00000080u;  // calcThisFrame & 2

struct alignas(16) GpuTerrainVertexInput {   // 32 B std430
    float    xy[2];          // 8 B @ offset 0
    float    elevation;      // 4 B @ offset 8
    float    hazeFactor;     // 4 B @ offset 12  DEAD post-Step-7 (haze computed inline in
                             //   gos_terrain_lighting.comp from worldPos; populate write
                             //   neutralized to 0.0f). RETAINED for std430 stride lockstep;
                             //   alignas(16) pads this 4 B regardless so static_assert(==32)
                             //   stays valid. Removal deferred to Step 10 (cpp_glsl_ubo_struct_lockstep.md).
    float    normal[3];      // 12 B @ offset 16
    uint32_t flags;          // 4 B @ offset 28
};
static_assert(sizeof(GpuTerrainVertexInput) == 32, "GpuTerrainVertexInput must be 32 B std430");
static_assert(offsetof(GpuTerrainVertexInput, normal) == 16, "normal vec3 must stay @16 — a field inserted before it breaks GLSL std430 lockstep");

struct alignas(16) GpuTerrainLight {         // 48 B std430 (Stage 2 extended)
    float    position[3];    // 12 B @ offset 0
    uint32_t lightType;      // 4 B @ offset 12
    float    color[3];       // 12 B @ offset 16  (R/G/B normalized 0..1)
    float    closeDistance;  // 4 B @ offset 28   (TG_Light::closeDistance)
    float    farDistance;    // 4 B @ offset 32   (TG_Light::farDistance)
    float    oneOverDistance;// 4 B @ offset 36   (TG_Light::oneOverDistance)
    float    _pad1;          // 4 B @ offset 40
    float    intensity;      // 4 B @ offset 44; applied once by each pool consumer
};
static_assert(sizeof(GpuTerrainLight) == 48, "GpuTerrainLight must be 48 B std430");
static_assert(offsetof(GpuTerrainLight, intensity) == 44, "pool intensity must stay @44");
static_assert(offsetof(GpuTerrainLight, color) == 16, "color vec3 must stay @16 — a field inserted before it breaks GLSL std430 lockstep");

struct alignas(4) GpuTerrainLightingOutput { //  8 B std430
    uint32_t lightRGB;       // packed BGRA per memory/mc2_argb_packing.md
    uint32_t fogRGB;
};
static_assert(sizeof(GpuTerrainLightingOutput) == 8, "GpuTerrainLightingOutput must be 8 B std430");

// ---------------------------------------------------------------------------
// Lifecycle (design doc Q5)
// ---------------------------------------------------------------------------

// Per-mission: size CPU input snapshots to numVertices = realVerticesMapSide^2.
// CRITICAL: pass realVerticesMapSide * realVerticesMapSide — NOT getNumVertices()
// (which returns 0 at mission_init call time per plan v2 CRITICAL-1).
// Call from code/mission.cpp:2788 alongside gpu_cull::compute_init().
void mission_init(uint32_t numVertices, uint32_t maxLights);

// Per-mission teardown: invalidate CPU snapshots and light-grid readiness.
// Call from Terrain::destroy (terrain.cpp:703).
void mission_shutdown();

// Per-frame: advance ring index. Call before PackAndDispatch.
void BeginFrame();

// Per-frame: pack the vertex window, camera lights and exact spatial light grid.
void PackAndDispatch();

bool IsEnabled();

// Number of lights packed into the pool SSBO by the LAST PackAndDispatch
// (0 when disabled / no eye / no registered pool lights this frame).
uint32_t GetLightPoolLightCount();

// CPU-owned views for native consumers. Pointers stay valid until the next
// PackAndDispatch / mission_shutdown.
// GetLightPoolCpu: returns the packed pool (count = GetLightPoolLightCount()),
// nullptr when not initialized.
const GpuTerrainLight* GetLightPoolCpu(uint32_t* outCount);
// GetLightGridCpu: false when the grid is disabled, empty or uninitialized.
struct LightGridCpu {
    uint32_t        dim;
    float           minXY[2];
    float           invCell[2];
    const uint32_t* header;       // 2 * dim * dim words ({first,count} per cell)
    uint32_t        headerWords;
    const uint32_t* indices;
    uint32_t        indexWords;
};
bool GetLightGridCpu(LightGridCpu& out);

struct ObjectLightPoolParams {
    int   count;
    int   soft;
    float moverDim;
    float moverScale;
    int   moverMax;
    int   compress;
    float objScale;
    const GpuTerrainLight* pool;
};
void GetObjectLightPoolParams(ObjectLightPoolParams* out);

// STEP 3.4P: the CPU-side inputs of THIS frame's PackAndDispatch (the exact arrays and scalars the GL compute was given), so the native Vulkan
// terrain-lighting compute (vk_native_producers) runs the same math on the same data. Pointers stay valid until the next PackAndDispatch /
// mission_shutdown. `window` points at vertex number `vertexOffset` (element i is vertex vertexOffset + i); the VK output is indexed by absolute
// vertex number like the GL output SSBO.
struct VkProducerSnapshot {
    uint32_t numVertices;            // realVerticesMapSide^2 (absolute bound)
    uint32_t vertexOffset;           // vertex number of window[0]
    uint32_t windowCount;            // vertices packed this frame (0 = nothing to light)
    const GpuTerrainVertexInput* window;
    uint32_t numLights;
    const GpuTerrainLight* lights;
    int      lightRgb[3];            // eye->lightRed/Green/Blue
    int      ambientRgb[3];          // eye->ambientRed/Green/Blue
    int      baseRgb[3];             // BaseVertexColor R,G,B
    int      lightening;             // TerrainQuad::lighteningLevel
    int      useFog;
    int      rendererSW;
    float    sunDir[3];
    float    rain;
    float    fogStart, fogFull;
    float    minHaze, maxClip, distFactor;
    float    playArea[4];
    float    camPos[3];
};
// False until this frame's CPU pack has published its inputs.
bool GetVkProducerSnapshot(VkProducerSnapshot& out);

} // namespace gos_terrain_lighting
