// GameOS/gameos/draw_packet_emitter.cpp
//
// DrawPacket v1 — static-prop packet emitter (type-table walk).
// Spec: docs/superpowers/plans/2026-05-25-draw-packet-v1-type-table.md
//
// NOTE on index space: Phase 2 (v1) iterates by desc.firstPacket..firstPacket+packetCount
// from the type desc table, accessing s_packets[] via batcher_getPacketDrawInfo().
// instanceCounts[] is indexed by typeId (dense, 0..maxTypeId).

#include "draw_packet_emitter.h"
#include "gos_static_prop_batcher.h"   // batcher_getPacketDrawInfo
#include "../../RenderCore/StaticPropTypeDesc.h"  // v1: type table walk

#include <vector>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "Gates/gate.h"
#include "mc2fmt.h"
// <climits> not needed: UINT32_MAX comes from <cstdint> already included via draw_packet_emitter.h

// Global debug snapshot — written each frame by gameosmain, read by ImGui panel.
DrawPacketsDebugSnapshot g_dpSnapshot;

// Per-selected-prop snapshot — written by gameosmain when a static prop is selected.
DrawPacketSelectedPropSnapshot g_dpSelProp;

// Selection bridge — written by EditorInspector on pick/clear events.
int32_t g_dpSelectedRecipeIndex = -1;

DrawPacketEmitStats emitStaticPropDrawPackets(const RenderSnapshot&          snap,
                                              StaticPropDrawPacketCandidate* out,
                                              uint32_t                       maxPackets) {
    DrawPacketEmitStats stats{};

    if (snap.arenaOverflow || snap.staticProps.empty() || !out || maxPackets == 0)
        return stats;

    const size_t propCount = snap.staticProps.size();

    // -----------------------------------------------------------------------
    // Phase 1: find maxTypeId, then tally instances per typeId.
    // -----------------------------------------------------------------------
    uint32_t maxTypeId = 0u;
    for (size_t i = 0; i < propCount; ++i) {
        const uint32_t tid = snap.staticProps.data[i].typeId;
        if (tid > maxTypeId) maxTypeId = tid;
    }

    // [DRAW_PACKET v1] WARNING family is default-SILENT (set MC2_DRAW_PACKET_WARN=1 to
    // re-enable). These can fire per-type per-frame (e.g. packetCount=0 skip) and were
    // spamming the player console. Counters (stats.*) still increment; only the prints
    // are gated. ERRORs (genuine aborts) stay unconditional.
    static const bool s_dpWarn = false;

    // Guard against sparse or corrupt typeId values.
    static constexpr uint32_t kMaxAllowedTypeId = 65535u;
    if (maxTypeId > kMaxAllowedTypeId) {
        mc2::fmt::print_err("[DRAW_PACKET v1] ERROR: maxTypeId={:d} exceeds cap {:d}; aborting\n", mc2::fmt::as_u32(maxTypeId), mc2::fmt::as_u32(kMaxAllowedTypeId));
        return stats;
    }

    std::vector<uint32_t> instanceCounts(maxTypeId + 1u, 0u);
    for (size_t i = 0; i < propCount; ++i)
        ++instanceCounts[snap.staticProps.data[i].typeId];

    // -----------------------------------------------------------------------
    // Phase 1b: collect per-type materialIdx from first-seen snapshot instance.
    // alphaClass/firstPacket/packetCount now come from the type table (v1).
    // materialIdx is still snapshot-sourced (not yet in StaticPropTypeDesc).
    // materialMismatches warns when two instances of the same type disagree —
    // expected 0 while materialIdx is effectively per-type.
    // -----------------------------------------------------------------------
    std::vector<uint32_t> typeToMatIdx(maxTypeId + 1u, 0xFFFFFFFFu);
    for (size_t i = 0; i < propCount; ++i) {
        const ExtractedStaticProp& prop = snap.staticProps.data[i];
        const uint32_t tid = prop.typeId;
        if (tid > maxTypeId) continue;
        if (prop.materialIdx == 0xFFFFFFFFu) continue;
        if (typeToMatIdx[tid] == 0xFFFFFFFFu) {
            typeToMatIdx[tid] = prop.materialIdx;
        } else if (typeToMatIdx[tid] != prop.materialIdx) {
            ++stats.materialMismatches;
            if (s_dpWarn) mc2::fmt::print_err("[DRAW_PACKET v1] WARNING: typeId={:d} materialIdx mismatch ({:d} vs {:d})\n", mc2::fmt::as_u32(tid), mc2::fmt::as_u32(typeToMatIdx[tid]), mc2::fmt::as_u32(prop.materialIdx));
        }
    }

    // -----------------------------------------------------------------------
    // Cross-check: compute oldExpected from the representative snapshot scan.
    // Equivalence: oldExpected == expectedPackets if and only if, for every
    // typeId visible this frame, prop.packetCount matches desc.packetCount.
    // Both paths apply the same sentinel + overflow guards so that a skipped
    // type is excluded from both sums.
    // -----------------------------------------------------------------------
    {
        std::vector<uint8_t> seenCheck(maxTypeId + 1u, 0u);
        for (size_t i = 0; i < propCount; ++i) {
            const ExtractedStaticProp& prop = snap.staticProps.data[i];
            const uint32_t tid = prop.typeId;
            if (tid > maxTypeId || seenCheck[tid]) continue;
            seenCheck[tid] = 1u;
            // Mirror the same validity guards as the new path.
            if (prop.packetCount == 0u || prop.firstPacket == 0xFFFFFFFFu)
                continue;
            if (prop.firstPacket > UINT32_MAX - prop.packetCount)
                continue;
            stats.oldExpected += prop.packetCount;
        }
    }

    // -----------------------------------------------------------------------
    // Phase 2 (v1): type-table walk.
    // firstPacket/packetCount/alphaClass from StaticPropTypeDesc[].
    // instanceCount from Phase 1 instanceCounts[].
    // materialIdx from Phase 1b typeToMatIdx[].
    // -----------------------------------------------------------------------
    uint32_t typeCount = 0u;
    const RenderCore::StaticPropTypeDesc* descs =
        batcher_getStaticPropTypeDescTable(&typeCount);
    if (!descs || typeCount == 0u) {
        // Benign during map/mission teardown: the batcher type table is cleared on
        // mission end, but the render snapshot can still carry stale prop rows for a few
        // frames (cache invalidation lags), producing props-present + empty-table here.
        // The emit correctly aborts regardless; gate the print behind the warn flag so it
        // doesn't spam the player console once-per-frame on the post-mission screens.
        if (s_dpWarn)
            mc2::fmt::print_err("[DRAW_PACKET v1] ERROR: type table empty; emit aborted\n");
        return stats;
    }

    for (uint32_t i = 0u; i < typeCount; ++i) {
        const RenderCore::StaticPropTypeDesc& desc = descs[i];

        // Validate dense indexing: the type table is populated as desc.typeId == i.
        // A mismatch indicates a populate bug and makes the candidate's typeId wrong.
        if (desc.typeId != i) {
            ++stats.invalidRanges;
            if (s_dpWarn) mc2::fmt::print_err("[DRAW_PACKET v1] WARNING: type table row mismatch row={:d} typeId={:d}\n", mc2::fmt::as_u32(i), mc2::fmt::as_u32(desc.typeId));
            continue;
        }

        const uint32_t tid = desc.typeId;

        // Skip types with no instances this frame.
        // tid may exceed maxTypeId if a registered type has zero snapshot entries.
        if (tid > maxTypeId || instanceCounts[tid] == 0u) continue;
        ++stats.distinctTypes;

        // packetCount==0 means a malformed slot; log and skip.
        if (desc.packetCount == 0u) {
            ++stats.skippedRanges;
            if (s_dpWarn) mc2::fmt::print_err("[DRAW_PACKET v1] WARNING: typeId={:d} skipped (packetCount=0)\n", mc2::fmt::as_u32(tid));
            continue;
        }
        // Overflow guard on firstPacket + packetCount arithmetic.
        if (desc.firstPacket > UINT32_MAX - desc.packetCount) {
            ++stats.skippedRanges;
            if (s_dpWarn) mc2::fmt::print_err("[DRAW_PACKET v1] WARNING: typeId={:d} packet range overflow (firstPacket={:d} packetCount={:d})\n", mc2::fmt::as_u32(tid), mc2::fmt::as_u32(desc.firstPacket), mc2::fmt::as_u32(desc.packetCount));
            continue;
        }

        stats.expectedPackets += desc.packetCount;

        // materialIdx from Phase 1b (still per-type, not yet in StaticPropTypeDesc).
        // generation=0 debug wrapper only; not a validated MaterialRegistry handle.
        RenderCore::MaterialHandle mat = RenderCore::MaterialHandle::invalid();
        if (tid <= maxTypeId && typeToMatIdx[tid] != 0xFFFFFFFFu)
            mat = RenderCore::MaterialHandle::make(typeToMatIdx[tid], 0u);

        const RenderCore::MeshHandle mesh = RenderCore::MeshHandle::invalid();

        const uint8_t  alphaPass = (desc.alphaClass == 1u) ? 8u : 0u;
        const uint16_t matHash   = mat.isValid()
                                     ? static_cast<uint16_t>(mat.bits & 0xFFFFu)
                                     : 0u;
        const uint64_t sortKey   = buildSortKey(alphaPass, 0u, 0u, matHash, 0u);
        const uint32_t ic        = instanceCounts[tid];

        const uint32_t pEnd = desc.firstPacket + desc.packetCount;
        for (uint32_t pi = desc.firstPacket; pi < pEnd; ++pi) {
            if (stats.emitted >= maxPackets) {
                stats.overflow = true;
                return stats;
            }

            uint32_t idxCount = 0u;
            uint32_t firstIdx = 0u;
            int32_t  baseVtx  = 0;
            uint32_t ownType  = 0u;
            if (!batcher_getPacketDrawInfo(pi, &idxCount, &firstIdx, &baseVtx, &ownType)) {
                ++stats.invalidRanges;
                continue;
            }
            (void)baseVtx;
            (void)ownType;

            // v2: per-packet pipeline ID from material flags.
            // No assert — diagnostic slice must not abort release smoke.
            // OOB pi is a bug in the emitter; pipelineId=Invalid lets compare count it.
            uint32_t packetMatFlags = 0u;
            RenderCore::PipelineId packetPipelineId = RenderCore::PipelineId::Invalid;
            if (!batcher_getPacketMaterialFlags(pi, &packetMatFlags)) {
                mc2::fmt::print_err("[DRAW_PACKET v2] batcher_getPacketMaterialFlags OOB pi={:d}\n", mc2::fmt::as_u32(pi));
                packetMatFlags = 0u;
            } else {
                const bool alpha = (packetMatFlags & STATIC_PROP_FLAG_ALPHA_TEST) != 0;
                packetPipelineId = alpha
                    ? RenderCore::PipelineId::StaticPropAlphaTest
                    : RenderCore::PipelineId::StaticPropOpaque;
            }

            // stats.emitted is both the write index and the running count.
            // The overflow guard above ensures emitted < maxPackets here.
            out[stats.emitted++] = StaticPropDrawPacketCandidate{
                tid,              // typeId
                pi,               // globalPacketIdx
                ic,               // instanceCount
                firstIdx,         // firstIndex
                idxCount,         // indexCount
                mat,              // material (generation=0 debug wrapper)
                mesh,             // mesh (invalid v0 stub)
                alphaPass,        // alphaPass (transitional — from desc.alphaClass)
                sortKey,          // sortKey
                packetPipelineId, // pipelineId (v2 — per-packet, from materialFlags)
                packetMatFlags    // cachedMaterialFlags (v2 — cached for compare round-trip)
            };
        }
    }

    return stats;
}

// ---------------------------------------------------------------------------
// DrawPacket v2 — compare
// ---------------------------------------------------------------------------

// s_compareEnabled lives at the CALL SITE (Task 5), not here — this function is only
// invoked when that gate fires. s_verboseEnabled lives here because it is read inside
// the loop; evaluating getenv() per candidate would be a syscall loop.
static const bool s_verboseEnabled = false;

DrawPacketCompareResult comparePacketsToLegacy(
    const StaticPropDrawPacketCandidate* candidates,
    uint32_t                             count,
    uint32_t                             frameIndex) {

    DrawPacketCompareResult r{};
    r.packets = count;

    for (uint32_t i = 0u; i < count; ++i) {
        const StaticPropDrawPacketCandidate& c = candidates[i];
        const RenderCore::PipelineId id = c.pipelineId;

        // 1. Invalid: accessor failed during assignment.
        if (id == RenderCore::PipelineId::Invalid) {
            ++r.pipelineInvalid;
            continue;
        }

        // 2. OOB: pipelineId outside valid range [1, Count_); count as anomaly.
        // getPipelineDesc() returns a zeroed sentinel for OOB rather than asserting,
        // but the guard is kept to isolate pipelineOob from pipelineInvalid (0)
        // and to give a clean diagnostic counter independent of accessor behavior.
        if (static_cast<uint32_t>(id) >=
            static_cast<uint32_t>(RenderCore::PipelineId::Count_)) {
            ++r.pipelineOob;
            continue;
        }

        // safe to call getPipelineDesc(); result unused here — just validates no assert.
        (void)RenderCore::getPipelineDesc(id);

        // 3 + 4. Geometry + owning-type check via batcher_getPacketDrawInfo.
        {
            uint32_t bIdxCount = 0u, bFirstIdx = 0u;
            int32_t  bBaseVtx  = 0;
            uint32_t bOwnType  = 0u;
            if (batcher_getPacketDrawInfo(c.globalPacketIdx,
                                          &bIdxCount, &bFirstIdx,
                                          &bBaseVtx,  &bOwnType)) {
                if (c.firstIndex != bFirstIdx || c.indexCount != bIdxCount) {
                    ++r.geomMismatch;
                    if (s_verboseEnabled) {
                        if (c.firstIndex != bFirstIdx)
                            mc2::fmt::print_err("[DRAW_PACKET_COMPARE detail] typeId={:d} globalIdx={:d} check=firstIndex cand={:d} batcher={:d}\n", mc2::fmt::as_u32(c.typeId), mc2::fmt::as_u32(c.globalPacketIdx), mc2::fmt::as_u32(c.firstIndex), mc2::fmt::as_u32(bFirstIdx));
                        if (c.indexCount != bIdxCount)
                            mc2::fmt::print_err("[DRAW_PACKET_COMPARE detail] typeId={:d} globalIdx={:d} check=indexCount cand={:d} batcher={:d}\n", mc2::fmt::as_u32(c.typeId), mc2::fmt::as_u32(c.globalPacketIdx), mc2::fmt::as_u32(c.indexCount), mc2::fmt::as_u32(bIdxCount));
                    }
                }
                if (c.typeId != bOwnType) {
                    ++r.typeMismatch;
                    if (s_verboseEnabled)
                        mc2::fmt::print_err("[DRAW_PACKET_COMPARE detail] typeId={:d} globalIdx={:d} check=owningType cand={:d} batcher={:d}\n", mc2::fmt::as_u32(c.typeId), mc2::fmt::as_u32(c.globalPacketIdx), mc2::fmt::as_u32(c.typeId), mc2::fmt::as_u32(bOwnType));
                }
            }
            // if batcher_getPacketDrawInfo returns false: globalPacketIdx was valid at
            // emit (emitter increments invalidRanges otherwise), so a false here means
            // a batcher state change mid-frame. Skip silently — not a candidate bug.
        }

        // 5. Alpha self-consistency: pipelineId vs cachedMaterialFlags.
        // This is a round-trip check of the assignment logic, NOT independent corroboration
        // (both values come from the same batcher_getPacketMaterialFlags call at emit time).
        {
            const bool pipelineIsAlpha = (id == RenderCore::PipelineId::StaticPropAlphaTest);
            const bool flagsIsAlpha =
                (c.cachedMaterialFlags & STATIC_PROP_FLAG_ALPHA_TEST) != 0;
            if (pipelineIsAlpha != flagsIsAlpha) {
                ++r.alphaMismatch;
                if (s_verboseEnabled)
                    mc2::fmt::print_err("[DRAW_PACKET_COMPARE detail] typeId={:d} globalIdx={:d} check=alpha pipelineId={:d} flags=0x{:08x}\n", mc2::fmt::as_u32(c.typeId), mc2::fmt::as_u32(c.globalPacketIdx), mc2::fmt::as_u32(static_cast<uint32_t>(id)), mc2::fmt::as_u32(c.cachedMaterialFlags));
            }
        }
    }

    // One summary line per frame — schema v1.
    // INCREMENT TAG if fields are added/removed in future slices.
    mc2::fmt::print_err("[DRAW_PACKET_COMPARE v1] frame={:d} packets={:d} pipeline_invalid={:d} pipeline_oob={:d} geom_mismatch={:d} type_mismatch={:d} alpha_mismatch={:d}\n",
        mc2::fmt::as_u32(frameIndex), mc2::fmt::as_u32(r.packets),
        mc2::fmt::as_u32(r.pipelineInvalid), mc2::fmt::as_u32(r.pipelineOob),
        mc2::fmt::as_u32(r.geomMismatch), mc2::fmt::as_u32(r.typeMismatch), mc2::fmt::as_u32(r.alphaMismatch));

    return r;
}

// ---------------------------------------------------------------------------
// DrawPacket v3 — candidate → RenderCore::DrawPacket ABI promotion.
// ---------------------------------------------------------------------------

DrawPacketBuildStats buildStaticPropDrawPackets(
    const StaticPropDrawPacketCandidate* candidates,
    uint32_t                             candidateCount,
    RenderCore::DrawPacket*              out,
    uint32_t                             maxPackets)
{
    DrawPacketBuildStats stats{};
    stats.inputCandidates = candidateCount;

    for (uint32_t i = 0; i < candidateCount; ++i) {
        if (stats.emitted >= maxPackets) { ++stats.overflow; continue; }

        const auto& c  = candidates[i];
        const RenderCore::PipelineId id = c.pipelineId;

        if (id == RenderCore::PipelineId::Invalid) {
            ++stats.invalidPipeline; continue;
        }
        if (static_cast<uint32_t>(id) >= static_cast<uint32_t>(RenderCore::PipelineId::Count_)) {
            ++stats.pipelineOutOfRange; continue;
        }
        // Prove descriptor lookup is safe for in-range IDs; (void) avoids unused-variable warning.
        // getPipelineDesc() asserts on Invalid/OOB — both are already excluded above.
        { const RenderCore::PipelineDesc& desc = RenderCore::getPipelineDesc(id); (void)desc; }
        if (c.indexCount == 0) {
            ++stats.invalidIndexCount; continue;
        }
        if (c.instanceCount == 0) {
            ++stats.invalidInstanceCount; continue;
        }

        RenderCore::DrawPacket& p = out[stats.emitted++];
        p.pipelineId    = static_cast<uint32_t>(id);
        p.mesh          = c.mesh;
        p.material      = c.material;
        p.objectIndex   = 0xFFFFFFFFu;   // sentinel: no per-object identity at type-packet level
        p.lightIndex    = 0xFFFFFFFFu;   // sentinel: no per-packet light index yet
        p.firstIndex    = c.firstIndex;
        p.indexCount    = c.indexCount;
        p.instanceCount = c.instanceCount;
        p.sortKey       = c.sortKey;

        ++stats.objectIndexSentinelCount;
        ++stats.lightIndexSentinelCount;
    }

    return stats;
}
