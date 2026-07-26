#pragma once
// gos_terrain_lighting.h — Phase 1: terrain lighting GPU compute public API.
//
// Design doc: docs/superpowers/specs/2026-05-10-quadsetuptextures-gpu-compute-port-design.md
// Plan:       docs/superpowers/plans/2026-05-10-quadsetuptextures-gpu-compute-port.md
//
// Stage 1: SSBO scaffold + 3-slot ring + dispatch. Output unused (no-op CopyResults).
// Stage 2: parity gate (shader math iteration).
// Stage 3: consumer switch (GPU authoritative, CPU block gated off).

#include <cstdint>
#include <cstddef>   // offsetof (struct-layout static_asserts)
#include <GL/glew.h>

// Forward-declare TerrainQuad (full definition in mclib/quad.h:59).
class TerrainQuad;

namespace gos_terrain_lighting {

// ---------------------------------------------------------------------------
// SSBO struct layout (design doc Q6) — lockstep with terrain_lighting_shared.hglsl
// Extending any struct here MUST extend the GLSL struct too
// (memory/cpp_glsl_ubo_struct_lockstep.md).
// ---------------------------------------------------------------------------

// Flag bits for GpuTerrainVertexInput.flags (Phase 1: bits 0-3, Phase 2: bits 4-7)
#define GPU_VERT_SHADOW          0x00000001u  // pVertex->shadow != 0
#define GPU_VERT_CALCFRAME_LIGHT 0x00000002u  // calcThisFrame & 1
#define GPU_VERT_BASE_COLOR_LIT  0x00000004u  // BaseVertexColor != 0
#define GPU_VERT_RAIN_DAMPEN     0x00000008u  // rainLightLevel < 1.0f
#define GPU_VERT_WATER           0x00000010u  // pVertex->water & 1
#define GPU_VERT_WATER_ANIM_NEG  0x00000020u  // water & 128
#define GPU_VERT_WATER_ANIM_POS  0x00000040u  // water & 64
#define GPU_VERT_CALCFRAME_WATER 0x00000080u  // calcThisFrame & 2

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
    float    _pad2;          // 4 B @ offset 44
};
static_assert(sizeof(GpuTerrainLight) == 48, "GpuTerrainLight must be 48 B std430");
static_assert(offsetof(GpuTerrainLight, color) == 16, "color vec3 must stay @16 — a field inserted before it breaks GLSL std430 lockstep");

struct alignas(4) GpuTerrainLightingOutput { //  8 B std430
    uint32_t lightRGB;       // packed BGRA per memory/mc2_argb_packing.md
    uint32_t fogRGB;
};
static_assert(sizeof(GpuTerrainLightingOutput) == 8, "GpuTerrainLightingOutput must be 8 B std430");

// ---------------------------------------------------------------------------
// Lifecycle (design doc Q5)
// ---------------------------------------------------------------------------

// Per-mission: allocate SSBOs sized to numVertices = realVerticesMapSide^2,
// compile shader on first call, reset ring.
// CRITICAL: pass realVerticesMapSide * realVerticesMapSide — NOT getNumVertices()
// (which returns 0 at mission_init call time per plan v2 CRITICAL-1).
// Call from code/mission.cpp:2788 alongside gpu_cull::compute_init().
void mission_init(uint32_t numVertices, uint32_t maxLights);

// Per-mission teardown: zero CPU state; keep GL allocs for reuse next mission.
// Call from Terrain::destroy (terrain.cpp:703).
void mission_shutdown();

// Per-frame: advance ring index. Call before PackAndDispatch.
void BeginFrame();

// Per-frame: pack input SSBO from vertex pool + camera lights, dispatch compute,
// issue memory barrier + glCopyBufferSubData → staging ring, place glFenceSync.
void PackAndDispatch();

// Per-frame: tryConsume ring (non-blocking T1/T2/T3), copy staging → vertices[i]->lightRGB.
// Stage 3: wired. Under parity mode: returns early (CPU body remains authoritative).
// Under authoritative mode: copies GPU output into vertex pool before setupTextures loop.
void CopyResultsToVertexPool(TerrainQuad* quadList, int numberQuads);

bool IsEnabled();
bool IsParityCheckEnabled();

// Parity API (design doc Q3) — Stage 2 implementation.
// Comparator walks quadList, filters by calcThisFrame & 1, indexes outputs[vertexNum].
void Parity_CompareFrame(TerrainQuad* quadList, int numberQuads,
                         const GpuTerrainLightingOutput* mappedOutput);

// Synchronously wait on current-frame fence and return mapped GPU output.
// ONLY valid in parity mode (MC2_TERRAIN_LIGHTING_PARITY=1).
// Never call this on the production hot path — GL_TIMEOUT_IGNORED blocks.
const GpuTerrainLightingOutput* GetMappedOutputForParity();

// Returns the GL buffer name of the per-vertex lighting output SSBO
// (lightRGB/fogRGB) written by the per-frame compute dispatch. Phase C
// compute shaders bind this at their input slot 1 to read lighting bytes
// directly, eliminating the CPU-mirror bounce that legacy pack loops
// require. Returns 0 if Phase 1 is disabled or not yet initialized;
// glBindBufferBase with buffer 0 unbinds the slot (well-defined per GL spec).
GLuint GetOutputSsbo();

// NIGHT-LIGHT-POOLS-1: expose the per-frame packed terrain LIGHT POOL SSBO
// (GpuTerrainLight[48B], lights [2..N-1] — sun/ambient excluded, uploaded every
// frame by PackAndDispatch) so a forward consumer (terrain_lod_chunk.frag) can
// re-run the stock per-light falloff per-fragment and restore night up-lighting
// (building spotlights / light props / mech-vehicle-weapon lights pooling onto
// terrain + cement). Returns 0 when Phase 1 is disabled or not initialized.
GLuint GetLightPoolSsbo();

// Number of lights packed into the pool SSBO by the LAST PackAndDispatch
// (0 when disabled / no eye / no registered pool lights this frame).
uint32_t GetLightPoolLightCount();

// LIGHT-CLUSTER-PERF-1 (2026-07-25): world-space XY light grid over the packed
// pool. Consumers bind headerSsbo/indexSsbo and upload dim/minXY/invCell, then
// loop only over the lights binned into the fragment's cell instead of the whole
// pool. Exact, not approximate: a light is stamped into every cell its ground
// circle (radius = UNSCALED farDistance) touches, and the shader only ever
// shrinks that reach — so the per-cell list is a conservative superset of the
// lights that can contribute, and the lit result is bit-identical to the linear
// full-pool loop.
//
// Returns false when the grid is not in play (MC2_LIGHT_GRID=0, pool too small,
// degenerate bbox, or nothing packed this frame); consumers must then fall back
// to the linear loop over GetLightPoolLightCount() lights.
struct LightGridParams {
    uint32_t dim;          // cells per axis (grid is dim x dim)
    float    minXY[2];     // world XY of cell (0,0) corner
    float    invCell[2];   // 1 / cell size, per axis
    GLuint   headerSsbo;   // uvec2 {firstIndex, count} per cell
    GLuint   indexSsbo;    // uint light indices, cell-major
};
bool GetLightGridParams(LightGridParams& out);

// POOL-CONSUMERS-1 (batch-6 item 6): bind the pool SSBO at binding 28 and
// upload the shared pool uniforms (u_numPoolLights + soft/dim/scale/blend/
// compress knobs, lockstep with the terrain-chunk consumer constants) onto
// `program` (must be the CURRENTLY BOUND program). Object consumers
// (static-prop batcher, mech batcher) call this per draw-batch setup.
// Uploads u_numPoolLights=0 (loop never entered, byte-identical) when the
// umbrella (MC2_TERRAIN_LIGHT_POOLS) or the object sub-gate
// (MC2_LIGHT_POOL_OBJECTS) is off, or no lights are packed this frame.
void UploadObjectLightPoolUniforms(unsigned int program);

} // namespace gos_terrain_lighting
