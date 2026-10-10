// GpuStaticPropRegistry: the persistent-store rebuild and the post-loop phases of flush() -- store reinject, the
// diagnostic summaries and the cull-record version bump. The orchestrator is in gos_static_prop_registry.cpp.
#include "gos_static_prop_registry_internal.h"

namespace GpuStaticPropRegistry {


// Rebuild the persistent static store and golden list from every registered recipe.
void flushFrozenStoreRebuild(GpuStaticPropBatcher& batcher_pb, bool s_storeDirty, uint64_t& s_pbExpectStaticCount) {
    if (s_staticFrozenReg && s_storeDirty) {
        // G2 GPU-Scene: under the frozen-records gate the store + golden are built
        // from ALL alive registered recipes (not the per-frame visible
        // s_liveRangeIndices), so the GPU owns frustum culling and the frozen set
        // never shrinks (dissolves the M1 freeze-collapse). Runs on dirty only; the
        // per-visible-range loop below skips its store-append + golden capture under
        // the gate (no double-build). Each leaf is stamped with the permanent
        // per-actor light slot (== regIdx, the slot G1 eager-bakes) so off-screen
        // leaves (never markVisible'd) carry a correct lightDataIndex. recipeToStoreSlot
        // is captured in the SAME order appendPersistentStaticRange fills the store
        // (per-type, range-ascending) so record-index == instance-pool-slot holds.
        std::fill(s_pbStoreCursor.begin(), s_pbStoreCursor.end(), 0u);
        s_goldenRecipeList.clear();
        s_goldenDirty = true;
        for (uint32_t aregIdx = 0u; aregIdx < s_recipeRanges.size(); ++aregIdx) {
            RecipeRange& arng = s_recipeRanges[aregIdx];
            if (arng.count == 0u || !arng.multi) continue;   // tombstone guard
            for (uint32_t i = 0u; i < arng.count; ++i) {
                const uint32_t ri = arng.first + i;
                if (ri >= s_recipes.size()) break;
                s_recipes[ri].lightDataIndex = aregIdx;   // permanent per-actor slot
            }
            batcher_pb.appendPersistentStaticRange(&s_recipes[arng.first], arng.count);
            s_pbExpectStaticCount += arng.count;
            for (uint32_t i = 0u; i < arng.count; ++i) {
                const uint32_t ri = arng.first + i;
                if (ri >= s_recipes.size()) break;
                if (ri < s_cachedActorRecordValid.size() && !s_cachedActorRecordValid[ri]) {
                    buildCachedActorRecord(arng, ri, s_cachedActorRecord[ri]);
                    s_cachedActorRecordValid[ri] = 1u;
                }
                const uint32_t tid = s_recipes[ri].typeID;
                if (tid >= s_pbStoreCursor.size()) s_pbStoreCursor.resize(tid + 1u, 0u);
                if (ri < s_recipeToStoreSlot.size())
                    s_recipeToStoreSlot[ri] = s_pbStoreCursor[tid]++;
                s_goldenRecipeList.push_back(ri);
            }
            if (aregIdx < static_cast<uint32_t>(s_recipeHasSubstrateRecord.size()))
                s_recipeHasSubstrateRecord[aregIdx] = 1u;
        }
    }
}

// Finalise the persistent static store and run its consistency oracle.
void flushReinjectPersistentStore(GpuStaticPropBatcher& batcher_pb, uint64_t s_currentGen, bool s_storeDirty, uint64_t& s_pbExpectStaticCount, uint64_t s_diag_flush_calls) {
    // 2b Stage 2 (Mechanism B-reinject): finalize the store on a dirty rebuild, then
    // bulk-inject the static blocks into s_bucketsByType every frame (one memcpy per
    // type). The downstream upload + coalesce pool read s_bucketsByType unchanged.
    if (s_persistentBuckets) {
        if (s_storeDirty) batcher_pb.setPersistentStaticGen(s_currentGen);
        batcher_pb.reinjectPersistentStatic();
        // [_COMPARE] store-consistency oracle: on a DIRTY rebuild the store total
        // must equal what we just appended (s_pbExpectStaticCount); on a CLEAN frame
        // it must be UNCHANGED from the last rebuild (the store is frozen between
        // dirty events). A mismatch = store corruption / a missed generation bump.
        // (Visual/instance equivalence vs legacy is validated by the rendered-output
        // parity + tier1 + Tracy, per the validation bar.)
        if (s_persistentBucketsCompare) {
            static uint64_t s_pbCmpChecks = 0, s_pbCmpMismatch = 0, s_pbCmpCap = 0;
            static uint64_t s_pbLastRebuildCount = 0;
            ++s_pbCmpChecks;
            const uint64_t storedCount = batcher_pb.persistentStaticTotalCount();
            const uint64_t expect = s_storeDirty ? s_pbExpectStaticCount : s_pbLastRebuildCount;
            if (s_storeDirty) s_pbLastRebuildCount = storedCount;
            if (storedCount != expect) {
                ++s_pbCmpMismatch;
                if (s_pbCmpCap < 32) { ++s_pbCmpCap;
                    fprintf(stderr, "[PERSIST_BUCKET_COMPARE v1] event=mismatch "
                        "sample=first32 (event=summary carries the true total) "
                        "stored=%llu expect=%llu dirty=%d\n",
                        (unsigned long long)storedCount, (unsigned long long)expect, s_storeDirty ? 1 : 0);
                    fflush(stderr);
                }
            }
            if ((s_diag_flush_calls % 600u) == 0u) {
                fprintf(stderr, "[PERSIST_BUCKET_COMPARE v1] event=summary checks=%llu mismatches=%llu stored=%llu\n",
                    (unsigned long long)s_pbCmpChecks, (unsigned long long)s_pbCmpMismatch,
                    (unsigned long long)storedCount);
                fflush(stderr);
            }
        }
    }
}

// Count the registered props whose light slot was never baked and dump their rows once.
void flushZeroLightProbe(uint64_t s_diag_flush_calls) {
    // [G1-STATIC-EAGER-LIGHT v1] Zero-light probe: count alive registered static props
    // whose permanent light slot (lightData_[recipeIndex]) has numLights_=0 (never baked).
    // With MC2_GPU_CULL_STATIC_EAGER_LIGHT_BAKE set, zeroLightSlots should reach 0 after
    // mission load. Without it, zeroLightSlots counts all props not yet on screen.
    // Emit every ~300 flushes. Gate default-OFF.
    static const bool s_staticLightZeroProbe = mc2::gate::get<"MC2_GPU_CULL_STATIC_LIGHT_ZERO_PROBE">();
    static const bool s_staticEagerGate      = mc2::gate::get<"MC2_GPU_CULL_STATIC_EAGER_LIGHT_BAKE">();
    if (s_staticLightZeroProbe && (s_diag_flush_calls % 300) == 0) {
        uint32_t aliveProps = 0u, zeroLightSlots = 0u;
        for (int32_t ri = 0; ri < static_cast<int32_t>(s_recipeRanges.size()); ++ri) {
            const RecipeRange& r = s_recipeRanges[static_cast<size_t>(ri)];
            if (r.count > 0u) {
                ++aliveProps;
                // recipeIndex == position in s_recipeRanges == permanent light slot index
                // (LIGHTBAKE v2 invariant: lightData_[recipeIndex] is the permanent slot).
                if (!mc2IsStaticLightSlotBaked(ri)) ++zeroLightSlots;
            }
        }
        fprintf(stderr,
            "[STATIC_LIGHT_ZERO v1] frame=%llu aliveProps=%u zeroLightSlots=%u eager=%d\n",
            (unsigned long long)s_diag_flush_calls, aliveProps, zeroLightSlots,
            s_staticEagerGate ? 1 : 0);
        fflush(stderr);

        // PROP-RECIPE-BAKE-1: per-recipe row dump, ONCE, under the same
        // already-registered gate (no new env var). The aggregate counter above
        // cannot distinguish "this truck is dark" from "this building is lit" --
        // this does, by dumping what static_prop.vert actually reads per recipe:
        //   baked   = slot ever written (0 => all-zero row => black actor)
        //   nL      = numLights_ in the permanent slot
        //   amb/sun = the shader's orientation-free vs NdotL-scaled terms
        //             (mc2PeekStaticLightSlotSummary, mirrors static_prop.vert:720)
        //   pos     = model-matrix translation -- the only reliable per-instance
        //             identifier, since shapeName is populated ONLY by the
        //             late-spawn path (registerStaticPropAndReturnRecipe:480);
        //             mission-load bulk registerRecipe() leaves it empty.
        // Read-only: no light data is written, no render state touched.
        static bool s_rowDumpDone = false;
        if (!s_rowDumpDone) {
            s_rowDumpDone = true;
            for (int32_t ri = 0; ri < static_cast<int32_t>(s_recipeRanges.size()); ++ri) {
                const RecipeRange& r = s_recipeRanges[static_cast<size_t>(ri)];
                if (r.count == 0u) continue;   // tombstone
                int   nL = 0;
                float amb[3] = {0,0,0}, sun[3] = {0,0,0};
                const bool baked = mc2PeekStaticLightSlotSummary(ri, &nL, amb, sun);
                uint32_t typeID = 0u;
                if (r.first < s_recipes.size()) typeID = s_recipes[r.first].typeID;
                float m[16] = {0};
                const bool haveM = staticPropGetModelMatrix(ri, m);
                const char* tname = "-";
                int alphaClass = -1;
                mc2PeekStaticPropTypeInfo(typeID, &tname, &alphaClass);
                fprintf(stderr,
                    "[PROPROW v1] ri=%d type=%u tname=%s alpha=%d leaves=%u name=%s "
                    "ldi=%u baked=%d nL=%d "
                    "amb=%.4f,%.4f,%.4f sun=%.4f,%.4f,%.4f pos=%.1f,%.1f,%.1f\n",
                    ri, typeID, tname, alphaClass, r.count,
                    (r.shapeName[0] ? r.shapeName : "-"),
                    r.lightDataIndex, baked ? 1 : 0, nL,
                    amb[0], amb[1], amb[2], sun[0], sun[1], sun[2],
                    haveM ? m[12] : 0.0f, haveM ? m[13] : 0.0f, haveM ? m[14] : 0.0f);
            }
            fflush(stderr);
        }
    }
}

// Print the per-call flush counters and the stale-drop discriminator.
void flushDiagSummary(uint64_t s_diag_flush_calls, uint64_t& s_diag_ranges_total, uint64_t& s_diag_ranges_tombstone, uint64_t& s_diag_ranges_stale_frame, uint64_t& s_diag_ranges_stale_after_drawn, uint64_t& s_diag_ranges_drawn, uint64_t& s_diag_leaves_appended, uint64_t& s_diag_persistent_vanish, uint64_t s_diag_total_ns, bool s_guard2, std::unordered_map<int, uint64_t>& s_staleByType, std::unordered_map<uint32_t, uint32_t>& s_staleStreak) {
    static const bool s_regflushTrace = mc2::gate::get<"MC2_REGFLUSH_DIAG_TRACE">();
    if (s_regflushTrace && (s_diag_flush_calls % 600) == 0) {
        const double mean_us = (s_diag_flush_calls > 0)
            ? (static_cast<double>(s_diag_total_ns) /
               static_cast<double>(s_diag_flush_calls)) / 1000.0
            : 0.0;
        fprintf(stderr,
            "[REGFLUSH_DIAG v1] event=summary calls=%llu ranges_seen=%llu "
            "tombstone=%llu stale_frame=%llu stale_after_drawn=%llu drawn=%llu leaves_appended=%llu liveSize=%zu "
            "mean_us=%.2f\n",
            (unsigned long long)s_diag_flush_calls,
            (unsigned long long)s_diag_ranges_total,
            (unsigned long long)s_diag_ranges_tombstone,
            (unsigned long long)s_diag_ranges_stale_frame,
            (unsigned long long)s_diag_ranges_stale_after_drawn,
            (unsigned long long)s_diag_ranges_drawn,
            (unsigned long long)s_diag_leaves_appended,
            s_liveRangeIndices.size(), mean_us);
        fflush(stderr);
        // GUARD-2 addendum: persistent-vanish count + worst stale typeID. A nonzero
        // persistent_vanish (or a single typeID dominating stale_by_type) is the
        // actionable on-screen-vanish signal; transient off-screen noise stays low.
        if (s_guard2) {
            int worstType = -1; uint64_t worstCount = 0;
            for (const auto& kv : s_staleByType)
                if (kv.second > worstCount) { worstCount = kv.second; worstType = kv.first; }
            fprintf(stderr,
                "[REGFLUSH_GUARD2 v1] event=summary persistent_vanish=%llu "
                "stale_types=%zu worst_typeID=%d worst_stale_total=%llu live_streaks=%zu\n",
                (unsigned long long)s_diag_persistent_vanish,
                s_staleByType.size(), worstType, (unsigned long long)worstCount,
                s_staleStreak.size());
            fflush(stderr);
        }
    }
}

// Emit the per-10-frame cost-split summary and reset its window.
void flushSpflushWindow() {
    // [SPFLUSH_COST_SPLIT v1] -- per-10-frame summary emit.
    // s_win_leaves_appended / s_win_ranges_drawn are incremented directly in the
    // leaf loop above (alongside the existing s_diag_* counters). Window accumulators
    // are reset after each emit, so they hold the current window-period totals.
    if (s_spflushEnabled) {
        if (!s_spflushCalibrated) spflushCalibrate();
        ++s_spflushWindowFrames;
        if (s_spflushWindowFrames >= 10) {
            // Consume batcher-side callee accumulators (read+reset).
            const unsigned long long map_lookup_cyc  = spflush_cost_split::ConsumeSubmitMapLookupCycles();
            const unsigned long long color_fill_cyc  = spflush_cost_split::ConsumeColorZeroFillCycles();
            // Consume txmmgr-side accumulators (file-scope externs declared below namespace).
            const unsigned long long bi_upload_cyc       = ::spflush_ConsumeBaseInstanceUploadCycles();
            const unsigned long long win_recipe_rebuilds = ::spflush_ConsumeRecipeRebuildsDelta();
            const unsigned long long tot_recipe_rebuilds = ::spflush_GetRecipeRebuildTotal();
            // Convert cycles -> ns using the calibrated cycles_per_ns.
            const double cpns = s_spflushCyclesPerNs;
            const double wf   = static_cast<double>(s_spflushWindowFrames);
            auto cyc2ns = [&](unsigned long long c) -> long long {
                return static_cast<long long>(static_cast<double>(c) / cpns / wf);
            };
            // Per-frame leaf/range averages (window totals / frames).
            const long long leaves_pf  = static_cast<long long>(s_win_leaves_appended / static_cast<unsigned long long>(s_spflushWindowFrames));
            const long long ranges_pf  = static_cast<long long>(s_win_ranges_drawn    / static_cast<unsigned long long>(s_spflushWindowFrames));
            fprintf(stderr,
                "[SPFLUSH_COST_SPLIT v1] event=summary frames=10 "
                "leaves=%lld ranges=%lld "
                "submit_loop_ns=%lld inst_build_ns=%lld "
                "map_lookup_ns=%lld color_fill_ns=%lld "
                "actor_record_ns=%lld world_to_block_ns=%lld "
                "substrate_append_ns=%lld baseinstance_upload_ns=%lld "
                "| CACHED: total_ns=%lld submit_ns=%lld records_ns=%lld builds_pf=%lld hits_pf=%lld "
                "| dirty(window): invalidates=%llu registrations=%llu rebuilds=%llu light_writes=%llu"
                " | dirty(total): invalidates=%llu registrations=%llu rebuilds=%llu light_writes=%llu\n",
                leaves_pf, ranges_pf,
                cyc2ns(s_w_submit_loop_total_cyc),
                cyc2ns(s_w_inst_build_cyc),
                cyc2ns(map_lookup_cyc),
                cyc2ns(color_fill_cyc),
                cyc2ns(s_w_actor_record_build_cyc),
                cyc2ns(s_w_world_to_block_idx_cyc),
                cyc2ns(s_w_substrate_append_cyc),
                cyc2ns(bi_upload_cyc),
                cyc2ns(s_w_cached_total_cyc),
                cyc2ns(s_w_cached_submit_cyc),
                cyc2ns(s_w_cached_records_cyc),
                static_cast<long long>(s_win_cache_builds / static_cast<unsigned long long>(s_spflushWindowFrames)),
                static_cast<long long>(s_win_cache_hits   / static_cast<unsigned long long>(s_spflushWindowFrames)),
                s_win_invalidates, s_win_registrations, win_recipe_rebuilds, win_recipe_rebuilds,
                s_total_invalidates, s_total_registrations, tot_recipe_rebuilds, tot_recipe_rebuilds);
            fflush(stderr);
            if (mc2_diag::tagEnabled("SPFLUSH_COST_SPLIT")) {
                char diag_buf[2048];
                snprintf(diag_buf, sizeof(diag_buf),
                    "{\"event\":\"summary\",\"frame\":%u,\"window_frames\":%d,"
                    "\"leaves\":%lld,\"ranges\":%lld,"
                    "\"submit_loop_ns\":%lld,\"inst_build_ns\":%lld,"
                    "\"map_lookup_ns\":%lld,\"color_fill_ns\":%lld,"
                    "\"actor_record_ns\":%lld,\"world_to_block_ns\":%lld,"
                    "\"substrate_append_ns\":%lld,\"baseinstance_upload_ns\":%lld,"
                    "\"cached_total_ns\":%lld,\"cached_submit_ns\":%lld,\"cached_records_ns\":%lld,"
                    "\"win_cache_builds\":%lld,\"win_cache_hits\":%lld,"
                    "\"win_invalidates\":%llu,\"win_registrations\":%llu,"
                    "\"win_recipe_rebuilds\":%llu,"
                    "\"tot_invalidates\":%llu,\"tot_registrations\":%llu,"
                    "\"tot_recipe_rebuilds\":%llu}",
                    g_mc2FrameCounter, s_spflushWindowFrames,
                    leaves_pf, ranges_pf,
                    cyc2ns(s_w_submit_loop_total_cyc), cyc2ns(s_w_inst_build_cyc),
                    cyc2ns(map_lookup_cyc), cyc2ns(color_fill_cyc),
                    cyc2ns(s_w_actor_record_build_cyc), cyc2ns(s_w_world_to_block_idx_cyc),
                    cyc2ns(s_w_substrate_append_cyc), cyc2ns(bi_upload_cyc),
                    cyc2ns(s_w_cached_total_cyc), cyc2ns(s_w_cached_submit_cyc),
                    cyc2ns(s_w_cached_records_cyc),
                    static_cast<long long>(s_win_cache_builds / static_cast<unsigned long long>(s_spflushWindowFrames)),
                    static_cast<long long>(s_win_cache_hits   / static_cast<unsigned long long>(s_spflushWindowFrames)),
                    s_win_invalidates, s_win_registrations, win_recipe_rebuilds,
                    s_total_invalidates, s_total_registrations, tot_recipe_rebuilds);
                mc2_diag::writeEvent("SPFLUSH_COST_SPLIT", 1, g_mc2FrameCounter, diag_buf);
            }
            // Reset window accumulators.
            s_w_submit_loop_total_cyc  = 0;
            s_w_inst_build_cyc         = 0;
            s_w_actor_record_build_cyc = 0;
            s_w_world_to_block_idx_cyc = 0;
            s_w_substrate_append_cyc   = 0;
            s_win_leaves_appended      = 0;
            s_win_ranges_drawn         = 0;
            s_w_cached_total_cyc       = 0;
            s_w_cached_submit_cyc      = 0;
            s_w_cached_records_cyc     = 0;
            s_win_cache_builds         = 0;
            s_win_cache_hits           = 0;
            s_win_invalidates          = 0;
            s_win_registrations        = 0;
            // s_win_recipe_rebuilds reset inside spflush_ConsumeRecipeRebuildsDelta() (txmmgr).
            s_spflushWindowFrames      = 0;
        }
    }
}

// Bump the cull-record version when the submitted prop set changed.
void flushCullRecordVersion() {
    // STATICPROP-SNAPSHOT-BRIDGE-COMPARE-1: bump s_cullRecordVersion only when the
    // set of props submitted to the GPU cull substrate changed vs the previous flush.
    // In a stable scene (same props visible each frame), this stays constant after warmup.
    {
        bool changed = (s_prevCullRecord.size() != s_recipeHasSubstrateRecord.size());
        if (!changed) {
            for (size_t ci = 0; ci < s_recipeHasSubstrateRecord.size(); ++ci) {
                if (s_prevCullRecord[ci] != s_recipeHasSubstrateRecord[ci]) { changed = true; break; }
            }
        }
        if (changed) {
            ++s_cullRecordVersion;
            s_prevCullRecord = s_recipeHasSubstrateRecord;
        }
    }
}

}  // namespace GpuStaticPropRegistry
