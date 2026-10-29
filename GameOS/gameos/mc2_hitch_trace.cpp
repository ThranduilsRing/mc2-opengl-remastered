// mc2_hitch_trace.cpp — H1a hitch attribution layer implementation.
// Legacy GL-named fields and log keys remain stable for trace consumers.
#include "mc2_hitch_trace.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <array>
#include "Gates/gate.h"
#include "mc2fmt.h"

// ---------------------------------------------------------------------------
// Globals (extern-declared in mc2_hitch_trace.h)
// ---------------------------------------------------------------------------
bool                        g_mc2HitchEnabled = false;
mc2_hitch::HitchFrameAccum  g_mc2HitchAccum   = {};

// GPU-UPDATE-BUFFER-COUNTER-1 globals.
bool                        g_mc2GpuBufCounter = false;
bool                        g_mc2GpuBufAccumOn = false;

// ---------------------------------------------------------------------------
// Private state
// ---------------------------------------------------------------------------
namespace {

double s_thresholdMs = 20.0;

using Clock = std::chrono::steady_clock;
std::array<Clock::time_point, static_cast<size_t>(mc2_hitch::HitchSpanKind::kCount)>
    s_spanStart = {};

struct HitchInit {
    HitchInit() {
        g_mc2HitchEnabled = mc2::gate::get<"MC2_HITCH_TRACE">();
        if (g_mc2HitchEnabled) {
            const double v = mc2::gate::get<"MC2_HITCH_MS">();   // row default 0 = keep the built-in threshold
            if (v > 0.0) s_thresholdMs = v;
            mc2::fmt::print_err("[HITCH_INIT] enabled threshold={:.1f}ms\n", s_thresholdMs);
            fflush(stderr);
        }
        // GPU-UPDATE-BUFFER-COUNTER-1: independent default-OFF gate. OR-in the
        // shared accumulation flag so the orphan macros count under either gate
        // without forcing the rest of the hitch trace (threshold emit) on.
        g_mc2GpuBufCounter = mc2::gate::get<"MC2_GPUBUF_COUNTER">();
        g_mc2GpuBufAccumOn = g_mc2HitchEnabled || g_mc2GpuBufCounter;
        if (g_mc2GpuBufCounter) {
            mc2::fmt::print_err("[GPUBUF_INIT] enabled (per-frame orphan counter)\n");
            fflush(stderr);
        }
    }
} s_hitchInit;

} // namespace

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------
namespace mc2_hitch {

void BeginFrame(uint32_t /*frame*/) {
    // Reset under either gate so the orphan counter sees per-frame deltas
    // even when MC2_HITCH_TRACE is off.
    if (!g_mc2GpuBufAccumOn) return;
    g_mc2HitchAccum = HitchFrameAccum{};
}

void EndFrame(uint32_t frame, double dtMs) {
    if (!g_mc2GpuBufAccumOn) return;

    const HitchFrameAccum& a = g_mc2HitchAccum;

    // GPU-UPDATE-BUFFER-COUNTER-1 — per-frame orphan emit. Driven ONLY by the
    // MC2_GPUBUF_COUNTER gate (no hitch threshold), so the normally-invisible
    // per-frame orphan-on-write churn is reported every frame.
    if (g_mc2GpuBufCounter) {
        using O = GpuBufOwner;
        const unsigned gos = static_cast<unsigned>(O::GosUpdateBuffer);
        const unsigned hud = static_cast<unsigned>(O::Hud);
        const unsigned lit = static_cast<unsigned>(O::LightSsbo);
        const unsigned sps = static_cast<unsigned>(O::StaticPropShadow);
        uint32_t totCalls = 0;
        uint64_t totBytes = 0;
        for (unsigned i = 0; i < static_cast<unsigned>(O::kCount); ++i) {
            totCalls += a.gpuBufOrphanCalls[i];
            totBytes += a.gpuBufOrphanBytes[i];
        }
        mc2::fmt::print_err("[GPUBUF v1] frame={:d} orphan_calls={:d} orphan_bytes={:d} by_owner=gos_UpdateBuffer:{:d}/{:d},hud:{:d}/{:d},light:{:d}/{:d},sp_shadow:{:d}/{:d}\n", mc2::fmt::as_u32(frame), mc2::fmt::as_u32(totCalls), mc2::fmt::as_u64(static_cast<unsigned long long>(totBytes)), mc2::fmt::as_u32(a.gpuBufOrphanCalls[gos]), mc2::fmt::as_u64(static_cast<unsigned long long>(a.gpuBufOrphanBytes[gos])), mc2::fmt::as_u32(a.gpuBufOrphanCalls[hud]), mc2::fmt::as_u64(static_cast<unsigned long long>(a.gpuBufOrphanBytes[hud])), mc2::fmt::as_u32(a.gpuBufOrphanCalls[lit]), mc2::fmt::as_u64(static_cast<unsigned long long>(a.gpuBufOrphanBytes[lit])), mc2::fmt::as_u32(a.gpuBufOrphanCalls[sps]), mc2::fmt::as_u64(static_cast<unsigned long long>(a.gpuBufOrphanBytes[sps])));
        fflush(stderr);
    }

    // The rest of the hitch-trace emit is gated by MC2_HITCH_TRACE + threshold.
    if (!g_mc2HitchEnabled) return;
    if (dtMs < s_thresholdMs) return;

    // Primary summary line.
    mc2::fmt::print_err("[HITCH] frame={:d} dt={:.2f} terrainSolid={:.2f} staticFlush={:.2f} staticSnapshot={:.2f} water={:.2f} glBufData={:d} glBufBytes={:d} glBufSubData={:d} glBufSubBytes={:d} glMapBuf={:d} glTexImage={:d} glTexImageBytes={:d} glBindTex={:d} glUseProgram={:d} fenceWaitMs={:.2f} fenceWaits={:d}\n",
        mc2::fmt::as_u32(frame), dtMs,
        a.terrainSolidMs, a.staticFlushMs, a.staticSnapshotMs, a.waterMs,
        mc2::fmt::as_u32(a.glBufferDataCalls),    mc2::fmt::as_u64(static_cast<unsigned long long>(a.glBufferDataBytes)),
        mc2::fmt::as_u32(a.glBufferSubDataCalls), mc2::fmt::as_u64(static_cast<unsigned long long>(a.glBufferSubDataBytes)),
        mc2::fmt::as_u32(a.glMapBufferCalls),
        mc2::fmt::as_u32(a.glTexImageCalls), mc2::fmt::as_u64(static_cast<unsigned long long>(a.glTexImageBytes)),
        mc2::fmt::as_u32(a.glBindTextureCalls), mc2::fmt::as_u32(a.glUseProgramCalls),
        a.vkFence.totalMs, mc2::fmt::as_u32(a.vkFence.count));

    // Detail line: terrain solid dispatch.
    if (a.terrainSolidMs > 0.0 || a.terrainBinds || a.terrainUploads || a.terrainTexImageCalls) {
        mc2::fmt::print_err("[HITCH_TERRAIN_TEX] quadSetupTexturesMs={:.2f} binds={:d} uploads={:d} texImageCalls={:d}\n", a.terrainSolidMs, mc2::fmt::as_u32(a.terrainBinds), mc2::fmt::as_u32(a.terrainUploads), mc2::fmt::as_u32(a.terrainTexImageCalls));
    }

    // Detail line: static-prop flush.
    if (a.staticFlushMs > 0.0 || a.staticRecords) {
        mc2::fmt::print_err("[HITCH_STATIC_FLUSH] flushMs={:.2f} records={:d} draws={:d} bufferBytes={:d} realloc={:d} map={:d} subData={:d}\n", a.staticFlushMs, mc2::fmt::as_u32(a.staticRecords), mc2::fmt::as_u32(a.staticDrawCmds), mc2::fmt::as_u64(static_cast<unsigned long long>(a.staticBufferBytes)), mc2::fmt::as_u32(a.staticRealloc), mc2::fmt::as_u32(a.glMapBufferCalls), mc2::fmt::as_u32(a.glBufferSubDataCalls)); // glMapBufferCalls = global approx; staticMap reserved
    }

    // Detail line: water.
    if (a.waterCalled) {
        mc2::fmt::print_err("[HITCH_WATER] called={:d} waterPresent={:d} earlyOut={:d} glCalls={:d}\n", mc2::fmt::as_u32(static_cast<unsigned>(a.waterCalled)), mc2::fmt::as_u32(static_cast<unsigned>(a.waterPresent)), mc2::fmt::as_u32(static_cast<unsigned>(a.waterEarlyOut)), mc2::fmt::as_u32(a.waterGlCalls));
        // H1b detail — only when water actually ran with measurable time
        if (a.waterMs > 0.5) {
            mc2::fmt::print_err("[HITCH_WATER_DETAIL] waterMs={:.2f} guards={:.2f} recipe={:.2f} buildWindow={:.2f} upload={:.2f} dispatch={:.2f} recipes={:d} window={:d} narrow={:d} recipeUploaded={:d}\n", a.waterMs, a.waterGuardsMs, a.waterRecipeMs, a.waterBuildWindowMs, a.waterUploadMs, a.waterDispatchMs, mc2::fmt::as_u32(a.waterRecipeCount), mc2::fmt::as_u32(a.waterWindowCount), mc2::fmt::as_u32(a.waterNarrowCount), mc2::fmt::as_u32(a.waterRecipeUploaded));
        }
    }

    // Detail line: legacy GL totals (stable log schema).
    mc2::fmt::print_err("[HITCH_GL] glBufferData={:d} glBufferDataBytes={:d} glBufferSubData={:d} glBufferSubDataBytes={:d} glMapBuffer={:d} glTexImage={:d} glTexImageBytes={:d} glTexSubImage={:d} glTexSubImageBytes={:d} glBindTexture={:d} glUseProgram={:d}\n",
        mc2::fmt::as_u32(a.glBufferDataCalls),    mc2::fmt::as_u64(static_cast<unsigned long long>(a.glBufferDataBytes)),
        mc2::fmt::as_u32(a.glBufferSubDataCalls), mc2::fmt::as_u64(static_cast<unsigned long long>(a.glBufferSubDataBytes)),
        mc2::fmt::as_u32(a.glMapBufferCalls),
        mc2::fmt::as_u32(a.glTexImageCalls), mc2::fmt::as_u64(static_cast<unsigned long long>(a.glTexImageBytes)),
        mc2::fmt::as_u32(a.glTexSubImageCalls), mc2::fmt::as_u64(static_cast<unsigned long long>(a.glTexSubImageBytes)),
        mc2::fmt::as_u32(a.glBindTextureCalls), mc2::fmt::as_u32(a.glUseProgramCalls));

    // Detail line: Vulkan frame-fence waits.
    {
        char line[160];
        if (a.vkFence.format(line, sizeof line, frame) > 0) { std::fputs(line, stderr); }
    }

    // H1c frame-phase attribution line.
    {
        const double h1aKnown = a.terrainSolidMs + a.staticFlushMs + a.staticSnapshotMs + a.waterMs;
        const double phaseTotal = a.phaseLogicMs + a.phaseRenderMs + a.phasePresentMs + a.phaseCapMs;
        const double unknown = (dtMs > phaseTotal + h1aKnown) ? (dtMs - phaseTotal - h1aKnown) : 0.0;
        mc2::fmt::print_err("[HITCH_PHASE] frame={:d} dt={:.2f} logic={:.2f} render={:.2f} present={:.2f} cap={:.2f} h1aKnown={:.2f} unknown={:.2f}\n", mc2::fmt::as_u32(frame), dtMs, a.phaseLogicMs, a.phaseRenderMs, a.phasePresentMs, a.phaseCapMs, h1aKnown, unknown);
    }

    fflush(stderr);
}

void RecordFenceWait(double ms) {
    if (!g_mc2HitchEnabled) return;
    if (!g_mc2HitchAccum.vkFence.record(ms))
        mc2::fmt::print_err("[HITCH_VK] rejected fence wait sample {:.3f}ms\n", ms);
}

void SpanBegin(HitchSpanKind kind) {
    if (!g_mc2HitchEnabled) return;
    s_spanStart[static_cast<size_t>(kind)] = Clock::now();
}

void SpanEnd(HitchSpanKind kind) {
    if (!g_mc2HitchEnabled) return;
    using namespace std::chrono;
    const auto dt = duration<double, std::milli>(
        Clock::now() - s_spanStart[static_cast<size_t>(kind)]).count();
    switch (kind) {
        case HitchSpanKind::TerrainSolidDispatch:   g_mc2HitchAccum.terrainSolidMs   += dt; break;
        case HitchSpanKind::GpuStaticPropsFlush:    g_mc2HitchAccum.staticFlushMs    += dt; break;
        case HitchSpanKind::GpuStaticPropsSnapshot: g_mc2HitchAccum.staticSnapshotMs += dt; break;
        case HitchSpanKind::WaterFastPath:          g_mc2HitchAccum.waterMs          += dt; break;
        case HitchSpanKind::PhaseGameLogic:         g_mc2HitchAccum.phaseLogicMs     += dt; break;
        case HitchSpanKind::PhaseRender:            g_mc2HitchAccum.phaseRenderMs    += dt; break;
        case HitchSpanKind::PhasePresent:           g_mc2HitchAccum.phasePresentMs   += dt; break;
        case HitchSpanKind::PhaseCap:               g_mc2HitchAccum.phaseCapMs       += dt; break;
        case HitchSpanKind::WaterGuards:            g_mc2HitchAccum.waterGuardsMs      += dt; break;
        case HitchSpanKind::WaterRecipe:            g_mc2HitchAccum.waterRecipeMs      += dt; break;
        case HitchSpanKind::WaterBuildWindow:       g_mc2HitchAccum.waterBuildWindowMs += dt; break;
        case HitchSpanKind::WaterUpload:            g_mc2HitchAccum.waterUploadMs      += dt; break;
        case HitchSpanKind::WaterDispatch:          g_mc2HitchAccum.waterDispatchMs    += dt; break;
        default: break;
    }
}

} // namespace mc2_hitch
