// GpuStaticPropRegistry: the per-range phases of flush() -- tombstone and currentness guards, the light-row and
// seam diagnostics and the legacy per-leaf and cached submission paths. The orchestrator is in
// gos_static_prop_registry.cpp.
#include <atomic>
#include "gos_static_prop_registry_internal.h"
#include "mc2fmt.h"

namespace GpuStaticPropRegistry {


// Count the range and skip a tombstoned one.
bool flushRangeTombstoneGuard(uint64_t& s_diag_ranges_total, uint64_t& s_diag_ranges_tombstone, uint32_t regIdx, RecipeRange& rng) {
    ++s_diag_ranges_total;
    // [TOMBDIAG] MC2_STATIC_LEAF_DIAG=1 — frame-sampled, logs EVERY range in the
    // visible set INCLUDING tombstoned (count==0) ones, BEFORE the skip below. This
    // catches the suspected no-trees-on-replay cause: a tree markVisible'd every frame
    // but whose recipe is tombstoned (count==0) -> silently skipped here -> not drawn,
    // while staticReg.registered stays true (no submitMultiShape fallback). Frames 0-3
    // + every 120th. count=0 on a tree regIdx = the smoking gun. Default-off.
    {
        static const bool s_td = false;
        static uint32_t s_tdFrame = 0xFFFFFFFFu;
        static int s_tdN = 0;
        if (s_td) {
            const uint32_t fr = static_cast<uint32_t>(g_mc2FrameCounter.load(std::memory_order_relaxed));
            if (fr != s_tdFrame) { s_tdFrame = fr; s_tdN = 0; }
            if ((fr <= 3u || (fr % 120u) == 0u) && s_tdN < 90) {
                ++s_tdN;
                const int tid = (rng.count > 0 && rng.begin < s_recipes.size())
                                  ? static_cast<int>(s_recipes[rng.begin].typeID) : -1;
                mc2::fmt::print_err("[TOMBDIAG] sample=first90_per_frame frame={:d} regIdx={:d} count={:d} multi={:d} typeID={:d}{}\n", mc2::fmt::as_u32(fr), mc2::fmt::as_u32(regIdx), mc2::fmt::as_u32(rng.count), mc2::fmt::as_i32(static_cast<int>((rng.multi() != nullptr))), mc2::fmt::as_i32(tid), mc2::fmt::safe((rng.count == 0) ? "  <<TOMBSTONED-BUT-VISIBLE" : ""));
                fflush(stderr);
            }
        }
    }
    if (rng.count == 0 || !rng.multi()) { ++s_diag_ranges_tombstone; return true; } // tombstone guard
    return false;
}

// Skip a range whose multi-shape was not refreshed this frame; count and report the stale drops.
bool flushRangeCurrentness(uint32_t currentFrame, uint64_t& s_diag_ranges_stale_frame, uint64_t& s_diag_ranges_stale_after_drawn, uint64_t& s_diag_ranges_drawn, uint64_t& s_diag_persistent_vanish, bool s_guard2, uint32_t kVanishFrames, std::unordered_map<int, uint64_t>& s_staleByType, std::unordered_map<uint32_t, uint32_t>& s_staleStreak, uint32_t regIdx, RecipeRange& rng) {
    // 2026-05-05: cull-aware static replay. Skip recipes whose multi-shape
    // cache wasn't refreshed this frame (offscreen actor whose update()
    // was cull-gate-skipped). The cached lightDataIndex would point at a
    // slot whose content this frame was filled by a different actor —
    // emitting a draw with that index produces wrong/black lighting.
    // Suppressing the draw means the offscreen actor doesn't render this
    // frame; the next frame after it returns to view will refresh the
    // cache and the static path resumes correctly.
    const bool isFirstFlush = !rng.firstFlushSeen;
    // A resident's object is not revisited, so its stamp is not refreshed; it was admitted when it settled.
    if (rng.resident) {
        rng.firstFlushSeen = true;
        ++s_diag_ranges_drawn;
        return false;
    }
    if (rng.multi()->getCachedFrame() != currentFrame) {
        if (isFirstFlush) {
            ++s_firstFrameSkipCount;
            static int s_warnPrinted = 0;
            if (s_warnPrinted < 16) {
                ++s_warnPrinted;
                // COUNTER-HONESTY-1: capped at 16 lines, but s_firstFrameSkipCount
                // is the uncapped truth -- print it so nobody counts log lines.
                MC2_DIAGFE(stderr,
                    "[STATIC_FIRST_FRAME v1] event=skip_first_flush "
                    "sample=first16 skips_so_far_this_mission=%llu "
                    "(destroy() emits event=summary with the final total) regIdx=%u "
                    "registeredOnFrame=%u currentFrame=%u cachedFrame=%u\n",
                    static_cast<unsigned long long>(s_firstFrameSkipCount),
                    regIdx, rng.registeredOnFrame, currentFrame,
                    rng.multi()->getCachedFrame());
                fflush(stderr);
            }
        }
        else {
            // R2B-STATIC-NATURAL-TOUCH-PRESERVE-1 REGRESSION GUARD. A range that
            // was already drawn at least once (firstFlushSeen) and is now stale =
            // a steady-state REGISTERED prop whose per-frame cachedFrame_ stamp was
            // skipped by some update/touch path (the black-tree-bug signature; the
            // R2b static-natural skip was the reintroduction). This used to be a
            // SILENT `continue`; surface it so it can never silently recur. Tree
            // typeIDs appearing here in steady state = trees will vanish on screen.
            // (Benign sibling: an actor that genuinely went off-screen also lands
            // here via cull-gated update — so a few transient counts are expected;
            // a PERSISTENT/GROWING count for an on-screen typeID is the bug.)
            ++s_diag_ranges_stale_after_drawn;
            const int tid = (rng.begin < s_recipes.size())
                              ? static_cast<int>(s_recipes[rng.begin].typeID) : -1;
            static std::set<int> s_loggedStaleTypes;
            if (auto [loggedPos, firstLog] = s_loggedStaleTypes.insert(tid); firstLog) {
                mc2::fmt::print_err("[STATIC_PROP_REGISTRY] event=stale_frame_drop_after_drawn typeID={:d} regIdx={:d} currentFrame={:d} cachedFrame={:d} (update/touch skip suspected — see R2B-STATIC-NATURAL-TOUCH-PRESERVE-1)\n", mc2::fmt::as_i32(tid), mc2::fmt::as_u32(regIdx), mc2::fmt::as_u32(currentFrame), mc2::fmt::as_u32(rng.multi()->getCachedFrame()));
                fflush(stderr);
            }
            // STATIC-REGISTRY-CURRENTNESS-GUARD-2: discriminate transient off-screen
            // (benign) from persistent vanish (the bug). Per-typeID totals + a
            // consecutive-stale streak per regIdx. Only when MC2_REGFLUSH_GUARD2 set.
            bool persistentVanish = false;
            if (s_guard2) {
                ++s_staleByType[tid];
                uint32_t& streak = s_staleStreak[regIdx];
                if (++streak == kVanishFrames) {
                    ++s_diag_persistent_vanish;
                    persistentVanish = true;
                    mc2::fmt::print_err("[REGFLUSH_GUARD2] event=persistent_vanish typeID={:d} regIdx={:d} streak={:d} currentFrame={:d} cachedFrame={:d} staleTotalForType={:d} (registered+drawn prop dropped every frame — producer stamp frozen)\n", mc2::fmt::as_i32(tid), mc2::fmt::as_u32(regIdx), mc2::fmt::as_u32(streak), mc2::fmt::as_u32(currentFrame), mc2::fmt::as_u32(rng.multi()->getCachedFrame()), mc2::fmt::as_u64(static_cast<unsigned long long>(s_staleByType[tid])));
                    fflush(stderr);
                }
            }
            // Opt-in teeth for CI / bisection: abort on the regression signature.
            // GUARD-2 narrows the fatal to the persistent-vanish case so a few
            // benign off-screen transients don't trip CI; the legacy fatal (no
            // guard2) still aborts on first stale_after_drawn for strict bisection.
            static const bool s_staleFatal = mc2::gate::get<"MC2_STATIC_STALE_DROP_FATAL">();
            if (s_staleFatal && (!s_guard2 || persistentVanish)) {
                mc2::fmt::print_err("[STATIC_PROP_REGISTRY] FATAL stale_frame_drop_after_drawn typeID={:d} (MC2_STATIC_STALE_DROP_FATAL{})\n", mc2::fmt::as_i32(tid), mc2::fmt::safe(s_guard2 ? ", persistent_vanish" : ""));
                fflush(stderr);
                std::abort();
            }
        }
        ++s_diag_ranges_stale_frame;
        return true;
    }
    rng.firstFlushSeen = true;
    // GUARD-2: a successful draw clears the consecutive-stale streak for this range
    // so only persistent (never-drawn-again) ranges accumulate toward kVanishFrames.
    if (s_guard2 && !s_staleStreak.empty()) s_staleStreak.erase(regIdx);
    ++s_diag_ranges_drawn;
    // 2026-05-10 diag: env-gated dump of multi-leaf ranges (MC2_REGFLUSH_MULTI=1).
    {
        static const bool s_traceMulti = false;
        static std::set<uint32_t> s_seenMulti;
        if (s_traceMulti && rng.count > 1 && s_seenMulti.size() < 100 &&
            s_seenMulti.find(regIdx) == s_seenMulti.end()) {
            s_seenMulti.insert(regIdx);
            uint32_t firstTid = s_recipes[rng.begin].typeID;
            mc2::fmt::print_err("[REGFLUSH_MULTI v1] sample=first100_distinct_regidx regIdx={:d} count={:d} firstTypeID={:d} multi={:016X}\n", mc2::fmt::as_u32(regIdx), mc2::fmt::as_u32(rng.count), mc2::fmt::as_u32(firstTid), mc2::fmt::as_u64(rng.multiId.value));
            fflush(stderr);
        }
    }
    return false;
}

// Count light rows that would reach the GPU empty and trace the first light slots.
void flushLightRowDiag(uint64_t s_diag_flush_calls, uint32_t regIdx, RecipeRange& rng, uint32_t freshLightIdx) {
    // ── [STATIC_LIGHT_ROW v1] ZERO-LIGHT-ROW-1 fail-loud invariant ──────────
    // WHY HERE: this is the LAST line that owns the light-row index before it
    // becomes GPU state. Both flush branches derive from freshLightIdx — the
    // legacy per-leaf path stamps it verbatim (`inst.lightDataIndex =
    // freshLightIdx`, ~:1300) and the cached path is only taken when it
    // provably equals the stamped recipe value (`useCachedBlob`, ~:1262). So
    // one test here covers every creation route upstream, whichever hole
    // produced the bad value.
    //
    // WHAT WAS MISSING: three lines below, `useCachedBlob` already tests
    // `freshLightIdx != 0xFFFFFFFFu`. The code therefore KNOWS the "no value"
    // sentinel can be live at this point — and uses that knowledge only to
    // pick a BRANCH, never to reject. The legacy branch it falls into then
    // writes the sentinel straight into inst.lightDataIndex unguarded, and
    // static_prop.vert does `light[int(inst.lightDataIndex)]` → int(-1) →
    // out-of-bounds SSBO read → an all-zero row on every driver that
    // implements robust buffer access. A row of zeros is not a lighting
    // result; it is a missing value wearing the costume of a computed one,
    // and every layer between here and the framebuffer treated it as data.
    //
    // WHY A COUNTER AND NOT AN ASSERT: there is measured evidence that stock
    // data already reaches a never-baked-slot state — [STATIC_LIGHT_ZERO v1]
    // reports a rock-steady zeroLightSlots=35 across 5100 frames of mc2_10
    // (scratch/ppp1/capA/mc2_10_capture.log). A default-ON hard assert would
    // therefore abort a STOCK mission on load: a far worse regression than
    // the defect. A default-ON visible marker would likewise repaint stock
    // props for a condition not yet proven illegitimate. So: default-ON,
    // zero pixels changed, one loud line the existing capture rails already
    // collect — LOUD without being DANGEROUS. Promote to assert/marker only
    // once a run proves the count is 0 on stock.
    //
    // peekLightSlot() is the tree's own classifier and needs no new gate:
    //   numLights <  0  → index outside the uploaded table (OOB → zero row)
    //   numLights == 0  → in range, but an empty row (reads as zero light)
    // Cost: one bounds-check + two loads per visible RANGE per frame (not
    // per leaf); ranges are O(100s). Nothing is allocated and nothing logs
    // after the first offending range.
    {
        static uint64_t s_zlr_sentinel   = 0;   // freshLightIdx == UINT32_MAX
        static uint64_t s_zlr_outOfRange = 0;   // index past the uploaded table
        static uint64_t s_zlr_emptyRow   = 0;   // slot present, numLights == 0
        static uint64_t s_zlr_instances  = 0;   // leaves shipped with a bad row
        static bool     s_zlr_firstShown = false;

        // peekLightSlot returns numLights == -1 for an index past the
        // uploaded table, which is exactly classify()'s out-of-range input.
        int zlrNumLights = -1;
        if (mc2::vkLights::active() && freshLightIdx != static_light_row::kNoLightIndex)
            zlrNumLights = mc2::vkLights::peekLightSlot(freshLightIdx).numLights;
        const static_light_row::Class zlrClass =
            static_light_row::classify(freshLightIdx, zlrNumLights);
        switch (zlrClass) {
            case static_light_row::kSentinel:   ++s_zlr_sentinel;   break;
            case static_light_row::kOutOfRange: ++s_zlr_outOfRange; break;
            case static_light_row::kEmptyRow:   ++s_zlr_emptyRow;   break;
            default: break;
        }
        if (zlrClass != static_light_row::kHealthy) {
            s_zlr_instances += rng.count;
            // Quiet by default (W106): the counters above are always collected; MC2_DIAG_TAGS=STATIC_LIGHT_ROW prints the lines.
            static const bool s_zlr_print = mc2_diag::tagRequested("STATIC_LIGHT_ROW");
            if (s_zlr_print && !s_zlr_firstShown) {
                s_zlr_firstShown = true;
                mc2::fmt::print_err("[STATIC_LIGHT_ROW v1] event=zero_row_reached_gpu class={} regIdx={:d} typeID={:d} leaves={:d} lightIdx={:d} numLights={:d} tableCount={:d} rngLightIdx={:d} multiCache={:d}\n", mc2::fmt::safe(static_light_row::className(zlrClass)), mc2::fmt::as_u32(regIdx), mc2::fmt::as_u32(s_recipes[rng.begin].typeID), mc2::fmt::as_u32(rng.count), mc2::fmt::as_u32(freshLightIdx), mc2::fmt::as_i32(zlrNumLights), mc2::fmt::as_u32(mc2::vkLights::active() ? mc2::vkLights::getLightStructCount() : 0u), mc2::fmt::as_u32(rng.lightDataIndex), mc2::fmt::as_u32(rng.multi()->getCachedGpuLightIndex()));
                fflush(stderr);
            }
        }
        // Periodic census on the same cadence as the other flush summaries so
        // a run that never trips the first-hit line still reports a clean 0.
        // 2026-10-05: census only once something has been counted (default build stays quiet).
        static const bool s_zlr_censusPrint = mc2_diag::tagRequested("STATIC_LIGHT_ROW");
        if (s_zlr_censusPrint && (s_diag_flush_calls % 300) == 0 &&
            (s_zlr_sentinel | s_zlr_outOfRange | s_zlr_emptyRow | s_zlr_instances) != 0) {
            mc2::fmt::print_err("[STATIC_LIGHT_ROW v1] event=census frame={:d} sentinel={:d} out_of_range={:d} empty_row={:d} bad_leaves={:d}\n", mc2::fmt::as_u64(static_cast<unsigned long long>(s_diag_flush_calls)), mc2::fmt::as_u64(static_cast<unsigned long long>(s_zlr_sentinel)), mc2::fmt::as_u64(static_cast<unsigned long long>(s_zlr_outOfRange)), mc2::fmt::as_u64(static_cast<unsigned long long>(s_zlr_emptyRow)), mc2::fmt::as_u64(static_cast<unsigned long long>(s_zlr_instances)));
            fflush(stderr);
        }
    }
    // ── end [STATIC_LIGHT_ROW v1] ──────────────────────────────────────────
    // 2026-05-05 black-billboard diagnostic: report numLights at the cached
    // slot. If numLights==0 here, calc_light returns base_light only — for
    // tree leaves with aRGBLight=0xFF000000 + BaseVertexColor=0, that's black.
    // Capped to first 16 SP_TRACE lines per session to avoid 300K-line spam
    // (~100 trees × 3000 frames in a 30s smoke).
    {
        static int s_flushTracePrinted = 0;
        if (s_trace && s_flushTracePrinted < 16) {
            ++s_flushTracePrinted;
            mc2::vkLights::LightSlotPeek peek = {-2, -2, 0, 0, 0};
            if (mc2::vkLights::active()) peek = mc2::vkLights::peekLightSlot(freshLightIdx);
            const uint32_t structCount = mc2::vkLights::active()
                ? mc2::vkLights::getLightStructCount() : 0;
            // H2 trace: if peek.numLights>0 but firstColor is (0,0,0) and/or
            // firstType is AMBIENT, that explains tree-leaf vertices going
            // black on the static replay (base_light=0 + ambient*0 = 0).
            SP_TRACE("flush sample=first16 regIdx=%u lightIdx=%u count=%u nL=%d type0=%d c0=(%.3f,%.3f,%.3f) sc=%u",
                     regIdx, freshLightIdx, rng.count,
                     peek.numLights, peek.firstType,
                     peek.firstColorR, peek.firstColorG, peek.firstColorB,
                     structCount);
        }
    }
}

// Trace the override-type recipes that reach the substrate emit.
void flushSeamProbe(uint32_t regIdx, RecipeRange& rng, const float (&actorWorldCenter)[3]) {
    // [SEAMPROBE] stage 9 (override branch): per-recipe flush-emit census
    // keyed to override typeIDs (hangar=33, tc1_1=41/42/43). Proves whether
    // the override recipe reaches the substrate emit loop each frame
    // (admission gate). Env-gated diagnostic, independent of the cached-blob
    // gate below.
    {
        static const bool s_seamFlush = mc2::gate::get<"MC2_MODOVERRIDE_TRACE">();
        static int s_seamFlushLogged = 0;
        if (s_seamFlush && rng.count > 0 && s_seamFlushLogged < 200) {
            const uint32_t t0 = s_recipes[rng.begin].typeID;
            if (t0 == 33u || t0 == 41u || t0 == 42u || t0 == 43u) {
                ++s_seamFlushLogged;
                mc2::fmt::print_err("[SEAMPROBE] sample=first200 flush-emit regIdx={:d} firstTypeID={:d} count={:d} extentRadius={:.2f} rootCtr=({:.1f},{:.1f},{:.1f})\n", mc2::fmt::as_u32(regIdx), mc2::fmt::as_u32(t0), mc2::fmt::as_u32(rng.count), rng.extentRadius, actorWorldCenter[0], actorWorldCenter[1], actorWorldCenter[2]);
                fflush(stderr);
            }
        }
    }
}

// Trace which branch a range takes and whether its first leaves are zero.
void flushLeafDiag(uint32_t regIdx, RecipeRange& rng, uint32_t freshLightIdx, bool useCachedBlob) {
    // [LEAFDIAG] MC2_STATIC_LEAF_DIAG=1 — pinpoint the no-trees-on-registered-path
    // (MC2_FORCE_DYNAMIC_TREES=0) bug. RenderDoc A/B proved: the main-view prop draw
    // fires with the correct instance COUNT but the bound per-instance modelMatrix
    // data is ZERO -> degenerate -> invisible. This logs, per visible static range,
    // which flush branch it takes and whether the recipe LEAF data it replays is
    // itself zero (the suspected root cause). Default-off: zero behavior change.
    {
        static const bool s_leafDiag = false;
        static int s_leafDiagLogged = 0;
        if (s_leafDiag && rng.count > 0 && s_leafDiagLogged < 48) {
            ++s_leafDiagLogged;
            const float* m0 = s_recipes[rng.begin].modelMatrix;
            const uint32_t midI = rng.count / 2u;
            const float* mm = s_recipes[rng.begin + midI].modelMatrix;
            const bool leaf0Zero = (m0[0]==0.0f && m0[3]==0.0f && m0[7]==0.0f && m0[11]==0.0f);
            const bool midZero   = (mm[0]==0.0f && mm[3]==0.0f && mm[7]==0.0f && mm[11]==0.0f);
            mc2::fmt::print_err("[LEAFDIAG] sample=first48 regIdx={:d} typeID={:d} count={:d} cachedBlob={:d} freshLight={:d} storedLight={:d} leaf0_rawXYZ=({:.1f},{:.1f},{:.1f}) leaf0Zero={:d} midZero={:d}\n", mc2::fmt::as_u32(regIdx), mc2::fmt::as_u32(s_recipes[rng.begin].typeID), mc2::fmt::as_u32(rng.count), mc2::fmt::as_i32(static_cast<int>(useCachedBlob)), mc2::fmt::as_u32(freshLightIdx), mc2::fmt::as_u32(s_recipes[rng.begin].lightDataIndex), -m0[3], m0[11], m0[7], mc2::fmt::as_i32(static_cast<int>(leaf0Zero)), mc2::fmt::as_i32(static_cast<int>(midZero)));
            fflush(stderr);
        }
    }
}

// Native prop draws consume the instance feed directly. Handleless prop leaves
// have no consumer in VisibilityHistory; rebuilding their old GL cull stream
// only added CPU work and enlarged every native cull upload and readback.
void flushLegacyRange(GpuStaticPropBatcher& batcher, RecipeRange& rng, uint32_t freshLightIdx) {
    if (s_staticFrozenReg) return; // already present in the all-recipes store
    for (uint32_t i = 0; i < rng.count; ++i) {
        GpuStaticPropInstance inst = s_recipes[rng.begin + i];
        inst.lightDataIndex = freshLightIdx;
        batcher.submitCachedInstance(inst);
    }
}

void flushCachedRange(GpuStaticPropBatcher& batcher, bool storeDirty, RecipeRange& rng) {
    if (storeDirty && !s_staticFrozenReg)
        batcher.appendPersistentStaticRange(&s_recipes[rng.begin], rng.count);
}

}  // namespace GpuStaticPropRegistry
