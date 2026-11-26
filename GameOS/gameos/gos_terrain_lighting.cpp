// CPU terrain-lighting inputs and light-grid ownership for native Vulkan consumers.
#include "gos_terrain_lighting.h"
#include "terrain_vertex_pack.h"
#include "../../mclib/terrain_vertex_dirty.h"
#include "light_pool_culling.h"
#include "beam_lights.h"
#include "../../code/render_view.h"
#include "local_light_shadow.h"   // LOCAL-LIGHTS-2: shadow tile selection
#include "vulkan_shadow_island_live.h"   // mc2_vk_local_shadows_capable
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1
#include "utils/mc2log.h"       // LOGGING-FACADE-P2: MC2_EVENT for the structured [TAG vN] lines
#include "Gates/gate.h"          // typed gate registry: gate::get<"MC2_X">()

#include "gos_profiler.h"  // ZoneScopedN, TracyGpuZone

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
static const bool s_hazeParity = false;
static double  s_hazeParityMaxAbsDelta = 0.0;
static uint64_t s_hazeParityViolations = 0;
static uint64_t s_hazeParitySummaryFrames = 0;

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------
namespace gos_terrain_lighting {

bool IsEnabled() {
    static const bool s_enabled = mc2::gate::get<"MC2_TERRAIN_LIGHTING_GPU">();
    return s_enabled;
}

} // namespace gos_terrain_lighting

namespace {

// ---------------------------------------------------------------------------
// Private file-scope constants
// ---------------------------------------------------------------------------
static bool     s_initialized  = false;

static uint32_t s_numVertices  = 0;
// STEP 3.4P: this frame's producer inputs for the native Vulkan lighting compute (see GetVkProducerSnapshot).
static gos_terrain_lighting::VkProducerSnapshot s_vkSnap{};
static bool s_vkSnapValid = false;
static uint32_t s_maxLights    = 0;

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
// The XY projection of each effective light sphere conservatively covers terrain
// and raised objects. Lists retain producer order and have no per-cell truncation:
// a dense cell pays for every accepted light instead of changing the image.
//
// Gate MC2_LIGHT_GRID (default ON, =0 killswitch -> linear full-pool loop).
// MC2_LIGHT_GRID_DIM overrides the cell count per axis (default 64).
// Small pools bypass the grid (below kLightGridMinLights the build+bind costs
// more than the loop it saves) by publishing dim=0, which the shader reads as
// "use the linear path".
// ---------------------------------------------------------------------------
static constexpr uint32_t kLightGridMinLights = 8u;   // below this: linear loop

static std::vector<uint32_t> s_gridHeader;   // 2 * dim * dim
static std::vector<uint32_t> s_gridIndices;
static std::vector<uint32_t> s_gridCounts;   // scratch: per-cell counter
static std::vector<uint32_t> s_objectPoolUpload;
static bool s_objectPoolDirty = true;
static float s_poolCamXY[2] = {0.0f, 0.0f};   // camera target of the last pack: which lights the capped object pool keeps
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

// Dense vertex pack buffer (persistent across frames to avoid per-frame alloc)
static gos_terrain_lighting::TerrainVertexPack s_vertexPack;
// Light pack buffer
static std::vector<gos_terrain_lighting::GpuTerrainLight> s_lightBuf;
static gos_terrain_lighting::LightGridInputs s_gridInputs;

// Build the CPU grid for s_lightBuf[0..numLights). Sets s_lightGridDim to 0
// when the grid is not in play (gate off, too few lights, degenerate bbox), which
// makes every consumer fall back to the exact linear loop.
static void BuildLightGrid(uint32_t numLights) {
    if (!s_gridInputs.update(std::span<const gos_terrain_lighting::GpuTerrainLight>(s_lightBuf.data(), numLights))) return;
    ZoneScopedN("Terrain.LightGrid.Rebuild");
    s_lightGridDim = 0;
    if (!LightGridEnabled() || numLights < kLightGridMinLights) return;

    // Both fragment consumers apply the same mover radius. Vertex lighting keeps
    // the original pool; only the conservative fragment candidate bounds shrink.
    static const float moverScale = mc2::gate::get<"MC2_LIGHT_POOL_MOVER_RADIUS">();
    const auto radius = [](const gos_terrain_lighting::GpuTerrainLight& l) {
        return l.farDistance * (l._pad1 > 0.5f && moverScale < 0.999f ? std::max(moverScale, 0.05f) : 1.0f);
    };
    float minX =  3.0e38f, minY =  3.0e38f;
    float maxX = -3.0e38f, maxY = -3.0e38f;
    uint32_t live = 0;
    for (uint32_t i = 0; i < numLights; ++i) {
        const gos_terrain_lighting::GpuTerrainLight& L = s_lightBuf[i];
        if (L.lightType == 0u) continue;      // shader skips these too
        const float r = radius(L);
        if (!(r > 0.0f)) continue;            // NaN-safe: excludes r<=0 and NaN
        ++live;
        minX = std::min(minX, L.position[0] - r);
        minY = std::min(minY, L.position[1] - r);
        maxX = std::max(maxX, L.position[0] + r);
        maxY = std::max(maxY, L.position[1] + r);
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
        const float r = radius(L);
        x0 = static_cast<int>(std::floor((L.position[0] - r - minX) * invW));
        y0 = static_cast<int>(std::floor((L.position[1] - r - minY) * invH));
        x1 = static_cast<int>(std::floor((L.position[0] + r - minX) * invW));
        y1 = static_cast<int>(std::floor((L.position[1] + r - minY) * invH));
        x0 = std::max(x0, 0);
        y0 = std::max(y0, 0);
        x1 = std::min(x1, static_cast<int>(dim) - 1);
        y1 = std::min(y1, static_cast<int>(dim) - 1);
    };
    // Circle/rectangle rejection removes the AABB corners without dropping any
    // sphere that can reach a fragment in this cell (including raised objects).
    const auto overlaps = [&](const gos_terrain_lighting::GpuTerrainLight& l, int x, int y) {
        const float xlo = minX + static_cast<float>(x) * cellW;
        const float ylo = minY + static_cast<float>(y) * cellH;
        const float dx = l.position[0] - std::clamp(l.position[0], xlo, xlo + cellW);
        const float dy = l.position[1] - std::clamp(l.position[1], ylo, ylo + cellH);
        const float r = radius(l) * 1.00001f;
        return dx * dx + dy * dy <= r * r;
    };
    uint32_t total = 0;
    for (uint32_t i = 0; i < numLights; ++i) {
        const gos_terrain_lighting::GpuTerrainLight& L = s_lightBuf[i];
        if (L.lightType == 0u || !(L.farDistance > 0.0f)) continue;
        int x0, y0, x1, y1; cellRange(L, x0, y0, x1, y1);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                if (!overlaps(L, x, y)) continue;
                ++s_gridCounts[y * dim + x]; ++total;
            }
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
                if (!overlaps(L, x, y)) continue;
                const uint32_t c = static_cast<uint32_t>(y) * dim + static_cast<uint32_t>(x);
                s_gridIndices[s_gridHeader[c * 2u] + s_gridCounts[c]++] = i;
            }
    }

    static const bool moverMax=true;
    static const bool poolSoft=true;
    static const float contactShadow=mc2::gate::get<"MC2_MOVER_CONTACT_SHADOW">();
    if(moverMax && contactShadow==0.0f) {
        gos_terrain_lighting::GridMoverDominance dominance(std::span{s_lightBuf}.first(numLights),poolSoft,moverScale);
        // appendFrameLights is called only by the main-thread render extract.
        // After swap this holds the old input; discard it before every rebuild.
        static std::vector<uint32_t> compact;
        compact.clear();compact.reserve(s_gridIndices.size());
        for(uint32_t c=0;c<cells;++c) {
            const uint32_t first=s_gridHeader[2*c],count=s_gridHeader[2*c+1];
            const uint32_t begin=static_cast<uint32_t>(compact.size());
            const float x0=minX+static_cast<float>(c%dim)*cellW,y0=minY+static_cast<float>(c/dim)*cellH;
            dominance.appendCell(std::span{s_lightBuf}.first(numLights),std::span{s_gridIndices}.subspan(first,count),x0,y0,x0+cellW,y0+cellH,compact);
            s_gridHeader[2*c]=begin;s_gridHeader[2*c+1]=static_cast<uint32_t>(compact.size())-begin;
        }
        s_gridIndices.swap(compact);total=static_cast<uint32_t>(s_gridIndices.size());
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
            peak = std::max(peak, static_cast<uint32_t>(s_gridHeader[c * 2u + 1u]));
        const bool newPeakCell = peak > s_peakCell;
        if (newPeakCell) {
            s_peakCell = peak;
            MC2_DIAGFE(stderr,
                "[LIGHT_GRID v1] dim=%u lights=%u stamped=%u peak_cell=%u avg_cell=%.2f\n",
                dim, numLights, total, s_peakCell,
                static_cast<double>(total) / static_cast<double>(cells));
            std::fflush(stderr);
        }
    }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
namespace gos_terrain_lighting {

// NIGHT-LIGHT-POOLS-1: forward-consumer accessors for the per-frame light pool.

uint32_t GetLightPoolLightCount() {
    return s_lastNumLights;
}

// LIGHT-CLUSTER-PERF-1: publish this frame's light grid. Returns false when the
// grid is not in play (gate MC2_LIGHT_GRID=0, pool below kLightGridMinLights,
// degenerate bbox, or nothing packed) — consumers then use the exact linear
// full-pool loop, which is the killswitch contract.

// Native consumers borrow CPU views until the next pack or mission reset.
const GpuTerrainLight* GetLightPoolCpu(uint32_t* outCount) {
    if (outCount) *outCount = s_initialized ? s_lastNumLights : 0u;
    if (!s_initialized || s_lightBuf.empty()) return nullptr;
    return s_lightBuf.data();
}

std::optional<LightGridCpu> GetLightGridCpu() {
    if (!s_initialized || s_lightGridDim == 0 ||
        s_gridHeader.empty() || s_gridIndices.empty())
        return std::nullopt;
    LightGridCpu out{};
    out.dim         = s_lightGridDim;
    out.minXY[0]    = s_lightGridMin[0];
    out.minXY[1]    = s_lightGridMin[1];
    out.invCell[0]  = s_lightGridInvCell[0];
    out.invCell[1]  = s_lightGridInvCell[1];
    out.header      = s_gridHeader.data();
    out.headerWords = static_cast<uint32_t>(s_gridHeader.size());
    out.indices     = s_gridIndices.data();
    out.indexWords  = static_cast<uint32_t>(s_gridIndices.size());
    return out;
}

const void* GetObjectLightPoolUpload(size_t& bytes) {
    if (s_objectPoolDirty || s_objectPoolUpload.empty()) {
        ObjectPoolHeader header{};
        const uint32_t count = std::min(s_lastNumLights, kObjectPoolLights);
        static bool warnedOverflow = false;
        if (s_lastNumLights > kObjectPoolLights && !warnedOverflow) {
            warnedOverflow = true;
            MC2_DIAGFE(stderr, "[OBJECT_POOL_CAP v1] wanted=%u cap=%u policy=nearest_to_camera dropped=%u\n",
                s_lastNumLights, kObjectPoolLights, s_lastNumLights - kObjectPoolLights);
        }
        // Over the cap, keep the lights whose circle reaches closest to the camera (not the first by source order),
        // in source order, and remap the grid's indices to the kept slots.
        std::vector<uint32_t> keptHeader, keptIndices;
        const auto gridOpt = GetLightGridCpu();
        const bool indexed = gridOpt.has_value();
        LightGridCpu grid = gridOpt.value_or(LightGridCpu{});
        if (s_lastNumLights > kObjectPoolLights) {
            std::vector<std::pair<float, uint32_t>> order(s_lastNumLights);
            for (uint32_t i = 0; i < s_lastNumLights; ++i) {
                const GpuTerrainLight& L = s_lightBuf[i];
                const float dx = L.position[0] - s_poolCamXY[0], dy = L.position[1] - s_poolCamXY[1];
                order[i] = { std::max(0.0f, std::sqrt(dx * dx + dy * dy) - L.farDistance), i };
            }
            std::nth_element(order.begin(), order.begin() + count, order.end());
            std::vector<uint32_t> keep(count);
            for (uint32_t k = 0; k < count; ++k) { const auto& [dist, index] = order[k]; (void)dist; keep[k] = index; }
            std::sort(keep.begin(), keep.end());
            std::vector<uint32_t> remap(s_lastNumLights, UINT32_MAX);
            for (uint32_t k = 0; k < count; ++k) {
                remap[keep[k]] = k;
                header.lights[k] = s_lightBuf[keep[k]];
            }
            if (indexed) {
                const uint32_t cells = grid.headerWords / 2u;
                keptHeader.resize(grid.headerWords);
                for (uint32_t c = 0; c < cells; ++c) {
                    keptHeader[c * 2u] = static_cast<uint32_t>(keptIndices.size());
                    for (uint32_t j = 0; j < grid.header[c * 2u + 1u]; ++j) {
                        const uint32_t m = remap[grid.indices[grid.header[c * 2u] + j]];
                        if (m != UINT32_MAX) keptIndices.push_back(m);
                    }
                    keptHeader[c * 2u + 1u] = static_cast<uint32_t>(keptIndices.size()) - keptHeader[c * 2u];
                }
                grid.header = keptHeader.data();
                grid.indices = keptIndices.data();
                grid.indexWords = static_cast<uint32_t>(keptIndices.size());
            }
        } else if (count) {
            std::memcpy(header.lights, s_lightBuf.data(), count * sizeof(GpuTerrainLight));
        }
        size_t words = 0;
        if (indexed) {
            header.minInvXY[0] = grid.minXY[0]; header.minInvXY[1] = grid.minXY[1];
            header.minInvXY[2] = grid.invCell[0]; header.minInvXY[3] = grid.invCell[1];
            header.gridInfo[0] = grid.dim; header.gridInfo[1] = grid.headerWords;
            header.gridInfo[2] = grid.indexWords;
            words = static_cast<size_t>(grid.headerWords) + grid.indexWords;
        }
        s_objectPoolUpload.resize(sizeof(header) / 4 + words);
        std::memcpy(s_objectPoolUpload.data(), &header, sizeof(header));
        if (indexed) {
            auto* tail = s_objectPoolUpload.data() + sizeof(header) / 4;
            std::memcpy(tail, grid.header, grid.headerWords * sizeof(uint32_t));
            std::memcpy(tail + grid.headerWords, grid.indices, grid.indexWords * sizeof(uint32_t));
        }
        s_objectPoolDirty = false;
    }
    bytes = s_objectPoolUpload.size() * sizeof(uint32_t);
    return s_objectPoolUpload.data();
}

// Shared object-pool parameters for native mech and prop consumers.
void GetObjectLightPoolParams(ObjectLightPoolParams* out) {
    if (!out) return;
    static const bool s_objects = true;   // default ON
    // Knob values lockstep with the terrain-chunk consumer defaults
    // (gos_terrain_lod_chunk.cpp / gameos_graphics.cpp overlay path).
    static const bool s_poolSoft = true;
    // Rows: MOVER_DIM default 0.25 range(0, 1); MOVER_RADIUS default 0.6 range (0, 1] (exclusive lower
    // bound); out of range falls back to the default.
    static const float s_poolMoverDim = mc2::gate::get<"MC2_LIGHT_POOL_MOVER_DIM">();
    static const float s_poolMoverRadius = mc2::gate::get<"MC2_LIGHT_POOL_MOVER_RADIUS">();
    static const bool s_poolMoverMax = true;
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
        const uint32_t n    = GetLightPoolLightCount();
        if (s_initialized && n > 0) {
            out->count = static_cast<int>(std::min(n, kObjectPoolLights));
            uint32_t cpuN = 0;
            out->pool = GetLightPoolCpu(&cpuN);
            if (!out->pool || cpuN < static_cast<uint32_t>(out->count))
                { out->pool = nullptr; out->count = 0; }
        }
    }
}

// ---------------------------------------------------------------------------
// mission_init — per-mission, called alongside gpu_cull::compute_init()
// ---------------------------------------------------------------------------
void mission_init(uint32_t numVertices, uint32_t maxLights) {
    if (!IsEnabled()) return;

    // Reset the previous mission's CPU snapshots and grid readiness.
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
        s_maxLights = std::max(s_maxLights, want);
    }

    // Pre-size pack buffers
    s_vertexPack.reset(numVertices);
    mc2::terrain::vertexDirtyFeed.reset(numVertices);
    s_lightBuf.resize(s_maxLights);
    s_frameIndex   = 0;
    s_initialized  = true;

    MC2_DIAGF("[TERRAIN_LIGHTING_GPU v1] event=mission_init verts=%u lights=%u native_inputs=1\n",
           numVertices, s_maxLights);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// mission_shutdown — per-mission teardown
// ---------------------------------------------------------------------------
void mission_shutdown() {
    if (!s_initialized) return;

    s_lightGridDim         = 0;
    s_gridInputs = {};
    s_objectPoolDirty = true;
    s_objectPoolUpload.clear();

    s_numVertices     = 0;
    s_maxLights       = 0;
    s_frameIndex      = 0;
    s_initialized     = false;
    s_vkSnap = {};
    s_vkSnapValid = false;
    s_lastNumLights   = 0;  // NIGHT-LIGHT-POOLS-1: no pool lights across mission boundary

    MC2_DIAGF("[TERRAIN_LIGHTING_GPU v1] event=mission_shutdown\n");
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// BeginFrame and native snapshot access
// ---------------------------------------------------------------------------
bool GetVkProducerSnapshot(VkProducerSnapshot& out) {
    if (!s_vkSnapValid) return false;
    out = s_vkSnap;
    return true;
}

void BeginFrame() {
    s_vkSnapValid = false;   // STEP 3.4P: set again by this frame's PackAndDispatch
    if (!IsEnabled() || !s_initialized) return;
    ++s_frameIndex;
}

// ---------------------------------------------------------------------------
// PackAndDispatch: CPU inputs for the native compute dispatch
// ---------------------------------------------------------------------------
void PackAndDispatch() {
    if (!IsEnabled() || !s_initialized) return;

    ZoneScopedN("Terrain::TerrainLightingDispatch");

    // --- Stage 2: Dense vertex pack by vertexNum ---
    // Walk the camera-windowed vertex list (land->vertexList, numberVertices entries).
    // For each vertex, slot it at inputs[vertexNum] (dense map-stable index).
    // Keep the visible dispatch range. Only source-dirty vertices are repacked;
    // native consumers retain their own upload receipts across frame slots.
    uint32_t packMinVN = s_numVertices;   // sentinel: will be lowered to min written vn
    uint32_t packMaxVN = 0;               // sentinel: will be raised to max written vn

    if (land && land->getVertexList() && land->getNumVertices() > 0) {
        const int nv = land->getNumVertices();
        const VertexPtr vlist = land->getVertexList();

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
            packMinVN = std::min(packMinVN, uvn);
            packMaxVN = std::max(packMaxVN, uvn);

            s_vertexPack.update(v, mc2::terrain::vertexDirtyFeed.revision(uvn));

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
                hpFrameMaxAbs = std::max(hpFrameMaxAbs, delta);
                if (delta > 1e-4) ++hpFrameViol;
            }

        }

        // Step 7: MC2_HAZE_PARITY — fold this frame into running totals, emit a
        // summary every 600 frames (matches the file's 600-frame cadence).
        if (s_hazeParity) {
            s_hazeParityMaxAbsDelta = std::max(s_hazeParityMaxAbsDelta, hpFrameMaxAbs);
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
        static const bool s_visGate = true;   // default ON
        // NIGHT-LIGHT-POOLS-2a: mover-light classification for the pool
        // consumer (frag dims mover pools; the compute vertex-lighting
        // consumer ignores the pad field — no legacy deviation). Slot tags
        // are recorded unconditionally at the mover registration sites
        // (mc2_spotlight_diag::tag_slot).
        static RegisteredMoverLights moverLights;
        TG_LightPtr* worldLights = eye->getWorldLights();
        moverLights.refresh(eye, mc2_spotlight_diag::registration_revision(),
            std::span<TG_LightPtr>(worldLights, worldLights ? MAX_LIGHTS_IN_WORLD : 0),
            [](size_t slot) {
                const auto src = mc2_spotlight_diag::is_e_slot(eye, static_cast<long>(slot));
                return src == mc2_spotlight_diag::Mech || src == mc2_spotlight_diag::Gv;
            });
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
        static const bool s_poolDump = false;
        static unsigned long s_poolDumpFrame = 0;
        const bool dumpThisFrame = s_poolDump && ((s_poolDumpFrame++ % 240) == 0);
        std::unordered_map<const void*, long> ptrToSlot;
        const auto slotOf = [&ptrToSlot](const void* p) {
            long r = -1;
            if (const auto it = ptrToSlot.find(p); it != ptrToSlot.end()) { const auto& [key, v] = *it; r = v; }
            return r;
        };
        if (dumpThisFrame) {
            if (TG_LightPtr* wl = eye->getWorldLights())
                for (long s = 0; s < MAX_LIGHTS_IN_WORLD; ++s)
                    if (wl[s]) {
                        // A light listed in two slots = a stale slot (freed owner, reused address).
                        if (const long prevSlot = slotOf(wl[s]); prevSlot >= 0)
                            MC2_EVENT(Terrain, "LIGHT_POOL_DUMP", 1, "dup_ptr slot_a={} slot_b={}", prevSlot, s);
                        ptrToSlot[wl[s]] = s;
                    }
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
                const long slot = slotOf(tl);
                mc2_spotlight_diag::SourceClass src;
                const bool eOwned = mc2_spotlight_diag::is_e_slot(eye, slot).transform([&src](auto value) {
                    src = value;
                    return true;
                }).value_or(false);
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
                    reinterpret_cast<std::uintptr_t>(mc2_spotlight_diag::slot_owner(eye, slot)),
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
            // LOCAL-LIGHTS-1: a coned light carries its aim + cone in the high bits
            // (light_pool_attenuation.hglsl lpa_spotCone); its radii are physical,
            // so the LIGHT-REALISM-1 pool reshape below does not apply to it.
            const bool coned = gl.lightType != 0u && tl->spotCosOuter > 0.0f;
            if (coned)
                gl.lightType = PackPoolLightType(gl.lightType, tl->spotDir.x, tl->spotDir.y,
                                                 tl->spotDir.z, tl->spotCosOuter, 0u, tl->moverLamp);
            // Preserve source color; each receiver applies the separate intensity once.
            const DWORD argb = tl->GetSourceRGB();
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
            if (s_lightRealism && !coned)
            {
                const float spill = s_lightRealismPoolSpill;   // farDistance scale
                const float core  = s_lightRealismPoolCore;    // closeDistance scale
                float farD   = tl->farDistance * spill;
                float closeD = tl->closeDistance * core;
                farD = std::max(farD, 1.0f);
                closeD = std::max(closeD, 0.0f);
                closeD = std::min(closeD, farD - 1.0f);
                gl.closeDistance   = closeD;
                gl.farDistance     = farD;
                gl.oneOverDistance = 1.0f / (farD - closeD);
            }
            // NIGHT-LIGHT-POOLS-2a: mover flag in the pad (pool frag dims
            // mover-attached lights; compute consumer never reads pads).
            // LOCAL-LIGHTS-2: a coned light is a building lamp, never a mover -- the
            // slot tags can be stale (mc2_10: the S_TurretControl lamps' slots read
            // as Mech), which dimmed them 4x, shrank them and denied them a shadow tile.
            gl._pad1           = (!coned && moverLights.contains(tl)) ? 1.0f : 0.0f;
            gl.intensity       = tl->intensity;
            // Lamp-type variation: terrain fixtures (runway, taxiway and pad edge lights, dozens per base)
            // marked the ground with rows of identical small bright discs. They are markers, not floodlights:
            // a soft wash. The smallest (runway edge lights, authored reach <= 40) stay compact;
            // the larger pad and taxiway fixtures keep a modest glow. Building and mover
            // lamps keep their pools.
            if (!coned && tl->lightType == TG_LIGHT_TERRAIN && tl->stockTerrainLamp) {
                const float kFixtureSpread = tl->farDistance <= 40.0f ? 0.8f : 1.6f;
                const float fixtureIntensity = tl->farDistance <= 40.0f ? 0.25f : 0.6f;
                gl.farDistance    *= kFixtureSpread;
                gl.closeDistance  *= kFixtureSpread;
                gl.oneOverDistance = 1.0f / std::max(gl.farDistance - gl.closeDistance, 1.0f);
                gl.intensity      *= fixtureIntensity;
            }
            ++numLights;
        }

        // LIGHT-POOL-CAP-1: report peak demand + any starvation. Always-on but
        // rate-limited to transitions of the high-water mark, so a scene that
        // never exceeds the cap prints one line and then stays silent, while a
        // starving scene names the number of slots it actually needs.
        {
            static uint32_t s_peakWanted = 0;
            static bool     s_warnedDrop = false;
            const bool newPeakWanted = poolWanted > s_peakWanted;
            if (newPeakWanted) {
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

    }
    // LOCAL-LIGHTS-2: the coned lights nearest the camera target get a shadow tile (bits 4-7 of
    // lightType); the VK shadow island renders exactly these tiles this frame. Potato / shadows
    // off / no island: no tiles, unshadowed lights.
    {
        const bool shadowTiles = eye && mc2_vk_local_shadows_capable() != 0;
        const float camX = eye ? eye->getPosition().x : 0.0f, camY = eye ? eye->getPosition().y : 0.0f;
        s_poolCamXY[0] = camX; s_poolCamXY[1] = camY;
        mc2localshadow::selectTiles(std::span(s_lightBuf.data(), numLights), camX, camY, shadowTiles);
    }
    // NIGHT-LIGHT-POOLS-1: publish this frame's packed count for the forward
    // consumer (LOD-chunk terrain per-fragment light loop).
    s_lastNumLights = numLights;
    s_objectPoolDirty = true;

    // Publish the current visible window and lighting scalars for the native Vulkan producer.
    {
        VkProducerSnapshot& sn = s_vkSnap;
        sn = VkProducerSnapshot{};
        sn.numVertices  = s_numVertices;
        sn.vertexOffset = (packMinVN <= packMaxVN) ? packMinVN : 0u;
        sn.windowCount  = (packMinVN <= packMaxVN) ? (packMaxVN - packMinVN + 1u) : 0u;
        sn.vertices     = s_vertexPack.vertices.data();
        sn.vertexVersions = s_vertexPack.versions.data();
        sn.numLights    = numLights;
        sn.lights       = s_lightBuf.data();
        if (eye) {
            const auto lit = eye->currentLighting();   // the one producer (a curve's state, else the camera's members)
            sn.sunDir[0] = lit.sunDir[0]; sn.sunDir[1] = lit.sunDir[1]; sn.sunDir[2] = lit.sunDir[2];
            sn.lightRgb[0] = static_cast<int>(lit.sun[0]);   sn.lightRgb[1] = static_cast<int>(lit.sun[1]);   sn.lightRgb[2] = static_cast<int>(lit.sun[2]);
            sn.ambientRgb[0] = static_cast<int>(lit.amb[0]); sn.ambientRgb[1] = static_cast<int>(lit.amb[1]); sn.ambientRgb[2] = static_cast<int>(lit.amb[2]);
            sn.fogStart = lit.fogStart;
            sn.fogFull  = lit.fogFull;
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
        sn.minHaze = mc2FogDistance(Camera::MinHazeDistance);
        sn.maxClip = mc2FogDistance(Camera::MaxClipDistance); // Fog endpoint only, not admission.
        sn.distFactor = mc2FogInvSpan(sn.minHaze, sn.maxClip);
        s_vkSnapValid = true;
    }

}

} // namespace gos_terrain_lighting

void mc2::beam_lights::appendFrameLights() {
    if (!eye || !s_vkSnapValid) return;
    Pool beams;
    const auto visible = [](const float p[3]) {
        glm::vec3 point(p[0], p[1], p[2]);
        Stuff::Vector4D screen;
        eye->projectForScreenXY(point, screen);
        const auto res = eye->getScreenRes();
        constexpr float margin = 160.0f;
        return screen.w > 0 && screen.x >= -margin && screen.y >= -margin &&
            screen.x <= res.x + margin && screen.y <= res.y + margin;
    };
    for (const auto& fx : mc2::render_view::list().fx) {
        if (fx.kind != mc2::render_view::FxPacket::Kind::Bolt ||
            !fx.has(mc2::render_view::Visible) || !fx.has(mc2::render_view::BoltGeometry) ||
            !fx.bolt.energyLight) continue;
        const bool impact = fx.has(mc2::render_view::HitTarget);
        const glm::vec3 start = fx.bolt.isBeam ? fx.position : fx.bolt.laserVertices[2];
        const glm::vec3 end = impact || fx.bolt.isBeam ? fx.aux[1] : fx.aux[0];
        const uint32_t rgb = fx.bolt.middleRGB ? fx.bolt.middleRGB :
            (fx.bolt.frontRGB ? fx.bolt.frontRGB : 0x80bfffu);
        beams.add({{start.x, start.y, start.z}, {end.x, end.y, end.z},
                   rgb, fx.bolt.lightRemaining, false}, visible);
        const auto& hit = fx.aux[1];
        beams.add({{hit.x, hit.y, hit.z}, {hit.x, hit.y, hit.z},
                   rgb, fx.bolt.impactLightRemaining, true}, visible);
    }
    uint32_t count = s_lastNumLights;
    const uint32_t cap = std::min(s_maxLights, gos_terrain_lighting::kObjectPoolLights);
    for (unsigned i = 0; i < beams.count && count < cap; ++i)
        s_lightBuf[count++] = beams.lights[i];
    // Preserve the early vertex window and existing shadow assignments; refresh
    // the shared pool/grid once, after beams, even when no beams were appended.
    // Native consumers record their inputs after render extraction.
    BuildLightGrid(count);
    s_lastNumLights = count;
    s_objectPoolDirty = true;
    s_vkSnap.numLights = count;
}
