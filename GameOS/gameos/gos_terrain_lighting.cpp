// gos_terrain_lighting.cpp — Phase 1 Stage 3: terrain lighting GPU compute — GPU authoritative.
//
// Design doc: docs/superpowers/specs/2026-05-10-quadsetuptextures-gpu-compute-port-design.md (ac7c492)
// Plan:       docs/superpowers/plans/2026-05-10-quadsetuptextures-gpu-compute-port.md (2a5ba54)
//
// Stage 3: GPU path authoritative.
//   CopyResultsToVertexPool: 3-slot T1/T2/T3 non-blocking tryConsume; copies
//     GPU lightRGB/fogRGB into vertices[i] before setupTextures loop.
//   CPU lighting block in quad.cpp:1266-1891 gated off via s_lightingGpuAuth.
//   CostSplitLightingScope bracket retained as retirement telemetry (~0 us post-flip).
//   Parity mode: CopyResultsToVertexPool returns early; CPU body still authoritative.
//   MC2_TERRAIN_LIGHTING_GPU=0 to force legacy CPU path (default-on as of
//   Stage 5 flip 2026-05-11; any other value, including unset, opts in).
//
// 3-slot ring pattern mirrors gpu_cull_readback.cpp (RING_FRAMES=3, dual-buffer,
// glCopyBufferSubData VRAM→BAR, timeout=0 always on hot path).
//
// tl_ prefix (terrain_lighting) on private compile helpers to avoid ODR conflicts
// with identical statics in gpu_cull_compute.cpp (design doc Q1).

#include "gos_terrain_lighting.h"
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1
#include "utils/mc2log.h"       // LOGGING-FACADE-P2: MC2_EVENT for the structured [TAG vN] lines
#include "Gates/gate.h"          // typed gate registry: gate::get<"MC2_X">()

#include "gos_profiler.h"  // ZoneScopedN, TracyGpuZone

#include <GL/glew.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>   // LIGHT-CLUSTER-PERF-1: std::fill over the grid cursor
#include <cstdint>
#include <unordered_map>   // GHOST-LIGHT-DUMP-1: light-ptr -> world slot

#include "../../mclib/terrain.h"    // Terrain::realVerticesMapSide, vertexList, numberVertices
#include "../../mclib/vertex.h"     // Vertex, PostcompVertex
#include "../../mclib/camera.h"     // CameraPtr eye, getTerrainLight(), lightDirection, fogStart/fogFull
#include "../../mclib/tgl.h"        // TG_Light, TG_LIGHT_* constants
#include "../../mclib/spotlight_diag.h"  // NIGHT-LIGHT-POOLS-2a: mover-light slot tags
#include "../../mclib/quad.h"       // TerrainQuad, TerrainQuad::rainLightLevel, ::lighteningLevel
#include "gameos.hpp"  // gosEnvironment Environment (Environment.Renderer)

#include "../../RenderCore/RenderResourceRegistry.h"  // TERRAIN-LIGHTING-SSBO-OWNER-1: registry + ids
#include "../../RenderCore/GpuBufferOwner.h"           // TERRAIN-LIGHTING-SSBO-OWNER-1: owner records

// External globals needed for packing
extern DWORD BaseVertexColor;  // defined in code/mechcmd2.cpp / code/logmain.cpp
extern bool  useFog;           // defined in mclib/terrain.cpp

// Step 7 (VPL retirement): MC2_HAZE_PARITY probe — env-gated default-off.
// Compares the inline-worldPos hazeFactor formula (CPU-recomputed from
// v.vx/v.vy/v.elevation + swizzled camera pos + Camera:: constants) against the
// still-live VPL-written v.hazeFactor (Step 7 KEEPS the VPL writes; deletion is
// Step 8c). Deltas ~0 stock: binary clamp (MinHaze==MaxClip==50000) on both
// sides; GetApproximateLength->length() only diverges if setFarClipDistance
// reshapes the haze band. Retire in Step 8c.
static const bool s_hazeParity = mc2::gate::get<"MC2_HAZE_PARITY">();
static double  s_hazeParityMaxAbsDelta = 0.0;
static uint64_t s_hazeParityViolations = 0;
static uint64_t s_hazeParitySummaryFrames = 0;

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------
namespace gos_terrain_lighting {

// Phase 1 Stage 5 — DEFAULT-ON flip (2026-05-11).
//
// Bisection guide for future lighting issues:
//   - Set MC2_TERRAIN_LIGHTING_GPU=0 in env to force the legacy CPU lighting
//     path (quad.cpp:1267-1890 runs; GPU pipeline dormant).
//   - Set MC2_TERRAIN_LIGHTING_PARITY=1 to force dual-run mode: CPU body
//     authoritative + GPU compute compared per-vertex; mismatches print to
//     stderr as [TERRAIN_LIGHTING_PARITY v1] event=mismatch ...
//   - Killswitch-off (env="0") was bit-identical to pre-Phase-1 at flip time;
//     any visual regression that disappears with env="0" is THIS slice.
//   - Killswitch-off that still shows the regression is unrelated.
//
// Stage 4 soak was skipped at user's direction (Tracy delta empirically
// validated: 7.24 ms → 1.87 ms mean on quadSetupTextures at mc2_10 wolfman,
// 4.2× σ compression, 0 parity mismatches across 64M vertex comparisons,
// tier1 5/5 PASS both env states — strong enough perf+parity evidence to
// flip without the 7-day burn-in window).
//
// Phase 1 commit chain for bisection: 4fa7a9a (recon) → 594add9 (Stage 1
// scaffold) → eda2431 (Stage 2 parity-clean math) → ff8de07 (Stage 3
// consumer flip) → THIS COMMIT (Stage 5 default-on).
bool IsEnabled() {
    static const bool s_enabled = mc2::gate::get<"MC2_TERRAIN_LIGHTING_GPU">();
    return s_enabled;
}

bool IsParityCheckEnabled() {
    static const bool s_parity = mc2::gate::get<"MC2_TERRAIN_LIGHTING_PARITY">();
    return s_parity;
}

} // namespace gos_terrain_lighting

namespace {

// ---------------------------------------------------------------------------
// Private file-scope constants
// ---------------------------------------------------------------------------
static constexpr uint32_t RING_FRAMES = 3u;  // matches gpu_cull_readback.cpp:40

// SSBO binding slots (must match gos_terrain_lighting.comp)
static constexpr uint32_t TL_VERTEX_INPUT_BINDING = 0u;
static constexpr uint32_t TL_LIGHT_INPUT_BINDING  = 1u;
static constexpr uint32_t TL_OUTPUT_BINDING        = 2u;

// Uniform slot indices — used to index s_uniformLocs[].
// Resolved at shader-compile time via glGetUniformLocation (NOT layout qualifiers —
// explicit layout(location=N) on compute uniforms is unreliable on AMD RX 7900 XTX;
// all other compute shaders in this codebase use glGetUniformLocation exclusively).
static constexpr int TL_UNI_NUM_VERTICES    = 0;
static constexpr int TL_UNI_NUM_LIGHTS      = 1;
static constexpr int TL_UNI_SUN_LIGHT_DIR   = 2;
static constexpr int TL_UNI_LIGHT_RED       = 3;
static constexpr int TL_UNI_LIGHT_GREEN     = 4;
static constexpr int TL_UNI_LIGHT_BLUE      = 5;
static constexpr int TL_UNI_AMBIENT_RED     = 6;
static constexpr int TL_UNI_AMBIENT_GREEN   = 7;
static constexpr int TL_UNI_AMBIENT_BLUE    = 8;
static constexpr int TL_UNI_BASE_COLOR_R    = 9;
static constexpr int TL_UNI_BASE_COLOR_G    = 10;
static constexpr int TL_UNI_BASE_COLOR_B    = 11;
static constexpr int TL_UNI_RAIN_LEVEL      = 12;
static constexpr int TL_UNI_LIGHTNING_LEVEL = 13;
static constexpr int TL_UNI_FOG_START       = 14;
static constexpr int TL_UNI_FOG_FULL        = 15;
static constexpr int TL_UNI_USE_FOG         = 16;
static constexpr int TL_UNI_RENDERER_SW     = 17;
// Step 7 (VPL retirement): inline-worldPos hazeFactor uniforms.
static constexpr int TL_UNI_PLAY_AREA       = 18;
static constexpr int TL_UNI_CAM_WORLD_POS   = 19;
static constexpr int TL_UNI_MIN_HAZE        = 20;
static constexpr int TL_UNI_MAX_CLIP        = 21;
static constexpr int TL_UNI_DIST_FACTOR     = 22;
// Visible-window dispatch: offset (packMinVN) added to gl_GlobalInvocationID.x in shader.
// Limits dispatch groups to the ~200x200 camera window instead of the full terrain.
static constexpr int TL_UNI_VERTEX_OFFSET   = 23;
static constexpr int TL_UNI_COUNT           = 24;

// Runtime-resolved uniform locations (populated once after shader compile).
static GLint s_uniformLocs[TL_UNI_COUNT];

// ---------------------------------------------------------------------------
// Private GL state
// ---------------------------------------------------------------------------
static bool     s_initialized  = false;
static bool     s_shaderBad    = false;   // sticky disable on compile failure
static GLuint   s_program      = 0;

static uint32_t s_numVertices  = 0;
// STEP 3.4P: this frame's producer inputs for the native Vulkan lighting compute (see GetVkProducerSnapshot).
static gos_terrain_lighting::VkProducerSnapshot s_vkSnap{};
static bool s_vkSnapValid = false;
static uint32_t s_maxLights    = 0;

// GPU-side SSBOs (compute reads/writes; no persistent map needed)
// TERRAIN-LIGHTING-SSBO-OWNER-1: the 3 non-ring compute SSBOs are narrowed behind
// GpuBufferOwner identity records (logical id + lifetime + debug name + GLuint
// value). GL gen/bind/bufferStorage/delete happen at the same sites with the same
// args/order/flags/binding-slots; the raw handle is reached only via owner.backendHandle.
// Lifetime Mission: created in mission_init, destroyed in mission_shutdown, rebuilt
// per mission load. The s_stagingRing[RING_FRAMES] below is Tier-2 (persistent-mapped
// BAR ring) and stays a raw GLuint array — intentionally NOT owned.
static RenderCore::GpuBufferOwner s_vertexInputSsbo{   // GpuTerrainVertexInput per vertex (binding 0)
    RenderCore::RenderResourceId::TerrainLightVertexInputSsbo,
    RenderCore::RenderResourceLifetime::Mission,
    "TerrainLightVertexInputSsbo",
    0u};
static RenderCore::GpuBufferOwner s_lightInputSsbo{    // GpuTerrainLight per light slot (binding 1)
    RenderCore::RenderResourceId::TerrainLightInputSsbo,
    RenderCore::RenderResourceLifetime::Mission,
    "TerrainLightInputSsbo",
    0u};
static RenderCore::GpuBufferOwner s_computeOutputSsbo{ // GpuTerrainLightingOutput per vertex (binding 2)
    RenderCore::RenderResourceId::TerrainLightComputeOutputSsbo,
    RenderCore::RenderResourceLifetime::Mission,
    "TerrainLightComputeOutputSsbo",
    0u};

// 3-slot staging ring (CPU-readable persistent map) matching gpu_cull_readback pattern
static GLuint   s_stagingRing[RING_FRAMES]   = {};
static void*    s_stagingMapped[RING_FRAMES] = {};
static GLsync   s_stagingFence[RING_FRAMES]  = {};
static uint32_t s_currentSlot               = 0;

// Output slot bytes (numVertices * sizeof(GpuTerrainLightingOutput))
static size_t   s_outputSlotBytes = 0;

static uint64_t s_frameIndex = 0;

// NIGHT-LIGHT-POOLS-1: light count packed by the LAST PackAndDispatch — read by
// gos_terrain_lighting::GetLightPoolLightCount() so the LOD-chunk terrain draw
// knows how many pool lights the (already-uploaded) light SSBO holds this frame.
static uint32_t s_lastNumLights = 0;

// ---------------------------------------------------------------------------
// LIGHT-CLUSTER-PERF-1 (2026-07-25): world-space XY light GRID.
//
// WHY. LIGHT-POOL-CAP-1 (5213d257) raised the pool cap 64 -> 256 because mc2_10
// genuinely wants 142 fixtures and 78 of them were being dropped by list order.
// Correct fix, but the consumer (terrain_lod_chunk.frag) loops EVERY pool light
// per fragment, so the cost is O(fragments * poolLights) — measured ~34% avg FPS
// at the busiest mc2_10 night pose. Storage was never the constraint (the pool is
// an unbounded SSBO); the per-fragment loop is.
//
// WHAT. A uniform grid over the XY bounding box of this frame's packed lights.
// Each cell holds the index list of lights whose ground circle overlaps it. The
// fragment maps v_worldPos.xy -> cell and loops only that cell's list (typically
// 1-6 lights instead of 142). Fragments outside the bbox get count 0 and skip the
// block entirely.
//
// WHY IT IS EXACT (not an approximation — no tolerance needed). A light
// contributes to a fragment only when len < farDistance, i.e. only when the
// fragment lies inside the light's ground circle of radius farDistance. Insertion
// uses that same UNSCALED farDistance to stamp the circle's AABB into the grid, so
// every cell containing any point the light can reach holds that light's index.
// The shader's mover-radius shrink (u_lightPoolMoverScale <= 1) only ever REDUCES
// the reach, so the stamped set stays a conservative superset. Same lights, same
// pixels, bit-for-bit — the loop merely visits fewer zero-contribution lights.
//
// Gate MC2_LIGHT_GRID (default ON, =0 killswitch -> linear full-pool loop).
// MC2_LIGHT_GRID_DIM overrides the cell count per axis (default 64).
// Small pools bypass the grid (below kLightGridMinLights the build+bind costs
// more than the loop it saves) by publishing dim=0, which the shader reads as
// "use the linear path".
// ---------------------------------------------------------------------------
static constexpr uint32_t kLightGridMinLights = 8u;   // below this: linear loop

// TERRAIN-LIGHTING-SSBO-OWNER-1 discipline: both grid SSBOs carry GpuBufferOwner
// identity (logical id + lifetime + debug name), reached only via backendHandle.
// Lifetime Mission: created lazily on the first frame that bins a pool, released
// in mission_shutdown, rebuilt on demand.
static RenderCore::GpuBufferOwner s_lightGridHeader{   // uvec2 {offset,count} per cell (binding 29)
    RenderCore::RenderResourceId::TerrainLightGridHeaderSsbo,
    RenderCore::RenderResourceLifetime::Mission,
    "TerrainLightGridHeaderSsbo",
    0u};
static RenderCore::GpuBufferOwner s_lightGridIndex{    // uint light index list (binding 30)
    RenderCore::RenderResourceId::TerrainLightGridIndexSsbo,
    RenderCore::RenderResourceLifetime::Mission,
    "TerrainLightGridIndexSsbo",
    0u};
static GLsizeiptr s_lightGridHeaderBytes = 0;
static GLsizeiptr s_lightGridIndexBytes  = 0;
static std::vector<uint32_t> s_gridHeader;   // 2 * dim * dim
static std::vector<uint32_t> s_gridIndices;
static std::vector<uint32_t> s_gridCounts;   // scratch: per-cell counter
static uint32_t s_lightGridDim     = 0;      // 0 => grid not active this frame
static float    s_lightGridMin[2]  = {0.0f, 0.0f};
static float    s_lightGridInvCell[2] = {0.0f, 0.0f};

static bool LightGridEnabled() {
    // default ON; =0 reverts to the linear loop
    static const bool s_on = mc2::gate::get<"MC2_LIGHT_GRID">();
    return s_on;
}

static uint32_t LightGridDimEnv() {
    // Row: default 64, range(2, 256), out of range falls back to the default.
    static const uint32_t s_dim = static_cast<uint32_t>(mc2::gate::get<"MC2_LIGHT_GRID_DIM">());
    return s_dim;
}

// Stage 3: tryConsume fallback telemetry counters (mirrors readback_fallback_n2 /
// readback_fallback_conservative pattern from gpu_cull_readback.cpp)
static uint64_t s_lightingFallbackN1          = 0;  // consumed N-1 (production path)
static uint64_t s_lightingFallbackN2          = 0;  // used N-2 fallback
static uint64_t s_lightingFallbackConservative = 0; // skipped update (both not ready)
static uint64_t s_fallbackSummaryFrames        = 0; // frames since last summary

// Parity state
static int       s_parityMismatchesThisFrame = 0;
static long long s_paritySummaryFrames       = 0;
static long long s_paritySummaryVerts        = 0;
static long long s_paritySummaryMismatches   = 0;

// Dense vertex pack buffer (persistent across frames to avoid per-frame alloc)
static std::vector<gos_terrain_lighting::GpuTerrainVertexInput> s_packBuf;
// Light pack buffer
static std::vector<gos_terrain_lighting::GpuTerrainLight> s_lightBuf;

// Build + upload the grid for s_lightBuf[0..numLights). Sets s_lightGridDim to 0
// when the grid is not in play (gate off, too few lights, degenerate bbox), which
// makes every consumer fall back to the exact linear loop.
static void BuildAndUploadLightGrid(uint32_t numLights) {
    s_lightGridDim = 0;
    if (!LightGridEnabled() || numLights < kLightGridMinLights) return;

    // --- bbox over every light's ground circle (unscaled farDistance) ---
    float minX =  3.0e38f, minY =  3.0e38f;
    float maxX = -3.0e38f, maxY = -3.0e38f;
    uint32_t live = 0;
    for (uint32_t i = 0; i < numLights; ++i) {
        const gos_terrain_lighting::GpuTerrainLight& L = s_lightBuf[i];
        if (L.lightType == 0u) continue;      // shader skips these too
        const float r = L.farDistance;
        if (!(r > 0.0f)) continue;            // NaN-safe: excludes r<=0 and NaN
        ++live;
        if (L.position[0] - r < minX) minX = L.position[0] - r;
        if (L.position[1] - r < minY) minY = L.position[1] - r;
        if (L.position[0] + r > maxX) maxX = L.position[0] + r;
        if (L.position[1] + r > maxY) maxY = L.position[1] + r;
    }
    if (live < kLightGridMinLights) return;
    const float spanX = maxX - minX, spanY = maxY - minY;
    if (!(spanX > 0.0f) || !(spanY > 0.0f)) return;   // degenerate -> linear

    const uint32_t dim   = LightGridDimEnv();
    const uint32_t cells = dim * dim;
    // Pad the bbox by half a cell so floor() at the exact max edge cannot land on
    // index == dim (the shader also range-checks, but keep the CPU stamp honest).
    const float cellW = spanX / static_cast<float>(dim);
    const float cellH = spanY / static_cast<float>(dim);
    const float invW  = 1.0f / cellW;
    const float invH  = 1.0f / cellH;

    // --- pass 1: count per cell ---
    s_gridCounts.assign(cells, 0u);
    auto cellRange = [&](const gos_terrain_lighting::GpuTerrainLight& L, int& x0, int& y0, int& x1, int& y1) {
        const float r = L.farDistance;
        x0 = static_cast<int>(std::floor((L.position[0] - r - minX) * invW));
        y0 = static_cast<int>(std::floor((L.position[1] - r - minY) * invH));
        x1 = static_cast<int>(std::floor((L.position[0] + r - minX) * invW));
        y1 = static_cast<int>(std::floor((L.position[1] + r - minY) * invH));
        if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
        if (x1 > static_cast<int>(dim) - 1) x1 = static_cast<int>(dim) - 1;
        if (y1 > static_cast<int>(dim) - 1) y1 = static_cast<int>(dim) - 1;
    };
    uint32_t total = 0;
    for (uint32_t i = 0; i < numLights; ++i) {
        const gos_terrain_lighting::GpuTerrainLight& L = s_lightBuf[i];
        if (L.lightType == 0u || !(L.farDistance > 0.0f)) continue;
        int x0, y0, x1, y1; cellRange(L, x0, y0, x1, y1);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) { ++s_gridCounts[y * dim + x]; ++total; }
    }
    if (total == 0) return;

    // --- prefix sum into the header (offset,count) ---
    s_gridHeader.assign(cells * 2u, 0u);
    uint32_t running = 0;
    for (uint32_t c = 0; c < cells; ++c) {
        s_gridHeader[c * 2u + 0u] = running;
        s_gridHeader[c * 2u + 1u] = s_gridCounts[c];
        running += s_gridCounts[c];
    }

    // --- pass 2: scatter indices (reuse s_gridCounts as a write cursor) ---
    s_gridIndices.assign(total, 0u);
    std::fill(s_gridCounts.begin(), s_gridCounts.end(), 0u);
    for (uint32_t i = 0; i < numLights; ++i) {
        const gos_terrain_lighting::GpuTerrainLight& L = s_lightBuf[i];
        if (L.lightType == 0u || !(L.farDistance > 0.0f)) continue;
        int x0, y0, x1, y1; cellRange(L, x0, y0, x1, y1);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const uint32_t c = static_cast<uint32_t>(y) * dim + static_cast<uint32_t>(x);
                s_gridIndices[s_gridHeader[c * 2u] + s_gridCounts[c]++] = i;
            }
    }

    // --- upload (grow-only; orphan on resize) ---
    if (s_lightGridHeader.backendHandle == 0) {
        GLuint local = 0; glGenBuffers(1, &local);
        s_lightGridHeader.backendHandle = static_cast<uint32_t>(local);
    }
    if (s_lightGridIndex.backendHandle == 0) {
        GLuint local = 0; glGenBuffers(1, &local);
        s_lightGridIndex.backendHandle = static_cast<uint32_t>(local);
    }
    const GLsizeiptr hdrBytes = static_cast<GLsizeiptr>(s_gridHeader.size()  * sizeof(uint32_t));
    const GLsizeiptr idxBytes = static_cast<GLsizeiptr>(s_gridIndices.size() * sizeof(uint32_t));
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(s_lightGridHeader.backendHandle));
    if (hdrBytes > s_lightGridHeaderBytes) {
        glBufferData(GL_SHADER_STORAGE_BUFFER, hdrBytes, s_gridHeader.data(), GL_STREAM_DRAW);
        s_lightGridHeaderBytes = hdrBytes;
    } else {
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, hdrBytes, s_gridHeader.data());
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(s_lightGridIndex.backendHandle));
    if (idxBytes > s_lightGridIndexBytes) {
        glBufferData(GL_SHADER_STORAGE_BUFFER, idxBytes, s_gridIndices.data(), GL_STREAM_DRAW);
        s_lightGridIndexBytes = idxBytes;
    } else {
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, idxBytes, s_gridIndices.data());
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // Keep the resource registry honest about the two grid buffers (observe-only
    // metadata; never read by the draw path). Re-published each frame because the
    // grow-only upload above can change sizeBytes.
    {
        auto publish = [](const RenderCore::GpuBufferOwner& o, GLsizeiptr bytes) {
            RenderCore::RenderResourceDesc d;
            d.id        = o.id;
            d.kind      = RenderCore::RenderResourceKind::Buffer;
            d.lifetime  = o.lifetime;
            d.format    = RenderCore::RenderResourceFormat::BufferRaw;
            d.debugName = o.debugName;
            d.glName    = static_cast<GLuint>(o.backendHandle);
            d.sizeBytes = static_cast<uint64_t>(bytes);
            d.valid     = true;
            RenderCore::registerOrUpdateRenderResource(d);
        };
        publish(s_lightGridHeader, s_lightGridHeaderBytes);
        publish(s_lightGridIndex,  s_lightGridIndexBytes);
    }

    s_lightGridMin[0]     = minX;
    s_lightGridMin[1]     = minY;
    s_lightGridInvCell[0] = invW;
    s_lightGridInvCell[1] = invH;
    s_lightGridDim        = dim;

    // Rate-limited receipt: peak per-cell occupancy is the number the fragment
    // loop actually pays, so it is the honest "did this help?" telemetry.
    {
        static uint32_t s_peakCell = 0;
        uint32_t peak = 0;
        for (uint32_t c = 0; c < cells; ++c)
            if (s_gridHeader[c * 2u + 1u] > peak) peak = s_gridHeader[c * 2u + 1u];
        if (peak > s_peakCell) {
            s_peakCell = peak;
            MC2_DIAGFE(stderr,
                "[LIGHT_GRID v1] dim=%u lights=%u stamped=%u peak_cell=%u avg_cell=%.2f\n",
                dim, numLights, total, s_peakCell,
                static_cast<double>(total) / static_cast<double>(cells));
            std::fflush(stderr);
        }
    }
}


// DRAM-side shadow copy of the last-consumed staging ring slot.
// BAR memory (s_stagingMapped) is WC (write-combining) — reads from it bypass
// CPU caches and go over PCIe, which is ~100-1000× slower than DRAM reads for
// random-access patterns. Reading 14K entries indexed by vertexNum from BAR
// causes severe perf regression. Fix: memcpy the entire staging slot from BAR
// to DRAM once per frame (sequential, WC-read-friendly), then index from DRAM.
static std::vector<gos_terrain_lighting::GpuTerrainLightingOutput> s_dramShadow;

// ---------------------------------------------------------------------------
// Private shader-compile helpers (tl_ prefix — mirrors gpu_cull_compute.cpp:145-231
// privately; design doc Q1: copy, don't factor until a third module appears)
// ---------------------------------------------------------------------------
static char* tl_load_text_file(const char* fname) {
    FILE* f = fopen(fname, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return nullptr; }
    char* buf = new char[sz + 1];
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

static GLuint tl_compile_compute_shader(const char** strings, int count) {
    GLuint sh = glCreateShader(GL_COMPUTE_SHADER);
    if (!sh) return 0;

    glShaderSource(sh, count, strings, nullptr);
    glCompileShader(sh);

    GLint status = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &status);
    if (!status) {
        GLint logLen = 0;
        glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &logLen);
        if (logLen > 0) {
            char* log = new char[logLen];
            glGetShaderInfoLog(sh, logLen, nullptr, log);
            MC2_DIAGF("[TERRAIN_LIGHTING_GPU v1] compute shader compile error:\n%s\n", log);
            fflush(stdout);
            delete[] log;
        }
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static GLuint tl_link_compute_program(GLuint shader) {
    GLuint prog = glCreateProgram();
    if (!prog) return 0;

    glAttachShader(prog, shader);
    glLinkProgram(prog);

    GLint status = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &status);
    if (!status) {
        GLint logLen = 0;
        glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &logLen);
        if (logLen > 0) {
            char* log = new char[logLen];
            glGetProgramInfoLog(prog, logLen, nullptr, log);
            MC2_DIAGF("[TERRAIN_LIGHTING_GPU v1] compute program link error:\n%s\n", log);
            fflush(stdout);
            delete[] log;
        }
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

// Build compute program from file with shared-header inlining.
// "#version 430\n" is always the first string (per CLAUDE.md Critical Rules).
static GLuint tl_build_terrain_lighting_program(const char* compFname,
                                                 const char* sharedFname,
                                                 const char* debugName)
{
    char* fileSrc    = tl_load_text_file(compFname);
    char* sharedSrc  = tl_load_text_file(sharedFname);

    if (!fileSrc) {
        MC2_DIAGF("[TERRAIN_LIGHTING_GPU v1] event=shader_load_fail source_missing=%s\n", compFname);
        fflush(stdout);
        delete[] sharedSrc;
        return 0;
    }

    // Inline terrain_lighting_shared.hglsl by replacing the marker block in the .comp file.
    static const char* kBeginMarker = "// --- begin terrain_lighting_shared.hglsl ---";
    static const char* kEndMarker   = "// --- end terrain_lighting_shared.hglsl ---";
    std::string fullSrc;

    if (sharedSrc) {
        char* beginPos = strstr(fileSrc, kBeginMarker);
        char* endPos   = beginPos ? strstr(beginPos, kEndMarker) : nullptr;
        if (beginPos && endPos) {
            fullSrc.append(fileSrc, beginPos - fileSrc);
            fullSrc += "\n// --- begin terrain_lighting_shared.hglsl (inlined) ---\n";
            fullSrc += sharedSrc;
            fullSrc += "\n// --- end terrain_lighting_shared.hglsl (inlined) ---\n";
            const char* afterEnd = endPos + strlen(kEndMarker);
            fullSrc += afterEnd;
        } else {
            fullSrc = fileSrc;
        }
    } else {
        MC2_DIAGF("[TERRAIN_LIGHTING_GPU v1] event=shader_load_warn shared_missing=%s\n", sharedFname);
        fflush(stdout);
        fullSrc = fileSrc;
    }

    delete[] fileSrc;
    delete[] sharedSrc;

    const char* kVersion = "#version 430\n";
    std::vector<const char*> srcStrs;
    srcStrs.push_back(kVersion);
    srcStrs.push_back(fullSrc.c_str());

    GLuint sh = tl_compile_compute_shader(srcStrs.data(), static_cast<int>(srcStrs.size()));
    if (!sh) {
        MC2_DIAGF("[TERRAIN_LIGHTING_GPU v1] event=shader_load_fail compile_failed=%s\n", debugName);
        fflush(stdout);
        return 0;
    }
    GLuint prog = tl_link_compute_program(sh);
    glDeleteShader(sh);
    if (!prog) {
        MC2_DIAGF("[TERRAIN_LIGHTING_GPU v1] event=shader_load_fail link_failed=%s\n", debugName);
        fflush(stdout);
    }
    return prog;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
namespace gos_terrain_lighting {

GLuint GetOutputSsbo() {
    return static_cast<GLuint>(s_computeOutputSsbo.backendHandle);
}

// NIGHT-LIGHT-POOLS-1: forward-consumer accessors for the per-frame light pool.
GLuint GetLightPoolSsbo() {
    if (!s_initialized) return 0;
    return static_cast<GLuint>(s_lightInputSsbo.backendHandle);
}

uint32_t GetLightPoolLightCount() {
    return s_lastNumLights;
}

// LIGHT-CLUSTER-PERF-1: publish this frame's light grid. Returns false when the
// grid is not in play (gate MC2_LIGHT_GRID=0, pool below kLightGridMinLights,
// degenerate bbox, or nothing packed) — consumers then use the exact linear
// full-pool loop, which is the killswitch contract.
bool GetLightGridParams(LightGridParams& out) {
    if (!s_initialized || s_lightGridDim == 0 ||
        s_lightGridHeader.backendHandle == 0 || s_lightGridIndex.backendHandle == 0)
        return false;
    out.dim        = s_lightGridDim;
    out.minXY[0]   = s_lightGridMin[0];
    out.minXY[1]   = s_lightGridMin[1];
    out.invCell[0] = s_lightGridInvCell[0];
    out.invCell[1] = s_lightGridInvCell[1];
    out.headerSsbo = static_cast<GLuint>(s_lightGridHeader.backendHandle);
    out.indexSsbo  = static_cast<GLuint>(s_lightGridIndex.backendHandle);
    return true;
}

// VK-PARITY-TERRAIN-2: CPU views of the arrays uploaded above (see header).
const GpuTerrainLight* GetLightPoolCpu(uint32_t* outCount) {
    if (outCount) *outCount = s_initialized ? s_lastNumLights : 0u;
    if (!s_initialized || s_lightBuf.empty()) return nullptr;
    return s_lightBuf.data();
}

bool GetLightGridCpu(LightGridCpu& out) {
    if (!s_initialized || s_lightGridDim == 0 ||
        s_lightGridHeader.backendHandle == 0 || s_lightGridIndex.backendHandle == 0 ||
        s_gridHeader.empty() || s_gridIndices.empty())
        return false;
    out.dim         = s_lightGridDim;
    out.minXY[0]    = s_lightGridMin[0];
    out.minXY[1]    = s_lightGridMin[1];
    out.invCell[0]  = s_lightGridInvCell[0];
    out.invCell[1]  = s_lightGridInvCell[1];
    out.header      = s_gridHeader.data();
    out.headerWords = static_cast<uint32_t>(s_gridHeader.size());
    out.indices     = s_gridIndices.data();
    out.indexWords  = static_cast<uint32_t>(s_gridIndices.size());
    return true;
}

// POOL-CONSUMERS-1 (batch-6 item 6): shared object-consumer upload. See header.
// VK-PARITY-MECH-1: shared object-pool producer (GL upload + VK mech island).
void GetObjectLightPoolParams(ObjectLightPoolParams* out) {
    if (!out) return;
    static const bool s_objects = mc2::gate::get<"MC2_LIGHT_POOL_OBJECTS">();   // default ON
    // Knob values lockstep with the terrain-chunk consumer defaults
    // (gos_terrain_lod_chunk.cpp / gameos_graphics.cpp overlay path).
    static const bool s_poolSoft = mc2::gate::get<"MC2_LIGHT_POOL_SOFT">();
    // Rows: MOVER_DIM default 0.25 range(0, 1); MOVER_RADIUS default 0.6 range (0, 1] (exclusive lower
    // bound); out of range falls back to the default.
    static const float s_poolMoverDim = mc2::gate::get<"MC2_LIGHT_POOL_MOVER_DIM">();
    static const float s_poolMoverRadius = mc2::gate::get<"MC2_LIGHT_POOL_MOVER_RADIUS">();
    static const bool s_poolMoverMax = mc2::gate::get<"MC2_LIGHT_POOL_MOVER_MAXBLEND">();
    static const bool s_poolCompress = mc2::gate::get<"MC2_LIGHT_POOL_COMPRESS">();
    // POOL-CONSUMERS-1: object-side intensity scale (objects hold baked light
    // rows already; keep the additive pool below the terrain level so the
    // composite stays atmospheric, not floodlit). MC2_LIGHT_POOL_OBJ_SCALE
    // override (default 0.7, clamp [0,2]; 0 behaves like the sub-gate off).
    static const float s_objScale = mc2::gate::get<"MC2_LIGHT_POOL_OBJ_SCALE">();   // row: default 0.7, clampRange(0, 2)

    out->count      = 0;
    out->pool       = nullptr;
    out->soft       = s_poolSoft ? 1 : 0;
    out->moverDim   = s_poolMoverDim;
    out->moverScale = s_poolMoverRadius;
    out->moverMax   = s_poolMoverMax ? 1 : 0;
    out->compress   = s_poolCompress ? 1 : 0;
    out->objScale   = s_objScale;
    if (s_objects && s_objScale > 0.0f) {
        const GLuint   ssbo = GetLightPoolSsbo();
        const uint32_t n    = GetLightPoolLightCount();
        if (ssbo != 0 && n > 0) {
            out->count = static_cast<int>(n > 256u ? 256u : n);
            uint32_t cpuN = 0;
            out->pool = GetLightPoolCpu(&cpuN);
            if (!out->pool || cpuN < static_cast<uint32_t>(out->count))
                out->pool = nullptr;   // GL still has its SSBO; VK declines pools
        }
    }
}

void UploadObjectLightPoolUniforms(unsigned int program) {
    const GLint locN = glGetUniformLocation(program, "u_numPoolLights");
    if (locN < 0) return;   // program has no pool block compiled in

    ObjectLightPoolParams pp{};
    GetObjectLightPoolParams(&pp);
    const int poolCount = pp.count;
    if (poolCount > 0)
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 28u, GetLightPoolSsbo());
    glUniform1i(locN, poolCount);
    if (poolCount == 0) return;
    const GLint locSoft     = glGetUniformLocation(program, "u_lightPoolSoft");
    const GLint locDim      = glGetUniformLocation(program, "u_lightPoolMoverDim");
    const GLint locScale    = glGetUniformLocation(program, "u_lightPoolMoverScale");
    const GLint locMax      = glGetUniformLocation(program, "u_lightPoolMoverMax");
    const GLint locCompress = glGetUniformLocation(program, "u_lightPoolCompress");
    const GLint locObjScale = glGetUniformLocation(program, "u_lightPoolObjScale");
    if (locSoft >= 0)     glUniform1i(locSoft, pp.soft);
    if (locDim >= 0)      glUniform1f(locDim, pp.moverDim);
    if (locScale >= 0)    glUniform1f(locScale, pp.moverScale);
    if (locMax >= 0)      glUniform1i(locMax, pp.moverMax);
    if (locCompress >= 0) glUniform1i(locCompress, pp.compress);
    if (locObjScale >= 0) glUniform1f(locObjScale, pp.objScale);
}

// ---------------------------------------------------------------------------
// mission_init — per-mission, called alongside gpu_cull::compute_init()
// ---------------------------------------------------------------------------
void mission_init(uint32_t numVertices, uint32_t maxLights) {
    if (!IsEnabled()) return;

    // Compile shader on first call (lazy, single process lifetime)
    if (!s_program && !s_shaderBad) {
        s_program = tl_build_terrain_lighting_program(
            "shaders/gos_terrain_lighting.comp",
            "shaders/include/terrain_lighting_shared.hglsl",
            "terrain_lighting");
        if (!s_program) {
            s_shaderBad = true;
            MC2_DIAGF("[TERRAIN_LIGHTING_GPU v1] event=shader_load_fail sticky_disable\n");
            fflush(stdout);
            return;
        }
        // Resolve all uniform locations once — no explicit layout qualifiers in shader
        // (AMD RX 7900 XTX doesn't honor layout(location=N) for compute uniforms reliably).
        s_uniformLocs[TL_UNI_NUM_VERTICES]    = glGetUniformLocation(s_program, "u_numVertices");
        s_uniformLocs[TL_UNI_NUM_LIGHTS]      = glGetUniformLocation(s_program, "u_numLights");
        s_uniformLocs[TL_UNI_SUN_LIGHT_DIR]   = glGetUniformLocation(s_program, "u_sunLightDir");
        s_uniformLocs[TL_UNI_LIGHT_RED]       = glGetUniformLocation(s_program, "u_lightRed");
        s_uniformLocs[TL_UNI_LIGHT_GREEN]     = glGetUniformLocation(s_program, "u_lightGreen");
        s_uniformLocs[TL_UNI_LIGHT_BLUE]      = glGetUniformLocation(s_program, "u_lightBlue");
        s_uniformLocs[TL_UNI_AMBIENT_RED]     = glGetUniformLocation(s_program, "u_ambientRed");
        s_uniformLocs[TL_UNI_AMBIENT_GREEN]   = glGetUniformLocation(s_program, "u_ambientGreen");
        s_uniformLocs[TL_UNI_AMBIENT_BLUE]    = glGetUniformLocation(s_program, "u_ambientBlue");
        s_uniformLocs[TL_UNI_BASE_COLOR_R]    = glGetUniformLocation(s_program, "u_baseVertexColorR");
        s_uniformLocs[TL_UNI_BASE_COLOR_G]    = glGetUniformLocation(s_program, "u_baseVertexColorG");
        s_uniformLocs[TL_UNI_BASE_COLOR_B]    = glGetUniformLocation(s_program, "u_baseVertexColorB");
        s_uniformLocs[TL_UNI_RAIN_LEVEL]      = glGetUniformLocation(s_program, "u_rainLightLevel");
        s_uniformLocs[TL_UNI_LIGHTNING_LEVEL] = glGetUniformLocation(s_program, "u_lighteningLevel");
        s_uniformLocs[TL_UNI_FOG_START]       = glGetUniformLocation(s_program, "u_fogStart");
        s_uniformLocs[TL_UNI_FOG_FULL]        = glGetUniformLocation(s_program, "u_fogFull");
        s_uniformLocs[TL_UNI_USE_FOG]         = glGetUniformLocation(s_program, "u_useFog");
        s_uniformLocs[TL_UNI_RENDERER_SW]     = glGetUniformLocation(s_program, "u_rendererSW");
        // Step 7 (VPL retirement): inline-worldPos hazeFactor uniforms.
        s_uniformLocs[TL_UNI_PLAY_AREA]       = glGetUniformLocation(s_program, "g_playAreaBounds");
        s_uniformLocs[TL_UNI_CAM_WORLD_POS]   = glGetUniformLocation(s_program, "u_cameraWorldPos");
        s_uniformLocs[TL_UNI_MIN_HAZE]        = glGetUniformLocation(s_program, "g_minHazeDistance");
        s_uniformLocs[TL_UNI_MAX_CLIP]        = glGetUniformLocation(s_program, "g_maxClipDistance");
        s_uniformLocs[TL_UNI_DIST_FACTOR]     = glGetUniformLocation(s_program, "g_distanceFactor");
        s_uniformLocs[TL_UNI_VERTEX_OFFSET]   = glGetUniformLocation(s_program, "u_vertexOffset");
        // Always-on diagnostic: print all resolved locations so black-terrain debug
        // can immediately see which (if any) came back -1 (uniform not found).
        MC2_DIAGF("[TERRAIN_LIGHTING_GPU v1] event=uniform_locs "
               "numVerts=%d numLights=%d sunDir=%d "
               "lightR=%d lightG=%d lightB=%d "
               "ambR=%d ambG=%d ambB=%d "
               "baseR=%d baseG=%d baseB=%d "
               "rain=%d lightning=%d fogStart=%d fogFull=%d "
               "useFog=%d rendSW=%d "
               "playArea=%d camWorldPos=%d minHaze=%d maxClip=%d distFactor=%d vertexOffset=%d\n",
               s_uniformLocs[TL_UNI_NUM_VERTICES],
               s_uniformLocs[TL_UNI_NUM_LIGHTS],
               s_uniformLocs[TL_UNI_SUN_LIGHT_DIR],
               s_uniformLocs[TL_UNI_LIGHT_RED],
               s_uniformLocs[TL_UNI_LIGHT_GREEN],
               s_uniformLocs[TL_UNI_LIGHT_BLUE],
               s_uniformLocs[TL_UNI_AMBIENT_RED],
               s_uniformLocs[TL_UNI_AMBIENT_GREEN],
               s_uniformLocs[TL_UNI_AMBIENT_BLUE],
               s_uniformLocs[TL_UNI_BASE_COLOR_R],
               s_uniformLocs[TL_UNI_BASE_COLOR_G],
               s_uniformLocs[TL_UNI_BASE_COLOR_B],
               s_uniformLocs[TL_UNI_RAIN_LEVEL],
               s_uniformLocs[TL_UNI_LIGHTNING_LEVEL],
               s_uniformLocs[TL_UNI_FOG_START],
               s_uniformLocs[TL_UNI_FOG_FULL],
               s_uniformLocs[TL_UNI_USE_FOG],
               s_uniformLocs[TL_UNI_RENDERER_SW],
               s_uniformLocs[TL_UNI_PLAY_AREA],
               s_uniformLocs[TL_UNI_CAM_WORLD_POS],
               s_uniformLocs[TL_UNI_MIN_HAZE],
               s_uniformLocs[TL_UNI_MAX_CLIP],
               s_uniformLocs[TL_UNI_DIST_FACTOR],
               s_uniformLocs[TL_UNI_VERTEX_OFFSET]);
        fflush(stdout);
    }
    if (s_shaderBad) return;

    // Teardown previous mission's SSBOs if they exist
    mission_shutdown();

    s_numVertices = numVertices;
    s_maxLights   = (maxLights > 0) ? maxLights : 1u;

    // LIGHT-POOL-CAP-1 (user directive, 2026-07-24: "ok just increase the
    // number of lights available"). Fixtures were silently losing their ground
    // pool to budget exhaustion — the pack loop below stops accepting lights
    // once numLights hits this cap, so whether a given lamp lit the ground
    // depended on its ORDER in the terrain-light list, not on anything the
    // player can see. That is the "one cone lights the ground, the identical
    // cone next to it doesn't" inconsistency.
    //
    // The callers pass 64. The pool is an SSBO with an UNBOUNDED array
    // (shaders/terrain_lod_chunk.frag binding 28) and the consumer loop is
    // bounded by the uniform u_numPoolLights, so there is no uniform-block or
    // shader-side limit to respect — only per-fragment loop cost. Raised to a
    // headroom default here (rather than at the two call sites) so the cap is
    // owned in one place; MC2_LIGHT_POOL_MAX overrides without a rebuild.
    {
        // Row: default 0 (use the built-in default below), range(0, 4096).
        static const uint32_t s_capOverride = static_cast<uint32_t>(mc2::gate::get<"MC2_LIGHT_POOL_MAX">());
        const uint32_t defaultCap = 256u;
        const uint32_t want = s_capOverride ? s_capOverride : defaultCap;
        if (want > s_maxLights) s_maxLights = want;
    }

    const GLsizeiptr vertInputBytes  = static_cast<GLsizeiptr>(numVertices * sizeof(GpuTerrainVertexInput));
    const GLsizeiptr lightInputBytes = static_cast<GLsizeiptr>(s_maxLights * sizeof(GpuTerrainLight));
    const GLsizeiptr outputBytes     = static_cast<GLsizeiptr>(numVertices * sizeof(GpuTerrainLightingOutput));
    s_outputSlotBytes = static_cast<size_t>(outputBytes);

    // Pre-size pack buffers
    s_packBuf.assign(numVertices, GpuTerrainVertexInput{});
    s_lightBuf.resize(s_maxLights);
    // DRAM shadow of staging ring: initialized to all-white (lightRGB/fogRGB=0xFFFFFFFF)
    // so that T3 (conservative, no update) leaves existing vertex colors unchanged on
    // first-frame startup (vertex pool already initialized to 0xFFFFFFFF at vertex.h:122).
    s_dramShadow.assign(numVertices, GpuTerrainLightingOutput{0xFFFFFFFFu, 0xFFFFFFFFu});

    // TERRAIN-LIGHTING-SSBO-OWNER-1: register a live owner in the resource registry
    // (observe-only metadata; never read by the compute/draw path). Called at create
    // (byte size known here) and invalidated in mission_shutdown.
    auto registerLightingSsbo = [](RenderCore::RenderResourceId id, const char* name,
                                   GLuint glName, GLsizeiptr bytes) {
        RenderCore::RenderResourceDesc d;
        d.id        = id;
        d.kind      = RenderCore::RenderResourceKind::Buffer;
        d.lifetime  = RenderCore::RenderResourceLifetime::Mission;
        d.format    = RenderCore::RenderResourceFormat::BufferRaw;
        d.debugName = name;
        d.glName    = glName;
        d.sizeBytes = static_cast<uint64_t>(bytes);
        d.valid     = true;
        RenderCore::registerOrUpdateRenderResource(d);
    };

    // --- Vertex input SSBO (GPU-write from CPU pack; no persistent map) ---
    {
        GLuint local = 0;
        glGenBuffers(1, &local);
        s_vertexInputSsbo.backendHandle = static_cast<uint32_t>(local);
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(s_vertexInputSsbo.backendHandle));
    glBufferStorage(GL_SHADER_STORAGE_BUFFER, vertInputBytes, nullptr,
                    GL_DYNAMIC_STORAGE_BIT | GL_MAP_WRITE_BIT);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    registerLightingSsbo(s_vertexInputSsbo.id, s_vertexInputSsbo.debugName,
                         static_cast<GLuint>(s_vertexInputSsbo.backendHandle), vertInputBytes);

    // --- Light input SSBO ---
    {
        GLuint local = 0;
        glGenBuffers(1, &local);
        s_lightInputSsbo.backendHandle = static_cast<uint32_t>(local);
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(s_lightInputSsbo.backendHandle));
    glBufferStorage(GL_SHADER_STORAGE_BUFFER, lightInputBytes, nullptr,
                    GL_DYNAMIC_STORAGE_BIT | GL_MAP_WRITE_BIT);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    registerLightingSsbo(s_lightInputSsbo.id, s_lightInputSsbo.debugName,
                         static_cast<GLuint>(s_lightInputSsbo.backendHandle), lightInputBytes);

    // --- Compute output SSBO (GPU writes here; no persistent map — pure VRAM) ---
    {
        GLuint local = 0;
        glGenBuffers(1, &local);
        s_computeOutputSsbo.backendHandle = static_cast<uint32_t>(local);
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(s_computeOutputSsbo.backendHandle));
    glBufferStorage(GL_SHADER_STORAGE_BUFFER, outputBytes, nullptr, GL_DYNAMIC_STORAGE_BIT);
    // glBufferStorage leaves VRAM UNINITIALISED. The lighting compute only writes
    // the visible vertex window [packMinVN, packMaxVN]; full-recipe-authoritative
    // water (gpu_driven_water.comp) reads lighting[vn].lightRGB for water quads
    // map-wide, including out-of-window vertices. On AMD fresh pages read back
    // ~zero (benign); NVIDIA hands back arbitrary prior-tenant VRAM → per-quad
    // green/blue "streaks" that re-roll each launch (restart appears to fix it).
    // Clear to 0xFFFFFFFF — the engine "no-tint" white sentinel, identical to the
    // CPU DRAM shadow init (s_dramShadow, ~line 447) and the vertex-pool default
    // (vertex.h:122). Cleared while still bound (glClearBufferData targets the
    // bound buffer); per-mission realloc means this one-time clear suffices.
    {
        const uint32_t kWhite = 0xFFFFFFFFu;
        glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &kWhite);
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    registerLightingSsbo(s_computeOutputSsbo.id, s_computeOutputSsbo.debugName,
                         static_cast<GLuint>(s_computeOutputSsbo.backendHandle), outputBytes);

    // --- 3-slot staging ring (CPU-readable persistent map, BAR memory) ---
    // Pattern from gpu_cull_readback.cpp:230-235.
    const GLbitfield stageStorageFlags = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT
                                       | GL_MAP_PERSISTENT_BIT
                                       | GL_MAP_COHERENT_BIT;
    const GLbitfield stageMapFlags     = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT
                                       | GL_MAP_PERSISTENT_BIT
                                       | GL_MAP_COHERENT_BIT;

    glGenBuffers(RING_FRAMES, s_stagingRing);
    for (uint32_t i = 0; i < RING_FRAMES; ++i) {
        glBindBuffer(GL_COPY_WRITE_BUFFER, s_stagingRing[i]);
        glBufferStorage(GL_COPY_WRITE_BUFFER, outputBytes, nullptr, stageStorageFlags);
        s_stagingMapped[i] = glMapBufferRange(GL_COPY_WRITE_BUFFER, 0, outputBytes, stageMapFlags);
        glBindBuffer(GL_COPY_WRITE_BUFFER, 0);

        if (!s_stagingMapped[i]) {
            MC2_DIAGFE(stderr, "[TERRAIN_LIGHTING_GPU v1] FATAL: staging persistent map failed slot=%u\n", i);
            fflush(stderr);
            s_shaderBad = true;
            mission_shutdown();
            return;
        }
        memset(s_stagingMapped[i], 0, static_cast<size_t>(outputBytes));
        s_stagingFence[i] = nullptr;
    }

    s_currentSlot  = 0;
    s_frameIndex   = 0;
    s_initialized  = true;

    MC2_DIAGF("[TERRAIN_LIGHTING_GPU v1] event=mission_init verts=%u lights=%u outputSlotKB=%zu\n",
           numVertices, s_maxLights, s_outputSlotBytes / 1024u);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// mission_shutdown — per-mission teardown
// ---------------------------------------------------------------------------
void mission_shutdown() {
    if (!s_initialized) return;

    // Wait for all in-flight fences before releasing (GL_TIMEOUT_IGNORED OK in teardown).
    for (uint32_t i = 0; i < RING_FRAMES; ++i) {
        if (s_stagingFence[i]) {
            glClientWaitSync(s_stagingFence[i], GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
            glDeleteSync(s_stagingFence[i]);
            s_stagingFence[i] = nullptr;
        }
    }

    // Unmap and release staging ring
    for (uint32_t i = 0; i < RING_FRAMES; ++i) {
        if (s_stagingRing[i] && s_stagingMapped[i]) {
            glBindBuffer(GL_COPY_WRITE_BUFFER, s_stagingRing[i]);
            glUnmapBuffer(GL_COPY_WRITE_BUFFER);
            glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
            s_stagingMapped[i] = nullptr;
        }
    }
    glDeleteBuffers(RING_FRAMES, s_stagingRing);
    memset(s_stagingRing, 0, sizeof(s_stagingRing));


    // TERRAIN-LIGHTING-SSBO-OWNER-1: delete via owner.backendHandle + mark the registry slot
    // unavailable (valid=false) on teardown. Same delete order as before.
    auto invalidateLightingSsbo = [](RenderCore::RenderResourceId id) {
        RenderCore::RenderResourceDesc invalid;
        invalid.id = id;
        RenderCore::registerOrUpdateRenderResource(invalid);
    };
    if (s_computeOutputSsbo.backendHandle) {
        GLuint buf = static_cast<GLuint>(s_computeOutputSsbo.backendHandle);
        glDeleteBuffers(1, &buf);
        s_computeOutputSsbo.backendHandle = 0;
        invalidateLightingSsbo(s_computeOutputSsbo.id);
    }
    if (s_lightInputSsbo.backendHandle) {
        GLuint buf = static_cast<GLuint>(s_lightInputSsbo.backendHandle);
        glDeleteBuffers(1, &buf);
        s_lightInputSsbo.backendHandle = 0;
        invalidateLightingSsbo(s_lightInputSsbo.id);
    }
    if (s_vertexInputSsbo.backendHandle) {
        GLuint buf = static_cast<GLuint>(s_vertexInputSsbo.backendHandle);
        glDeleteBuffers(1, &buf);
        s_vertexInputSsbo.backendHandle = 0;
        invalidateLightingSsbo(s_vertexInputSsbo.id);
    }

    // LIGHT-CLUSTER-PERF-1: release the light-grid SSBOs. Rebuilt lazily on the
    // next frame that packs enough lights; dim=0 keeps consumers on the linear
    // loop in the meantime.
    if (s_lightGridHeader.backendHandle) {
        GLuint buf = static_cast<GLuint>(s_lightGridHeader.backendHandle);
        glDeleteBuffers(1, &buf);
        s_lightGridHeader.backendHandle = 0;
        invalidateLightingSsbo(s_lightGridHeader.id);
    }
    if (s_lightGridIndex.backendHandle) {
        GLuint buf = static_cast<GLuint>(s_lightGridIndex.backendHandle);
        glDeleteBuffers(1, &buf);
        s_lightGridIndex.backendHandle = 0;
        invalidateLightingSsbo(s_lightGridIndex.id);
    }
    s_lightGridHeaderBytes = 0;
    s_lightGridIndexBytes  = 0;
    s_lightGridDim         = 0;


    s_numVertices     = 0;
    s_maxLights       = 0;
    s_outputSlotBytes = 0;
    s_currentSlot     = 0;
    s_frameIndex      = 0;
    s_initialized     = false;
    s_lastNumLights   = 0;  // NIGHT-LIGHT-POOLS-1: no pool lights across mission boundary
    s_dramShadow.clear();

    MC2_DIAGF("[TERRAIN_LIGHTING_GPU v1] event=mission_shutdown\n");
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// BeginFrame — advance ring slot
// ---------------------------------------------------------------------------
bool GetVkProducerSnapshot(VkProducerSnapshot& out) {
    if (!s_vkSnapValid) return false;
    out = s_vkSnap;
    return true;
}

bool ReadbackOutputForParity(uint32_t first, uint32_t count, GpuTerrainLightingOutput* dst) {
    if (!dst || count == 0 || !s_initialized || first + count > s_numVertices) return false;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(s_computeOutputSsbo.backendHandle));
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, static_cast<GLintptr>(first) * sizeof(GpuTerrainLightingOutput),
                       static_cast<GLsizeiptr>(count) * sizeof(GpuTerrainLightingOutput), dst);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    return true;
}

void BeginFrame() {
    s_vkSnapValid = false;   // STEP 3.4P: set again by this frame's PackAndDispatch
    if (!IsEnabled() || !s_initialized || s_shaderBad) return;
    s_currentSlot = (s_currentSlot + 1u) % RING_FRAMES;
    ++s_frameIndex;
}

// ---------------------------------------------------------------------------
// PackAndDispatch — pack vertex + light SSBOs, dispatch compute, barrier, copy, fence
// ---------------------------------------------------------------------------
void PackAndDispatch() {
    if (!IsEnabled() || !s_initialized || s_shaderBad) return;

    ZoneScopedN("Terrain::TerrainLightingDispatch");

    // --- Stage 2: Dense vertex pack by vertexNum ---
    // Walk the camera-windowed vertex list (land->vertexList, numberVertices entries).
    // For each vertex, slot it at inputs[vertexNum] (dense map-stable index).
    // Zero-initialize the pack buffer each frame so unreached slots are clean.
    //
    // packMinVN/packMaxVN hoisted to function scope: the dispatch section below uses
    // them to limit glDispatchCompute to only the visible window range.
    uint32_t packMinVN = s_numVertices;   // sentinel: will be lowered to min written vn
    uint32_t packMaxVN = 0;               // sentinel: will be raised to max written vn

    if (land && land->getVertexList() && land->getNumVertices() > 0) {
        const int nv = land->getNumVertices();
        const VertexPtr vlist = land->getVertexList();

        // PERF-FIX: previously memset'd the entire pack buffer (O(N²) = up to 32 MB
        // for a 1k map) every frame even though only ~40K entries (200×200 visible
        // window) are written.  The compute dispatch is bounded by the solid-window
        // SSBO (camera-windowed recipe indices), so stale entries for off-screen quads
        // are never dispatched and stale lighting data causes no visual artifact.
        // Instead we skip the full memset and upload only the min–max contiguous range
        // covering this frame's visible window — typically ~6.5 MB for a 1k map vs 32 MB.
        // For the full-range fallback (s_solidUseWindow=0), off-screen quads have stale
        // lighting but are frustum-culled by the compute shader before reaching the draw
        // command, so correctness is maintained in both modes.

        // Step 7: MC2_HAZE_PARITY — per-frame accumulators (first ~256 verts).
        int    hpProbed = 0;
        double hpFrameMaxAbs = 0.0;
        uint64_t hpFrameViol = 0;
        const float hpM    = Terrain::worldUnitsMapSide / 2.0f - Terrain::worldUnitsPerVertex * 2.0f;
        const float hpCamX = eye ? -eye->getCameraOrigin().x : 0.0f;
        const float hpCamY = eye ?  eye->getCameraOrigin().z : 0.0f;
        const float hpCamZ = eye ?  eye->getCameraOrigin().y : 0.0f;

        for (int i = 0; i < nv; ++i) {
            const Vertex& v = vlist[i];
            if (v.vertexNum < 0 || static_cast<uint32_t>(v.vertexNum) >= s_numVertices) continue;

            const uint32_t uvn = static_cast<uint32_t>(v.vertexNum);
            if (uvn < packMinVN) packMinVN = uvn;
            if (uvn > packMaxVN) packMaxVN = uvn;

            GpuTerrainVertexInput& vi = s_packBuf[uvn];
            vi.xy[0]      = v.vx;
            vi.xy[1]      = v.vy;
            vi.elevation  = v.pVertex ? v.pVertex->elevation : 0.0f;
            vi.hazeFactor = 0.0f; // Step 7: computed inline in compute; SSBO field dead, retained for std430 stride (alignas(16) stride-neutral; removal deferred to Step 10 cleanup, cpp_glsl_ubo_struct_lockstep.md)

            // Step 7: MC2_HAZE_PARITY probe — CPU-recompute the inline formula and
            // compare to the still-live VPL-written v.hazeFactor (first ~256 verts).
            if (s_hazeParity && hpProbed < 256) {
                ++hpProbed;
                const float hx = v.vx;
                const float hy = v.vy;
                const float he = v.pVertex ? v.pVertex->elevation : 0.0f;
                float hf;
                const bool outside = (hx <= -hpM) || (hx >= hpM) ||
                                     (hy <= -hpM) || (hy >= hpM);
                if (outside) {
                    hf = 1.0f;
                } else {
                    const float dx = hx - hpCamX;
                    const float dy = hy - hpCamY;
                    const float dz = he - hpCamZ;
                    const float d  = sqrtf(dx * dx + dy * dy + dz * dz);
                    if      (d > Camera::MaxClipDistance) hf = 1.0f;
                    else if (d > Camera::MinHazeDistance) hf = (d - Camera::MinHazeDistance) * Camera::DistanceFactor;
                    else                                  hf = 0.0f;
                }
                const double delta = fabs(static_cast<double>(hf) - static_cast<double>(v.hazeFactor));
                if (delta > hpFrameMaxAbs) hpFrameMaxAbs = delta;
                if (delta > 1e-4) ++hpFrameViol;
            }

            if (v.pVertex) {
                vi.normal[0] = v.pVertex->vertexNormal.x;
                vi.normal[1] = v.pVertex->vertexNormal.y;
                vi.normal[2] = v.pVertex->vertexNormal.z;
            } else {
                vi.normal[0] = vi.normal[1] = 0.0f;
                vi.normal[2] = 1.0f;
            }

            // Pack flags (Phase 1 bits 0-3 + Phase 2 bits 4-7)
            uint32_t flags = 0;
            if (v.pVertex) {
                if (v.pVertex->shadow)       flags |= GPU_VERT_SHADOW;
                if (v.calcThisFrame & 1)      flags |= GPU_VERT_CALCFRAME_LIGHT;
                if (v.pVertex->water & 1)     flags |= GPU_VERT_WATER;
                if (v.pVertex->water & 128)   flags |= GPU_VERT_WATER_ANIM_NEG;
                if (v.pVertex->water & 64)    flags |= GPU_VERT_WATER_ANIM_POS;
                if (v.calcThisFrame & 2)      flags |= GPU_VERT_CALCFRAME_WATER;
            }
            vi.flags = flags;
        }

        // Step 7: MC2_HAZE_PARITY — fold this frame into running totals, emit a
        // summary every 600 frames (matches the file's 600-frame cadence).
        if (s_hazeParity) {
            if (hpFrameMaxAbs > s_hazeParityMaxAbsDelta) s_hazeParityMaxAbsDelta = hpFrameMaxAbs;
            s_hazeParityViolations += hpFrameViol;
            ++s_hazeParitySummaryFrames;
            if (s_hazeParitySummaryFrames % 600 == 0) {
                MC2_EVENT_OUT(Terrain, "HAZE_PARITY", 1,
                    "event=summary frames={} max_abs_delta={:g} violations={}",
                    static_cast<unsigned long long>(s_hazeParitySummaryFrames),
                    s_hazeParityMaxAbsDelta,
                    static_cast<unsigned long long>(s_hazeParityViolations));
                fflush(stdout);
            }
        }

        // Upload only the visible window range [packMinVN, packMaxVN].
        // For a 1k map (N=1020) with a 200-vertex view window this is ~6.5 MB
        // instead of the previous ~32 MB full-buffer upload.
        if (packMinVN <= packMaxVN) {
            const GLintptr  uploadOffset = static_cast<GLintptr >(packMinVN * sizeof(GpuTerrainVertexInput));
            const GLsizeiptr uploadBytes = static_cast<GLsizeiptr>((packMaxVN - packMinVN + 1) * sizeof(GpuTerrainVertexInput));
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(s_vertexInputSsbo.backendHandle));
            glBufferSubData(GL_SHADER_STORAGE_BUFFER, uploadOffset, uploadBytes,
                            s_packBuf.data() + packMinVN);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        }
    }

    // --- Pack light input SSBO (lights starting at index 2 — first two factored into sun) ---
    // quad.cpp:1325: "First two lights are already factored into the above equations!"
    // The shader receives only lights [2..N-1]; u_numLights = count of those.
    uint32_t numLights = 0;
    if (eye) {
        // LIGHT-POOL-VIS-GATE-1 (2026-07-21, fog-of-war leak): the legacy CPU
        // consumer honors TG_Light::active (tgl.cpp:2123) but this pack loop
        // did not — INACTIVE lights (mover appearances set active=false for
        // enemy movers without visual contact; camera sets it false for
        // off-screen point/spot lights) were packed anyway, so night light
        // pools rendered at red sensor-blip positions of UNSEEN enemies
        // (positional intel leak) and off-screen lights burned pool budget.
        // Skip !active — legacy parity. Killswitch MC2_LIGHT_POOL_VIS_GATE=0.
        static const bool s_visGate = mc2::gate::get<"MC2_LIGHT_POOL_VIS_GATE">();   // default ON
        // NIGHT-LIGHT-POOLS-2a: mover-light classification for the pool
        // consumer (frag dims mover pools; the compute vertex-lighting
        // consumer ignores the pad field — no legacy deviation). Slot tags
        // are recorded unconditionally at the mover registration sites
        // (mc2_spotlight_diag::tag_slot).
        static std::vector<TG_LightPtr> s_moverLightPtrs;
        s_moverLightPtrs.clear();
        if (TG_LightPtr* wl = eye->getWorldLights()) {
            for (long s = 0; s < MAX_LIGHTS_IN_WORLD; ++s) {
                if (!wl[s]) continue;
                mc2_spotlight_diag::SourceClass src;
                if (mc2_spotlight_diag::is_e_slot(s, &src)
                    && (src == mc2_spotlight_diag::Mech
                        || src == mc2_spotlight_diag::Gv))
                    s_moverLightPtrs.push_back(wl[s]);
            }
        }
        auto lightIsMover = [&](TG_LightPtr lp) -> bool {
            for (TG_LightPtr p : s_moverLightPtrs)
                if (p == lp) return true;
            return false;
        };
        // LIGHT-REALISM-1: pool-shape knobs (see the reshape block below).
        // CONE-SPILL-POLICY-1: default ON (user-approved variant B; free
        // with MC2_LIGHT_GRID=1 per PERF-HARNESS-TRUTH-1). =0 reverts.
        // Keep in lockstep with mclib/cone_glow.cpp realism_enabled().
        static const bool s_lightRealism = mc2::gate::get<"MC2_LIGHT_REALISM">();
        // AUDIT-TRUTH-1 variant B (user-approved; A was 1.6): row default 2.2, clampRange(0.25, 4).
        static const float s_lightRealismPoolSpill = mc2::gate::get<"MC2_LIGHT_REALISM_POOL_SPILL">();
        // AUDIT-TRUTH-1 variant B (user-approved; A was 0.15): row default 0, clampRange(0, 1).
        static const float s_lightRealismPoolCore = mc2::gate::get<"MC2_LIGHT_REALISM_POOL_CORE">();
        // GHOST-LIGHT-DUMP-1 (batch-7 item 5, env MC2_LIGHT_POOL_DUMP=1):
        // periodic dump of every light the pool pack ACCEPTS this frame, with
        // world-light slot + (E)-owner attribution (mc2_spotlight_diag), so a
        // rendered pool with no visible caster can be traced to its owner.
        // Default-off, zero cost when unset.
        static const bool s_poolDump = mc2::gate::get<"MC2_LIGHT_POOL_DUMP">();
        static unsigned long s_poolDumpFrame = 0;
        const bool dumpThisFrame = s_poolDump && ((s_poolDumpFrame++ % 240) == 0);
        std::unordered_map<const void*, long> ptrToSlot;
        if (dumpThisFrame) {
            if (TG_LightPtr* wl = eye->getWorldLights())
                for (long s = 0; s < MAX_LIGHTS_IN_WORLD; ++s)
                    if (wl[s]) ptrToSlot[wl[s]] = s;
            MC2_EVENT(Terrain, "LIGHT_POOL_DUMP", 1,
                "frame_tick={} isNight={} nTerrainLights={}",
                static_cast<unsigned long>(s_poolDumpFrame), eye->isNight ? 1 : 0,
                static_cast<long>(eye->getNumTerrainLights()));
        }
        const long nLights = eye->getNumTerrainLights();
        // LIGHT-POOL-CAP-1: measure real DEMAND, not just what fit. The loop no
        // longer breaks at the cap — it keeps scanning and counts the lights it
        // had to drop, so the log states the worst-case slot requirement for a
        // scene instead of just reporting "full". s_poolWanted counts every
        // light that passed the visibility gate and therefore wanted a slot.
        uint32_t poolWanted = 0, poolDropped = 0;
        for (long i = 2; i < nLights; ++i) {
            TG_LightPtr tl = eye->getTerrainLight(i);
            if (!tl) continue;
            if (dumpThisFrame) {
                long slot = -1;
                auto its = ptrToSlot.find(tl);
                if (its != ptrToSlot.end()) slot = its->second;
                mc2_spotlight_diag::SourceClass src;
                const bool eOwned = mc2_spotlight_diag::is_e_slot(slot, &src);
                // owner prints like printf %p on MSVC x64: 16 upper-case hex digits, no 0x.
                MC2_EVENT(Terrain, "LIGHT_POOL_DUMP", 1,
                    "idx={} slot={} type={} active={} ownerEnabled={} "
                    "pos=({:.1f},{:.1f},{:.1f}) argb={:08x} far={:.0f} class={} owner={:016X} packed={}",
                    static_cast<long>(i), static_cast<long>(slot), static_cast<long>(tl->lightType),
                    tl->active ? 1 : 0, static_cast<int>(tl->ownerEnabled),
                    tl->position.x, tl->position.y, tl->position.z,
                    static_cast<unsigned long>(tl->GetaRGB()), tl->farDistance,
                    eOwned ? (src == mc2_spotlight_diag::Bldg ? "bldg"
                              : src == mc2_spotlight_diag::Mech ? "mech" : "gv")
                           : "other",
                    reinterpret_cast<std::uintptr_t>(mc2_spotlight_diag::slot_owner(slot)),
                    (!s_visGate || tl->active) ? 1 : 0);
                std::fflush(stderr);
            }
            if (s_visGate && !tl->active) continue;   // LIGHT-POOL-VIS-GATE-1
            ++poolWanted;                              // LIGHT-POOL-CAP-1 demand
            if (numLights >= s_maxLights) { ++poolDropped; continue; }
            GpuTerrainLight& gl = s_lightBuf[numLights];
            gl.position[0]   = tl->position.x;
            gl.position[1]   = tl->position.y;
            gl.position[2]   = tl->position.z;
            // Map MC2 light types to GPU constants
            switch (tl->lightType) {
                case TG_LIGHT_POINT:   gl.lightType = 1u; break;  // TG_LIGHT_POINT_GPU
                case TG_LIGHT_SPOT:    gl.lightType = 2u; break;  // TG_LIGHT_SPOT_GPU
                case TG_LIGHT_TERRAIN: gl.lightType = 3u; break;  // TG_LIGHT_TERRAIN_GPU
                default:               gl.lightType = 0u; break;
            }
            // Extract RGB from packed aRGB (0xAARRGGBB) → normalize to 0..1
            const DWORD argb = tl->GetaRGB();
            gl.color[0]        = static_cast<float>((argb >> 16) & 0xFFu) / 255.0f;  // R
            gl.color[1]        = static_cast<float>((argb >>  8) & 0xFFu) / 255.0f;  // G
            gl.color[2]        = static_cast<float>( argb        & 0xFFu) / 255.0f;  // B
            gl.closeDistance   = tl->closeDistance;
            gl.farDistance     = tl->farDistance;
            gl.oneOverDistance = tl->oneOverDistance;
            // LIGHT-REALISM-1 (2026-07-24, user rejection of the mc2_10 night
            // look: "the light needs to spill from there, not be just in random
            // spots ... mimic LIGHT SOURCES (real ones)").
            //
            // TWO artefacts come straight out of these three packed fields, so
            // both are fixable HERE — no shader edit, and every consumer (the
            // LOD-chunk frag pool loop, terrain_overlay, mech/static_prop, and
            // the compute vertex-lighting path) gets the same coherent shape.
            //
            //  (a) HARD DISC. The falloff is literally `1.0` for len <= close-
            //      Distance — a flat-topped plateau. That plateau IS the
            //      "hard-edged bright disc": a saturated core with an abrupt
            //      shoulder where the ramp starts. Shrinking closeDistance
            //      collapses the plateau, leaving a smooth peak-and-decay bell
            //      (the existing MC2_LIGHT_POOL_SOFT smoothstep then rounds the
            //      tail), i.e. bright at the source with smooth falloff out.
            //
            //  (b) DISCONNECTED BLOB. LIGHT-CONE-MODELS-1 Stage C moves the
            //      pool centre to the far end of the beam (node pos + aim x
            //      cone length x MC2_LIGHT_CONE_SPILL). Correct aim, but with
            //      the stock radius the lit patch stops short of the lamp, so
            //      it reads as an unrelated bright spot with dark ground
            //      between it and its emitter. Widening farDistance stretches
            //      the pool back over the emitter, making cone and ground pool
            //      ONE continuous light.
            //
            // oneOverDistance is the ramp slope 1/(far-close) (TG_Light::
            // GetFalloff) and MUST be recomputed in lockstep or the ramp
            // detaches from the radii. Gate default-OFF -> untouched -> byte-
            // identical.
            if (s_lightRealism)
            {
                const float spill = s_lightRealismPoolSpill;   // farDistance scale
                const float core  = s_lightRealismPoolCore;    // closeDistance scale
                float farD   = tl->farDistance * spill;
                float closeD = tl->closeDistance * core;
                if (farD < 1.0f) farD = 1.0f;
                if (closeD < 0.0f) closeD = 0.0f;
                if (closeD > farD - 1.0f) closeD = farD - 1.0f;
                gl.closeDistance   = closeD;
                gl.farDistance     = farD;
                gl.oneOverDistance = 1.0f / (farD - closeD);
            }
            // NIGHT-LIGHT-POOLS-2a: mover flag in the pad (pool frag dims
            // mover-attached lights; compute consumer never reads pads).
            gl._pad1           = lightIsMover(tl) ? 1.0f : 0.0f;
            gl._pad2           = 0.0f;
            ++numLights;
        }

        // LIGHT-POOL-CAP-1: report peak demand + any starvation. Always-on but
        // rate-limited to transitions of the high-water mark, so a scene that
        // never exceeds the cap prints one line and then stays silent, while a
        // starving scene names the number of slots it actually needs.
        {
            static uint32_t s_peakWanted = 0;
            static bool     s_warnedDrop = false;
            if (poolWanted > s_peakWanted) {
                s_peakWanted = poolWanted;
                MC2_DIAGFE(stderr,
                    "[LIGHT_POOL_CAP v1] peak_wanted=%u packed=%u cap=%u dropped=%u\n",
                    s_peakWanted, numLights, s_maxLights, poolDropped);
                std::fflush(stderr);
            }
            if (poolDropped > 0 && !s_warnedDrop) {
                s_warnedDrop = true;
                MC2_DIAGFE(stderr,
                    "[LIGHT_POOL_CAP v1] STARVED: %u light(s) dropped at cap=%u "
                    "(wanted=%u). Raise MC2_LIGHT_POOL_MAX.\n",
                    poolDropped, s_maxLights, poolWanted);
                std::fflush(stderr);
            }
        }

        if (numLights > 0) {
            const GLsizeiptr uploadBytes = static_cast<GLsizeiptr>(numLights * sizeof(GpuTerrainLight));
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(s_lightInputSsbo.backendHandle));
            glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, uploadBytes, s_lightBuf.data());
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        }
    }
    // LIGHT-CLUSTER-PERF-1: bin this frame's pool into the world-space XY grid so
    // the forward consumers loop only over lights whose circle covers the
    // fragment. Must run AFTER the pack loop (reads s_lightBuf[0..numLights)) and
    // is a no-op that publishes dim=0 (= linear loop) when gated off or the pool
    // is too small to be worth binning.
    BuildAndUploadLightGrid(numLights);
    // NIGHT-LIGHT-POOLS-1: publish this frame's packed count for the forward
    // consumer (LOD-chunk terrain per-fragment light loop).
    s_lastNumLights = numLights;

    // STEP 3.4P: publish the exact inputs this frame's GL dispatch uses for the native Vulkan producer. The scalar block mirrors the uniform
    // uploads below value for value.
    {
        VkProducerSnapshot& sn = s_vkSnap;
        sn = VkProducerSnapshot{};
        sn.numVertices  = s_numVertices;
        sn.vertexOffset = (packMinVN <= packMaxVN) ? packMinVN : 0u;
        sn.windowCount  = (packMinVN <= packMaxVN) ? (packMaxVN - packMinVN + 1u) : 0u;
        sn.window       = (packMinVN <= packMaxVN) ? (s_packBuf.data() + packMinVN) : nullptr;
        sn.numLights    = numLights;
        sn.lights       = s_lightBuf.data();
        if (eye) {
            sn.sunDir[0] = eye->lightDirection.x; sn.sunDir[1] = eye->lightDirection.y; sn.sunDir[2] = eye->lightDirection.z;
            sn.lightRgb[0] = static_cast<int>(eye->lightRed);   sn.lightRgb[1] = static_cast<int>(eye->lightGreen);   sn.lightRgb[2] = static_cast<int>(eye->lightBlue);
            sn.ambientRgb[0] = static_cast<int>(eye->ambientRed); sn.ambientRgb[1] = static_cast<int>(eye->ambientGreen); sn.ambientRgb[2] = static_cast<int>(eye->ambientBlue);
            sn.fogStart = eye->fogStart;
            sn.fogFull  = eye->fogFull;
            sn.camPos[0] = -eye->getCameraOrigin().x; sn.camPos[1] = eye->getCameraOrigin().z; sn.camPos[2] = eye->getCameraOrigin().y;
        }
        sn.baseRgb[0] = static_cast<int>((BaseVertexColor >> 16) & 0xFFu);
        sn.baseRgb[1] = static_cast<int>((BaseVertexColor >>  8) & 0xFFu);
        sn.baseRgb[2] = static_cast<int>( BaseVertexColor        & 0xFFu);
        sn.rain       = TerrainQuad::rainLightLevel;
        sn.lightening = static_cast<int>(TerrainQuad::lighteningLevel);
        sn.useFog     = useFog ? 1 : 0;
        sn.rendererSW = (Environment.Renderer == 3) ? 1 : 0;
        const float pm = Terrain::worldUnitsMapSide / 2.0f - Terrain::worldUnitsPerVertex * 2.0f;
        sn.playArea[0] = -pm; sn.playArea[1] = -pm; sn.playArea[2] = pm; sn.playArea[3] = pm;
        sn.minHaze = Camera::MinHazeDistance;
        sn.maxClip = Camera::MaxClipDistance;
        sn.distFactor = Camera::DistanceFactor;
        s_vkSnapValid = true;
    }

    // --- Dispatch compute ---
    glUseProgram(s_program);

    // Bind SSBOs
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, TL_VERTEX_INPUT_BINDING, static_cast<GLuint>(s_vertexInputSsbo.backendHandle));
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, TL_LIGHT_INPUT_BINDING,  static_cast<GLuint>(s_lightInputSsbo.backendHandle));
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, TL_OUTPUT_BINDING,        static_cast<GLuint>(s_computeOutputSsbo.backendHandle));

    // Set per-frame uniforms via runtime-resolved locations.
    glUniform1i(s_uniformLocs[TL_UNI_NUM_VERTICES], static_cast<GLint>(s_numVertices));
    glUniform1i(s_uniformLocs[TL_UNI_NUM_LIGHTS],   static_cast<GLint>(numLights));

    if (eye) {
        // Sun direction (eye->lightDirection — already computed by camera each frame)
        glUniform3f(s_uniformLocs[TL_UNI_SUN_LIGHT_DIR],
                    eye->lightDirection.x,
                    eye->lightDirection.y,
                    eye->lightDirection.z);
        // getLightRed/Green/Blue channels (unsigned char, 0-255)
        glUniform1i(s_uniformLocs[TL_UNI_LIGHT_RED],    static_cast<GLint>(eye->lightRed));
        glUniform1i(s_uniformLocs[TL_UNI_LIGHT_GREEN],  static_cast<GLint>(eye->lightGreen));
        glUniform1i(s_uniformLocs[TL_UNI_LIGHT_BLUE],   static_cast<GLint>(eye->lightBlue));
        glUniform1i(s_uniformLocs[TL_UNI_AMBIENT_RED],  static_cast<GLint>(eye->ambientRed));
        glUniform1i(s_uniformLocs[TL_UNI_AMBIENT_GREEN],static_cast<GLint>(eye->ambientGreen));
        glUniform1i(s_uniformLocs[TL_UNI_AMBIENT_BLUE], static_cast<GLint>(eye->ambientBlue));
        // Fog parameters
        glUniform1f(s_uniformLocs[TL_UNI_FOG_START], eye->fogStart);
        glUniform1f(s_uniformLocs[TL_UNI_FOG_FULL],  eye->fogFull);
    }

    // BaseVertexColor (DWORD 0xAARRGGBB)
    glUniform1i(s_uniformLocs[TL_UNI_BASE_COLOR_R], static_cast<GLint>((BaseVertexColor >> 16) & 0xFFu));
    glUniform1i(s_uniformLocs[TL_UNI_BASE_COLOR_G], static_cast<GLint>((BaseVertexColor >>  8) & 0xFFu));
    glUniform1i(s_uniformLocs[TL_UNI_BASE_COLOR_B], static_cast<GLint>( BaseVertexColor        & 0xFFu));

    // rainLightLevel + lighteningLevel (TerrainQuad statics)
    glUniform1f(s_uniformLocs[TL_UNI_RAIN_LEVEL],      TerrainQuad::rainLightLevel);
    glUniform1i(s_uniformLocs[TL_UNI_LIGHTNING_LEVEL], static_cast<GLint>(TerrainQuad::lighteningLevel));

    // useFog (terrain.cpp global)
    glUniform1i(s_uniformLocs[TL_UNI_USE_FOG], useFog ? 1 : 0);

    // Environment.Renderer == 3 => software renderer (skip lighting)
    glUniform1i(s_uniformLocs[TL_UNI_RENDERER_SW], (Environment.Renderer == 3) ? 1 : 0);

    // Step 7 (VPL retirement): inline-worldPos hazeFactor uniforms.
    // Play-area bounds == Terrain::IsGameSelectTerrainPosition (terrain.cpp:687-700).
    // Camera world pos swizzle matches terrain.cpp:1417-1419 exactly:
    //   cameraPos.x = -getCameraOrigin().x; .y = getCameraOrigin().z; .z = getCameraOrigin().y
    {
        const float m = Terrain::worldUnitsMapSide / 2.0f - Terrain::worldUnitsPerVertex * 2.0f;
        glUniform4f(s_uniformLocs[TL_UNI_PLAY_AREA], -m, -m, m, m);
        if (eye) {
            glUniform3f(s_uniformLocs[TL_UNI_CAM_WORLD_POS],
                        -eye->getCameraOrigin().x,
                         eye->getCameraOrigin().z,
                         eye->getCameraOrigin().y);
        }
        glUniform1f(s_uniformLocs[TL_UNI_MIN_HAZE],    Camera::MinHazeDistance);
        glUniform1f(s_uniformLocs[TL_UNI_MAX_CLIP],    Camera::MaxClipDistance);
        glUniform1f(s_uniformLocs[TL_UNI_DIST_FACTOR], Camera::DistanceFactor);
    }

    // PERF-FIX: limit dispatch to the visible window [packMinVN, packMaxVN].
    // Previously dispatched for all s_numVertices (e.g. 16,257 groups for a 1020-map).
    // Now dispatches only for the ~200x200 visible window range (~3,200 groups, 5x fewer).
    // The shader adds u_vertexOffset to gl_GlobalInvocationID.x so it still maps to the
    // correct SSBO indices.  u_numVertices remains the absolute upper-bound guard.
    const uint32_t dispatchStart = (packMinVN <= packMaxVN) ? packMinVN : 0u;
    const uint32_t dispatchCount = (packMinVN <= packMaxVN) ? (packMaxVN - packMinVN + 1u) : 0u;
    const uint32_t numGroups     = (dispatchCount + 63u) / 64u;

    if (numGroups > 0) {
        glUniform1i(s_uniformLocs[TL_UNI_VERTEX_OFFSET], static_cast<GLint>(dispatchStart));
        glDispatchCompute(numGroups, 1, 1);
    }

    glUseProgram(0);

    // First-frame always-on diagnostic — prints once per process to confirm
    // ambient/light values are non-zero and the range-dispatch looks sane.
    if (s_frameIndex <= 1) {
        MC2_DIAGF("[TERRAIN_LIGHTING_GPU v1] event=first_dispatch "
               "frame=%llu totalVerts=%u dispatchRange=[%u,%u] numGroups=%u "
               "eye=%s ambR=%d ambG=%d ambB=%d lightR=%d lightG=%d lightB=%d\n",
               static_cast<unsigned long long>(s_frameIndex), s_numVertices,
               dispatchStart, dispatchStart + dispatchCount, numGroups,
               eye ? "ok" : "NULL",
               eye ? static_cast<int>(eye->ambientRed)   : -1,
               eye ? static_cast<int>(eye->ambientGreen) : -1,
               eye ? static_cast<int>(eye->ambientBlue)  : -1,
               eye ? static_cast<int>(eye->lightRed)     : -1,
               eye ? static_cast<int>(eye->lightGreen)   : -1,
               eye ? static_cast<int>(eye->lightBlue)    : -1);
        fflush(stdout);
    }

    // --- Memory barrier: ensure compute shader writes complete ---
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    // --- Copy compute output → staging ring slot (VRAM→BAR) ---
    const GLintptr  slotOff = 0;
    const GLsizeiptr slotSz = static_cast<GLsizeiptr>(s_outputSlotBytes);
    glBindBuffer(GL_COPY_READ_BUFFER,  static_cast<GLuint>(s_computeOutputSsbo.backendHandle));
    glBindBuffer(GL_COPY_WRITE_BUFFER, s_stagingRing[s_currentSlot]);
    glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, slotOff, slotOff, slotSz);
    glBindBuffer(GL_COPY_READ_BUFFER,  0);
    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);

    // --- Make persistent-map copy visible to CPU ---
    glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT);

    // --- Delete any prior fence for this slot, then place new fence ---
    if (s_stagingFence[s_currentSlot]) {
        glDeleteSync(s_stagingFence[s_currentSlot]);
        s_stagingFence[s_currentSlot] = nullptr;
    }
    s_stagingFence[s_currentSlot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}

// ---------------------------------------------------------------------------
// CopyResultsToVertexPool — Stage 3: 3-slot T1/T2/T3 non-blocking tryConsume.
// Copies GPU staging ring → vertices[i]->lightRGB / ->fogRGB.
//
// Under parity mode (IsParityCheckEnabled()==true): returns early.
//   The comparator (Parity_CompareFrame) reads GPU values from the mapped ring
//   slot directly via GetMappedOutputForParity() with GL_TIMEOUT_IGNORED.
//   CPU body runs authoritative for vertices[i] in parity mode.
//   (plan v2 MIN-2 fold — avoid CPU-overwrites-GPU dataflow ambiguity.)
//
// Under authoritative mode (IsEnabled() && !IsParityCheckEnabled()):
//   Non-blocking tryConsume with three-tier fallback (design doc Q2):
//     T1: N-1 fence signaled → copy (1-frame latency, normal case).
//     T2: N-2 fence signaled → copy (2-frame latency, emit N2 counter).
//     T3: neither ready → skip (retain prior-frame values, emit conservative counter).
//   glClientWaitSync timeout is ALWAYS 0 on hot path. Never GL_TIMEOUT_IGNORED.
// ---------------------------------------------------------------------------
void CopyResultsToVertexPool(TerrainQuad* quadList, int numberQuads) {
    if (!IsEnabled() || !s_initialized || s_shaderBad) return;

    // Under parity mode: do NOT write GPU output into vertex pool.
    // Parity_CompareFrame reads the mapped ring directly.
    if (IsParityCheckEnabled()) return;

    // ---------------------------------------------------------------------------
    // Non-blocking tryConsume (T1/T2/T3) — timeout=0 always.
    // Mirrors readback_tryConsume() pattern in gpu_cull_readback.cpp:317-424.
    // ---------------------------------------------------------------------------
    const uint32_t n1Slot = (s_currentSlot + RING_FRAMES - 1u) % RING_FRAMES;
    const uint32_t n2Slot = (s_currentSlot + RING_FRAMES - 2u) % RING_FRAMES;

    // Helper: non-blocking fence check (timeout=0).
    auto isFenceReady = [](uint32_t slot) -> bool {
        if (!s_stagingFence[slot]) return false;
        GLenum result = glClientWaitSync(s_stagingFence[slot], 0, 0);
        return (result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED);
    };

    uint32_t readSlot = RING_FRAMES;  // sentinel: no slot selected
    bool     isN2     = false;

    if (isFenceReady(n1Slot)) {
        glDeleteSync(s_stagingFence[n1Slot]);
        s_stagingFence[n1Slot] = nullptr;
        readSlot = n1Slot;
        isN2     = false;
    } else if (isFenceReady(n2Slot)) {
        glDeleteSync(s_stagingFence[n2Slot]);
        s_stagingFence[n2Slot] = nullptr;
        readSlot = n2Slot;
        isN2     = true;
    }

    ++s_fallbackSummaryFrames;

    if (readSlot == RING_FRAMES) {
        // T3: neither slot ready — retain prior DRAM shadow, vertex pool not updated.
        ++s_lightingFallbackConservative;
    } else {
        // T1 or T2: memcpy BAR staging slot → DRAM shadow (sequential WC read, fast),
        // then scatter DRAM shadow values into vertex pool (DRAM→DRAM, cached, fast).
        //
        // WHY: BAR persistent-mapped memory is WC (write-combining) on x86/AMD.
        // Random-indexed reads from WC memory bypass CPU caches and cause PCIe
        // round-trips. At 14K quad × 4 vertices × indexed reads = ~56K random WC reads
        // → massive perf regression. Sequential memcpy from WC is stream-friendly (WC
        // read coalesces naturally at 64B cache-line granularity) and is 100-1000× faster
        // than random indexed WC reads. After memcpy, all indexing is in DRAM.
        const GpuTerrainLightingOutput* barSrc =
            static_cast<const GpuTerrainLightingOutput*>(s_stagingMapped[readSlot]);

        if (barSrc && !s_dramShadow.empty()) {
            // Step 1: sequential BAR → DRAM copy (stream-friendly WC read).
            memcpy(s_dramShadow.data(), barSrc, s_numVertices * sizeof(GpuTerrainLightingOutput));

            // Step 2: scatter DRAM shadow into vertex pool (DRAM→DRAM, cached).
            const GpuTerrainLightingOutput* src = s_dramShadow.data();
            for (int q = 0; q < numberQuads; ++q) {
                const TerrainQuad& quad = quadList[q];
                for (int vi = 0; vi < 4; ++vi) {
                    Vertex* v = quad.vertices[vi];
                    if (!v) continue;
                    if (v->vertexNum < 0 || static_cast<uint32_t>(v->vertexNum) >= s_numVertices) continue;
                    const GpuTerrainLightingOutput& out = src[v->vertexNum];
                    v->lightRGB = out.lightRGB;
                    v->fogRGB   = out.fogRGB;
                }
            }
        }

        if (isN2) {
            ++s_lightingFallbackN2;
        } else {
            ++s_lightingFallbackN1;
        }
    }

    // 600-frame fallback summary.
    if (s_fallbackSummaryFrames % 600 == 0) {
        MC2_DIAGF("[TERRAIN_LIGHTING_GPU v1] event=fallback_summary frames=%llu "
               "n1=%llu n2=%llu conservative=%llu\n",
               static_cast<unsigned long long>(s_fallbackSummaryFrames),
               static_cast<unsigned long long>(s_lightingFallbackN1),
               static_cast<unsigned long long>(s_lightingFallbackN2),
               static_cast<unsigned long long>(s_lightingFallbackConservative));
        fflush(stdout);
    }
}

// ---------------------------------------------------------------------------
// Parity_CompareFrame — Stage 2 implementation
// Design doc Q3: walk quadList, filter calcThisFrame&1, index outputs[vertexNum].
// ---------------------------------------------------------------------------

// Kept out of line: inlined into the per-vertex compare loop, the format call's template body costs the
// parity pass about a third of its frame rate.
[[msvc::noinline]] static void Parity_PrintMismatch(uint64_t frame, int vn, const char* field,
                                 uint32_t legacy, uint32_t gpu)
{
    if (s_parityMismatchesThisFrame >= 16) return;  // throttle 16/frame
    ++s_parityMismatchesThisFrame;
    ++s_paritySummaryMismatches;
    MC2_EVENT(Terrain, "TERRAIN_LIGHTING_PARITY", 1,
              "event=mismatch frame={} vertex={} field={} legacy=0x{:08X} gpu=0x{:08X}",
              static_cast<unsigned long long>(frame), vn, field ? field : "?", legacy, gpu);
    fflush(stderr);
}

void Parity_CompareFrame(TerrainQuad* quadList, int numberQuads,
                         const GpuTerrainLightingOutput* mappedOutput)
{
    if (!IsParityCheckEnabled() || !mappedOutput) return;

    // Reset per-frame mismatch counter for this new frame
    s_parityMismatchesThisFrame = 0;
    ++s_paritySummaryFrames;

    long long vertsChecked = 0;

    for (int q = 0; q < numberQuads; ++q) {
        const TerrainQuad& quad = quadList[q];
        for (int i = 0; i < 4; ++i) {
            const Vertex* v = quad.vertices[i];
            if (!v) continue;
            if (v->vertexNum < 0) continue;       // off-map vertex
            if (!(v->calcThisFrame & 1)) continue; // CPU didn't write this vertex this frame

            const int vn = v->vertexNum;
            ++vertsChecked;

            // Compare lightRGB
            uint32_t legacyLight = v->lightRGB;
            uint32_t gpuLight    = mappedOutput[vn].lightRGB;
            if (legacyLight != gpuLight) {
                Parity_PrintMismatch(s_frameIndex, vn, "lightRGB", legacyLight, gpuLight);
            }

            // Compare fogRGB
            uint32_t legacyFog = v->fogRGB;
            uint32_t gpuFog    = mappedOutput[vn].fogRGB;
            if (legacyFog != gpuFog) {
                Parity_PrintMismatch(s_frameIndex, vn, "fogRGB", legacyFog, gpuFog);
            }
        }
    }

    s_paritySummaryVerts += vertsChecked;

    // 600-frame summary (mirrors gos_terrain_indirect.cpp:291 pattern)
    if (s_paritySummaryFrames % 600 == 0) {
        MC2_EVENT(Terrain, "TERRAIN_LIGHTING_PARITY", 1,
                  "event=summary frames={} verts_checked={} total_mismatches={}",
                  static_cast<long long>(s_paritySummaryFrames), static_cast<long long>(s_paritySummaryVerts),
                  static_cast<long long>(s_paritySummaryMismatches));
        fflush(stderr);
    }
}

// Helper: synchronously wait and return mapped output for parity comparison.
// ONLY called from terrain.cpp parity path — uses GL_TIMEOUT_IGNORED.
// Production hot path NEVER calls this.
const GpuTerrainLightingOutput* GetMappedOutputForParity()
{
    if (!IsParityCheckEnabled() || !s_initialized) return nullptr;

    // Synchronously wait on current frame's fence (parity mode — stall OK)
    if (s_stagingFence[s_currentSlot]) {
        glClientWaitSync(s_stagingFence[s_currentSlot],
                         GL_SYNC_FLUSH_COMMANDS_BIT,
                         GL_TIMEOUT_IGNORED);
    }
    return static_cast<const GpuTerrainLightingOutput*>(s_stagingMapped[s_currentSlot]);
}

} // namespace gos_terrain_lighting

// Slice B4 Stage 1b — C-linkage accessor for the per-vertex lighting output SSBO.
// Used by gos_terrain_mask_dispatch::DrawMaskSolid() to bind at binding=2.
// Defined in the same TU as s_computeOutputSsbo (which has internal linkage
// via the anonymous namespace); the implicit using-directive makes the name
// visible by unqualified lookup at file scope.
extern "C" GLuint gos_terrain_lighting_getOutputSSBO() {
    return static_cast<GLuint>(s_computeOutputSsbo.backendHandle);
}
