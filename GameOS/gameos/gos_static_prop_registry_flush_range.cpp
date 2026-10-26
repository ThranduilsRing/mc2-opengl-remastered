// GpuStaticPropRegistry: the per-range phases of flush() -- tombstone and currentness guards, the light-row and
// seam diagnostics and the legacy per-leaf and cached submission paths. The orchestrator is in
// gos_static_prop_registry.cpp.
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
        static const bool s_td = mc2::gate::get<"MC2_STATIC_LEAF_DIAG">();
        static uint32_t s_tdFrame = 0xFFFFFFFFu;
        static int s_tdN = 0;
        if (s_td) {
            const uint32_t fr = static_cast<uint32_t>(g_mc2FrameCounter);
            if (fr != s_tdFrame) { s_tdFrame = fr; s_tdN = 0; }
            if ((fr <= 3u || (fr % 120u) == 0u) && s_tdN < 90) {
                ++s_tdN;
                const int tid = (rng.count > 0 && rng.first < s_recipes.size())
                                  ? static_cast<int>(s_recipes[rng.first].typeID) : -1;
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
            const int tid = (rng.first < s_recipes.size())
                              ? static_cast<int>(s_recipes[rng.first].typeID) : -1;
            static std::set<int> s_loggedStaleTypes;
            if (s_loggedStaleTypes.insert(tid).second) {
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
        static const bool s_traceMulti = mc2::gate::get<"MC2_REGFLUSH_MULTI">();
        static std::set<uint32_t> s_seenMulti;
        if (s_traceMulti && rng.count > 1 && s_seenMulti.size() < 100 &&
            s_seenMulti.find(regIdx) == s_seenMulti.end()) {
            s_seenMulti.insert(regIdx);
            uint32_t firstTid = s_recipes[rng.first].typeID;
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
            if (!s_zlr_firstShown) {
                s_zlr_firstShown = true;
                mc2::fmt::print_err("[STATIC_LIGHT_ROW v1] event=zero_row_reached_gpu class={} regIdx={:d} typeID={:d} leaves={:d} lightIdx={:d} numLights={:d} tableCount={:d} rngLightIdx={:d} multiCache={:d}\n", mc2::fmt::safe(static_light_row::className(zlrClass)), mc2::fmt::as_u32(regIdx), mc2::fmt::as_u32(s_recipes[rng.first].typeID), mc2::fmt::as_u32(rng.count), mc2::fmt::as_u32(freshLightIdx), mc2::fmt::as_i32(zlrNumLights), mc2::fmt::as_u32(mc2::vkLights::active() ? mc2::vkLights::getLightStructCount() : 0u), mc2::fmt::as_u32(rng.lightDataIndex), mc2::fmt::as_u32(rng.multi()->getCachedGpuLightIndex()));
                fflush(stderr);
            }
        }
        // Periodic census on the same cadence as the other flush summaries so
        // a run that never trips the first-hit line still reports a clean 0.
        // 2026-10-05: census only once something has been counted (default build stays quiet).
        if ((s_diag_flush_calls % 300) == 0 &&
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
            const uint32_t t0 = s_recipes[rng.first].typeID;
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
        static const bool s_leafDiag = mc2::gate::get<"MC2_STATIC_LEAF_DIAG">();
        static int s_leafDiagLogged = 0;
        if (s_leafDiag && rng.count > 0 && s_leafDiagLogged < 48) {
            ++s_leafDiagLogged;
            const float* m0 = s_recipes[rng.first].modelMatrix;
            const uint32_t midI = rng.count / 2u;
            const float* mm = s_recipes[rng.first + midI].modelMatrix;
            const bool leaf0Zero = (m0[0]==0.0f && m0[3]==0.0f && m0[7]==0.0f && m0[11]==0.0f);
            const bool midZero   = (mm[0]==0.0f && mm[3]==0.0f && mm[7]==0.0f && mm[11]==0.0f);
            mc2::fmt::print_err("[LEAFDIAG] sample=first48 regIdx={:d} typeID={:d} count={:d} cachedBlob={:d} freshLight={:d} storedLight={:d} leaf0_rawXYZ=({:.1f},{:.1f},{:.1f}) leaf0Zero={:d} midZero={:d}\n", mc2::fmt::as_u32(regIdx), mc2::fmt::as_u32(s_recipes[rng.first].typeID), mc2::fmt::as_u32(rng.count), mc2::fmt::as_i32(static_cast<int>(useCachedBlob)), mc2::fmt::as_u32(freshLightIdx), mc2::fmt::as_u32(s_recipes[rng.first].lightDataIndex), -m0[3], m0[11], m0[7], mc2::fmt::as_i32(static_cast<int>(leaf0Zero)), mc2::fmt::as_i32(static_cast<int>(midZero)));
            fflush(stderr);
        }
    }
}

// Submit each leaf of the range with the fresh light index and append its cull record.
void flushLegacyRange(GpuStaticPropBatcher& batcher, uint64_t s_diag_flush_calls, uint64_t& s_diag_leaves_appended, uint32_t regIdx, RecipeRange& rng, uint32_t freshLightIdx, const float (&actorWorldCenter)[3]) {
    // Per-leaf rebuild path: a range whose light row does not match its stamped recipe value.
    for (uint32_t i = 0; i < rng.count; ++i) {
        GpuStaticPropInstance inst = s_recipes[rng.first + i]; // stack copy
        inst.lightDataIndex = freshLightIdx;
        // G2: under the frozen-records gate this legacy (light-mismatch) range is
        // already in the all-recipes persistent store — skip the per-frame submit
        // to avoid double-counting the instance.
        if (!s_staticFrozenReg)
            batcher.submitCachedInstance(inst);

        // C1b GPU authority flip: emit one GpuActorRecord per submitted static prop
        // instance so the compute cull shader can scatter it into the correct bucket
        // (typeID). This appends to the already-flushed substrate slot; compute_dispatch()
        // runs AFTER this loop (moved from mission.cpp to txmmgr.cpp) and picks up the
        // updated hdr->recordCount.
        //
        // category encoding: low 4 bits = Cat_StaticProp (5), upper 28 bits = typeID.
        // Shader: uint cat = rec.category & CATEGORY_MASK; if cat == CAT_STATIC_PROP,
        //         uint bucket = rec.category >> 4;  →  correct bucket scatter.
        //
        // Flag_AlwaysVisible NOT set: the GPU frustum test runs (using worldCenter +
        // boundingRadius). This can conservatively cull props near the frustum edge;
        // acceptable because the CPU-side markVisible() already gates which recipes
        // reach this loop (CPU visibility IS the admission gate for registry path).
        // Any GPU-culled prop that the CPU admitted will just fall back to 0-count
        // draw — invisible for that frame, restored next frame as it re-enters frustum.
        if (gpu_cull::substrate_isEnabled()) {
            gpu_cull::GpuActorRecord gpuRec{};
            // World position: inst.modelMatrix is Stuff row-vector
            // convention stored in column-major array order
            // (mclib/stuff/matrix.hpp:133 — `entries[(column<<2)+row]`).
            // Translation lives at row 3, columns 0..2. With column-
            // major storage, M(row,col) = entries[(col<<2)+row], so:
            //   M(3,0) = entries[(0<<2)+3] = entries[3]   (tx)
            //   M(3,1) = entries[(1<<2)+3] = entries[7]   (ty)
            //   M(3,2) = entries[(2<<2)+3] = entries[11]  (tz)
            // entries[12]/[13]/[14] are the BOTTOM ROW of columns 0..2
            // (always 0 for affine transforms). Reading those produces
            // worldCenter≈(0,0,0) for every static prop — the cull then
            // projects every prop to the world origin, so all admit/
            // reject together as the camera rotates (1° flip = all on/
            // all off). Verified against Matrix4D::BuildTranslation at
            // mclib/stuff/matrix.cpp:214 and the existing diagnostic at
            // gos_static_prop_batcher.cpp:1879-1882.
            //
            // Coord-space: the translation is in Stuff/MLR camera frame
            // (.x=-rawX, .y=elev, .z=rawY) per BldgAppearance and
            // TreeAppearance::registerStatic in mclib/bdactor.cpp.
            // gos_GetTerrainMVPMat4 (axisSwap * worldToClip) expects raw
            // MC2 world coords (x=east, y=north, z=elev) and bakes the
            // swap, so unswap here:
            //   raw.x = -stuff.x  =  -entries[3]
            //   raw.y =  stuff.z  =   entries[11]
            //   raw.z =  stuff.y  =   entries[7]   (elev)
            // Mirrors the per-vertex swap in static_prop.vert at
            // `world_mc2 = vec3(-world_stuff.x, world_stuff.z, world_stuff.y)`.
            // 2026-05-10 actor-center fix: use the parent multishape's
            // root translation (computed once per range above) for EVERY
            // leaf's substrate record. Cull treats the actor as a single
            // visibility unit; all (typeID, leaf) records of one actor
            // accept-or-reject as a group. Per-leaf inst.modelMatrix
            // remains correct in the per-frame instance SSBO for shader
            // placement — only the cull-side worldCenter is unified.
            gpuRec.worldCenter[0] = actorWorldCenter[0];
            gpuRec.worldCenter[1] = actorWorldCenter[1];
            gpuRec.worldCenter[2] = actorWorldCenter[2];
            // Bounding radius: use per-prop extent radius captured by markVisible()
            // from bldgShape/treeShape->GetExtentRadius() (F4 T3, 2026-05-22).
            // This fixes static-prop pop-in: post-F1 the correct GL-NDC matrix
            // produces a tighter clipSpaceFrustumAdmitSphere envelope than the
            // old D3D-pixel-homog matrix; large props (>200 units) were
            // over-culled when their centroid exited the frustum by >200 units.
            // Fallback to 200.0f when extentRadius == 0.0f (unpatched callers or
            // missing shape pointer) preserves prior behavior.
            gpuRec.boundingRadius = (rng.extentRadius > 0.0f) ? rng.extentRadius : 200.0f;
            gpuRec.worldAabbMin[0] = gpuRec.worldCenter[0] - gpuRec.boundingRadius;
            gpuRec.worldAabbMin[1] = gpuRec.worldCenter[1] - gpuRec.boundingRadius;
            gpuRec.worldAabbMin[2] = gpuRec.worldCenter[2] - gpuRec.boundingRadius;
            gpuRec.worldAabbMax[0] = gpuRec.worldCenter[0] + gpuRec.boundingRadius;
            gpuRec.worldAabbMax[1] = gpuRec.worldCenter[1] + gpuRec.boundingRadius;
            gpuRec.worldAabbMax[2] = gpuRec.worldCenter[2] + gpuRec.boundingRadius;
            // Category: typeID in upper 28 bits + Cat_StaticProp (5) in lower 4 bits.
            gpuRec.category = (static_cast<uint32_t>(inst.typeID) << 4)
                            | static_cast<uint32_t>(gpu_cull::Cat_StaticProp);
            // 2026-05-10 diag: temp force always-visible to A/B-test whether
            // the cull is rejecting buildings.
            static const bool s_diag_forceAdmit = mc2::gate::get<"MC2_STATIC_FORCE_ADMIT">();
            gpuRec.flags          = s_diag_forceAdmit
                                      ? static_cast<uint32_t>(gpu_cull::Flag_AlwaysVisible)
                                      : gpu_cull::Flag_None;
            gpuRec.actorId        = 0u;   // static props have no actor handle
            gpuRec.prevVisibilityBit = 1u; // CPU admitted this prop this frame
            gpuRec.consumerFlags  = 0u;
            // C1b temporal-superset Slice 1: real terrain block index so
            // the block rollup can stamp the right block. worldCenter[0]
            // is raw-MC2 east, [1] raw-MC2 north (the -stuff.x unswap was
            // applied above producing the east-frame). Feed [0],[1] ONLY;
            // NEVER [2] (elevation). worldCenter fully populated above.
            gpuRec.blockIdx       = static_cast<uint32_t>(
                Terrain::worldToBlockIdx(gpuRec.worldCenter[0],
                                         gpuRec.worldCenter[1]));
            // [BLKIDX v1] env-gated GEOMETRIC probe (demote-not-delete).
            // Non-degeneracy is provably blind to a frame mirror — assert
            // the helper == hand-rolled CPU block math for THIS prop's
            // true raw position, plus zero_verify of the [2]-exclusion.
            {
                static const bool s_blkidxTrace = mc2::gate::get<"MC2_BLKIDX_TRACE">();
                if (s_blkidxTrace) {
                    const float pwx = gpuRec.worldCenter[0];
                    const float pwy = gpuRec.worldCenter[1];
                    long mx = (static_cast<long>(pwx) >> 7) + Terrain::halfVerticesMapSide;
                    long bx = static_cast<long>((mx * Terrain::oneOverVerticesBlockSide));
                    long my = Terrain::halfVerticesMapSide -
                              ((static_cast<long>(pwy) >> 7) + 1);
                    long by = static_cast<long>((my * Terrain::oneOverVerticesBlockSide));
                    long cpuBlk = bx + (by * Terrain::blocksMapSide);
                    long helperBlk = Terrain::worldToBlockIdx(pwx, pwy);
                    mc2::fmt::print_err("[BLKIDX v1] event=geom_check src=registry wx={:.1f} wy={:.1f} wz={:.1f} helper={:d} cpu={:d} match={:d}\n", pwx, pwy, gpuRec.worldCenter[2], mc2::fmt::as_i32(helperBlk), mc2::fmt::as_i32(cpuBlk), mc2::fmt::as_i32((helperBlk == cpuBlk) ? 1 : 0));
                    // zero_verify: elevation [2] must NEVER influence the
                    // index — recompute ignoring [2] (trivially true here
                    // since helper takes only [0],[1]) and assert equality.
                    mc2::fmt::print_err("[BLKIDX v1] event=zero_verify src=registry blockIdx={:d} z_excluded=1\n",
                        mc2::fmt::as_u32(gpuRec.blockIdx));
                    fflush(stderr);
                }
            }
            // M1: under the frozen-records gate, the legacy (light-mismatch)
            // path is unsupported — these ranges go to submitCachedInstance
            // (per-frame bucket), NOT the persistent store, so they have no
            // frozen golden slot. In the default config (per-instance light on)
            // NO range reaches this path. If one does under the gate it will
            // surface as oracle emptyPoolButVisible. Skip the per-frame append.
            if (!s_staticFrozenReg)
                gpu_cull::substrate_appendStaticPropRecord(gpuRec);
            ++s_diag_leaves_appended;
            // v2: record substrate submission for this recipe (extraction reads previous frame's state).
            if (regIdx < static_cast<uint32_t>(s_recipeHasSubstrateRecord.size()))
                s_recipeHasSubstrateRecord[regIdx] = 1u;
            // 2026-05-10 diag: typeID histogram. MC2_REGFLUSH_TYPEHIST=1 to enable.
            {
                static const bool s_th = mc2::gate::get<"MC2_REGFLUSH_TYPEHIST">();
                static std::array<uint64_t, 1024> s_typeHist{};
                if (s_th) {
                    if (inst.typeID < s_typeHist.size()) ++s_typeHist[inst.typeID];
                    if (s_diag_flush_calls == 600 && i == 0) {
                        mc2::fmt::print_err("[REGFLUSH_TYPEHIST v1] non-zero buckets:\n");
                        for (size_t t = 0; t < s_typeHist.size(); ++t) {
                            if (s_typeHist[t] > 0) {
                                mc2::fmt::print_err("[REGFLUSH_TYPEHIST v1] typeID={:d} count={:d}\n", mc2::fmt::as_size(t), mc2::fmt::as_u64(static_cast<unsigned long long>(s_typeHist[t])));
                            }
                        }
                        fflush(stderr);
                    }
                }
            }
        }
    }
}

// Bulk-submit the range from the cached records and the persistent store.
void flushCachedRange(GpuStaticPropBatcher& batcher, bool s_storeDirty, uint64_t& s_diag_leaves_appended, uint32_t regIdx, RecipeRange& rng) {
    // 1. Instances: recipes carry the permanent lightDataIndex, so no per-frame light patch is needed. Append to the
    //    persistent store ONLY on a dirty generation; a clean frame skips this entirely (it is reinjected in bulk after
    //    the loop). Under the frozen-records gate the store is built from ALL recipes in the all-recipes pass, so the
    //    per-visible append is skipped.
    if (s_storeDirty && !s_staticFrozenReg)
        batcher.appendPersistentStaticRange(&s_recipes[rng.first], rng.count);
    // 2. Per-recipe cached cull records.
    if (gpu_cull::substrate_isEnabled()) {
        for (uint32_t i = 0; i < rng.count; ++i) {
            const uint32_t ri = rng.first + i;
            // Lazy build of the cached cull record on first flush of this recipe.
            if (!s_cachedActorRecordValid[ri]) {
                buildCachedActorRecord(rng, ri, s_cachedActorRecord[ri]);
                s_cachedActorRecordValid[ri] = 1u;
            }
            // Under the frozen-records gate the golden (records + store rank) is built from ALL alive recipes in the
            // all-recipes pass, so the per-visible loop adds nothing here.
            if (!s_staticFrozenReg) {
                gpu_cull::substrate_appendStaticPropRecord(s_cachedActorRecord[ri]);
            }
            ++s_diag_leaves_appended;
            // v2: record substrate submission for this recipe (matches the per-leaf path exactly).
            if (regIdx < static_cast<uint32_t>(s_recipeHasSubstrateRecord.size()))
                s_recipeHasSubstrateRecord[regIdx] = 1u;
        }
    }
}

}  // namespace GpuStaticPropRegistry
