// GpuStaticPropRegistry: the persistent-store rebuild and the post-loop phases of flush() -- store reinject, the
// diagnostic summaries and the cull-record version bump. The orchestrator is in gos_static_prop_registry.cpp.
#include "gos_static_prop_registry_internal.h"
#include "mc2fmt.h"

namespace GpuStaticPropRegistry {


// Rebuild the persistent static store and golden list from every registered recipe.
void flushFrozenStoreRebuild(GpuStaticPropBatcher& batcher_pb, bool s_storeDirty) {
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
            if (arng.count == 0u || !arng.multi()) continue;   // tombstone guard
            for (uint32_t i = 0u; i < arng.count; ++i) {
                const uint32_t ri = arng.first + i;
                if (ri >= s_recipes.size()) break;
                s_recipes[ri].lightDataIndex = aregIdx;   // permanent per-actor slot
            }
            batcher_pb.appendPersistentStaticRange(&s_recipes[arng.first], arng.count);
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

// Finalise the persistent static store and bulk-inject it into the per-type buckets.
void flushReinjectPersistentStore(GpuStaticPropBatcher& batcher_pb, uint64_t s_currentGen, bool s_storeDirty) {
    // 2b Stage 2 (Mechanism B-reinject): finalize the store on a dirty rebuild, then
    // bulk-inject the static blocks into s_bucketsByType every frame (one memcpy per
    // type). The downstream upload + coalesce pool read s_bucketsByType unchanged.
    if (s_storeDirty) batcher_pb.setPersistentStaticGen(s_currentGen);
    batcher_pb.reinjectPersistentStatic();
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
        mc2::fmt::print_err("[STATIC_LIGHT_ZERO v1] frame={:d} aliveProps={:d} zeroLightSlots={:d} eager={:d}\n", mc2::fmt::as_u64(static_cast<unsigned long long>(s_diag_flush_calls)), mc2::fmt::as_u32(aliveProps), mc2::fmt::as_u32(zeroLightSlots), mc2::fmt::as_i32(s_staticEagerGate ? 1 : 0));
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
                mc2::fmt::print_err("[PROPROW v1] ri={:d} type={:d} tname={} alpha={:d} leaves={:d} name={} ldi={:d} baked={:d} nL={:d} amb={:.4f},{:.4f},{:.4f} sun={:.4f},{:.4f},{:.4f} pos={:.1f},{:.1f},{:.1f}\n", mc2::fmt::as_i32(ri), mc2::fmt::as_u32(typeID), mc2::fmt::safe(tname), mc2::fmt::as_i32(alphaClass), mc2::fmt::as_u32(r.count), mc2::fmt::safe((r.shapeName[0] ? r.shapeName : "-")), mc2::fmt::as_u32(r.lightDataIndex), mc2::fmt::as_i32(baked ? 1 : 0), mc2::fmt::as_i32(nL), amb[0], amb[1], amb[2], sun[0], sun[1], sun[2], haveM ? m[12] : 0.0f, haveM ? m[13] : 0.0f, haveM ? m[14] : 0.0f);
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
        mc2::fmt::print_err("[REGFLUSH_DIAG v1] event=summary calls={:d} ranges_seen={:d} tombstone={:d} stale_frame={:d} stale_after_drawn={:d} drawn={:d} leaves_appended={:d} liveSize={:d} mean_us={:.2f}\n", mc2::fmt::as_u64(static_cast<unsigned long long>(s_diag_flush_calls)), mc2::fmt::as_u64(static_cast<unsigned long long>(s_diag_ranges_total)), mc2::fmt::as_u64(static_cast<unsigned long long>(s_diag_ranges_tombstone)), mc2::fmt::as_u64(static_cast<unsigned long long>(s_diag_ranges_stale_frame)), mc2::fmt::as_u64(static_cast<unsigned long long>(s_diag_ranges_stale_after_drawn)), mc2::fmt::as_u64(static_cast<unsigned long long>(s_diag_ranges_drawn)), mc2::fmt::as_u64(static_cast<unsigned long long>(s_diag_leaves_appended)), mc2::fmt::as_size(s_liveRangeIndices.size()), mean_us);
        fflush(stderr);
        // GUARD-2 addendum: persistent-vanish count + worst stale typeID. A nonzero
        // persistent_vanish (or a single typeID dominating stale_by_type) is the
        // actionable on-screen-vanish signal; transient off-screen noise stays low.
        if (s_guard2) {
            int worstType = -1; uint64_t worstCount = 0;
            for (const auto& kv : s_staleByType)
                if (kv.second > worstCount) { worstCount = kv.second; worstType = kv.first; }
            mc2::fmt::print_err("[REGFLUSH_GUARD2 v1] event=summary persistent_vanish={:d} stale_types={:d} worst_typeID={:d} worst_stale_total={:d} live_streaks={:d}\n", mc2::fmt::as_u64(static_cast<unsigned long long>(s_diag_persistent_vanish)), mc2::fmt::as_size(s_staleByType.size()), mc2::fmt::as_i32(worstType), mc2::fmt::as_u64(static_cast<unsigned long long>(worstCount)), mc2::fmt::as_size(s_staleStreak.size()));
            fflush(stderr);
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
