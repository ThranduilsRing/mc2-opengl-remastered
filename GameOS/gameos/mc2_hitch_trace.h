// mc2_hitch_trace.h — H1a hitch attribution layer.
// Include AFTER all GL/GameOS includes in suspect TUs. Never include in
// shared or precompiled headers.
//
// Gates: MC2_HITCH_TRACE=1 (enable), MC2_HITCH_MS=<float> (threshold, default 20).
// Zero cost when disabled: g_mc2HitchEnabled is a static-init bool; MC2_GL_*
// macros reduce to raw GL calls with a single predicted-branch bool check.
#pragma once
#include <chrono>
#include <cstdint>

#include "mc2_hitch_fence.h"

namespace mc2_hitch {

// ---------------------------------------------------------------------------
// Accumulator — reset each BeginFrame, read each EndFrame.
// ---------------------------------------------------------------------------
struct HitchFrameAccum {
    // Wall-clock spans (ms). Accumulated by HitchScope.
    double terrainSolidMs       = 0.0;
    double staticFlushMs        = 0.0;
    double staticSnapshotMs     = 0.0;
    double waterMs              = 0.0;

    // Terrain solid dispatch sub-counters (filled by gos_terrain_indirect.cpp)
    uint32_t terrainBinds       = 0;
    uint32_t terrainUploads     = 0;   // glBufferData + glBufferSubData count
    uint32_t terrainTexImageCalls = 0;

    // Static-prop flush sub-counters (filled by gos_static_prop_batcher.cpp)
    uint32_t staticRecords      = 0;   // caller fills after flush
    uint32_t staticDrawCmds     = 0;   // caller fills after flush
    uint64_t staticBufferBytes  = 0;   // cumulative from MC2_GL_* wrappers
    uint32_t staticRealloc      = 0;   // caller sets on buffer orphan/realloc
    uint32_t staticMap          = 0;   // glMapBufferRange count

    // Water sub-counters (filled by gos_terrain_water_stream.cpp)
    uint8_t  waterCalled        = 0;
    uint8_t  waterPresent       = 0;
    uint8_t  waterEarlyOut      = 0;
    uint32_t waterGlCalls       = 0;   // total MC2_GL_* hits inside water scope

    // H1b water CPU sub-spans (ms). Only populated when waterMs is significant.
    double waterGuardsMs      = 0.0;  // early-out checks + lazy init
    double waterRecipeMs      = 0.0;  // EnsureRecipeBufferUploaded
    double waterBuildWindowMs = 0.0;  // BuildQuadWindowSSBO (CPU loop + hash lookup)
    double waterUploadMs      = 0.0;  // glBufferSubData for quad window SSBO
    double waterDispatchMs    = 0.0;  // compute dispatch + glMemoryBarrier calls
    // H1b water stats
    uint32_t waterRecipeCount    = 0;  // g_recipes.size() at dispatch time
    uint32_t waterWindowCount    = 0;  // windowCount / dispatchCount
    uint32_t waterNarrowCount    = 0;  // g_narrowQuadsThisFrame.size()
    uint32_t waterRecipeUploaded = 0;  // whether a recipe upload happened this frame

    // H1c frame-phase spans (ms). Populated by HitchScope in gameosmain.cpp.
    double phaseLogicMs      = 0.0;  // GameLogic (DoGameLogic)
    double phaseRenderMs     = 0.0;  // DrawScreen (draw_screen)
    double phasePresentMs    = 0.0;  // SwapWindow (SDL_GL_SwapWindow + any preFinish)
    double phaseCapMs        = 0.0;  // Frame cap sleep

    // GL call counts (all instrumented TUs combined)
    uint32_t glBufferDataCalls        = 0;
    uint64_t glBufferDataBytes        = 0;
    uint32_t glBufferSubDataCalls     = 0;
    uint64_t glBufferSubDataBytes     = 0;
    uint32_t glMapBufferCalls         = 0;
    uint32_t glTexImageCalls          = 0;
    uint64_t glTexImageBytes          = 0;
    uint32_t glTexSubImageCalls       = 0;
    uint64_t glTexSubImageBytes       = 0;
    uint32_t glBindTextureCalls       = 0;
    uint32_t glUseProgramCalls        = 0;

    // Vulkan frame-fence waits (the present path's retire of the previous submit).
    FenceWaitStats vkFence;

    // -----------------------------------------------------------------------
    // GPU-UPDATE-BUFFER-COUNTER-1 — per-owner orphan-on-write tally.
    // Counts full glBufferData re-spec (orphan, no fence/ring) per owner so
    // the per-frame churn that is INVISIBLE in the hitch trace (it only emits
    // on >threshold frames) becomes visible under the MC2_GPUBUF_COUNTER gate.
    // Fixed-size, indexed by GpuBufOwner. Reset each BeginFrame.
    // -----------------------------------------------------------------------
    uint32_t gpuBufOrphanCalls[/*GpuBufOwner::kCount*/ 4] = {0, 0, 0, 0};
    uint64_t gpuBufOrphanBytes[/*GpuBufOwner::kCount*/ 4] = {0, 0, 0, 0};
};

// ---------------------------------------------------------------------------
// Orphan-on-write owner tags (GPU-UPDATE-BUFFER-COUNTER-1).
// Declared inside namespace mc2_hitch (already open). Macros reference it as
// ::mc2_hitch::GpuBufOwner.
// ---------------------------------------------------------------------------
enum class GpuBufOwner : uint8_t {
    GosUpdateBuffer  = 0,   // gos_UpdateBuffer (scene-data UBO respec)
    Hud              = 1,   // updateBuffer 5-arg overload (gosMesh/HUD per-batch)
    LightSsbo        = 2,   // light SSBO create/grow/orphan
    StaticPropShadow = 3,   // static/dynamic prop transient shadow SSBOs
    kCount           = 4
};

// ---------------------------------------------------------------------------
// Span kinds — one per sub-system.
// ---------------------------------------------------------------------------
enum class HitchSpanKind : uint8_t {
    TerrainSolidDispatch    = 0,
    GpuStaticPropsFlush     = 1,
    GpuStaticPropsSnapshot  = 2,
    WaterFastPath           = 3,
    PhaseGameLogic          = 4,
    PhaseRender             = 5,
    PhasePresent            = 6,
    PhaseCap                = 7,
    WaterGuards             = 8,
    WaterRecipe             = 9,
    WaterBuildWindow        = 10,
    WaterUpload             = 11,
    WaterDispatch           = 12,
    kCount
};

// ---------------------------------------------------------------------------
// Public API — implemented in mc2_hitch_trace.cpp.
// ---------------------------------------------------------------------------
void BeginFrame(uint32_t frame);
void EndFrame(uint32_t frame, double dtMs);
void SpanBegin(HitchSpanKind kind);
void SpanEnd(HitchSpanKind kind);
// One wait on the Vulkan frame fence, in milliseconds. No-op while the trace is off.
void RecordFenceWait(double ms);

// ---------------------------------------------------------------------------
// RAII scope — records wall-clock span for one sub-system.
// Zero cost (no clock call) when disabled.
// ---------------------------------------------------------------------------
struct HitchScope {
    HitchSpanKind kind_;
    explicit HitchScope(HitchSpanKind k) : kind_(k) { SpanBegin(k); }
    ~HitchScope() { SpanEnd(kind_); }
    HitchScope(const HitchScope&)            = delete;
    HitchScope& operator=(const HitchScope&) = delete;
};

} // namespace mc2_hitch

// ---------------------------------------------------------------------------
// Globals exposed so MC2_GL_* macros inline without a function call.
// ---------------------------------------------------------------------------
extern bool                        g_mc2HitchEnabled;
extern mc2_hitch::HitchFrameAccum  g_mc2HitchAccum;

// GPU-UPDATE-BUFFER-COUNTER-1: independent default-OFF gate. When set it
// enables per-owner orphan accumulation + a per-frame [GPUBUF v1] emit
// WITHOUT forcing the rest of the hitch trace on (no threshold gating).
// g_mc2GpuBufAccumOn = (g_mc2HitchEnabled || g_mc2GpuBufCounter) is the
// single cached bool the hot-path macros branch on.
extern bool                        g_mc2GpuBufCounter;
extern bool                        g_mc2GpuBufAccumOn;
