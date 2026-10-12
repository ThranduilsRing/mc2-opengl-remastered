// GameOS/gameos/gos_terrain_water_stream.cpp
//
// Stage 2 of the renderWater architectural slice (CPU→GPU offload).
// Builds a static, map-keyed CPU-side recipe array describing every
// water-bearing terrain quad in the mission. Each frame, walks the live
// (camera-windowed) quadList and emits a thin record per in-window water
// quad, looked up by stable map vertexNum → recipe-index hash.
//
// Why the indirection: `Terrain::quadList` is rebuilt every frame as a
// camera-relative window (mapdata.cpp:1072 makeLists), so a "static recipe
// indexed by quadList slot" is invalid by frame 2. The MAP coordinates
// (mapX, mapY) of each vertex are stable; the recipe is keyed by the
// top-left vertex's `vertexNum = mapY*W + mapX` (set at mapdata.cpp:1104).
//
// Spec: docs/superpowers/specs/2026-04-29-renderwater-fastpath-design.md.

#include "gos_terrain_water_stream.h"
#include "../../mclib/terrain_material.h"  // single-sourced terrain type -> material
extern int gos_WaterShoreMode();
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1
#include "gos_gpu_sync.h"   // GPU-SYNC-CONTRACT: SSBO bind-offset alignment helper

#include "gos_profiler.h"
#include "gpu_driven_common.h"
#include "gos_terrain_lighting.h"
#include "gos_static_prop_killswitch.h"  // gos_GetTerrainMVPMat4()
#include "gos_terrain_indirect.h"        // IsFrameSolidArmed()
#include "gos_render_pass_timer.h"       // [RENDER_PASS_TIME v1] nesting guard (QueryActive)
#include <algorithm>                     // std::sort -- surfaced by MC2_ASAN build (Tracy-disabled config drops the transitive include)
#include <cassert>

// WATER-BIAS-LINEARIZE-1 (gameos.hpp): FAST-water bias mode (0 = proportional
// default, 1 = MC2_WATER_BIAS_LEGACY). The pzOk cull gate must match the VS.
extern int mc2WaterBiasLegacy();

#include <vector>
#include <unordered_map>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>

#include <GL/glew.h>

#include "../../mclib/terrain.h"
#include "../../mclib/quad.h"
#include "../../mclib/vertex.h"
#include "../../mclib/mapdata.h"
#include "mc2_hitch_trace.h"

#include "../../RenderCore/GpuBufferOwner.h"  // WATER-REGISTRY-0: MDI water buffer owner records (L7.4 prep)
#include "Gates/gate.h"
#include "mc2fmt.h"

// [WATER_DEPTHPROBE v1] cross-TU Probe-8 fingerprint accessors (terrain-solid
// side). Paired per-frame against the water-upload MVP fingerprint to
// discriminate a RUNTIME temporal MVP divergence (fps differ under camera
// motion) from a static depth-derivation/fudge mismatch (fps stay equal).
extern "C" uint32_t gos_terrain_indirect_getDispatchMvpFp();
extern "C" uint64_t gos_terrain_indirect_getDispatchMvpFrameIdx();
// Water-consistency fix (2026-05-17): full MVP terrain-solid baked its Fix-B
// clipPos with this frame. Used only when IsFrameSolidArmed() (then it is
// this-frame-fresh) so the drawn water matches the drawn terrain exactly.
extern "C" const float* gos_terrain_indirect_getDispatchMvp16();

namespace WaterStream {

// SHORELINE-STAIRCASE-1: MC2_WATER_SHORE_FEATHER floor on effective alphaDepth
// (world units). Widens the water FS shore smoothstep band + the beach-ring
// shoreExt geometry in lockstep so the tile-quantized water-edge staircase
// dissolves into a soft, waterline-hugging alpha gradient. Read once; default
// unset/<=0 -> returns 0 -> EffAlphaDepth is the identity -> byte-identical.
float ShoreFeatherFloorWu() {
    static const float s_floor = mc2::gate::get<"MC2_WATER_SHORE_FEATHER">();
    return s_floor;
}

float EffAlphaDepth(float baseAlphaDepth) {
    const float floorWu = ShoreFeatherFloorWu();
    return floorWu > 0.0f ? fmaxf(baseAlphaDepth, floorWu) : baseAlphaDepth;
}

// SHORELINE-GEOMETRY-1 — see the header block for the rationale.
// WATER-MEANDER-CLAMP-1 (2026-07-26): default REVERTED 75 -> 0 (OFF).
//
// AUDIT-TRUTH-1 baked in 75 as "the amplitude the user graded and APPROVED".
// That approval does not survive wide, normal-altitude captures on poses that
// actually frame water:
//
//   * mc2_17 (flat shelf) at A=75 DELETES water. Wide pose shelf_east_wide loses
//     11.2 percentage points of frame water (42.95% -> 31.78%), and the loss is
//     ONE-WAY -- 11.23% lost against 0.05% gained. The perturbation is zero-mean,
//     but `if (shore <= 0.0) discard;` is not: a seaward excursion adds nothing,
//     while a landward one destroys the fragment. The result is a smeared brown
//     blob eating the bay with detached islands floating in open water -- the
//     user's "the mc2_17 water is atrocious / the smoothing line looks terrible".
//   * mc2_10 -- the map the approval was granted on -- ALSO loses water at 75.
//     Nobody had seen it because no mc2_10 bookmark framed water until this slice
//     authored one. On shore_south_wide an entire inlet that is water at A=0 is a
//     brown wedge at A=75. On shore_west_wide, meanwhile, A=75 is BYTE-IDENTICAL
//     to A=0 -- so where the approved look is good, the meander contributed
//     exactly nothing to it.
//
// Root cause: the amplitude is a THICKNESS carrying fBm gain 2.40, so A=75 is a
// +/-180 wu excursion -- larger than the entire water depth range of either map
// (mc2_17 tops out at 120 wu, mc2_10 at 80). It cannot act as an edge treatment
// when it exceeds the field it perturbs everywhere; it punches holes in open water.
//
// Two rescues were implemented and MEASURED, and both rejected:
//   1. Clamp horizontal reach (deferred; no gate).
//      Reduces water loss monotonically (-11.2 -> -2.5 at D=24, -1.3 at D=12) but
//      is SELF-DEFEATING: it scales the perturbation by |grad(thickness)|, and the
//      staircase it must cure lives precisely where |grad| is small, so it
//      suppresses the cure exactly where it is needed -- the sawtooth survived
//      every clamped arm. It also facets, since the gradient of a linearly
//      interpolated varying is piecewise-constant per primitive. Code reverted.
//   2. Lower the amplitude (A=25 / A=15). Fixes mc2_17 handsomely, but at A=15
//      mc2_10's shore_west_wide grows a hard blocky staircase that neither A=0 nor
//      A=75 has -- EffShoreExt widens tile admission BY A, so a small A clips
//      landward excursions back to tile edges and re-creates the risers. The knob
//      is not monotonic and no single global amplitude is safe.
//
// So the default parks at 0 = OFF, provably the stock, long-shipped waterline.
// What is lost is the staircase refinement, not correctness; what is gained is a
// shipping default that stops deleting water on two maps. MC2_WATER_SHORE_MEANDER=75
// restores the previous behaviour exactly for anyone re-grading it. Re-enabling by
// default needs a fix that separates the cure from the defect -- the |grad|-keyed
// clamp provably cannot, because both have the same cause.
float ShoreMeanderWu() {
    static const float s_amp = mc2::gate::get<"MC2_WATER_SHORE_MEANDER">();
    return s_amp;
}

float ShoreMeanderFreq() {
    // Default 1/384 wu = one dominant fBm cycle per ~3 tiles (tile = 128 wu).
    // Long enough to bend a whole riser rather than serrate it, short enough
    // that a single tile edge never stays straight across its full length.
    static const float s_freq = mc2::gate::get<"MC2_WATER_SHORE_MEANDER_FREQ">();
    return s_freq;
}

float EffShoreExt(float baseAlphaDepth) {
    const float effAD = EffAlphaDepth(baseAlphaDepth);
    const float base  = (effAD * 0.5f > 0.0f) ? effAD * 0.5f : 15.0f;
    // Admit tiles the meander can reach. The perturbation is zero-mean with
    // amplitude ShoreMeanderWu(), so the waterline can rise by at most that
    // much locally; without this widening the landward excursions would be
    // clipped to the stock tile boundary and re-expose the step.
    float ext = base + ShoreMeanderWu();
    // MC2_WATER_SHORE_MODE=3 (GL prototype): widen admission so geometry exists
    // beyond the true sidecar contour for the per-pixel discard to carve.
    if (gos_WaterShoreMode() == 3)
        ext += 60.0f;
    return ext;
}

namespace {

std::vector<WaterRecipe> g_recipes;
// vertexNum (= mapY*realVerticesMapSide + mapX of top-left corner) → recipeIdx
std::unordered_map<uint32_t, uint32_t> g_vertexNumToRecipe;
bool g_ready = false;

// Static recipe buffer (uploaded once per mission).
GLuint   g_recipeBuffer = 0;
uint32_t g_recipeBufferUploadedCount = 0;

// Per-frame thin-record ring (mirrors patch_stream thin-record ring).
constexpr uint32_t kThinRingSlots = 3;
GLuint   g_thinBuffer = 0;
uint32_t g_thinSlot = 0;
uint32_t g_thinSlotCapacity = 0;
std::vector<WaterThinRecord> g_thinStaging;

// WATER-THINRING-FENCE-1: per-slot GPU fences. A ring slot must not be
// overwritten while a draw from kThinRingSlots frames ago is still reading it.
// The existing glMemoryBarrier(SHADER_STORAGE) protects write-visibility within
// a frame but NOT cross-frame CPU/GPU lifetime reuse — that race is what these
// fences close. Mirrors the solid thin-ring fence lifecycle in
// gos_terrain_indirect.cpp (wait-before-reuse at slot advance; create-after-draw
// in the bridge). Trace gate: MC2_WATER_THINRING_TRACE=1.
GLsync   g_thinFences[kThinRingSlots] = { nullptr, nullptr, nullptr };
bool     g_thinRingTraceEnabled = false;
bool     g_thinRingTraceQueried = false;
uint32_t g_thinTraceSlot       = 0;   // captured at wait site, emitted at fence create
uint32_t g_thinTraceBytes      = 0;
int      g_thinTraceWaited     = 0;
unsigned g_thinTraceWaitResult = 0;
uint64_t g_thinTraceFrame      = 0;

bool ThinRingTraceOn() {
    if (!g_thinRingTraceQueried) {
        g_thinRingTraceQueried = true;
        g_thinRingTraceEnabled = mc2::gate::get<"MC2_WATER_THINRING_TRACE">();
    }
    return g_thinRingTraceEnabled;
}

// Call right AFTER advancing g_thinSlot and BEFORE writing the slot. Waits on
// the fence guarding the slot about to be reused, then clears it. slotBytes is
// for trace only. No-op (apart from trace bookkeeping) when no fence is present
// (warmup / first kThinRingSlots frames).
void WaitAndClearThinFenceForCurrentSlot(uint32_t slotBytes) {
    int      waited = 0;
    unsigned result = 0;
    if (g_thinFences[g_thinSlot]) {
        result = (unsigned)glClientWaitSync(g_thinFences[g_thinSlot],
                                            GL_SYNC_FLUSH_COMMANDS_BIT, 10000000u /* 10ms */);
        glDeleteSync(g_thinFences[g_thinSlot]);
        g_thinFences[g_thinSlot] = nullptr;
        waited = 1;
    }
    if (ThinRingTraceOn()) {
        g_thinTraceSlot       = g_thinSlot;
        g_thinTraceBytes      = slotBytes;
        g_thinTraceWaited     = waited;
        g_thinTraceWaitResult = result;
    }
}

// Narrow-walk candidate vector. Populated by AppendNarrowCandidate during
// the per-frame setupTextures loop; consumed by UploadAndBindThinRecords.
// Default-on; env-gated `MC2_WATER_UPLOAD_NARROW=0` falls back to full walk.
std::vector<TerrainQuadPtr> g_narrowQuadsThisFrame;
uint32_t g_narrowMaxSeen = 0;
bool s_narrowEnabledKnown = false;
bool s_narrowEnabled = true;
inline bool NarrowEnabledImpl() {
    if (!s_narrowEnabledKnown) {
        s_narrowEnabled = mc2::gate::get<"MC2_WATER_UPLOAD_NARROW">();
        s_narrowEnabledKnown = true;
    }
    return s_narrowEnabled;
}
// Per-600-frame upload summary (so the user can confirm the narrow path
// actually fires and the volume drop is real).
uint32_t g_uploadSummaryFrames = 0;
uint64_t g_uploadSummaryNarrowQuads = 0;
uint64_t g_uploadSummaryFullWalkQuads = 0;

// B4 Stage 1c — water parity mask.
// One bit per corner-0 vertexNum. Set for every quad that UploadAndBindThinRecords
// emits a thin record for (= quads the legacy water path draws this frame).
// Reset at the top of UploadAndBindThinRecords and in Reset().
// Indexed and sized identically to gos_terrain_mask_dispatch's s_waterMask.
static constexpr int32_t kWaterParityMaskWords = 450;  // ceil(14400/32)
static uint32_t s_waterParityMask[kWaterParityMaskWords];

bool s_debugEnabledKnown = false;
bool s_debugEnabled = false;
bool DebugOn() {
    if (!s_debugEnabledKnown) {
        s_debugEnabled = mc2::gate::get<"MC2_WATER_STREAM_DEBUG">();
        s_debugEnabledKnown = true;
    }
    return s_debugEnabled;
}

// ---------------------------------------------------------------------------
// Phase C Stage 1 compute resources
// ---------------------------------------------------------------------------
GLuint   g_waterComputeProgram    = 0;
GLuint   g_cmdPatchProgram        = 0;
GLuint   g_quadWindowSsbo         = 0;   // per-frame: recipe indices in quadList window
uint32_t g_quadWindowSsboCapacity = 0;   // CPU-side mirror of g_quadWindowSsbo allocated size (bytes)
GLuint   g_waterBucketHeaderSsbo  = 0;   // GpuDrivenBucketHeader (16 B)
GLuint   g_waterIndirectCmdBuffer = 0;   // 2 × DrawArraysIndirectCommand (32 B)
bool     g_waterGpuDrivenArmed    = false;

// VK-P9a WATER-INTEROP: track the thin-ring slot the GPU compute packed THIS frame
// (byte offset into g_thinBuffer) so the VK island can read back exactly those
// records. Set at the end of ComputeDispatchAndBindThinRecords; read by
// ReadbackGpuThinForVk. maxThinRecords is the per-slot capacity in records.
GLintptr g_vkLastThinSlotOffset = 0;
uint32_t g_vkLastMaxThinRecords = 0;
std::vector<WaterThinRecord> g_vkThinReadback;   // CPU copy for the VK feed

// WATER-REGISTRY-0: parallel GpuBufferOwner identity records for the 4 live MDI
// water buffers (thin ring / recipe / bucket-header / indirect-cmd). Pattern (b):
// the raw GLuint globals above are UNCHANGED so every gen/bind/map/delete site
// keeps its exact GL args/order; the owner's backendHandle is mirrored straight
// from the freshly gen'd handle right after glGenBuffers (like view_uniforms /
// mech-batcher rings). Registered at create, invalidated on destroy. Metadata
// only — NEVER read by the compute chain or the MDI draw. Lifetime Mission:
// these are rebuilt per mission load (ReleaseGlResources on unload). L7.4 prep.
RenderCore::GpuBufferOwner g_thinBufferOwner{
    RenderCore::RenderResourceId::WaterThinBuf,
    RenderCore::RenderResourceLifetime::Mission,
    "WaterThinBuf",
    0u};
RenderCore::GpuBufferOwner g_recipeBufferOwner{
    RenderCore::RenderResourceId::WaterRecipeBuf,
    RenderCore::RenderResourceLifetime::Mission,
    "WaterRecipeBuf",
    0u};
RenderCore::GpuBufferOwner g_waterBucketHeaderSsboOwner{
    RenderCore::RenderResourceId::WaterHeaderBuf,
    RenderCore::RenderResourceLifetime::Mission,
    "WaterHeaderBuf",
    0u};
RenderCore::GpuBufferOwner g_waterIndirectCmdBufferOwner{
    RenderCore::RenderResourceId::WaterIndirectCmdBuf,
    RenderCore::RenderResourceLifetime::Mission,
    "WaterIndirectCmdBuf",
    0u};

// WATER-REGISTRY-0: tiny helpers so each gen/destroy site reduces to one call
// (mirror the desc-build boilerplate the mech-batcher inlines per site). GL-free.
inline void RegisterWaterBufferOwner(RenderCore::GpuBufferOwner& owner, GLuint glName,
                                     uint64_t sizeBytes) {
    owner.backendHandle = glName;  // mirror handle into owner (pattern (b))
    RenderCore::RenderResourceDesc d;
    d.id        = owner.id;
    d.kind      = RenderCore::RenderResourceKind::Buffer;
    d.lifetime  = owner.lifetime;
    d.format    = RenderCore::RenderResourceFormat::BufferRaw;
    d.debugName = owner.debugName;
    d.glName    = glName;
    d.sizeBytes = sizeBytes;
    d.valid     = true;
    RenderCore::registerOrUpdateRenderResource(d);
}
inline void InvalidateWaterBufferOwner(RenderCore::GpuBufferOwner& owner) {
    owner.backendHandle = 0;
    RenderCore::RenderResourceDesc invalid;
    invalid.id = owner.id;   // valid=false → slot marked unavailable
    RenderCore::registerOrUpdateRenderResource(invalid);
}

// CPU staging array for BuildQuadWindowSSBO. Persists across frames to avoid
// per-frame allocation. Capacity grows to the high-water-mark and stays there.
std::vector<uint32_t> g_quadWindowStaging;

// Single-sourced in mclib/terrain_material.h (was a local copy missing type 20).
inline uint8_t terrainTypeToMaterialLocal(uint32_t terrainType) {
    return mc2terrain::terrainTypeToMaterial(terrainType);
}

} // namespace

void Reset() {
    g_recipes.clear();
    g_recipes.shrink_to_fit();
    g_vertexNumToRecipe.clear();
    g_ready = false;
    g_recipeBufferUploadedCount = 0;
    g_narrowQuadsThisFrame.clear();
    g_narrowQuadsThisFrame.shrink_to_fit();
    g_narrowMaxSeen = 0;
    memset(s_waterParityMask, 0, sizeof(s_waterParityMask));
}

bool NarrowEnabled() {
    return NarrowEnabledImpl();
}

// WATER-GPU-FULL-RECIPE-CULL-1B: authoritative full-recipe GPU cull. When ON, the
// compute path culls the whole world-indexed recipe set directly (no CPU candidate
// window), its output FEEDS THE DRAW, and the CPU narrow walk is skipped. Default-OFF
// (only literal "0" disambiguates the off-with-value case). Proven byte-identical to
// the candidate path by 1A parity across tier1 + a camera-pan-across-water-edge check.
bool IsFullRecipeAuthoritative() {
    static bool s_known = false, s_val = false;
    if (!s_known) {
        // DEFAULT-ON since 2026-06-03 (1A byte-identical parity across tier1 +
        // user pan-across-water-edge + visual soak): only literal "0" opts out
        // (revert escape hatch); any other value INCLUDING UNSET opts in.
        s_val = mc2::gate::get<"MC2_WATER_GPU_FULL_RECIPE_AUTHORITATIVE">();
        s_known = true;
    }
    return s_val;
}

void BeginFrameNarrow() {
    if (!NarrowEnabledImpl()) return;
    // Reserve last-frame max + 10% slack, capped at recipe count (the hard
    // upper bound — every map water quad simultaneously). vector::clear is
    // O(n) destructor-call here but TerrainQuadPtr is a trivial pointer, so
    // it's effectively a size reset. No allocation in the hot loop.
    const size_t reserve = (size_t)(g_narrowMaxSeen + (g_narrowMaxSeen / 10) + 64);
    if (g_narrowQuadsThisFrame.capacity() < reserve)
        g_narrowQuadsThisFrame.reserve(reserve);
    g_narrowQuadsThisFrame.clear();
}

void AppendNarrowCandidate(const void* quadPtr) {
    // Caller asserts the quad already passed the same predicate
    // UploadAndBindThinRecords applies (corners non-null, vertexNum >= 0,
    // waterHandle != 0xffffffff). The Upload loop re-checks defensively.
    g_narrowQuadsThisFrame.push_back((TerrainQuadPtr)quadPtr);
    if (g_narrowQuadsThisFrame.size() > g_narrowMaxSeen)
        g_narrowMaxSeen = (uint32_t)g_narrowQuadsThisFrame.size();
}

void Build() {
    g_recipes.clear();
    g_vertexNumToRecipe.clear();
    g_ready = false;

    // Source of truth: MapData::blocks (PostcompVertex array, mission-static,
    // dimensioned realVerticesMapSide × realVerticesMapSide). Iterate the
    // FULL MAP and build one WaterRecipe per water-bearing quad. World
    // coordinates derived from map indices via mapdata.cpp:1123-1124.
    if (!Terrain::mapData) {
        if (DebugOn()) {
            mc2::fmt::print_err("[WATER_STREAM v1] event=build_skipped reason=no_mapdata\n");
            fflush(stderr);
        }
        return;
    }

    const PostcompVertexPtr blocks = Terrain::mapData->getBlocks();
    if (!blocks) {
        if (DebugOn()) {
            mc2::fmt::print_err("[WATER_STREAM v1] event=build_skipped reason=no_blocks\n");
            fflush(stderr);
        }
        return;
    }

    const long mapSide  = Terrain::realVerticesMapSide;
    const long halfSide = Terrain::halfVerticesMapSide;
    const float wupv    = Terrain::worldUnitsPerVertex;
    const bool has_terrainTextures2 = (Terrain::terrainTextures2 != nullptr);

    auto blockAt = [blocks, mapSide](long mx, long my) -> const PostcompVertex& {
        return blocks[mx + my * mapSide];
    };
    auto worldX = [halfSide, wupv](long mx) -> float {
        return float(mx - halfSide) * wupv;
    };
    auto worldY = [halfSide, wupv](long my) -> float {
        return float(halfSide - my) * wupv;
    };

    g_recipes.reserve((size_t)(mapSide * mapSide / 8));  // rough upper bound
    g_vertexNumToRecipe.reserve((size_t)(mapSide * mapSide / 8));

    long candidates = 0;

    // Quads span (mx, my)..(mx+1, my+1) so the outer loop runs to mapSide-1.
    // The corner ordering matches mapdata.cpp:1175-ish setup of TerrainQuad:
    //   v0 = (mx,   my)     top-left
    //   v1 = (mx+1, my)     top-right
    //   v2 = (mx+1, my+1)   bottom-right
    //   v3 = (mx,   my+1)   bottom-left
    for (long my = 0; my < mapSide - 1; ++my) {
        for (long mx = 0; mx < mapSide - 1; ++mx) {
            ++candidates;
            const PostcompVertex& p0 = blockAt(mx,     my);
            const PostcompVertex& p1 = blockAt(mx + 1, my);
            const PostcompVertex& p2 = blockAt(mx + 1, my + 1);
            const PostcompVertex& p3 = blockAt(mx,     my + 1);

            // Include EVERY map quad in the recipe (no water-bit filter).
            //
            // Why: legacy `setupTextures` sets `waterHandle != 0xffffffff` for
            // any quad where the WATER PLANE PROJECTS INTO THE FRUSTUM (per
            // the `clipped1||clipped2` gate at quad.cpp:963). That's a
            // dynamic, camera-driven criterion — independent of whether the
            // quad has any per-vertex `water & 1` bit set. On flat-but-large
            // missions like mc2_03 / mc2_10 there exist pure-land quads above
            // the water plane where the water plane still projects on-screen
            // and legacy emits semi-transparent (alphaEdge) water vertices.
            //
            // The Stage 2 build used `water & 1` as a recipe-inclusion gate,
            // which fired `lookup_miss` parity mismatches on those missions
            // because `UploadAndBindThinRecords` would silently skip quads
            // with no recipe (mc2_03: 7,131 misses/run; mc2_10: 12,888).
            //
            // Including every map quad lifts recipe SSBO size ~10× (mc2_01
            // 8K → ~80K records, 0.5 → ~5 MB) but keeps the per-frame thin
            // record count unchanged — pz-gate culls non-visible quads at
            // draw time exactly as before. Memory growth is bounded by
            // mapSide² ; well within budget.
            (void)p0; (void)p1; (void)p2; (void)p3;

            const float v0x = worldX(mx);
            const float v0y = worldY(my);
            const float v1x = worldX(mx + 1);
            const float v1y = worldY(my);
            const float v2x = worldX(mx + 1);
            const float v2y = worldY(my + 1);
            const float v3x = worldX(mx);
            const float v3y = worldY(my + 1);

            // The runtime quad uvMode depends on the (mx, my) parity per
            // mapdata.cpp:115 — `((tileR & 1) == (tileC & 1)) ? BOTTOMRIGHT : BOTTOMLEFT`.
            // tileR = my, tileC = mx for our convention.
            const bool isBottomLeft = ((my & 1L) != (mx & 1L));

            WaterRecipe r{};
            r.v0x = v0x; r.v0y = v0y;
            r.v1x = v1x; r.v1y = v1y;
            r.v2x = v2x; r.v2y = v2y;
            r.v3x = v3x; r.v3y = v3y;
            r.v0e = p0.elevation;
            r.v1e = p1.elevation;
            r.v2e = p2.elevation;
            r.v3e = p3.elevation;
            r.quadIdx = (uint32_t)(mx + my * mapSide);  // top-left vertexNum

            uint32_t flags = 0;
            if (isBottomLeft)         flags |= kFlagBitUvModeBottomLeft;
            if (has_terrainTextures2) flags |= kFlagBitHasDetail;
            r.flags = flags;

            const uint32_t t0 = (uint32_t)(uint8_t)p0.terrainType;
            const uint32_t t1 = (uint32_t)(uint8_t)p1.terrainType;
            const uint32_t t2 = (uint32_t)(uint8_t)p2.terrainType;
            const uint32_t t3 = (uint32_t)(uint8_t)p3.terrainType;
            r.terrainTypes = t0 | (t1 << 8) | (t2 << 16) | (t3 << 24);

            const uint32_t w0 = (uint32_t)(p0.water & 0xFFu);
            const uint32_t w1 = (uint32_t)(p1.water & 0xFFu);
            const uint32_t w2 = (uint32_t)(p2.water & 0xFFu);
            const uint32_t w3 = (uint32_t)(p3.water & 0xFFu);
            r.waterBits = w0 | (w1 << 8) | (w2 << 16) | (w3 << 24);

            const uint32_t recipeIdx = (uint32_t)g_recipes.size();
            g_recipes.push_back(r);
            g_vertexNumToRecipe.emplace((uint32_t)(mx + my * mapSide), recipeIdx);
        }
    }

    g_ready = true;

    // [WATER_MAT v1] positive-marker probe (env MC2_WATER_MATERIAL_PROBE; SEPARATE
    // from the retained [WATER_DEPTHPROBE v2] MVP instrument - do not share its env).
    // Recomputes the VS thickness formula CPU-side over the populated recipes so a
    // smoke can assert the elevation path is live (max > 0), not a flat unbound read.
    {
        static const bool s_waterMatProbe = mc2::gate::get<"MC2_WATER_MATERIAL_PROBE">();
        if (s_waterMatProbe && !g_recipes.empty()) {
            float tmin = 1e30f, tmax = -1e30f;
            for (const WaterRecipe& r : g_recipes) {
                float floorMin = r.v0e;
                floorMin = (r.v1e < floorMin) ? r.v1e : floorMin;
                floorMin = (r.v2e < floorMin) ? r.v2e : floorMin;
                floorMin = (r.v3e < floorMin) ? r.v3e : floorMin;
                float thick = (float)Terrain::waterElevation - floorMin;
                if (thick < 0.0f) thick = 0.0f;
                if (thick < tmin) tmin = thick;
                if (thick > tmax) tmax = thick;
            }
            mc2::fmt::print_err("[WATER_MAT v1] event=summary recipes={:d} thickness_min={:.3f} thickness_max={:.3f} waterElevation={:.3f}\n", mc2::fmt::as_size(g_recipes.size()), (double)tmin, (double)tmax, (double)Terrain::waterElevation);
            fflush(stderr);
        }
    }

    if (DebugOn()) {
        mc2::fmt::print_err("[WATER_STREAM v1] event=build_done recipes={:d} candidates_walked={:d} waterElevation={:.3f} has_terrainTextures2={:d} mapSide={:d} halfSide={:d} wupv={:.3f}\n", mc2::fmt::as_size(g_recipes.size()), mc2::fmt::as_i32(candidates), (double)Terrain::waterElevation, mc2::fmt::as_i32(has_terrainTextures2 ? 1 : 0), mc2::fmt::as_i32(mapSide), mc2::fmt::as_i32(halfSide), (double)wupv);
        // Dump several recipes spread across the map to spot uniformity bugs.
        const size_t n = g_recipes.size();
        const size_t dumps[] = { 0, n/8, n/4, n/2, n*3/4, n-1 };
        for (size_t di = 0; di < sizeof(dumps)/sizeof(dumps[0]); ++di) {
            if (dumps[di] >= n) continue;
            const WaterRecipe& r = g_recipes[dumps[di]];
            const bool uniform_elev = (r.v0e == r.v1e && r.v1e == r.v2e && r.v2e == r.v3e);
            mc2::fmt::print_err("[WATER_STREAM v1] event=recipe[{:d}] elev=({:.1f},{:.1f},{:.1f},{:.1f}) {} v0=({:.0f},{:.0f}) v3=({:.0f},{:.0f}) quadIdx={:d}\n", mc2::fmt::as_size(dumps[di]), (double)r.v0e, (double)r.v1e, (double)r.v2e, (double)r.v3e, mc2::fmt::safe(uniform_elev ? "[UNIFORM]" : "[VARIED]"), (double)r.v0x, (double)r.v0y, (double)r.v3x, (double)r.v3y, mc2::fmt::as_u32(r.quadIdx));
        }
        fflush(stderr);
    }
}

const WaterRecipe* GetRecipes() {
    return g_recipes.empty() ? nullptr : g_recipes.data();
}

uint32_t GetRecipeCount() {
    return (uint32_t)g_recipes.size();
}

bool IsReady() {
    return g_ready;
}

// B4 Stage 1c — water parity mask accessor.
const uint32_t* GetWaterParityMask(int* outWords) {
    if (outWords) *outWords = kWaterParityMaskWords;
    return s_waterParityMask;
}

const WaterRecipe* RecipeForVertexNum(int32_t vn) {
    if (vn < 0) return nullptr;
    auto it = g_vertexNumToRecipe.find(static_cast<uint32_t>(vn));
    if (it == g_vertexNumToRecipe.end()) return nullptr;
    assert(it->second < (uint32_t)g_recipes.size()); // invariant: Build populates both atomically
    return &g_recipes[it->second];
}

unsigned int EnsureRecipeBufferUploaded() {
    if (!g_ready || g_recipes.empty())
        return 0;

    if (g_recipeBuffer != 0 && g_recipeBufferUploadedCount == g_recipes.size())
        return g_recipeBuffer;

    const GLsizeiptr bytes = (GLsizeiptr)(g_recipes.size() * sizeof(WaterRecipe));

    if (g_recipeBuffer == 0) {
        glGenBuffers(1, &g_recipeBuffer);
        g_recipeBufferOwner.backendHandle = g_recipeBuffer;  // WATER-REGISTRY-0: mirror handle into owner (pattern b)
        // WATER-REGISTRY-0: register owner (metadata only; not read by any draw).
        RegisterWaterBufferOwner(g_recipeBufferOwner, g_recipeBuffer, (uint64_t)bytes);
    }

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_recipeBuffer);
    MC2_GL_BufferData(GL_SHADER_STORAGE_BUFFER, bytes, g_recipes.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    g_recipeBufferUploadedCount = (uint32_t)g_recipes.size();

    if (DebugOn()) {
        mc2::fmt::print_err("[WATER_STREAM v1] event=recipe_upload bytes={:d} records={:d} buf={:d}\n", mc2::fmt::as_i64((long long)bytes), mc2::fmt::as_u32(g_recipeBufferUploadedCount), mc2::fmt::as_u32((unsigned)g_recipeBuffer));
        fflush(stderr);
    }

    return g_recipeBuffer;
}

uint32_t UploadAndBindThinRecords(bool forceFullWalk) {
    ZoneScopedN("WaterFast.UploadThin");
    if (!g_ready || g_recipes.empty())
        return 0;

    const TerrainPtr terrainPtr = land;
    const TerrainQuadPtr quads = terrainPtr ? terrainPtr->getQuadList() : nullptr;
    const long total = terrainPtr ? terrainPtr->getNumQuads() : 0;
    if (!quads || total <= 0) return 0;

    // Candidate selection: narrow vector (default) vs full quadList walk
    // (env-opt-out). Narrow vector was populated during the engine's existing
    // per-frame setupTextures loop with the SAME corner-validity + waterHandle
    // gate this loop applies — so iterating it covers every quad the legacy
    // walk would have admitted, but typically with 100x fewer iterations.
    //
    // VK-P9a WATER-INTEROP: forceFullWalk overrides narrow. On the armed
    // full-recipe-authoritative GPU path (default-on), the CPU narrow walk is
    // SKIPPED (g_narrowQuadsThisFrame empty) -> narrow-enabled + empty-narrow =
    // ZERO iterations -> g_thinStaging never populated. The VK water island needs
    // a CPU-staged thin feed, so it calls this with forceFullWalk=true to walk the
    // full quadList and populate g_thinStaging regardless of the narrow/auth gates.
    const bool narrow = forceFullWalk ? false : NarrowEnabledImpl();
    const TerrainQuadPtr* narrowArr = narrow && !g_narrowQuadsThisFrame.empty()
        ? g_narrowQuadsThisFrame.data() : nullptr;
    const size_t narrowN = narrow ? g_narrowQuadsThisFrame.size() : 0;
    const long iterCount = narrow ? (long)narrowN : total;

    // Per-frame upload-summary accounting (per 600 frames).
    g_uploadSummaryNarrowQuads   += narrow ? narrowN : 0;
    g_uploadSummaryFullWalkQuads += narrow ? 0 : (uint64_t)total;
    if (++g_uploadSummaryFrames >= 600) {
        mc2::fmt::print_err("[WATER_FAST v1] event=upload_summary frames={:d} narrowed={:d} full_walk={:d} narrow_enabled={:d}\n", mc2::fmt::as_u32(g_uploadSummaryFrames), mc2::fmt::as_u64((unsigned long long)g_uploadSummaryNarrowQuads), mc2::fmt::as_u64((unsigned long long)g_uploadSummaryFullWalkQuads), mc2::fmt::as_i32(narrow ? 1 : 0));
        fflush(stderr);
        g_uploadSummaryFrames = 0;
        g_uploadSummaryNarrowQuads = 0;
        g_uploadSummaryFullWalkQuads = 0;
    }

    // Walk candidates. For each water-bearing quad, look up its stable
    // recipe by top-left-vertex `vertexNum` and emit a thin record carrying
    // live light/fog/pzValid.
    g_thinStaging.clear();
    g_thinStaging.reserve((size_t)iterCount);

    // B4 Stage 1c: zero parity mask before rebuilding it this frame.
    memset(s_waterParityMask, 0, sizeof(s_waterParityMask));

    uint32_t pzValidCount = 0;
    uint32_t waterHandleCount = 0;
    uint32_t recipeMissCount = 0;
    uint32_t pzDropCount = 0;
    for (long i = 0; i < iterCount; ++i) {
        const TerrainQuad& q = narrowArr ? *narrowArr[i] : quads[i];
        // Re-check the eligibility gate even on the narrow path. The narrow
        // appender uses the SAME predicate, so this is a no-op there; the
        // legacy walk needs it. Cost is two compares + a load on the narrow
        // path (negligible) and keeps the walk semantically identical.
        if (!q.vertices[0] || !q.vertices[1] ||
            !q.vertices[2] || !q.vertices[3]) continue;
        // Skip quads where ANY corner is the map-edge blankVertex (vertexNum < 0).
        // Legacy `setupTextures` reads blankVertex's zero data for those corners
        // and emits degenerate triangles that don't rasterize, but the parity
        // check would surface them as `lookup_miss` against a recipe built
        // only for fully-bounded quads. Matching legacy's effective behavior:
        // treat any-corner-blankVertex as a no-emit case.
        if (q.vertices[0]->vertexNum < 0 || q.vertices[1]->vertexNum < 0 ||
            q.vertices[2]->vertexNum < 0 || q.vertices[3]->vertexNum < 0) continue;
        // Outer gate: legacy water emit at quad.cpp:2742. Skip non-water quads
        // and quads where setupTextures decided no water emission this frame.
        // Fix A (staircase): also include submerged tiles that lack water&1.
        // setupTextures never sets waterHandle for those (quad.cpp:973 gate),
        // but the GPU FS handles them via WaterThickness/shore smoothstep.
        // Recipe lookup is hoisted here so the submerged check and main path
        // share a single find(). On the narrow path all entries are either
        // water-flagged or submerged (terrain.cpp append predicate mirrors this),
        // so the lookup cost is bounded.
        const uint32_t topLeftVN = (uint32_t)q.vertices[0]->vertexNum;
        auto it = g_vertexNumToRecipe.find(topLeftVN);

        bool submergedSandTile = false;
        if (q.waterHandle == 0xffffffffu) {
            if (it == g_vertexNumToRecipe.end()) continue;
            const WaterRecipe& recS = g_recipes[it->second];
            const float we = Terrain::waterElevation;
            // Shore-extension: keep tiles slightly above waterElevation (VS sits them
            // on terrain surface; FS fades via negative-WaterThickness smoothstep).
            // SHORELINE-STAIRCASE-1 (feather floor) + SHORELINE-GEOMETRY-1
            // (meander widening), both folded into EffShoreExt — the single
            // source of truth shared by all four admission mirrors.
            const float shoreExt = EffShoreExt(MapData::alphaDepth);
            if (recS.v0e >= we + shoreExt && recS.v1e >= we + shoreExt &&
                recS.v2e >= we + shoreExt && recS.v3e >= we + shoreExt)
                continue;
            submergedSandTile = true;
        }
        ++waterHandleCount;

        if (it == g_vertexNumToRecipe.end()) {
            ++recipeMissCount;
            continue;
        }

        // Per-triangle pz validity. For each triangle (BOTTOMRIGHT or
        // BOTTOMLEFT diagonal) check that ALL THREE corners' wz ∈ [0,1).
        // wz comes from the water-projected screen.z stored by setupTextures
        // at quad.cpp:715-722. This is THE LOAD-BEARING gate per
        // memory:terrain_tes_projection.md — there is no GPU-side equivalent.
        // Mirrors the legacy per-triangle gVertex.z range check at
        // quad.cpp:2812-2817.
        const float wz0 = q.vertices[0]->wz;
        const float wz1 = q.vertices[1]->wz;
        const float wz2 = q.vertices[2]->wz;
        const float wz3 = q.vertices[3]->wz;
        auto pzOk = [](float z) { return z >= 0.0f && z < 1.0f; };
        const bool ok0 = pzOk(wz0);
        const bool ok1 = pzOk(wz1);
        const bool ok2 = pzOk(wz2);
        const bool ok3 = pzOk(wz3);

        bool pzTri1, pzTri2;
        if (submergedSandTile) {
            // No CPU-side wz for tiles outside water&1 path. VS computes its
            // own clip position from the recipe; let HW near-plane clipping
            // handle any off-frustum vertices.
            pzTri1 = pzTri2 = true;
        } else if (q.uvMode == BOTTOMRIGHT) {
            // tri1=corners[0,1,2], tri2=corners[0,2,3]
            pzTri1 = ok0 && ok1 && ok2;
            pzTri2 = ok0 && ok2 && ok3;
        } else {
            // tri1=corners[0,1,3], tri2=corners[1,2,3]
            pzTri1 = ok0 && ok1 && ok3;
            pzTri2 = ok1 && ok2 && ok3;
        }
        if (!pzTri1 && !pzTri2) {
            ++pzDropCount;
            continue;  // entire quad fails - drop record
        }

        WaterThinRecord tr{};
        tr.recipeIdx = it->second;
        uint32_t flags = 0;
        if (pzTri1) flags |= kWaterThinFlagPzTri1Valid;
        if (pzTri2) flags |= kWaterThinFlagPzTri2Valid;
        tr.flags = flags;
        ++pzValidCount;
        tr.lightRGB0 = q.vertices[0]->lightRGB;
        tr.lightRGB1 = q.vertices[1]->lightRGB;
        tr.lightRGB2 = q.vertices[2]->lightRGB;
        tr.lightRGB3 = q.vertices[3]->lightRGB;
        // Legacy `drawWater()` patches the LOW byte of each vertex's fogRGB
        // with `terrainTypeToMaterial(terrainType)` before queueing
        // (quad.cpp:2781 etc.). The high 24 bits carry the per-vertex
        // FogValue alpha that the FS samples; the low byte carries the
        // material index. The current water FS only consumes the high byte,
        // but for byte-parity with the legacy `addVertices` arg stream we
        // mirror the patch here. Pure CPU; cost is one switch per vertex.
        const uint32_t m0 = terrainTypeToMaterialLocal((uint32_t)q.vertices[0]->pVertex->terrainType);
        const uint32_t m1 = terrainTypeToMaterialLocal((uint32_t)q.vertices[1]->pVertex->terrainType);
        const uint32_t m2 = terrainTypeToMaterialLocal((uint32_t)q.vertices[2]->pVertex->terrainType);
        const uint32_t m3 = terrainTypeToMaterialLocal((uint32_t)q.vertices[3]->pVertex->terrainType);
        tr.fogRGB0   = (q.vertices[0]->fogRGB & 0xFFFFFF00u) | m0;
        tr.fogRGB1   = (q.vertices[1]->fogRGB & 0xFFFFFF00u) | m1;
        tr.fogRGB2   = (q.vertices[2]->fogRGB & 0xFFFFFF00u) | m2;
        tr.fogRGB3   = (q.vertices[3]->fogRGB & 0xFFFFFF00u) | m3;
        g_thinStaging.push_back(tr);

        // B4 Stage 1c: set parity mask bit (topLeftVN is the corner-0 vertexNum).
        if (topLeftVN < (uint32_t)(kWaterParityMaskWords * 32))
            s_waterParityMask[topLeftVN >> 5] |= (1u << (topLeftVN & 31u));
    }

    {
        static bool s_haveLast = false;
        static uint32_t s_lastThin = 0;
        static uint32_t s_lastWaterHandles = 0;
        const uint32_t thinCountNow = (uint32_t)g_thinStaging.size();
        const bool disappeared = (waterHandleCount > 0 && thinCountNow == 0);
        const bool recovered = (s_haveLast && s_lastThin == 0 && thinCountNow > 0);
        if (disappeared || recovered || !s_haveLast) {
            mc2::fmt::print_err("[WATER_STREAM v1] event=thin_summary total_quads={:d} water_handles={:d} thin={:d} pz_valid={:d} recipe_miss={:d} pz_drop={:d} state={} prev_water_handles={:d} prev_thin={:d}\n", mc2::fmt::as_i32(total), mc2::fmt::as_u32(waterHandleCount), mc2::fmt::as_u32(thinCountNow), mc2::fmt::as_u32(pzValidCount), mc2::fmt::as_u32(recipeMissCount), mc2::fmt::as_u32(pzDropCount), mc2::fmt::safe(disappeared ? "disappeared" : (recovered ? "recovered" : "initial")), mc2::fmt::as_u32(s_lastWaterHandles), mc2::fmt::as_u32(s_lastThin));
            fflush(stderr);
        }
        s_haveLast = true;
        s_lastThin = thinCountNow;
        s_lastWaterHandles = waterHandleCount;
    }

    const uint32_t thinCount = (uint32_t)g_thinStaging.size();
    if (thinCount == 0) return 0;

    const GLsizeiptr slotBytes = (GLsizeiptr)(thinCount * sizeof(WaterThinRecord));

    // Lazy alloc / grow the ring buffer.
    if (g_thinBuffer == 0 || (uint32_t)slotBytes > g_thinSlotCapacity) {
        if (g_thinBuffer != 0) {
            glDeleteBuffers(1, &g_thinBuffer);
            g_thinBuffer = 0;
            InvalidateWaterBufferOwner(g_thinBufferOwner);  // WATER-REGISTRY-0: realloc-on-grow
        }
        glGenBuffers(1, &g_thinBuffer);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_thinBuffer);
        // Round capacity up to leave headroom (max recipe count is the
        // worst case — every map water quad in the window simultaneously).
        const uint32_t capPerSlot =
            (uint32_t)(g_recipes.size() * sizeof(WaterThinRecord));
        uint32_t cap = (capPerSlot > (uint32_t)slotBytes)
                              ? capPerSlot : (uint32_t)slotBytes;
        // SSBO-BIND-ALIGN: slotOffset = g_thinSlot * g_thinSlotCapacity is the
        // glBindBufferRange offset; it must be a multiple of
        // GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT (256 on NVIDIA) or slots 1+ are
        // rejected with GL_INVALID_VALUE -> the water thin SSBO never binds -> water
        // flashes/vanishes on NVIDIA (AMD tolerated it). Pad the per-slot byte
        // capacity up to the alignment so every slot offset is aligned. (Drives the
        // alloc below and the per-frame slotOffset.)
        cap = (uint32_t)gpuAlignUp((unsigned long long)cap,
                                   (unsigned long long)gpuSsboOffsetAlignment());
        MC2_GL_BufferData(GL_SHADER_STORAGE_BUFFER,
                     (GLsizeiptr)(cap * kThinRingSlots),
                     nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        g_thinSlotCapacity = cap;
        g_thinSlot = 0;
        // WATER-REGISTRY-0: register owner after final sizing (ring = 3 slots).
        RegisterWaterBufferOwner(g_thinBufferOwner, g_thinBuffer,
                                 (uint64_t)cap * kThinRingSlots);
    }

    g_thinSlot = (g_thinSlot + 1) % kThinRingSlots;
    WaitAndClearThinFenceForCurrentSlot((uint32_t)slotBytes);   // WATER-THINRING-FENCE-1
    const GLintptr slotOffset = (GLintptr)(g_thinSlot * g_thinSlotCapacity);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_thinBuffer);
    MC2_GL_BufferSubData(GL_SHADER_STORAGE_BUFFER, slotOffset, slotBytes,
                    g_thinStaging.data());
    gpuBindSsboRange(kWaterThinSsboBinding, g_thinBuffer,
                     (long long)slotOffset, (long long)slotBytes,
                     "water.thin.upload");

    if (DebugOn()) {
        static uint32_t s_diagFramesPrinted = 0;
        static uint32_t s_diagFrameCounter = 0;
        if (++s_diagFrameCounter >= 1200 && s_diagFramesPrinted < 5) {
            ++s_diagFramesPrinted;
            mc2::fmt::print_err("[WATER_STREAM v1] event=thin_upload records={:d} pz_valid={:d}\n", mc2::fmt::as_u32(thinCount), mc2::fmt::as_u32(pzValidCount));
            fflush(stderr);
        }
    }

    return thinCount;
}

// ----------------------------------------------------------------------------
// Stage 3: parity check
// ----------------------------------------------------------------------------
namespace {

bool s_parityEnabledKnown = false;
bool s_parityEnabled      = false;

uint64_t s_parityFrameCounter   = 0;
uint64_t s_parityQuadsChecked   = 0;
uint64_t s_parityMismatchTotal  = 0;

// GPU-driven parity counters — gated by gpu_driven::IsParityEnabled()
static uint64_t s_gpuParityFrames    = 0;
static uint64_t s_gpuParityQuads     = 0;
static uint64_t s_gpuParityMismatches = 0;

constexpr uint32_t kMaxMismatchPrintsPerFrame = 16;
constexpr uint64_t kSummaryEveryFrames        = 600;

// Look up a per-corner DWORD-byte from a packed uint (corner 0 in low byte).
inline uint32_t cornerByte(uint32_t packed, uint32_t cornerIdx) {
    return (packed >> (cornerIdx * 8u)) & 0xFFu;
}

// Replicate the legacy alphaMode classifier from quad.cpp:2825-2856.
inline uint32_t legacyAlphaMode(float elev, float waterElevation,
                                 float alphaDepth,
                                 uint32_t alphaEdgeDw, uint32_t alphaMiddleDw,
                                 uint32_t alphaDeepDw)
{
    uint32_t mode = alphaMiddleDw;
    if (elev >= (waterElevation - alphaDepth))             mode = alphaEdgeDw;
    if (elev <= (waterElevation - (alphaDepth * 3.0f)))    mode = alphaDeepDw;
    return mode;
}

// Replicate the wave-displacement modulator from setupTextures' water-projection
// block at quad.cpp:689-700 (and the fast-path VS waveOurCos at
// shaders/gos_terrain_water_fast.vert:133).
inline float legacyWaveOurCos(uint32_t waterBits, float frameCos) {
    if ((waterBits & 0x80u) != 0u) return -frameCos;
    return frameCos;
}

// Per-tri corner index table. uvMode 0 = BOTTOMRIGHT (BR), 1 = BOTTOMLEFT (BL).
//   BR top: 0,1,2  bot: 0,2,3
//   BL top: 0,1,3  bot: 1,2,3
inline uint32_t triCorner(uint32_t uvMode, uint32_t tri, uint32_t vert) {
    if (uvMode == 0) {
        if (tri == 0) { return (vert == 0) ? 0u : (vert == 1) ? 1u : 2u; }
        else          { return (vert == 0) ? 0u : (vert == 1) ? 2u : 3u; }
    } else {
        if (tri == 0) { return (vert == 0) ? 0u : (vert == 1) ? 1u : 3u; }
        else          { return (vert == 0) ? 1u : (vert == 1) ? 2u : 3u; }
    }
}

inline bool pzInRange(float z) { return z >= 0.0f && z < 1.0f; }

// One-shot per-frame mismatch counter for throttling.
struct MismatchPrintBudget {
    uint32_t printed = 0;
    bool canPrint() { return printed < kMaxMismatchPrintsPerFrame; }
    void note()    { ++printed; }
};

inline void printMismatch(MismatchPrintBudget& budget,
                          uint64_t frame, uint32_t quadIdx,
                          const char* layer, uint32_t tri, uint32_t vert,
                          const char* field,
                          uint32_t legacyBits, uint32_t fastBits)
{
    if (!budget.canPrint()) return;
    budget.note();
    mc2::fmt::print_err("[WATER_PARITY v1] event=mismatch frame={:d} quad={:d} layer={} tri={:d} vert={:d} field={} legacy=0x{:08x} fast=0x{:08x}\n", mc2::fmt::as_u64((unsigned long long)frame), mc2::fmt::as_u32((unsigned)quadIdx), mc2::fmt::safe(layer), mc2::fmt::as_u32((unsigned)tri), mc2::fmt::as_u32((unsigned)vert), mc2::fmt::safe(field), mc2::fmt::as_u32((unsigned)legacyBits), mc2::fmt::as_u32((unsigned)fastBits));
    fflush(stderr);
}

// `screen=0` (recipe) or `screen=1` (thin) field tag for a non-derived field.
inline void printFieldMismatch(MismatchPrintBudget& budget,
                               uint64_t frame, uint32_t quadIdx,
                               const char* scope, uint32_t cornerIdx,
                               const char* field,
                               uint32_t legacyBits, uint32_t fastBits)
{
    if (!budget.canPrint()) return;
    budget.note();
    mc2::fmt::print_err("[WATER_PARITY v1] event=mismatch frame={:d} quad={:d} scope={} corner={:d} field={} legacy=0x{:08x} fast=0x{:08x}\n", mc2::fmt::as_u64((unsigned long long)frame), mc2::fmt::as_u32((unsigned)quadIdx), mc2::fmt::safe(scope), mc2::fmt::as_u32((unsigned)cornerIdx), mc2::fmt::safe(field), mc2::fmt::as_u32((unsigned)legacyBits), mc2::fmt::as_u32((unsigned)fastBits));
    fflush(stderr);
}

inline uint32_t bitcastFloatToUint(float f) {
    uint32_t u = 0;
    static_assert(sizeof(u) == sizeof(f), "float must be 32 bits");
    memcpy(&u, &f, sizeof(u));
    return u;
}

} // namespace

void CheckParityFrame(const ParityFrameUniforms& u) {
    if (!s_parityEnabledKnown) {
        s_parityEnabled = mc2::gate::get<"MC2_RENDER_WATER_PARITY_CHECK">();
        s_parityEnabledKnown = true;
        if (s_parityEnabled) {
            mc2::fmt::print_err("[WATER_PARITY v1] event=enabled note=silent_on_pass scope=stock_tier1_only print_budget={:d} summary_every={:d}\n", mc2::fmt::as_u32((unsigned)kMaxMismatchPrintsPerFrame), mc2::fmt::as_u64((unsigned long long)kSummaryEveryFrames));
            fflush(stderr);
        }
    }
    if (!s_parityEnabled) return;
    if (!g_ready || g_recipes.empty()) return;

    const uint64_t frame = ++s_parityFrameCounter;
    MismatchPrintBudget budget;

    const TerrainPtr terrainPtr = land;
    const TerrainQuadPtr quads  = terrainPtr ? terrainPtr->getQuadList() : nullptr;
    const long total            = terrainPtr ? terrainPtr->getNumQuads() : 0;
    if (!quads || total <= 0) return;

    // Walk quads in the SAME order UploadAndBindThinRecords uses, so the i-th
    // qualifying quad matches g_thinStaging[i] by construction. No lookup map.
    uint32_t thinIdx = 0;
    const uint32_t thinCount = (uint32_t)g_thinStaging.size();

    for (long i = 0; i < total; ++i) {
        const TerrainQuad& q = quads[i];
        if (!q.vertices[0] || !q.vertices[1] || !q.vertices[2] || !q.vertices[3]) continue;
        // Mirror UploadAndBindThinRecords' all-corners-valid gate; map-edge
        // blankVertex degenerate quads have no recipe and emit garbage in
        // legacy. Treat both paths as no-emit on these quads.
        if (q.vertices[0]->vertexNum < 0 || q.vertices[1]->vertexNum < 0 ||
            q.vertices[2]->vertexNum < 0 || q.vertices[3]->vertexNum < 0) continue;
        if (q.waterHandle == 0xffffffffu) continue;

        const uint32_t topLeftVN = (uint32_t)q.vertices[0]->vertexNum;
        auto it = g_vertexNumToRecipe.find(topLeftVN);
        if (it == g_vertexNumToRecipe.end()) {
            // Recipe miss: the static recipe didn't classify this quad as
            // water-bearing, but legacy is about to emit. Real bug class.
            printFieldMismatch(budget, frame, topLeftVN,
                               "recipe", 0, "lookup_miss", 1u, 0u);
            continue;
        }

        const WaterRecipe& rec = g_recipes[it->second];

        // Per-vertex pz validity exactly mirrors UploadAndBindThinRecords.
        const float wz0 = q.vertices[0]->wz;
        const float wz1 = q.vertices[1]->wz;
        const float wz2 = q.vertices[2]->wz;
        const float wz3 = q.vertices[3]->wz;
        const bool ok0 = pzInRange(wz0);
        const bool ok1 = pzInRange(wz1);
        const bool ok2 = pzInRange(wz2);
        const bool ok3 = pzInRange(wz3);
        bool pzTri1, pzTri2;
        if (q.uvMode == BOTTOMRIGHT) {
            pzTri1 = ok0 && ok1 && ok2;
            pzTri2 = ok0 && ok2 && ok3;
        } else {
            pzTri1 = ok0 && ok1 && ok3;
            pzTri2 = ok1 && ok2 && ok3;
        }
        if (!pzTri1 && !pzTri2) continue;  // dropped — no thin record

        // Bounds-check thin record before deref.
        if (thinIdx >= thinCount) {
            printFieldMismatch(budget, frame, topLeftVN,
                               "thin", 0, "missing_record", 1u, 0u);
            ++thinIdx;
            continue;
        }
        const WaterThinRecord& trec = g_thinStaging[thinIdx];
        ++thinIdx;
        ++s_parityQuadsChecked;

        // 1. Recipe-input parity ---------------------------------------------
        // Verify recipe carries the same per-corner data that the legacy emit
        // would read from q.vertices[i] / blocks[].
        const VertexPtr v[4] = { q.vertices[0], q.vertices[1],
                                  q.vertices[2], q.vertices[3] };
        const float recVx[4] = { rec.v0x, rec.v1x, rec.v2x, rec.v3x };
        const float recVy[4] = { rec.v0y, rec.v1y, rec.v2y, rec.v3y };
        const float recElev[4] = { rec.v0e, rec.v1e, rec.v2e, rec.v3e };
        for (uint32_t c = 0; c < 4; ++c) {
            if (recVx[c] != v[c]->vx)
                printFieldMismatch(budget, frame, topLeftVN, "recipe", c, "vx",
                                   bitcastFloatToUint(v[c]->vx),
                                   bitcastFloatToUint(recVx[c]));
            if (recVy[c] != v[c]->vy)
                printFieldMismatch(budget, frame, topLeftVN, "recipe", c, "vy",
                                   bitcastFloatToUint(v[c]->vy),
                                   bitcastFloatToUint(recVy[c]));
            const float legElev = (float)v[c]->pVertex->elevation;
            if (recElev[c] != legElev)
                printFieldMismatch(budget, frame, topLeftVN, "recipe", c, "elevation",
                                   bitcastFloatToUint(legElev),
                                   bitcastFloatToUint(recElev[c]));
            const uint32_t recTType = cornerByte(rec.terrainTypes, c);
            const uint32_t legTType = (uint32_t)(uint8_t)v[c]->pVertex->terrainType;
            if (recTType != legTType)
                printFieldMismatch(budget, frame, topLeftVN, "recipe", c,
                                   "terrainType", legTType, recTType);
            const uint32_t recWBits = cornerByte(rec.waterBits, c);
            const uint32_t legWBits = (uint32_t)(v[c]->pVertex->water & 0xFFu);
            if (recWBits != legWBits)
                printFieldMismatch(budget, frame, topLeftVN, "recipe", c,
                                   "waterBits", legWBits, recWBits);
        }
        // uvMode parity (recipe.flags bit 0 vs q.uvMode).
        const uint32_t recUvMode = (rec.flags & kFlagBitUvModeBottomLeft) ? 1u : 0u;
        const uint32_t legUvMode = (q.uvMode == BOTTOMRIGHT) ? 0u : 1u;
        if (recUvMode != legUvMode)
            printFieldMismatch(budget, frame, topLeftVN, "recipe", 0, "uvMode",
                               legUvMode, recUvMode);
        // hasDetail parity (recipe.flags bit 1 vs runtime terrainTextures2 presence).
        const uint32_t recHasDetail = (rec.flags & kFlagBitHasDetail) ? 1u : 0u;
        const uint32_t legHasDetail = u.terrainTextures2Present ? 1u : 0u;
        if (recHasDetail != legHasDetail)
            printFieldMismatch(budget, frame, topLeftVN, "recipe", 0, "hasDetail",
                               legHasDetail, recHasDetail);

        // 2. Thin-record parity ---------------------------------------------
        // recipeIdx
        if (trec.recipeIdx != it->second)
            printFieldMismatch(budget, frame, topLeftVN, "thin", 0, "recipeIdx",
                               (uint32_t)it->second, trec.recipeIdx);
        // pz bits
        const uint32_t expectedFlags =
            (pzTri1 ? kWaterThinFlagPzTri1Valid : 0u) |
            (pzTri2 ? kWaterThinFlagPzTri2Valid : 0u);
        if ((trec.flags & (kWaterThinFlagPzTri1Valid | kWaterThinFlagPzTri2Valid))
            != expectedFlags) {
            printFieldMismatch(budget, frame, topLeftVN, "thin", 0, "pz_flags",
                               expectedFlags, trec.flags);
        }
        // per-corner lightRGB / fogRGB
        const uint32_t trLight[4] = { trec.lightRGB0, trec.lightRGB1,
                                       trec.lightRGB2, trec.lightRGB3 };
        const uint32_t trFog[4]   = { trec.fogRGB0, trec.fogRGB1,
                                       trec.fogRGB2, trec.fogRGB3 };
        for (uint32_t c = 0; c < 4; ++c) {
            if (trLight[c] != v[c]->lightRGB)
                printFieldMismatch(budget, frame, topLeftVN, "thin", c, "lightRGB",
                                   v[c]->lightRGB, trLight[c]);
            // The thin record's fogRGB has its low byte patched with
            // terrainTypeToMaterial(terrainType) at upload time so the GPU's
            // emitted gos_VERTEX matches legacy's `(fogRGB & 0xFFFFFF00) |
            // material` per quad.cpp:2781. Parity expectation matches.
            const uint32_t expectFog =
                (v[c]->fogRGB & 0xFFFFFF00u) |
                terrainTypeToMaterialLocal((uint32_t)v[c]->pVertex->terrainType);
            if (trFog[c] != expectFog)
                printFieldMismatch(budget, frame, topLeftVN, "thin", c, "fogRGB",
                                   expectFog, trFog[c]);
        }

        // 3. Derived gos_VERTEX byte parity (u, v, argb, frgb-high-byte) -----
        // Both sides synthesize on CPU using the same uniforms; identity by
        // construction once recipe + thin-record fields above match. Surfaces
        // formula divergence (e.g. cornerIdx mapping bug, uvMode swap, alpha-
        // band classifier ordering bug, MaxMinUV wrap miscompute).
        for (uint32_t tri = 0; tri < 2; ++tri) {
            const bool pzOkTri = (tri == 0) ? pzTri1 : pzTri2;

            // Legacy emit gate (per-tri):
            //   base   : pzOkTri && (alphaMode0+alphaMode1+alphaMode2 != 0)
            //   detail : pzOkTri && useWaterInterestTexture &&
            //            q.waterDetailHandle != 0xffffffff
            // Compute alphaMode sum to mirror legacy gate at quad.cpp:2886.
            const uint32_t triAM[3] = {
                legacyAlphaMode((float)v[triCorner(legUvMode, tri, 0)]->pVertex->elevation,
                                u.waterElevation, u.alphaDepth,
                                u.alphaEdgeDword, u.alphaMiddleDword, u.alphaDeepDword),
                legacyAlphaMode((float)v[triCorner(legUvMode, tri, 1)]->pVertex->elevation,
                                u.waterElevation, u.alphaDepth,
                                u.alphaEdgeDword, u.alphaMiddleDword, u.alphaDeepDword),
                legacyAlphaMode((float)v[triCorner(legUvMode, tri, 2)]->pVertex->elevation,
                                u.waterElevation, u.alphaDepth,
                                u.alphaEdgeDword, u.alphaMiddleDword, u.alphaDeepDword),
            };
            const uint32_t alphaSum = triAM[0] + triAM[1] + triAM[2];
            const bool legBaseEmit   = pzOkTri && (alphaSum != 0u);
            const bool legDetailEmit = pzOkTri && u.useWaterInterestTexture &&
                                       (q.waterDetailHandle != 0xffffffffu);
            // Fast-path side: emits base whenever the per-tri pz bit is set.
            // Detail emits when the recipe.hasDetail bit is set AND the runtime
            // detail handle is bound (per-frame uniform). Per-quad equivalence:
            const bool fastBaseEmit   = pzOkTri;
            const bool fastDetailEmit = pzOkTri && (rec.flags & kFlagBitHasDetail) &&
                                        (u.waterDetailHandleSentinel != 0xffffffffu);
            if (legBaseEmit != fastBaseEmit) {
                printMismatch(budget, frame, topLeftVN, "base", tri, 0,
                              "emit_gate", legBaseEmit ? 1u : 0u, fastBaseEmit ? 1u : 0u);
            }
            if (legDetailEmit != fastDetailEmit) {
                printMismatch(budget, frame, topLeftVN, "detail", tri, 0,
                              "emit_gate", legDetailEmit ? 1u : 0u, fastDetailEmit ? 1u : 0u);
            }
            if (!legBaseEmit && !legDetailEmit) continue;

            // Per-vertex CPU-side synthesis. We compute u, v, argb, frgb-high
            // for each of the 3 verts, in cornerIdx-resolved order.
            // Pre-compute MaxMinUV wrap shift (legacy quad.cpp:2863-2884).
            float uPre[3], vPre[3];
            float uDetPre[3], vDetPre[3];
            for (uint32_t k = 0; k < 3; ++k) {
                const uint32_t c = triCorner(legUvMode, tri, k);
                uPre[k] = (v[c]->vx - u.mapTopLeftX) * u.oneOverTF + u.cloudOffsetX;
                vPre[k] = (u.mapTopLeftY - v[c]->vy) * u.oneOverTF + u.cloudOffsetY;
                uDetPre[k] = (v[c]->vx - u.mapTopLeftX) * u.oneOverWaterTF + u.sprayOffsetX;
                vDetPre[k] = (u.mapTopLeftY - v[c]->vy) * u.oneOverWaterTF + u.sprayOffsetY;
            }
            // Legacy wrap correction (matches drawWater() block at 2863-2884).
            auto applyWrap = [&](float (&uu)[3], float (&vv)[3], float maxMinUV) {
                if ((uu[0] > maxMinUV) || (vv[0] > maxMinUV) ||
                    (uu[1] > maxMinUV) || (vv[1] > maxMinUV) ||
                    (uu[2] > maxMinUV) || (vv[2] > maxMinUV))
                {
                    float maxU = uu[0]; if (uu[1]>maxU) maxU=uu[1]; if (uu[2]>maxU) maxU=uu[2];
                    float maxV = vv[0]; if (vv[1]>maxV) maxV=vv[1]; if (vv[2]>maxV) maxV=vv[2];
                    maxU = floorf(maxU - (maxMinUV - 1.0f));
                    maxV = floorf(maxV - (maxMinUV - 1.0f));
                    uu[0] -= maxU; uu[1] -= maxU; uu[2] -= maxU;
                    vv[0] -= maxV; vv[1] -= maxV; vv[2] -= maxV;
                }
            };
            float uLeg[3]    = { uPre[0], uPre[1], uPre[2] };
            float vLeg[3]    = { vPre[0], vPre[1], vPre[2] };
            float uLegD[3]   = { uDetPre[0], uDetPre[1], uDetPre[2] };
            float vLegD[3]   = { vDetPre[0], vDetPre[1], vDetPre[2] };
            applyWrap(uLeg, vLeg, u.maxMinUV);
            // Detail layer is memcpy'd from base BEFORE base's wrap shift in
            // legacy (quad.cpp:2897 `memcpy(sVertex, gVertex, ...)` happens
            // AFTER the base wrap-shift block exits the inner if). The detail
            // ARGB is then patched, but UVs are reassigned to detail-scale —
            // detail UVs do NOT inherit the base wrap shift. They compute
            // fresh against oneOverWaterTF + sprayOffset, with NO wrap-shift
            // applied (legacy doesn't apply the MaxMinUV wrap to detail). Fast
            // path mirrors this: detail uses uvScale=oneOverWaterTF + uvOffset
            // = sprayOffset, with the MaxMinUV wrap branch firing only against
            // the detail-derived UVs (which are typically much smaller, so
            // wrap rarely applies). To stay byte-faithful, parity-check the
            // wrap behavior on the detail layer the same way the VS does:
            applyWrap(uLegD, vLegD, u.maxMinUV);

            // Fast-path-equivalent CPU synthesis. Recipe lives in `rec`; per-
            // frame uniforms in `u`. cornerIdx → recipe's stored vx/vy,
            // identical to legacy v[c]->vx/vy when recipe parity holds.
            float uFast[3], vFast[3], uFastD[3], vFastD[3];
            for (uint32_t k = 0; k < 3; ++k) {
                const uint32_t c   = triCorner(recUvMode, tri, k);
                const float    rvx = (c == 0) ? rec.v0x : (c == 1) ? rec.v1x : (c == 2) ? rec.v2x : rec.v3x;
                const float    rvy = (c == 0) ? rec.v0y : (c == 1) ? rec.v1y : (c == 2) ? rec.v2y : rec.v3y;
                uFast[k]  = (rvx - u.mapTopLeftX) * u.oneOverTF + u.cloudOffsetX;
                vFast[k]  = (u.mapTopLeftY - rvy) * u.oneOverTF + u.cloudOffsetY;
                uFastD[k] = (rvx - u.mapTopLeftX) * u.oneOverWaterTF + u.sprayOffsetX;
                vFastD[k] = (u.mapTopLeftY - rvy) * u.oneOverWaterTF + u.sprayOffsetY;
            }
            applyWrap(uFast, vFast, u.maxMinUV);
            applyWrap(uFastD, vFastD, u.maxMinUV);

            // Derived ARGB (base): legacy = (vertices[c]->lightRGB & 0x00ffffff) | alphaMode_c
            // Fast path equivalent: (thinRec.lightRGB[c] & 0x00FFFFFFu) |
            //                       (alphaByte_from_elev << 24)
            // alphaByte fast path = (alphaModeDword >> 24) & 0xFF for the same
            // band classifier — yields identical DWORD when the legacy
            // alphaMode DWORDs match the bridge-passed bytes (which they do,
            // since the bridge uses (alphaEdge >> 24) & 0xFF etc.).
            // Derived ARGB (detail): legacy = (sVertex.argb & 0xff000000) | 0x00ffffff
            // Fast detail equivalent: (alphaByte << 24) | 0x00FFFFFFu
            // We replicate both formulas and compare.
            for (uint32_t k = 0; k < 3; ++k) {
                const uint32_t legC  = triCorner(legUvMode, tri, k);
                const uint32_t recC  = triCorner(recUvMode, tri, k);

                // base u/v
                if (bitcastFloatToUint(uLeg[k]) != bitcastFloatToUint(uFast[k]))
                    printMismatch(budget, frame, topLeftVN, "base", tri, k, "u",
                                  bitcastFloatToUint(uLeg[k]),
                                  bitcastFloatToUint(uFast[k]));
                if (bitcastFloatToUint(vLeg[k]) != bitcastFloatToUint(vFast[k]))
                    printMismatch(budget, frame, topLeftVN, "base", tri, k, "v",
                                  bitcastFloatToUint(vLeg[k]),
                                  bitcastFloatToUint(vFast[k]));
                // detail u/v
                if (bitcastFloatToUint(uLegD[k]) != bitcastFloatToUint(uFastD[k]))
                    printMismatch(budget, frame, topLeftVN, "detail", tri, k, "u",
                                  bitcastFloatToUint(uLegD[k]),
                                  bitcastFloatToUint(uFastD[k]));
                if (bitcastFloatToUint(vLegD[k]) != bitcastFloatToUint(vFastD[k]))
                    printMismatch(budget, frame, topLeftVN, "detail", tri, k, "v",
                                  bitcastFloatToUint(vLegD[k]),
                                  bitcastFloatToUint(vFastD[k]));

                // base argb
                const uint32_t legAlphaMode =
                    legacyAlphaMode((float)v[legC]->pVertex->elevation,
                                    u.waterElevation, u.alphaDepth,
                                    u.alphaEdgeDword, u.alphaMiddleDword, u.alphaDeepDword);
                const uint32_t legArgbBase = (v[legC]->lightRGB & 0x00ffffffu) | (legAlphaMode & 0xff000000u);
                // Fast path: alpha byte derived from elev band (recipe elev),
                // OR'd with low 24 bits of thinRec.lightRGB[c].
                const float    fastElev      = (recC == 0) ? rec.v0e : (recC == 1) ? rec.v1e : (recC == 2) ? rec.v2e : rec.v3e;
                uint32_t       fastAlphaByte = (u.alphaMiddleDword >> 24) & 0xFFu;
                if (fastElev >= (u.waterElevation - u.alphaDepth))
                    fastAlphaByte = (u.alphaEdgeDword >> 24) & 0xFFu;
                if (fastElev <= (u.waterElevation - (u.alphaDepth * 3.0f)))
                    fastAlphaByte = (u.alphaDeepDword >> 24) & 0xFFu;
                const uint32_t fastLight = trLight[recC];
                const uint32_t fastArgbBase = (fastLight & 0x00ffffffu) | (fastAlphaByte << 24);
                if (legArgbBase != fastArgbBase)
                    printMismatch(budget, frame, topLeftVN, "base", tri, k, "argb",
                                  legArgbBase, fastArgbBase);

                // detail argb
                const uint32_t legArgbDetail  = (legArgbBase & 0xff000000u) | 0x00ffffffu;
                const uint32_t fastArgbDetail = (fastAlphaByte << 24) | 0x00ffffffu;
                if (legArgbDetail != fastArgbDetail)
                    printMismatch(budget, frame, topLeftVN, "detail", tri, k, "argb",
                                  legArgbDetail, fastArgbDetail);

                // frgb high byte (FogValue) — only consumed byte downstream.
                const uint32_t legFrgbHi  = (v[legC]->fogRGB >> 24) & 0xFFu;
                const uint32_t fastFrgbHi = (trFog[recC]   >> 24) & 0xFFu;
                if (legFrgbHi != fastFrgbHi)
                    printMismatch(budget, frame, topLeftVN, "base", tri, k, "frgb_hi",
                                  legFrgbHi, fastFrgbHi);

                // frgb low byte: legacy patches with terrainTypeToMaterial; the
                // current fast-path FS uses only FogValue (high byte). The low
                // byte is therefore an information-only diff: surface it but
                // tag the field so future readers know it's pixel-irrelevant
                // today. If we ever wire material into the water FS, the
                // mismatch will already be calling this out.
                const uint32_t legFrgbLo =
                    terrainTypeToMaterialLocal((uint32_t)v[legC]->pVertex->terrainType);
                const uint32_t fastFrgbLo = (trFog[recC] & 0xFFu);
                if (legFrgbLo != fastFrgbLo) {
                    // After the UploadAndBindThinRecords fix that ORs material
                    // into the thin record's fogRGB low byte, this should be
                    // silent. If it fires, either the thin-record builder lost
                    // the material patch or quad.cpp's terrainTypeToMaterial
                    // table drifted from the local copy — both worth surfacing.
                    printMismatch(budget, frame, topLeftVN, "base", tri, k,
                                  "frgb_lo", legFrgbLo, fastFrgbLo);
                }
            }
        }

        if (budget.printed > 0) {
            // The first mismatch this frame already incremented; bump the
            // global tally only once per frame to keep the summary line
            // meaningful (counts frames-with-issue, not raw print events).
        }
    }

    if (budget.printed > 0) s_parityMismatchTotal += budget.printed;

    if ((frame % kSummaryEveryFrames) == 0) {
        mc2::fmt::print_err("[WATER_PARITY v1] event=summary frames={:d} quads_checked={:d} total_mismatches={:d}\n", mc2::fmt::as_u64((unsigned long long)frame), mc2::fmt::as_u64((unsigned long long)s_parityQuadsChecked), mc2::fmt::as_u64((unsigned long long)s_parityMismatchTotal));
        fflush(stderr);
    }
}

// ---------------------------------------------------------------------------
// Phase C Stage 1 implementation
// ---------------------------------------------------------------------------

// BuildQuadWindowSSBO — populate per-frame recipe-index window for the GPU compute dispatch.
//
// When SOLID is armed (IsFrameSolidArmed() == true): setupTextures is gated off,
// so waterHandle is never set. terrain.cpp populates g_narrowQuadsThisFrame using
// the pVertex->water & 1 primary gate (same predicate as quad.cpp:956-959 water block
// entry). The GPU compute shader's pzOk gate (gpu_driven_water.comp:236) handles the
// secondary clip-range check, replacing the clipped1||clipped2 gate in setupTextures.
//
// When SOLID is NOT armed (legacy path): use the per-frame narrow walk filtered by
// waterHandle, matching UploadAndBindThinRecords' gate exactly.
//
// Returns the count of window entries written (0 if none).
static uint32_t BuildQuadWindowSSBO() {
    if (!g_ready || g_recipes.empty()) return 0;

    g_quadWindowStaging.clear();

    // H1b: time the CPU hash-lookup loop separately from the GPU upload.
    {
        mc2_hitch::HitchScope _hBuild(mc2_hitch::HitchSpanKind::WaterBuildWindow);
        if (gos_terrain_indirect::IsFrameSolidArmed()) {
            // Armed path: narrow list populated by terrain.cpp via clipInfo gate.
            // No waterHandle check — GPU pz gate (gpu_driven_water.comp:236) handles it.
            const long iterCount = (long)g_narrowQuadsThisFrame.size();
            g_quadWindowStaging.reserve((size_t)iterCount);
            for (long i = 0; i < iterCount; ++i) {
                const TerrainQuad& q = *g_narrowQuadsThisFrame[(size_t)i];
                if (!q.vertices[0] || q.vertices[0]->vertexNum < 0) continue;
                const uint32_t topLeftVN = (uint32_t)q.vertices[0]->vertexNum;
                auto it = g_vertexNumToRecipe.find(topLeftVN);
                if (it == g_vertexNumToRecipe.end()) continue;
                g_quadWindowStaging.push_back(it->second);
            }
        } else {
            const TerrainPtr terrainPtr = land;
            const TerrainQuadPtr quads  = terrainPtr ? terrainPtr->getQuadList()  : nullptr;
            const long total            = terrainPtr ? terrainPtr->getNumQuads()  : 0;
            if (!quads || total <= 0) return 0;

            const bool narrow   = NarrowEnabledImpl();
            const TerrainQuadPtr* narrowArr = narrow && !g_narrowQuadsThisFrame.empty()
                                              ? g_narrowQuadsThisFrame.data() : nullptr;
            const long iterCount = narrow ? (long)g_narrowQuadsThisFrame.size() : total;

            g_quadWindowStaging.reserve((size_t)iterCount);

            for (long i = 0; i < iterCount; ++i) {
                const TerrainQuad& q = narrowArr ? *narrowArr[i] : quads[i];
                if (!q.vertices[0] || !q.vertices[1] ||
                    !q.vertices[2] || !q.vertices[3]) continue;
                if (q.vertices[0]->vertexNum < 0 || q.vertices[1]->vertexNum < 0 ||
                    q.vertices[2]->vertexNum < 0 || q.vertices[3]->vertexNum < 0) continue;
                if (q.waterHandle == 0xffffffffu) continue;

                const uint32_t topLeftVN = (uint32_t)q.vertices[0]->vertexNum;
                auto it = g_vertexNumToRecipe.find(topLeftVN);
                if (it == g_vertexNumToRecipe.end()) continue;

                g_quadWindowStaging.push_back(it->second);
            }
        }
    } // end WaterBuildWindow scope

    const uint32_t windowCount = (uint32_t)g_quadWindowStaging.size();
    if (windowCount == 0) return 0;

    // Grow g_quadWindowSsbo lazily if capacity is too small.
    // Capacity is sized to the recipe count (all water quads simultaneously).
    // Use the CPU-side g_quadWindowSsboCapacity mirror to avoid a
    // glGetBufferParameteriv GPU round-trip every frame.
    if (g_quadWindowSsbo != 0 &&
        g_quadWindowSsboCapacity < windowCount * (uint32_t)sizeof(uint32_t)) {
        glDeleteBuffers(1, &g_quadWindowSsbo);
        g_quadWindowSsbo = 0;
        g_quadWindowSsboCapacity = 0;
    }
    if (g_quadWindowSsbo == 0) {
        const uint32_t cap = (uint32_t)(g_recipes.size() * sizeof(uint32_t));
        // TIER2-EXCLUDED: dead-path
        glGenBuffers(1, &g_quadWindowSsbo);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_quadWindowSsbo);
        MC2_GL_BufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)cap, nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        g_quadWindowSsboCapacity = cap;
    }

    // H1b: time the GPU upload separately.
    {
        mc2_hitch::HitchScope _hUpload(mc2_hitch::HitchSpanKind::WaterUpload);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_quadWindowSsbo);
        MC2_GL_BufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                        (GLsizeiptr)(windowCount * sizeof(uint32_t)),
                        g_quadWindowStaging.data());
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    } // end WaterUpload scope

    return windowCount;
}

bool ComputeDispatchAndBindThinRecords(float frameCos) {
    g_waterGpuDrivenArmed = false;
    mc2_hitch::HitchScope _hitchWater(mc2_hitch::HitchSpanKind::WaterFastPath);
    if (g_mc2HitchEnabled) {
        ++g_mc2HitchAccum.waterCalled;
        g_mc2HitchAccum.waterPresent = g_recipes.empty() ? 0u : 1u;
    }

    {
        mc2_hitch::HitchScope _hGuards(mc2_hitch::HitchSpanKind::WaterGuards);
        if (!gpu_driven::IsWaterEnabled()) {
            if (g_mc2HitchEnabled) ++g_mc2HitchAccum.waterEarlyOut;
            return false;
        }

        // Lazy-build compute programs and GPU resources on first call.
        if (g_waterComputeProgram == 0) {
        g_waterComputeProgram = gpu_driven::BuildComputeProgramFromFile(
            "shaders/gpu_driven_water.comp", nullptr, 0, "gpu_driven_water");
        g_cmdPatchProgram = gpu_driven::BuildComputeProgramFromFile(
            "shaders/gpu_driven_cmd_patch.comp", nullptr, 0, "gpu_driven_cmd_patch");
        if (!g_waterComputeProgram || !g_cmdPatchProgram) {
            // Don't leave partial state.
            if (g_waterComputeProgram) { glDeleteProgram(g_waterComputeProgram); g_waterComputeProgram = 0; }
            if (g_cmdPatchProgram)     { glDeleteProgram(g_cmdPatchProgram);     g_cmdPatchProgram = 0; }
            if (g_mc2HitchEnabled) ++g_mc2HitchAccum.waterEarlyOut;
            return false;
        }

        // Bucket header: 16 B GpuDrivenBucketHeader (visibleCount + 3 pads).
        glGenBuffers(1, &g_waterBucketHeaderSsbo);
        g_waterBucketHeaderSsboOwner.backendHandle = g_waterBucketHeaderSsbo;  // WATER-REGISTRY-0: mirror (pattern b)
        RegisterWaterBufferOwner(g_waterBucketHeaderSsboOwner, g_waterBucketHeaderSsbo, 16u); // WATER-REGISTRY-0
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_waterBucketHeaderSsbo);
        MC2_GL_BufferData(GL_SHADER_STORAGE_BUFFER, 16, nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

        // Indirect cmd buffer: 2 × DrawArraysIndirectCommand = 2 × 16 B = 32 B.
        glGenBuffers(1, &g_waterIndirectCmdBuffer);
        g_waterIndirectCmdBufferOwner.backendHandle = g_waterIndirectCmdBuffer;  // WATER-REGISTRY-0: mirror (pattern b)
        RegisterWaterBufferOwner(g_waterIndirectCmdBufferOwner, g_waterIndirectCmdBuffer, 32u); // WATER-REGISTRY-0
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, g_waterIndirectCmdBuffer);
        MC2_GL_BufferData(GL_DRAW_INDIRECT_BUFFER, 32, nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
    }
    } // end WaterGuards scope

    // Ensure recipe SSBO is ready.
    uint32_t recipeUploadedCount = 0;
    {
        mc2_hitch::HitchScope _hRecipe(mc2_hitch::HitchSpanKind::WaterRecipe);
        recipeUploadedCount = EnsureRecipeBufferUploaded();
    }
    if (g_mc2HitchEnabled) g_mc2HitchAccum.waterRecipeUploaded = recipeUploadedCount;
    if (!recipeUploadedCount) { if (g_mc2HitchEnabled) ++g_mc2HitchAccum.waterEarlyOut; return false; }
    if (g_recipeBuffer == 0) { if (g_mc2HitchEnabled) ++g_mc2HitchAccum.waterEarlyOut; return false; }

    // Ensure thin-record SSBO is allocated (UploadAndBindThinRecords creates it lazily;
    // on the GPU path we may skip that function, so we ensure the buffer exists here).
    // The thin buffer must have at least one slot large enough for g_recipes.size() records.
    const uint32_t maxThinRecords = (uint32_t)g_recipes.size();
    if (maxThinRecords == 0) { if (g_mc2HitchEnabled) ++g_mc2HitchAccum.waterEarlyOut; return false; }

    // M1 guard: lighting SSBO must be ready before water compute reads it (binding 1).
    // GetOutputSsbo() returns 0 until the lighting compute has run (mission_init path).
    // Binding GL name 0 → reads return driver-defined values, usually zero → black water
    // with correct geometry — a silent-wrong-render that passes smoke gates.
    {
        static bool s_warnedLightSsbo = false;
        const GLuint lightSsbo = gos_terrain_lighting::GetOutputSsbo();
        if (lightSsbo == 0) {
            if (!s_warnedLightSsbo) {
                mc2::fmt::print_out("[GPU_DRIVEN_WATER v1] event=warn msg=lighting_ssbo_not_ready frame=deferred_to_cpu\n");
                fflush(stdout);
                s_warnedLightSsbo = true;
            }
            if (g_mc2HitchEnabled) ++g_mc2HitchAccum.waterEarlyOut;
            return false;
        }
        s_warnedLightSsbo = false; // reset so it re-warns if SSBO goes away again
    }

    const GLsizeiptr thinSlotBytes = (GLsizeiptr)(maxThinRecords * sizeof(WaterThinRecord));
    // SSBO-BIND-ALIGN (ARMED PATH): the per-slot bind offset thinSlotOffset =
    // g_thinSlot * g_thinSlotCapacity must be a multiple of
    // GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT. sizeof(WaterThinRecord)=48, so
    // maxThinRecords*48 is aligned only when maxThinRecords%16==0 -> otherwise slots
    // 1+ bind misaligned on NVIDIA (GL_INVALID_VALUE -> the water thin SSBO never
    // binds -> the cull writes/reads nothing -> water FLASHES). This is the live
    // armed water path (the earlier fix only padded the unarmed fallback). Pad the
    // per-slot STRIDE; thinSlotBytes (raw) stays the bind SIZE (<= stride).
    const GLsizeiptr thinSlotStride = (GLsizeiptr)gpuAlignUp(
        (unsigned long long)thinSlotBytes, (unsigned long long)gpuSsboOffsetAlignment());
    if (g_thinBuffer == 0) {
        glGenBuffers(1, &g_thinBuffer);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_thinBuffer);
        MC2_GL_BufferData(GL_SHADER_STORAGE_BUFFER,
                     thinSlotStride * (GLsizeiptr)kThinRingSlots,
                     nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        g_thinSlotCapacity = (uint32_t)thinSlotStride;
        g_thinSlot = 0;
        // WATER-REGISTRY-0: register owner (armed-path lazy alloc of the thin ring).
        RegisterWaterBufferOwner(g_thinBufferOwner, g_thinBuffer,
                                 (uint64_t)thinSlotStride * (uint64_t)kThinRingSlots);
    }
    // Build and upload per-frame quad window SSBO.
    // WATER-GPU-FULL-RECIPE-CULL-1B: in authoritative full-recipe mode the CPU narrow
    // walk is skipped, so g_narrowQuadsThisFrame is empty and BuildQuadWindowSSBO would
    // return 0. Bypass the window build AND the windowCount==0 early-return [adversarial
    // C-1]; the dispatch bound becomes the full world-indexed recipe set. The full-recipe
    // output writes the SAME real thin/header buffers the candidate path uses, so DISPATCH
    // 2 + the VS draw consume it unchanged. Non-auth (1A/legacy) path is untouched.
    const bool fullRecipeAuth = IsFullRecipeAuthoritative();
    const uint32_t windowCount   = fullRecipeAuth ? 0u : BuildQuadWindowSSBO();
    const uint32_t dispatchCount = fullRecipeAuth ? maxThinRecords : windowCount;
    if (dispatchCount == 0) { if (g_mc2HitchEnabled) ++g_mc2HitchAccum.waterEarlyOut; return false; }

    // H1b stats: populate after build, before dispatch.
    if (g_mc2HitchEnabled) {
        g_mc2HitchAccum.waterWindowCount = dispatchCount;
        g_mc2HitchAccum.waterNarrowCount = (uint32_t)g_narrowQuadsThisFrame.size();
        g_mc2HitchAccum.waterRecipeCount = (uint32_t)g_recipes.size();
    }

    // Advance ring slot so GPU write doesn't stomp a slot the GPU is still consuming.
    // Must happen AFTER the windowCount==0 early-return so we don't burn a slot
    // on frames where no water quads are visible (stale-data draw on the next frame).
    g_thinSlot = (g_thinSlot + 1) % kThinRingSlots;
    WaitAndClearThinFenceForCurrentSlot((uint32_t)thinSlotBytes);   // WATER-THINRING-FENCE-1
    const GLintptr thinSlotOffset = (GLintptr)(g_thinSlot * g_thinSlotCapacity);

    // Zero the bucket header (reset visibleCount to 0) before each dispatch.
    {
        const uint32_t zero = 0u;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_waterBucketHeaderSsbo);
        glClearBufferSubData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, 0, 16,
                             GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

    // Bind the thin-record ring slot for GPU output.
    gpuBindSsboRange(kWaterThinSsboBinding, g_thinBuffer,
                     (long long)thinSlotOffset, (long long)thinSlotBytes,
                     "water.thin.draw");

    // ------------------------------------------------------------------
    // DISPATCH 1: cull/pack (gpu_driven_water.comp)
    // Bindings: 0=recipe, 1=lighting, 2=quadWindow, 3=thin, 6=header
    // ------------------------------------------------------------------
    // H1b: time both dispatch + barrier phases together.
    mc2_hitch::HitchScope _hDispatch(mc2_hitch::HitchSpanKind::WaterDispatch);
    glUseProgram(g_waterComputeProgram);

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, g_recipeBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, gos_terrain_lighting::GetOutputSsbo());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, g_quadWindowSsbo);
    // Slot 3 (thin output, compute-only): the compute shader writes thin records
    // here. kWaterThinSsboBinding (= 6) is the DRAW-phase binding where the VS
    // reads thin records; they are different slots. The compute shader ALSO uses
    // binding 6 for the bucket header (coherent buffer Header). After both
    // dispatches finish, binding 6 must be restored to the thin-record range for
    // the VS draw (see re-bind after final glMemoryBarrier below).
    gpuBindSsboRange(3, g_thinBuffer,
                     (long long)thinSlotOffset, (long long)thinSlotBytes,
                     "water.thin.compute");
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, g_waterBucketHeaderSsbo);

    // Uniforms for gpu_driven_water.comp.
    const GLint locWindowCount = glGetUniformLocation(g_waterComputeProgram, "u_windowCount");
    const GLint locMaxThin     = glGetUniformLocation(g_waterComputeProgram, "u_maxThinRecords");
    const GLint locWaterElev   = glGetUniformLocation(g_waterComputeProgram, "u_waterElevation");
    const GLint locFrameCos    = glGetUniformLocation(g_waterComputeProgram, "u_frameCos");
    const GLint locMapSide     = glGetUniformLocation(g_waterComputeProgram, "u_mapSide");
    const GLint locMVP         = glGetUniformLocation(g_waterComputeProgram, "u_worldToClipGL");
    // WATER-FULL-RECIPE-GPU-TIMER-SPIKE-0 uniforms (optional — absent on un-patched shader).
    const GLint locFullMode    = glGetUniformLocation(g_waterComputeProgram, "u_fullRecipeMode");
    const GLint locShoreExt    = glGetUniformLocation(g_waterComputeProgram, "u_shoreExt");
    // WATER-BIAS-LINEARIZE-1: pzOk admission gate must agree with the thin VS
    // bias mode. Uniform int defaults to 0 (proportional) if absent.
    const GLint locBiasLegacy  = glGetUniformLocation(g_waterComputeProgram, "u_waterBiasLegacy");
    // SHORELINE-STAIRCASE-1 + SHORELINE-GEOMETRY-1: same EffShoreExt the CPU
    // mirrors use, so the GPU cull admits exactly the same tile set.
    const float spikeShoreExt  = EffShoreExt(MapData::alphaDepth);

    if (locWindowCount < 0) {
        mc2::fmt::print_err("[GPU_DRIVEN_WATER v1] event=warn msg=u_windowCount_not_found\n");
        fflush(stderr);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, 0);
        glUseProgram(0);
        if (g_mc2HitchEnabled) ++g_mc2HitchAccum.waterEarlyOut;
        return false;
    }
    glUniform1i(locWindowCount, (int)dispatchCount);   // 1B: full recipe count when authoritative

    if (locMaxThin < 0) {
        mc2::fmt::print_err("[GPU_DRIVEN_WATER v1] event=warn msg=u_maxThinRecords_not_found\n");
        fflush(stderr);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, 0);
        glUseProgram(0);
        if (g_mc2HitchEnabled) ++g_mc2HitchAccum.waterEarlyOut;
        return false;
    }
    glUniform1i(locMaxThin, (int)maxThinRecords);

    if (locWaterElev < 0) {
        mc2::fmt::print_err("[GPU_DRIVEN_WATER v1] event=warn msg=u_waterElevation_not_found\n");
        fflush(stderr);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, 0);
        glUseProgram(0);
        if (g_mc2HitchEnabled) ++g_mc2HitchAccum.waterEarlyOut;
        return false;
    }
    glUniform1f(locWaterElev, Terrain::waterElevation);

    // u_frameCos: per-vertex wave Z-lift for pz gate. Not required to abort if
    // missing — shader defaults to 0, degrading to the old approximation rather
    // than producing a wrong cull pass.
    if (locFrameCos >= 0) glUniform1f(locFrameCos, frameCos);

    // WATER-BIAS-LINEARIZE-1: mirror the VS bias mode into the cull gate.
    if (locBiasLegacy >= 0) glUniform1i(locBiasLegacy, mc2WaterBiasLegacy());

    if (locMapSide < 0) {
        mc2::fmt::print_err("[GPU_DRIVEN_WATER v1] event=warn msg=u_mapSide_not_found\n");
        fflush(stderr);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, 0);
        glUseProgram(0);
        if (g_mc2HitchEnabled) ++g_mc2HitchAccum.waterEarlyOut;
        return false;
    }
    glUniform1i(locMapSide, (int)Terrain::realVerticesMapSide);

    if (locMVP < 0) {
        // Uniform not found in shader — dispatching with a zeroed MVP would
        // produce a fully-culled or garbage cull pass. Abort instead.
        mc2::fmt::print_err("[GPU_DRIVEN_WATER v1] event=warn msg=u_terrainMVP_not_found\n");
        fflush(stderr);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, 0);  // unbind stale thin slot
        glUseProgram(0);
        if (g_mc2HitchEnabled) ++g_mc2HitchAccum.waterEarlyOut;
        return false;
    }
    {
        // Water-consistency fix (2026-05-17): when SOLID is armed the drawn
        // terrain is terrain-solid's Fix-B clipPos, baked EARLIER this frame
        // with terrain-solid's dispatch MVP. terrain_mvp_ is updated between
        // that bake point and here, so gos_GetTerrainMVPMat4() returns a
        // one-frame-newer matrix -> water projects a frame ahead of terrain ->
        // shoreline recede/flicker/vanish under zoom/elevation/motion
        // ([WATER_DEPTHPROBE v1] proved the exact 1-frame lag; static was
        // already bit-identical). Project water with the SAME dispatch MVP so
        // water is bit-consistent with the drawn terrain (both share terrain's
        // long-standing, accepted ~1-frame dispatch lag). Unarmed: terrain is
        // NOT the clipPos path, so the legacy live MVP is correct (pre-fix
        // behavior). ring_slot_state_must_travel_with_slot.md.
        const float* mvp =
            gos_terrain_indirect::IsFrameSolidArmed()
                ? gos_terrain_indirect_getDispatchMvp16()
                : gos_GetTerrainMVPMat4();
        if (!mvp) mvp = gos_GetTerrainMVPMat4();  // safety: pre-arm/first frame
        if (mvp) {
            // GL_FALSE: row-major upload. See memory/terrain_mvp_gl_false.md.
            glUniformMatrix4fv(locMVP, 1, GL_FALSE, mvp);
            // [WATER_DEPTHPROBE v1] RETAINED verification instrument (env-gated
            // MC2_WATER_DEPTHPROBE, silent by default; demote-not-delete per
            // the debug-instrumentation rule). It diagnosed the water/terrain
            // 1-frame MVP divergence (terrain_solid_fp[N]==water_fp[N-1] under
            // motion) that caused the shoreline recede/flicker/intro-pan-vanish;
            // the 2026-05-17 water-consistency fix (water reads terrain-solid's
            // dispatch MVP via gos_terrain_indirect_getDispatchMvp16) closed it.
            // INVARIANT this now proves: with the fix in place equal==1 on
            // EVERY frame incl. camera motion. A regression here (equal==0 on
            // moved frames) means the MVP-consistency contract broke again.
            // FNV-1a over the water-uploaded MVP's first 12 floats, byte-
            // identical to gos_terrain_indirect.cpp Probe 8.
            static const bool s_waterDepthProbe = mc2::gate::get<"MC2_WATER_DEPTHPROBE">();
            if (s_waterDepthProbe) {
                uint32_t wfp = 2166136261u;
                for (int k = 0; k < 12; ++k) {
                    uint32_t bits = 0;
                    memcpy(&bits, &mvp[k], sizeof(bits));
                    wfp ^= bits; wfp *= 16777619u;
                }
                const uint32_t tfp =
                    gos_terrain_indirect_getDispatchMvpFp();
                const uint64_t tfi =
                    gos_terrain_indirect_getDispatchMvpFrameIdx();
                // v2: un-armed intro-pan coverage + cause discriminator.
                // The v1 `f > 120` warmup SKIPPED the intro pan entirely (it
                // is the early frames) — that is why we had zero intro-pan
                // data. v2 always prints in the un-armed phase. Extra fields
                // separate the two possible un-armed causes:
                //  - wstream_ready==0 || recipe_count==0  => water simply NOT
                //    produced yet (WaterStream not built during early intro);
                //    a readiness issue, NOT an MVP problem — no MVP fix helps.
                //  - ready==1 && recipe>0 && armed==0      => water IS produced
                //    un-armed; livecam_fp vs water_fp + moved shows whether the
                //    un-armed projection is itself frame-divergent.
                const bool armed =
                    gos_terrain_indirect::IsFrameSolidArmed();
                const bool wready = IsReady();
                const uint32_t rc = GetRecipeCount();
                uint32_t lcfp = 2166136261u;
                if (const float* lc = gos_GetTerrainMVPMat4()) {
                    for (int k = 0; k < 12; ++k) {
                        uint32_t b = 0;
                        memcpy(&b, &lc[k], sizeof(b));
                        lcfp ^= b; lcfp *= 16777619u;
                    }
                } else {
                    lcfp = 0u;
                }
                static uint64_t s_wframe  = 0;
                static uint32_t s_prevWfp = 0;
                const uint64_t f = ++s_wframe;
                const bool moved = (s_prevWfp != 0 && wfp != s_prevWfp);
                // Cadence: ALWAYS print while un-armed (the intro/deployment
                // pan — the regime under investigation, no warmup gate), PLUS
                // the armed regime's post-warmup first-8 / moved / 240-static
                // baseline. Demote-not-delete; silent unless env set.
                const bool emit =
                    !armed
                    || (f > 120 && (f <= 128 || moved || (f % 240 == 0)));
                if (emit) {
                    mc2::fmt::print_out("[WATER_DEPTHPROBE v2] event=mvp_pair wframe={:d} armed={:d} wstream_ready={:d} recipe_count={:d} water_fp={:08x} terrain_solid_fp={:08x} livecam_fp={:08x} equal={:d} w_eq_livecam={:d} moved={:d} terrain_solid_frameidx={:d} w_mvp0={:.6f}\n", mc2::fmt::as_u64((unsigned long long)f), mc2::fmt::as_i32(armed ? 1 : 0), mc2::fmt::as_i32(wready ? 1 : 0), mc2::fmt::as_u32((unsigned)rc), mc2::fmt::as_u32(wfp), mc2::fmt::as_u32(tfp), mc2::fmt::as_u32(lcfp), mc2::fmt::as_i32((wfp == tfp) ? 1 : 0), mc2::fmt::as_i32((wfp == lcfp) ? 1 : 0), mc2::fmt::as_i32(moved ? 1 : 0), mc2::fmt::as_u64((unsigned long long)tfi), (double)mvp[0]);
                    fflush(stdout);
                }
                s_prevWfp = wfp;
            }
            // [WATER_RENDERPROBE v1] Fix B matrix-share release gate. Env-gated
            // (MC2_WATER_RENDERPROBE), silent by default, demote-not-delete.
            // Invariant A (TRIPWIRE, trivially-true today): every armed frame the
            // water cull-feed matrix FP == terrain dispatch FP. The render binds
            // (gameos_graphics.cpp water + the 3 overlay/decal callers) inline the
            // byte-identical symmetric-mirror expression this cull-feed uses, and
            // terrain_mvp_ has a single per-frame writer (gamecam.cpp gos_SetWorldToClipGL)
            // before all consumers, so wfp is a faithful render-bind-FP proxy. A==0 on
            // an armed frame ONLY if a future change mutates terrain_mvp_ mid-frame
            // (the one residual hazard). It does NOT by itself prove correctness.
            // Invariant B (RELEASE GATE): on the arming-transition frame (armed flips
            // vs prev frame) water-bind FP must == terrain's this-frame source FP
            // (dispatch FP if armed, live-cam FP if un-armed). RenderDoc cannot catch
            // this 1-frame transient; passing B on a captured transition frame is the
            // gate. Latched: printed exactly once.
            // NOTE: wfp/tfp/tfi/lcfp/armed/f are scoped inside if(s_waterDepthProbe)
            // above and are not visible here; all are recomputed locally using the
            // identical FNV-1a idiom so this block is fully self-contained.
            static const bool s_waterRenderProbe = mc2::gate::get<"MC2_WATER_RENDERPROBE">();
            if (s_waterRenderProbe) {
                uint32_t wfp2 = 2166136261u;
                for (int k = 0; k < 12; ++k) {
                    uint32_t bits = 0;
                    memcpy(&bits, &mvp[k], sizeof(bits));
                    wfp2 ^= bits; wfp2 *= 16777619u;
                }
                const uint32_t tfp2 =
                    gos_terrain_indirect_getDispatchMvpFp();
                const uint64_t tfi2 =
                    gos_terrain_indirect_getDispatchMvpFrameIdx();
                const bool armed2 =
                    gos_terrain_indirect::IsFrameSolidArmed();
                uint32_t lcfp2 = 2166136261u;
                if (const float* lc2 = gos_GetTerrainMVPMat4()) {
                    for (int k = 0; k < 12; ++k) {
                        uint32_t b = 0;
                        memcpy(&b, &lc2[k], sizeof(b));
                        lcfp2 ^= b; lcfp2 *= 16777619u;
                    }
                } else {
                    lcfp2 = 0u;
                }
                static uint64_t s_rpFrame = 0;
                const uint64_t f2 = ++s_rpFrame;
                static int  s_rpPrevArmed = -1;            // -1 = no prior PROBE frame (includes frames skipped while
                                                           //   mvp==null or the env var is off); a transition that
                                                           //   occurs before the first mvp-non-null probe frame is not
                                                           //   caught -- acceptable: the release gate fires on a
                                                           //   running mission where mvp is reliably non-null.
                static bool s_rpInvBLatched = false;
                const int   armedI = armed2 ? 1 : 0;
                // Invariant A: armed-frame tripwire (trivially-true; A==0 => mid-frame
                // terrain_mvp_ mutation regression).
                if (armed2) {
                    mc2::fmt::print_out("[WATER_RENDERPROBE v1] event=invA wframe={:d} water_fp={:08x} terrain_dispatch_fp={:08x} equal={:d} terrain_dispatch_frameidx={:d}\n", mc2::fmt::as_u64((unsigned long long)f2), mc2::fmt::as_u32(wfp2), mc2::fmt::as_u32(tfp2), mc2::fmt::as_i32((wfp2 == tfp2) ? 1 : 0), mc2::fmt::as_u64((unsigned long long)tfi2));
                    fflush(stdout);
                }
                // Invariant B: first arming-transition frame only, printed once.
                if (s_rpPrevArmed != -1 && s_rpPrevArmed != armedI
                    && !s_rpInvBLatched) {
                    const uint32_t srcFp = armed2 ? tfp2 : lcfp2;
                    mc2::fmt::print_out("[WATER_RENDERPROBE v1] event=invB_transition wframe={:d} armed={:d} prev_armed={:d} water_fp={:08x} terrain_src_fp={:08x} equal={:d} terrain_dispatch_frameidx={:d}\n", mc2::fmt::as_u64((unsigned long long)f2), mc2::fmt::as_i32(armedI), mc2::fmt::as_i32(s_rpPrevArmed), mc2::fmt::as_u32(wfp2), mc2::fmt::as_u32(srcFp), mc2::fmt::as_i32((wfp2 == srcFp) ? 1 : 0), mc2::fmt::as_u64((unsigned long long)tfi2));
                    fflush(stdout);
                    s_rpInvBLatched = true;
                }
                s_rpPrevArmed = armedI;
            }
        } else {
            // MVP not available this frame (terrain not yet rendered). Bail out.
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, 0);  // unbind stale thin slot
            glUseProgram(0);
            if (g_mc2HitchEnabled) ++g_mc2HitchAccum.waterEarlyOut;
            return false;
        }
    }

    // Live-dispatch mode: full-recipe (authoritative 1B, writes the real draw buffers)
    // or window (1A/legacy candidate-fed).
    if (locFullMode >= 0) glUniform1i(locFullMode, fullRecipeAuth ? 1 : 0);
    if (locShoreExt >= 0) glUniform1f(locShoreExt, spikeShoreExt);

    // WATER-FULL-RECIPE-GPU-TIMER-SPIKE-0 (MC2_WATER_FULL_RECIPE_SPIKE): measurement
    // only. Times the live candidate-fed cull dispatch, then runs a PROTOTYPE
    // full-recipe cull dispatch over the whole world-indexed recipe set (GPU
    // eligibility predicate) to SCRATCH buffers never fed to draw, and logs the
    // GPU-us comparison every 600 frames. Default-off: zero behavior change.
    static const bool s_spikeOn = mc2::gate::get<"MC2_WATER_FULL_RECIPE_SPIKE">();
    // WATER-GPU-FULL-RECIPE-CULL-1A (MC2_WATER_GPU_FULL_RECIPE_CULL, default-OFF):
    // runs the full-recipe cull path NON-AUTHORITATIVELY (candidate path still draws,
    // CPU narrow walk stays alive) and parity-compares the two GPU outputs. Authority
    // flip + walk-skip is 1B. [adversarial M-3: walk alive so the candidate reference
    // is non-empty; M-1: this whole function only runs when the GPU water path is
    // armed (fast path owns the draw); C-2: recipes stay at binding 0; M-2: u_worldToClipGL.]
    static const bool s_cullOn = mc2::gate::get<"MC2_WATER_GPU_FULL_RECIPE_CULL">();
    // The scratch full-recipe dispatch (spike timer / 1A parity) is the NON-authoritative
    // comparison harness — pointless in 1B authoritative mode (the live dispatch IS
    // full-recipe to the real buffers), so disable it there.
    const bool runFullRecipe = (s_spikeOn || s_cullOn) && !fullRecipeAuth;
    static GLuint s_qCand = 0, s_qSpike = 0;
    if (s_spikeOn) {
        if (!s_qCand)  glGenQueries(1, &s_qCand);
        if (!s_qSpike) glGenQueries(1, &s_qSpike);
    }

    // [RENDER_PASS_TIME v1] coexistence: GL_TIME_ELAPSED cannot nest. This
    // dispatch can run inside a render-pass-timer scope, so the spike query
    // (and its readback below) yields while one of those scopes is open.
    const bool spikeQ = s_spikeOn && !gos_render_pass_timer::QueryActive();
    const uint32_t groups = (dispatchCount + 63u) / 64u;   // 1B: full recipe set when authoritative
    if (spikeQ) glBeginQuery(GL_TIME_ELAPSED, s_qCand);
    glDispatchCompute(groups, 1, 1);   // AUTHORITATIVE dispatch (feeds draw): full-recipe (1B) or candidate (1A/legacy)
    if (spikeQ) glEndQuery(GL_TIME_ELAPSED);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    // 1B sanity telemetry: confirm the authoritative full-recipe dispatch actually
    // produced water records (visibleCount > 0), every 600 frames.
    if (fullRecipeAuth) {
        static uint32_t s_authFrame = 0;
        if ((s_authFrame++ % 600u) == 0u) {
            uint32_t vis = 0;
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_waterBucketHeaderSsbo);
            glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, 4, &vis);
            MC2_DIAGF("[WATER_FULL_RECIPE_AUTH v1] recipeCount=%u authoritativeOutput=%u\n",
                   maxThinRecords, vis);
            fflush(stdout);
        }
    }

    if (runFullRecipe && locFullMode >= 0 && locShoreExt >= 0) {
        static GLuint s_spikeThin = 0, s_spikeHeader = 0;
        static GLsizeiptr s_spikeThinBytes = 0;
        const GLsizeiptr needThin = (GLsizeiptr)maxThinRecords * (GLsizeiptr)sizeof(WaterThinRecord);
        if (!s_spikeThin || needThin > s_spikeThinBytes) {
            if (s_spikeThin) glDeleteBuffers(1, &s_spikeThin);
            // TIER2-EXCLUDED: dead-path
            glGenBuffers(1, &s_spikeThin);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_spikeThin);
            MC2_GL_BufferData(GL_SHADER_STORAGE_BUFFER, needThin, nullptr, GL_DYNAMIC_COPY);
            s_spikeThinBytes = needThin;
        }
        if (!s_spikeHeader) {
            // TIER2-EXCLUDED: dead-path
            glGenBuffers(1, &s_spikeHeader);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_spikeHeader);
            MC2_GL_BufferData(GL_SHADER_STORAGE_BUFFER, 16, nullptr, GL_DYNAMIC_COPY);
        }
        const uint32_t zero4[4] = {0u, 0u, 0u, 0u};
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_spikeHeader);
        MC2_GL_BufferSubData(GL_SHADER_STORAGE_BUFFER, 0, 16, zero4);  // reset visibleCount accumulator
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, s_spikeThin);   // scratch thin-write (NOT fed to draw)
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, s_spikeHeader); // scratch header
        const uint32_t recipeCount = (uint32_t)g_recipes.size();
        glUniform1i(locFullMode, 1);
        glUniform1i(locWindowCount, (int)recipeCount);  // dispatch bound = full recipe set
        const uint32_t frGroups = (recipeCount + 63u) / 64u;
        if (spikeQ) glBeginQuery(GL_TIME_ELAPSED, s_qSpike);
        glDispatchCompute(frGroups, 1, 1);
        if (spikeQ) glEndQuery(GL_TIME_ELAPSED);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        // Restore window mode + release scratch bindings (final rebind below fixes 6).
        glUniform1i(locFullMode, 0);
        glUniform1i(locWindowCount, (int)windowCount);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, 0);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, 0);

        uint32_t frOut = 0;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_spikeHeader);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, 4, &frOut);

        // ---- Spike timer log (MC2_WATER_FULL_RECIPE_SPIKE only) ----
        if (spikeQ) {
            GLuint64 nsCand = 0, nsSpike = 0;
            glGetQueryObjectui64v(s_qCand,  GL_QUERY_RESULT, &nsCand);
            glGetQueryObjectui64v(s_qSpike, GL_QUERY_RESULT, &nsSpike);
            static double s_sumCand = 0.0, s_sumSpike = 0.0; static uint32_t s_n = 0;
            static uint32_t s_lastRecipe = 0, s_lastCand = 0, s_lastFrOut = 0;
            s_sumCand += (double)nsCand / 1000.0; s_sumSpike += (double)nsSpike / 1000.0; s_n++;
            s_lastRecipe = recipeCount; s_lastCand = windowCount; s_lastFrOut = frOut;
            if (s_n >= 600u) {
                mc2::fmt::print_out("[WATER_FULL_RECIPE_SPIKE v1] recipeCount={:d} candidateCount={:d} candidateGpuUs={:.1f} fullRecipeGpuUs={:.1f} deltaUs={:.1f} fullRecipeOut={:d} (mean/600)\n", mc2::fmt::as_u32(s_lastRecipe), mc2::fmt::as_u32(s_lastCand), s_sumCand / (double)s_n, s_sumSpike / (double)s_n, (s_sumSpike - s_sumCand) / (double)s_n, mc2::fmt::as_u32(s_lastFrOut));
                fflush(stdout);
                s_sumCand = 0.0; s_sumSpike = 0.0; s_n = 0u;
            }
        }

        // ---- 1A parity: candidate-fed (authoritative) vs full-recipe GPU output ----
        // Readback is expensive (GPU->CPU stall); cadence-gated. The candidate output
        // lives in g_thinBuffer at thinSlotOffset (count = g_waterBucketHeaderSsbo
        // .visibleCount, written by the authoritative dispatch above, NOT yet consumed
        // by DISPATCH 2). The full-recipe output is in s_spikeThin (count = frOut).
        if (s_cullOn) {
            static uint32_t s_parityFrame = 0;
            const bool doParity = ((s_parityFrame++ % 120u) == 0u);
            if (doParity) {
                uint32_t candCount = 0;
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_waterBucketHeaderSsbo);
                glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, 4, &candCount);
                if (candCount > maxThinRecords) candCount = maxThinRecords;
                uint32_t frCount = (frOut > maxThinRecords) ? maxThinRecords : frOut;
                std::vector<WaterThinRecord> candRec(candCount), frRec(frCount);
                if (candCount) {
                    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_thinBuffer);
                    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, thinSlotOffset,
                        (GLsizeiptr)candCount * (GLsizeiptr)sizeof(WaterThinRecord), candRec.data());
                }
                if (frCount) {
                    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_spikeThin);
                    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                        (GLsizeiptr)frCount * (GLsizeiptr)sizeof(WaterThinRecord), frRec.data());
                }
                // Compare order-independently by recipeIdx (atomicAdd slot alloc is
                // nondeterministic). Set match + per-field match for shared recipeIdx.
                std::unordered_map<uint32_t, const WaterThinRecord*> candMap;
                candMap.reserve(candCount * 2u + 1u);
                for (const auto& r : candRec) candMap[r.recipeIdx] = &r;
                uint32_t shared = 0, onlyFr = 0, fieldMismatch = 0;
                for (const auto& f : frRec) {
                    auto it = candMap.find(f.recipeIdx);
                    if (it == candMap.end()) { ++onlyFr; continue; }
                    ++shared;
                    const WaterThinRecord& c = *it->second;
                    if (f.flags != c.flags ||
                        f.lightRGB0 != c.lightRGB0 || f.lightRGB1 != c.lightRGB1 ||
                        f.lightRGB2 != c.lightRGB2 || f.lightRGB3 != c.lightRGB3 ||
                        f.fogRGB0 != c.fogRGB0 || f.fogRGB1 != c.fogRGB1 ||
                        f.fogRGB2 != c.fogRGB2 || f.fogRGB3 != c.fogRGB3)
                        ++fieldMismatch;
                }
                const uint32_t onlyCand = (candCount > shared) ? (candCount - shared) : 0u;
                mc2::fmt::print_out("[WATER_FULL_RECIPE_PARITY v1] candCount={:d} fullRecipeCount={:d} shared={:d} onlyCandidate={:d} onlyFullRecipe={:d} fieldMismatch={:d} setMatch={:d}\n", mc2::fmt::as_u32(candCount), mc2::fmt::as_u32(frCount), mc2::fmt::as_u32(shared), mc2::fmt::as_u32(onlyCand), mc2::fmt::as_u32(onlyFr), mc2::fmt::as_u32(fieldMismatch), mc2::fmt::as_i32((onlyCand == 0u && onlyFr == 0u && fieldMismatch == 0u) ? 1 : 0));
                fflush(stdout);
            }
        }
    }

    // ------------------------------------------------------------------
    // DISPATCH 2: cmd-patch (gpu_driven_cmd_patch.comp)
    // Bindings: 0=header, 1=indirectCmd
    // ------------------------------------------------------------------
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, g_waterBucketHeaderSsbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, g_waterIndirectCmdBuffer);

    glUseProgram(g_cmdPatchProgram);
    const GLint locVertsPerElem = glGetUniformLocation(g_cmdPatchProgram, "u_vertsPerElement");
    const GLint locCmdCount     = glGetUniformLocation(g_cmdPatchProgram, "u_cmdCount");
    const GLint locMaxThin2     = glGetUniformLocation(g_cmdPatchProgram, "u_maxThinRecords");
    if (locVertsPerElem >= 0) glUniform1i(locVertsPerElem, 6);
    if (locCmdCount     >= 0) glUniform1i(locCmdCount, 2);
    if (locMaxThin2     >= 0) glUniform1i(locMaxThin2, (int)maxThinRecords);
    glDispatchCompute(1, 1, 1);

    // Final barrier: SSBO writes visible + indirect cmd ready for MDI draw.
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);

    glUseProgram(0);

    // M2 fix: restore kWaterThinSsboBinding (6) to thin records for the VS draw.
    // Dispatch 1 (cull/pack setup) overwrote slot 6 with g_waterBucketHeaderSsbo to
    // satisfy the compute shader's binding-6 header read. The VS reads thin records
    // from slot 6, so we must restore the correct binding after both dispatches
    // and barriers complete.
    gpuBindSsboRange(kWaterThinSsboBinding, g_thinBuffer,
                     (long long)thinSlotOffset, (long long)thinSlotBytes,
                     "water.thin.restore");

    // Unbind compute-only slots (0=recipe, 1=lighting, 2=window, 3=thin-write)
    // so Stage 2 compute subsystems don't inherit stale bindings on entry.
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, 0);

    g_waterGpuDrivenArmed = true;
    // VK-P9a: remember which thin-ring slot the GPU packed this frame + the per-slot
    // record capacity, so ReadbackGpuThinForVk can map exactly those records.
    g_vkLastThinSlotOffset = thinSlotOffset;
    g_vkLastMaxThinRecords = maxThinRecords;
    if (g_mc2HitchEnabled) {
        // waterGlCalls = cumulative GL upload calls at this point in the frame.
        // Approximation: includes terrain/static calls made before water in the same frame.
        g_mc2HitchAccum.waterGlCalls =
            g_mc2HitchAccum.glBufferDataCalls +
            g_mc2HitchAccum.glBufferSubDataCalls +
            g_mc2HitchAccum.glBindTextureCalls;
    }
    return true;
}

bool IsGpuDrivenArmed() {
    return g_waterGpuDrivenArmed;
}

void ComputeDispatchParity_Check() {
    if (!gpu_driven::IsParityEnabled()) return;
    if (!g_waterGpuDrivenArmed) return;
    if (!g_waterBucketHeaderSsbo || !g_thinBuffer || g_thinSlotCapacity == 0) return;
    if (g_recipes.empty()) return;

    ++s_gpuParityFrames;

    // Flush GPU thin-record writes to client-visible memory before readback.
    // ComputeDispatch already issued GL_SHADER_STORAGE_BARRIER_BIT; here we
    // also need GL_BUFFER_UPDATE_BARRIER_BIT so glGetBufferSubData sees the writes.
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);

    // -----------------------------------------------------------------------
    // 1. Read back GPU visible count from the bucket header.
    // -----------------------------------------------------------------------
    const uint32_t gpuSlot = g_thinSlot;  // the ring slot compute wrote to

    gpu_driven::GpuDrivenBucketHeader hdr{};
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_waterBucketHeaderSsbo);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)sizeof(hdr), &hdr);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    const uint32_t gpuCount = std::min(hdr.visibleCount, (uint32_t)g_recipes.size());

    // -----------------------------------------------------------------------
    // 2. Read back GPU thin records from the ring slot compute used.
    // -----------------------------------------------------------------------
    std::vector<WaterThinRecord> gpuRecs(gpuCount);
    if (gpuCount > 0) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_thinBuffer);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER,
                           (GLintptr)(gpuSlot * (GLintptr)g_thinSlotCapacity),
                           (GLsizeiptr)(gpuCount * sizeof(WaterThinRecord)),
                           gpuRecs.data());
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

    // -----------------------------------------------------------------------
    // 3. Run the CPU thin-record pack (UploadAndBindThinRecords advances the
    //    ring to a new slot; GPU output is left intact at gpuSlot).
    // -----------------------------------------------------------------------
    const uint32_t cpuCount = UploadAndBindThinRecords();
    const uint32_t cpuSlot  = g_thinSlot;  // UploadAndBind advanced this

    // -----------------------------------------------------------------------
    // 4. Read back CPU thin records from the slot UploadAndBind wrote to.
    // -----------------------------------------------------------------------
    std::vector<WaterThinRecord> cpuRecs(cpuCount);
    if (cpuCount > 0) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_thinBuffer);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER,
                           (GLintptr)(cpuSlot * (GLintptr)g_thinSlotCapacity),
                           (GLsizeiptr)(cpuCount * sizeof(WaterThinRecord)),
                           cpuRecs.data());
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

    // -----------------------------------------------------------------------
    // 5. Sort both by recipeIdx for order-agnostic comparison (GPU atomicAdd
    //    output order is non-deterministic; CPU pack order follows quadList).
    // -----------------------------------------------------------------------
    auto byRecipeIdx = [](const WaterThinRecord& a, const WaterThinRecord& b) {
        return a.recipeIdx < b.recipeIdx;
    };
    std::sort(gpuRecs.begin(), gpuRecs.end(), byRecipeIdx);
    std::sort(cpuRecs.begin(), cpuRecs.end(), byRecipeIdx);

    // -----------------------------------------------------------------------
    // 6. Walk merged sorted lists and compare field-by-field.
    // -----------------------------------------------------------------------
    MismatchPrintBudget budget;
    uint32_t quadsChecked = 0;
    uint32_t frameMismatches = 0;
    uint32_t gi = 0, ci = 0;

    while (gi < gpuCount || ci < cpuCount) {
        const uint32_t gpuIdx = (gi < gpuCount) ? gpuRecs[gi].recipeIdx : UINT32_MAX;
        const uint32_t cpuIdx = (ci < cpuCount) ? cpuRecs[ci].recipeIdx : UINT32_MAX;

        if (gpuIdx < cpuIdx) {
            // GPU record not in CPU output
            if (budget.canPrint()) {
                budget.note();
                mc2::fmt::print_err("[GPU_DRIVEN_WATER_PARITY v1] event=mismatch frame={:d} recipe={:d} gpu_only=1\n", mc2::fmt::as_u64((unsigned long long)s_gpuParityFrames), mc2::fmt::as_u32((unsigned)gpuIdx));
                fflush(stderr);
            }
            ++frameMismatches;
            ++gi;
        } else if (cpuIdx < gpuIdx) {
            // CPU record not in GPU output
            if (budget.canPrint()) {
                budget.note();
                mc2::fmt::print_err("[GPU_DRIVEN_WATER_PARITY v1] event=mismatch frame={:d} recipe={:d} cpu_only=1\n", mc2::fmt::as_u64((unsigned long long)s_gpuParityFrames), mc2::fmt::as_u32((unsigned)cpuIdx));
                fflush(stderr);
            }
            ++frameMismatches;
            ++ci;
        } else {
            // Same recipeIdx — compare field by field.
            const WaterThinRecord& g = gpuRecs[gi];
            const WaterThinRecord& c = cpuRecs[ci];
            ++quadsChecked;

            if (g.flags != c.flags) {
                ++frameMismatches;
                if (budget.canPrint()) {
                    budget.note();
                    mc2::fmt::print_err("[GPU_DRIVEN_WATER_PARITY v1] event=mismatch frame={:d} recipe={:d} field=flags gpu=0x{:x} cpu=0x{:x}\n", mc2::fmt::as_u64((unsigned long long)s_gpuParityFrames), mc2::fmt::as_u32((unsigned)gpuIdx), mc2::fmt::as_u32((unsigned)g.flags), mc2::fmt::as_u32((unsigned)c.flags));
                    fflush(stderr);
                }
            }
            if (g.lightRGB0 != c.lightRGB0 || g.lightRGB1 != c.lightRGB1 ||
                g.lightRGB2 != c.lightRGB2 || g.lightRGB3 != c.lightRGB3) {
                ++frameMismatches;
                if (budget.canPrint()) {
                    budget.note();
                    mc2::fmt::print_err("[GPU_DRIVEN_WATER_PARITY v1] event=mismatch frame={:d} recipe={:d} field=lightRGB gpu=[0x{:x},0x{:x},0x{:x},0x{:x}] cpu=[0x{:x},0x{:x},0x{:x},0x{:x}]\n", mc2::fmt::as_u64((unsigned long long)s_gpuParityFrames), mc2::fmt::as_u32((unsigned)gpuIdx), mc2::fmt::as_u32((unsigned)g.lightRGB0), mc2::fmt::as_u32((unsigned)g.lightRGB1), mc2::fmt::as_u32((unsigned)g.lightRGB2), mc2::fmt::as_u32((unsigned)g.lightRGB3), mc2::fmt::as_u32((unsigned)c.lightRGB0), mc2::fmt::as_u32((unsigned)c.lightRGB1), mc2::fmt::as_u32((unsigned)c.lightRGB2), mc2::fmt::as_u32((unsigned)c.lightRGB3));
                    fflush(stderr);
                }
            }
            if (g.fogRGB0 != c.fogRGB0 || g.fogRGB1 != c.fogRGB1 ||
                g.fogRGB2 != c.fogRGB2 || g.fogRGB3 != c.fogRGB3) {
                ++frameMismatches;
                if (budget.canPrint()) {
                    budget.note();
                    mc2::fmt::print_err("[GPU_DRIVEN_WATER_PARITY v1] event=mismatch frame={:d} recipe={:d} field=fogRGB gpu=[0x{:x},0x{:x},0x{:x},0x{:x}] cpu=[0x{:x},0x{:x},0x{:x},0x{:x}]\n", mc2::fmt::as_u64((unsigned long long)s_gpuParityFrames), mc2::fmt::as_u32((unsigned)gpuIdx), mc2::fmt::as_u32((unsigned)g.fogRGB0), mc2::fmt::as_u32((unsigned)g.fogRGB1), mc2::fmt::as_u32((unsigned)g.fogRGB2), mc2::fmt::as_u32((unsigned)g.fogRGB3), mc2::fmt::as_u32((unsigned)c.fogRGB0), mc2::fmt::as_u32((unsigned)c.fogRGB1), mc2::fmt::as_u32((unsigned)c.fogRGB2), mc2::fmt::as_u32((unsigned)c.fogRGB3));
                    fflush(stderr);
                }
            }
            ++gi; ++ci;
        }
    }

    s_gpuParityQuads      += quadsChecked;
    s_gpuParityMismatches += frameMismatches;

    if (s_gpuParityFrames % kSummaryEveryFrames == 0) {
        mc2::fmt::print_err("[GPU_DRIVEN_WATER_PARITY v1] event=summary frames={:d} quads_checked={:d} total_mismatches={:d}\n", mc2::fmt::as_u64((unsigned long long)s_gpuParityFrames), mc2::fmt::as_u64((unsigned long long)s_gpuParityQuads), mc2::fmt::as_u64((unsigned long long)s_gpuParityMismatches));
        fflush(stderr);
    }
}

GLuint GetIndirectCmdBuffer() {
    return g_waterIndirectCmdBuffer;
}

// WATER-THINRING-FENCE-1: create the GPU fence guarding the thin-record ring
// slot that was produced + drawn this frame. Called once per frame from the
// bridge (renderWaterFastPath) AFTER the water draw is submitted, mirroring the
// solid path's post-draw glFenceSync. The next time this slot is reused (after
// kThinRingSlots frames), WaitAndClearThinFenceForCurrentSlot() blocks on this
// fence so the CPU/compute write cannot stomp a slot the GPU is still reading.
void EndThinRingFrameFence() {
    if (g_thinBuffer == 0) return;   // no ring allocated yet → nothing to fence
    if (g_thinFences[g_thinSlot]) {
        glDeleteSync(g_thinFences[g_thinSlot]);
    }
    g_thinFences[g_thinSlot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (ThinRingTraceOn()) {
        mc2::fmt::print_err("WATER_THINRING: frame={:d} slot={:d} bytes={:d} waited={:d} wait_result=0x{:x} fence_created=1\n", mc2::fmt::as_u64((unsigned long long)g_thinTraceFrame), mc2::fmt::as_u32(g_thinTraceSlot), mc2::fmt::as_u32(g_thinTraceBytes), mc2::fmt::as_i32(g_thinTraceWaited), mc2::fmt::as_u32(g_thinTraceWaitResult));
        fflush(stderr);
        ++g_thinTraceFrame;
    }
}

// ----------------------------------------------------------------------------
// VK-P9a WATER-INTEROP: GPU thin-record READBACK feed for the Vulkan water island.
//
// STEP 0 REVISION (empirically forced): the CPU thin pack (UploadAndBindThinRecords)
// returns 0 on the live armed path — the armed full-recipe-authoritative GPU path is
// the sole culler and does NOT populate the per-vertex CPU projection state (wz /
// waterHandle) the CPU pz-gate reads, so the CPU walk rejects every quad. So we read
// back the GPU-packed thin records that the compute ACTUALLY drew (the ground truth,
// count in the bucket header), and hand that CPU copy to VK. This is STEP 0's named
// fallback ("VK-side re-pack ... a known workaround, not a blocker") — one small
// per-frame GL readback of the compute-packed slot (hundreds–few-thousand × 48 B),
// done AFTER the compute's SHADER_STORAGE barrier so the data is visible and stable.
// No fenced GL buffer crosses into VK (we copy on the CPU) -> no GL->VK sync hazard.
//
// Returns the record count; *outPtr points at the internal CPU copy (48B each), valid
// until the next call. Returns 0 (and *outPtr=nullptr) if the GPU path didn't run.
uint32_t ReadbackGpuThinForVk(const void** outPtr) {
    if (outPtr) *outPtr = nullptr;
    if (!g_waterGpuDrivenArmed || g_thinBuffer == 0 || g_waterBucketHeaderSsbo == 0) return 0;
    if (g_vkLastMaxThinRecords == 0) return 0;

    // 1) Read the visible record count the compute wrote into the bucket header
    //    (GpuDrivenBucketHeader.visibleCount at offset 0). The final barrier in
    //    ComputeDispatchAndBindThinRecords already made these writes visible.
    uint32_t visibleCount = 0;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_waterBucketHeaderSsbo);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(uint32_t), &visibleCount);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    // Clamp to the per-slot capacity (defensive; the compute clamps too).
    if (visibleCount > g_vkLastMaxThinRecords) visibleCount = g_vkLastMaxThinRecords;
    if (visibleCount == 0) return 0;

    // 2) Read back exactly those records from the thin ring slot the compute packed.
    const GLsizeiptr bytes = (GLsizeiptr)visibleCount * (GLsizeiptr)sizeof(WaterThinRecord);
    if (g_vkThinReadback.size() < visibleCount) g_vkThinReadback.resize(visibleCount);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_thinBuffer);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, g_vkLastThinSlotOffset, bytes,
                       g_vkThinReadback.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    if (outPtr) *outPtr = g_vkThinReadback.data();
    return visibleCount;
}

void ReleaseGlResources() {
    if (g_recipeBuffer != 0) {
        glDeleteBuffers(1, &g_recipeBuffer);
        g_recipeBuffer = 0;
        InvalidateWaterBufferOwner(g_recipeBufferOwner);  // WATER-REGISTRY-0
    }
    if (g_thinBuffer != 0) {
        glDeleteBuffers(1, &g_thinBuffer);
        g_thinBuffer = 0;
        InvalidateWaterBufferOwner(g_thinBufferOwner);  // WATER-REGISTRY-0
    }
    // WATER-THINRING-FENCE-1: drop any outstanding ring fences with the buffer.
    for (uint32_t i = 0; i < kThinRingSlots; ++i) {
        if (g_thinFences[i]) { glDeleteSync(g_thinFences[i]); g_thinFences[i] = nullptr; }
    }
    g_recipeBufferUploadedCount = 0;
    g_thinSlotCapacity = 0;
    g_thinSlot = 0;
    g_thinStaging.clear();
    g_thinStaging.shrink_to_fit();

    // Phase C Stage 1 resources.
    if (g_waterComputeProgram != 0) {
        glDeleteProgram(g_waterComputeProgram);
        g_waterComputeProgram = 0;
    }
    if (g_cmdPatchProgram != 0) {
        glDeleteProgram(g_cmdPatchProgram);
        g_cmdPatchProgram = 0;
    }
    if (g_quadWindowSsbo != 0) {
        glDeleteBuffers(1, &g_quadWindowSsbo);
        g_quadWindowSsbo = 0;
        g_quadWindowSsboCapacity = 0;
    }
    if (g_waterBucketHeaderSsbo != 0) {
        glDeleteBuffers(1, &g_waterBucketHeaderSsbo);
        g_waterBucketHeaderSsbo = 0;
        InvalidateWaterBufferOwner(g_waterBucketHeaderSsboOwner);  // WATER-REGISTRY-0
    }
    if (g_waterIndirectCmdBuffer != 0) {
        glDeleteBuffers(1, &g_waterIndirectCmdBuffer);
        g_waterIndirectCmdBuffer = 0;
        InvalidateWaterBufferOwner(g_waterIndirectCmdBufferOwner);  // WATER-REGISTRY-0
    }
    g_waterGpuDrivenArmed = false;
    g_quadWindowStaging.clear();
    g_quadWindowStaging.shrink_to_fit();

    if (s_parityEnabled) {
        mc2::fmt::print_err("[WATER_PARITY v1] event=summary frames={:d} quads_checked={:d} total_mismatches={:d} reason=shutdown\n", mc2::fmt::as_u64((unsigned long long)s_parityFrameCounter), mc2::fmt::as_u64((unsigned long long)s_parityQuadsChecked), mc2::fmt::as_u64((unsigned long long)s_parityMismatchTotal));
        fflush(stderr);
    }
}

} // namespace WaterStream
