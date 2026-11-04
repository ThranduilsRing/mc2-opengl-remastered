// GameOS/gameos/render_snapshot.cpp
//
// Per-frame RenderSnapshot implementation.
// ExtractRenderSnapshot() populates the snapshot from RenderWorld and
// GpuStaticPropRegistry each frame (after sim, before render).
//
// v0: mech + light blocks (placeholder sentinel records; structs are empty in v0)
// v1: static-prop block (identity + transform, observational)
// v1.1: texArrayLayer/materialIdx wired from per-typeID primary material cache.
//
// Arena: three module-static buffers in BSS, wrapped by
// RenderCore::FrameArena instances. No per-frame heap allocation.

#include "render_snapshot.h"
#include "../../RenderCore/FramePackets.h"
#include "../../RenderWorld/RenderWorld.h"
#include "gos_static_prop_registry.h"
#include "gos_static_prop_batcher.h"
#include "gos_mech_batcher.h"  // MECH-EXTRACTION-0: batcher_getMechPendingCount/Entry/compareMechSnapshot
#include "gos_profiler.h"      // EXTRACT-SNAPSHOT-INSTRUMENT-1: ZoneScopedN (Tracy shim)
#ifdef MC2_IMGUI
#include "../../GuiRuntime/GraphicsOptionsWindow.h"
#include "draw_packet_emitter.h"
#endif

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include "Gates/gate.h"
#include "mc2fmt.h"
#include "../../mclib/sim_overlap.h"

// ---------------------------------------------------------------------------
// Module-static three-slot arenas (never freed).
// Each extraction resets its sequence's slot; the previous two extractions'
// data stays untouched. Explicit CPU leases prevent reuse until record completion.
// FrameArena instances are non-owning wrappers; actual buffers are s_arenaBuffers.
// ---------------------------------------------------------------------------

// STATICPROP-SNAPSHOT-FILL-DIRTYONLY-1: on clean registry/cull generations, skip
// fillStaticPropSlots + the per-prop WriteLoop and memcpy the cached rows into the
// frame arena instead. Gate: MC2_STATIC_PROP_SNAPSHOT_FILL_DIRTYONLY (default-ON
// after Tracy proof — ExtractRenderSnapshot ~1.68ms -> ~37us median, -97%;
// unset/=1 = dirty-only fast path, =0 = legacy full rebuild kill-switch).
static const bool s_dirtyOnlyEnabled = mc2::gate::get<"MC2_STATIC_PROP_SNAPSHOT_FILL_DIRTYONLY">();
// Production row cache (DIRTYONLY-1): a copy of the legacy propBuf rows from the last
// dirty frame. Clean frames memcpy these into the frame arena; snap.staticProps NEVER
// points at this persistent buffer directly.
static std::vector<ExtractedStaticProp> s_spRowCache;
static uint32_t s_cacheWriteIdx = 0u;          // valid rows in s_spRowCache
static bool     s_cacheValid    = false;       // false until first dirty rebuild
static uint64_t s_cacheRegGen   = UINT64_MAX;  // registryGeneration of cached rows
// DIRTYONLY-1: per-prop diagnostic counters captured on the dirty rebuild and restored
// verbatim on clean frames (the WriteLoop that computes them is skipped).
struct CachedSnapshotCounters {
    uint32_t validationFail, sentinelMat, sentinelCull, texWired, texSentinel,
             matWired, matSentinel, primaryAlphaOn, multiPacket,
             hasBounds, alphaOn, hasShapeName, packetRangesOk,
             packetRangeFail;
};
static CachedSnapshotCounters s_cachedCounters = {};
// Cover 65,536 native prop rows (mapgen budgets vegetation at 60,000),
// plus 1 MiB for draw-slot packets, mechs and alignment. The old 1 MiB
// total overflowed at ~9,500 props. Derive row storage from the actual ABI.
static constexpr size_t kArenaBytes = 65536u * sizeof(ExtractedStaticProp) + 1024u * 1024u;
alignas(16) static uint8_t s_arenaBuffers[RenderCore::FrameSlotArenas::kSlots][kArenaBytes];
static RenderCore::FrameSlotArenas s_slotArenas;   // slot = extraction sequence % 3
static bool                        s_arenasInited = false;

// ---------------------------------------------------------------------------
// v2.3: last-produced snapshot, valid for one full frame.
// Written by ExtractRenderSnapshot() just before return; read by getLastRenderSnapshot()
// which is called from flush() in the same frame. The Span pointers inside are backed
// by the three-slot arena; the latest descriptor is replaced at the next extraction.
// s_hasLastSnapshot is false until the first extraction completes; getLastRenderSnapshot()
// returns nullptr before that, so flush() on frame 1 safely gets null and skips snap-cull.
// ---------------------------------------------------------------------------
static RenderSnapshot s_lastSnapshot;
static bool           s_hasLastSnapshot = false;

const RenderSnapshot* getLastRenderSnapshot() {
    return s_hasLastSnapshot ? &s_lastSnapshot : nullptr;
}

void RetireRenderSnapshot(uint64_t packetSequence) {
    mc2::overlap::assertOwner(mc2::overlap::Owner::Main, "RetireRenderSnapshot");
    if (!packetSequence || !s_slotArenas.retire(packetSequence)) std::abort();
}

// ---------------------------------------------------------------------------
// ExtractRenderSnapshot
//
// Called once per frame between DoGameLogic() and draw_screen().
// Returns a snapshot whose Span pointers are backed by a module-static
// three-slot arena (snap.arena is non-owning). The spans survive two subsequent
// extractions; the third resets and reuses the same slot.
// ---------------------------------------------------------------------------
RenderSnapshot ExtractRenderSnapshot(uint64_t packetSequence)
{
    mc2::overlap::assertOwner(mc2::overlap::Owner::Main, "ExtractRenderSnapshot");
    static uint64_t s_frameIndex = 0;

    RenderSnapshot snap;
    snap.frameIndex    = ++s_frameIndex;
    snap.arenaOverflow = false;
    // Lazy init on first call (avoids static-init order dependency).
    if (!s_arenasInited) {
        s_slotArenas.init(s_arenaBuffers[0], s_arenaBuffers[1], s_arenaBuffers[2], kArenaBytes);
        s_arenasInited = true;
    }
    // The game loop keys both value arenas and deferred frees by this lease.
    const auto sequence = packetSequence ? packetSequence : s_frameIndex;
    if (packetSequence && !s_slotArenas.retain(sequence)) std::abort();
    snap.frameArena = s_slotArenas.begin(sequence);

    // -----------------------------------------------------------------------
    // v0: mech placeholder (legacy zero-size struct; kept for log line compat)
    // -----------------------------------------------------------------------
    snap.mechs = Span<ExtractedMech>(nullptr, 0);

    // -----------------------------------------------------------------------
    // MECH-EXTRACTION-0: per-actor mech snapshot
    // Gate: MC2_SNAPSHOT_MECH_EXTRACT=1 (default OFF).
    // Observational only — no GL mutation, no dispatch change.
    // materialIdx is 0xFFFFFFFFu sentinel in v0 (wired in Mech-1 follow-up).
    // -----------------------------------------------------------------------
    static const bool s_mechExtractEnabled = mc2::gate::get<"MC2_SNAPSHOT_MECH_EXTRACT">();
    snap.mechPackets = Span<ExtractedMechPacket>(nullptr, 0);
    if (s_mechExtractEnabled) {
        const uint32_t mechCount = batcher_getMechPendingCount();
        RenderCore::Span<ExtractedMechPacket> mechSpan =
            snap.frameArena.allocArray<ExtractedMechPacket>(mechCount, "ExtractedMechPacket");
        ExtractedMechPacket* mechBuf = mechSpan.data;
        if (!mechBuf && mechCount > 0u) {
            snap.arenaOverflow = true;
        } else {
            uint32_t wrote = 0u;
            for (uint32_t i = 0u; i < mechCount; ++i) {
                ExtractedMechPacket pkt{};
                if (batcher_getMechPendingEntry(i, &pkt)) {
                    mechBuf[wrote++] = pkt;
                    if (pkt.materialIdx != 0xFFFFFFFFu)
                        ++snap.mechMatValid;
                    else
                        ++snap.mechMatSentinel;
                }
            }
            snap.mechPackets       = Span<ExtractedMechPacket>(mechBuf, wrote);
            snap.mechSnapshotCount = wrote;
        }
        batcher_compareMechSnapshot(&snap);
    }

    // -----------------------------------------------------------------------
    // v0: light block
    // LightRecord is an empty struct placeholder in v0.
    // -----------------------------------------------------------------------
    snap.lights = Span<LightRecord>(nullptr, 0);

    // -----------------------------------------------------------------------
    // Extraction v1: static-prop snapshot
    // -----------------------------------------------------------------------
    // Native color/shadow dispatch consumes its own feeds. Exhaustive prop rows
    // serve diagnostics only; keep the small packet/summary records below live.
    static const bool propDiagnostics = mc2::gate::get<"MC2_DEBUG_STATE_DUMP">() ||
        false ||
        false ||
        false ||
        false;
    bool materializeProps = propDiagnostics;
#ifdef MC2_IMGUI
    materializeProps = materializeProps || GraphicsOptionsWindow::isOpen() || g_dpSelectedRecipeIndex >= 0;
#endif
    static bool reportedOmittedProps = false;
    if (!materializeProps && !reportedOmittedProps) {
        std::fprintf(stderr, "[RENDER_SNAPSHOT] static_prop_details=not_materialized (diagnostics/inspector only)\n");
        reportedOmittedProps = true;
    }
    if (materializeProps) {
        // DIRTYONLY-1: read generation signals up front to choose clean vs dirty path.
        const uint64_t curRegGen  = GpuStaticPropRegistry::getRegistryGeneration();
        const bool gensClean    = (curRegGen == s_cacheRegGen);
        const bool useCleanPath = s_dirtyOnlyEnabled && s_cacheValid && gensClean;

        if (useCleanPath) {
            // ---- CLEAN FAST PATH: skip Fill + WriteLoop; memcpy cached rows into arena. ----
            ZoneScopedN("Extract.SP.CleanCopy");
            const uint32_t n = s_cacheWriteIdx;
            RenderCore::Span<ExtractedStaticProp> propSpan =
                snap.frameArena.allocArray<ExtractedStaticProp>(n, "ExtractedStaticProp");
            if (!propSpan.data && n > 0u) {
                snap.arenaOverflow = true;
                mc2::fmt::print_err("[RENDER_SNAPSHOT] WARNING: arena overflow allocating {:d} ExtractedStaticProp (clean)\n", mc2::fmt::as_u32(n));
            } else {
                if (n) std::memcpy(propSpan.data, s_spRowCache.data(),
                                   static_cast<size_t>(n) * sizeof(ExtractedStaticProp));
                snap.staticProps              = Span<ExtractedStaticProp>(propSpan.data, n);
                // Restore the cached per-prop diagnostic counters verbatim (WriteLoop skipped).
                snap.staticPropValidationFail = s_cachedCounters.validationFail;
                snap.staticPropSentinelMat    = s_cachedCounters.sentinelMat;
                snap.staticPropSentinelCull   = s_cachedCounters.sentinelCull;
                snap.staticPropTexWired       = s_cachedCounters.texWired;
                snap.staticPropTexSentinel    = s_cachedCounters.texSentinel;
                snap.staticPropMatWired       = s_cachedCounters.matWired;
                snap.staticPropMatSentinel    = s_cachedCounters.matSentinel;
                snap.staticPropPrimaryAlphaOn = s_cachedCounters.primaryAlphaOn;
                snap.staticPropMultiPacket    = s_cachedCounters.multiPacket;
                snap.staticPropHasBounds        = s_cachedCounters.hasBounds;
                snap.staticPropAlphaOn          = s_cachedCounters.alphaOn;
                snap.staticPropHasShapeName     = s_cachedCounters.hasShapeName;
                snap.staticPropPacketRangesOk   = s_cachedCounters.packetRangesOk;
                snap.staticPropPacketRangesFail = s_cachedCounters.packetRangeFail;
            }
        } else {
        // ---- DIRTY / LEGACY PATH: full Fill + WriteLoop rebuild (also refreshes cache). ----
        // Temp views on the heap — NOT the frame arena (extraction scratch only).
        const uint32_t slotCount = RenderWorld::getStaticPropSlotCount();
        std::vector<RenderWorld::StaticPropRecordView> views;
        views.resize(slotCount);

        {
            // PERF-INSTRUMENT (EXTRACT-SNAPSHOT-INSTRUMENT-1): block-granularity zone.
            // Times the per-frame static-prop slot gather (RenderWorld -> views).
            ZoneScopedN("Extract.SP.Fill");
            const uint32_t total = RenderWorld::fillStaticPropSlots(views.data(), slotCount);

            // Handle growth between getStaticPropSlotCount() and fill (rare race).
            if (total > slotCount) {
                views.resize(total);
                const uint32_t total2 = RenderWorld::fillStaticPropSlots(views.data(), total);
                if (total2 > total) {
                    // Slot count grew again between retries (extremely unlikely, not worth looping).
                    // Process what we have; sp_fail will reflect any misses from the shortened view.
                    // The mismatch will appear as sp_vis_delta in the log.
                }
            }
        }

        // Count alive records for arena allocation.
        uint32_t aliveCount = 0;
        for (uint32_t i = 0; i < static_cast<uint32_t>(views.size()); ++i) {
            if (views[i].alive) ++aliveCount;
        }

        // Allocate from frame arena.
        RenderCore::Span<ExtractedStaticProp> propSpan =
            snap.frameArena.allocArray<ExtractedStaticProp>(aliveCount, "ExtractedStaticProp");
        ExtractedStaticProp* propBuf = propSpan.data;
        if (!propBuf && aliveCount > 0) {
            snap.arenaOverflow = true;
            mc2::fmt::print_err("[RENDER_SNAPSHOT] WARNING: arena overflow allocating {:d} ExtractedStaticProp\n",
                mc2::fmt::as_u32(aliveCount));
            // Leave staticProps empty; counters remain zero.
        } else {
            uint32_t writeIdx       = 0;
            uint32_t validationFail = 0;
            uint32_t sentinelMat    = 0;
            uint32_t sentinelCull   = 0;
            uint32_t texWired       = 0;
            uint32_t texSentinel    = 0;
            uint32_t matWired       = 0;
            uint32_t matSentinel    = 0;
            uint32_t primaryAlphaOn = 0;
            uint32_t multiPacket    = 0;

            uint32_t hasBounds        = 0;
            uint32_t alphaOn          = 0;
            uint32_t hasShapeName     = 0;
            uint32_t packetRangesOk   = 0;
            uint32_t packetRangeFail  = 0;

            {
            // PERF-INSTRUMENT (EXTRACT-SNAPSHOT-INSTRUMENT-1): block-granularity zone.
            // Times the per-alive-prop registry-gather write loop (~9 registry calls
            // + shapeName snprintf per prop). Suspected dominant ExtractRenderSnapshot cost.
            ZoneScopedN("Extract.SP.WriteLoop");
            for (uint32_t i = 0; i < static_cast<uint32_t>(views.size()); ++i) {
                const RenderWorld::StaticPropRecordView& v = views[i];
                if (!v.alive)           continue;
                if (!v.generationValid) { ++validationFail; continue; }  // belt-and-suspenders

                // Required field: model matrix.
                float mtx[16];
                if (!GpuStaticPropRegistry::staticPropGetModelMatrix(v.recipeIndex, mtx)) {
                    ++validationFail;
                    continue;
                }

                // Required field: typeId.
                uint32_t typeId = 0;
                if (!GpuStaticPropRegistry::staticPropGetTypeId(v.recipeIndex, &typeId)) {
                    ++validationFail;
                    continue;
                }

                // Required field: instanceFlags (v2).
                // staticPropGetInstanceFlags uses the same recipeValid() check as
                // staticPropGetModelMatrix above; failure here means the registry
                // entry was invalidated between the two calls (extremely unlikely).
                uint32_t instanceFlags = 0u;
                if (!GpuStaticPropRegistry::staticPropGetInstanceFlags(
                        v.recipeIndex, &instanceFlags)) {
                    ++validationFail;
                    continue;
                }

                ExtractedStaticProp& p = propBuf[writeIdx++];
                p.rwHandle    = v.handle;
                p.recipeIndex = v.recipeIndex;
                p.typeId      = typeId;
                p.instanceFlags = instanceFlags;
                std::memcpy(p.worldMatrix, mtx, sizeof(float) * 16);

                // Translation: row-major Stuff-space axis swap.
                // Must match gos_static_prop_registry.cpp:550-555.
                p.worldCenterX = -mtx[3];    // MC2 east
                p.worldCenterY =  mtx[11];  // MC2 north
                p.worldCenterZ =  mtx[7];   // MC2 elevation

                // Optional fields — failure keeps sentinel, no validation error.
                p.boundingRadius = 0.0f;
                GpuStaticPropRegistry::staticPropGetExtentRadius(v.recipeIndex, &p.boundingRadius);

                p.lightDataIndex = 0xFFFFFFFFu;
                GpuStaticPropRegistry::staticPropGetLightDataIndex(v.recipeIndex, &p.lightDataIndex);

                // v1.1 + v2: wire material cache fields (single call fills all).
                p.texArrayLayer = -1;
                p.materialIdx   = 0xFFFFFFFFu;
                p.alphaClass    = 0u;
                p.packetCount   = 0u;
                p.firstPacket   = 0xFFFFFFFFu;

                GpuStaticPropRegistry::StaticPropTypeMaterialCache matInfo{};
                if (GpuStaticPropRegistry::staticPropGetMaterialCacheInfo(
                        v.recipeIndex, &matInfo)) {
                    p.texArrayLayer = matInfo.texArrayLayer;
                    p.materialIdx   = matInfo.materialIdx;
                    p.alphaClass    = matInfo.alphaClass;    // v2
                    p.packetCount   = matInfo.packetCount;   // v2
                    p.firstPacket   = matInfo.firstPacket;   // v2
                    if (matInfo.primaryWasAlphaOn) ++primaryAlphaOn;
                    if (matInfo.multiPacket)       ++multiPacket;
                    // primaryWasAlphaOn and multiPacket are diagnostic counters, not ExtractedStaticProp fields;
                    // they are intentionally excluded from the per-row comparison.
                }

                // v2: shapeName — safe truncating copy.
                p.shapeName[0] = '\0';
                {
                    const char* sn = GpuStaticPropRegistry::getRecipeShapeName(v.recipeIndex);
                    if (sn) {
                        mc2::fmt::to_buf_n(p.shapeName, sizeof(p.shapeName), "{}", mc2::fmt::safe(sn));
                    }
                }

                // v1 counters (backward compat)
                if (p.materialIdx == 0xFFFFFFFFu) ++sentinelMat;
                ++sentinelCull;

                // v1.1 counters
                if (p.texArrayLayer != -1)        ++texWired;   else ++texSentinel;
                if (p.materialIdx != 0xFFFFFFFFu) ++matWired;   else ++matSentinel;

                // v2 counters
                if (p.boundingRadius > 0.0f)   ++hasBounds;
                if (p.alphaClass == 1u)         ++alphaOn;
                if (p.shapeName[0] != '\0')     ++hasShapeName;

                // v2: packet range validity — confirm sidecar covers this type's range.
                // Checks bounds via the last packet index; does NOT verify per-packet layer quality.
                if (p.packetCount > 0u && p.firstPacket != 0xFFFFFFFFu) {
                    int32_t dummy = -1;
                    if (batcher_getPacketTexArrayLayer(
                            p.firstPacket + p.packetCount - 1u, &dummy)) {
                        ++packetRangesOk;
                    } else {
                        ++packetRangeFail;
                    }
                }
            }
            }  // end Extract.SP.WriteLoop scope

            snap.staticProps              = Span<ExtractedStaticProp>(propBuf, writeIdx);
            snap.staticPropValidationFail = validationFail;
            snap.staticPropSentinelMat    = sentinelMat;
            snap.staticPropSentinelCull   = sentinelCull;
            snap.staticPropTexWired       = texWired;
            snap.staticPropTexSentinel    = texSentinel;
            snap.staticPropMatWired       = matWired;
            snap.staticPropMatSentinel    = matSentinel;
            snap.staticPropPrimaryAlphaOn = primaryAlphaOn;
            snap.staticPropMultiPacket    = multiPacket;
            snap.staticPropHasBounds        = hasBounds;
            snap.staticPropAlphaOn          = alphaOn;
            snap.staticPropHasShapeName     = hasShapeName;
            snap.staticPropPacketRangesOk   = packetRangesOk;
            snap.staticPropPacketRangesFail = packetRangeFail;

            // DIRTYONLY-1: snapshot this dirty frame's rows + counters into the persistent
            // cache so subsequent clean frames can memcpy them. Copy from propBuf (== the
            // independent oracle, proven by BRIDGE-COMPARE); never alias snap.staticProps.
            if (s_dirtyOnlyEnabled) {
                s_spRowCache.assign(propBuf, propBuf + writeIdx);
                s_cacheWriteIdx = writeIdx;
                s_cachedCounters.validationFail  = validationFail;
                s_cachedCounters.sentinelMat     = sentinelMat;
                s_cachedCounters.sentinelCull    = sentinelCull;
                s_cachedCounters.texWired        = texWired;
                s_cachedCounters.texSentinel     = texSentinel;
                s_cachedCounters.matWired        = matWired;
                s_cachedCounters.matSentinel     = matSentinel;
                s_cachedCounters.primaryAlphaOn  = primaryAlphaOn;
                s_cachedCounters.multiPacket     = multiPacket;
                s_cachedCounters.hasBounds       = hasBounds;
                s_cachedCounters.alphaOn         = alphaOn;
                s_cachedCounters.hasShapeName    = hasShapeName;
                s_cachedCounters.packetRangesOk  = packetRangesOk;
                s_cachedCounters.packetRangeFail = packetRangeFail;
                s_cacheRegGen  = curRegGen;
                s_cacheValid   = true;
            }
        }
        }  // end dirty/legacy branch (DIRTYONLY-1)
    }

    // -----------------------------------------------------------------------
    // Extraction v2.1: per-draw-slot packet snapshot
    // -----------------------------------------------------------------------
    {
        const uint32_t slotCount = batcher_getDrawSlotCount();
        RenderCore::Span<ExtractedStaticPropPacket> pktSpan =
            snap.frameArena.allocArray<ExtractedStaticPropPacket>(slotCount, "ExtractedStaticPropPacket");
        ExtractedStaticPropPacket* pktBuf = pktSpan.data;
        if (!pktBuf && slotCount > 0u) {
            snap.arenaOverflow = true;
        } else {
            uint32_t pktWrite   = 0u;
            uint32_t pktInvalid = 0u;
            {
            // PERF-INSTRUMENT (EXTRACT-SNAPSHOT-INSTRUMENT-1): block-granularity zone.
            // Times the per-draw-slot packet gather (batcher_getDrawSlotEntry per slot).
            ZoneScopedN("Extract.SP.Packets");
            for (uint32_t slot = 0u; slot < slotCount; ++slot) {
                ExtractedStaticPropPacket pkt{};
                if (!batcher_getDrawSlotEntry(slot, &pkt)) {
                    ++pktInvalid;
                    continue;
                }
                pktBuf[pktWrite++] = pkt;
            }
            }  // end Extract.SP.Packets scope
            snap.staticPropPackets       = Span<ExtractedStaticPropPacket>(pktBuf, pktWrite);
            snap.staticPropPacketCount   = pktWrite;
            snap.staticPropPacketInvalid = pktInvalid;
        }
    }

    // -----------------------------------------------------------------------
    // Extraction v2.2: dispatch-fact compare (batcher internal state vs snapshot rows)
    // -----------------------------------------------------------------------
    // Always call — fills spCompareLiveCount even when snapshot data is null (e.g. arena
    // overflow), so the log always shows live_count and a count_mismatch=1 rather than
    // silently leaving both zero. batcher_compareSnapshotPackets handles null data internally.
    if (materializeProps) batcher_compareSnapshotPackets(&snap);

    // v3: read snapshot build stats from the most recent flush().
    {
        uint32_t attempted = 0u, countMis = 0u, pktMis = 0u, metaMis = 0u, fallback = 0u;
        batcher_getSnapshotBuildStats(&attempted, &countMis, &pktMis, &metaMis, &fallback);
        snap.spBuildAttempted      = attempted;
        snap.spBuildCountMismatch  = countMis;
        snap.spBuildPacketMismatch = pktMis;
        snap.spBuildMetaMismatch   = metaMis;
        snap.spBuildFallback       = fallback;
    }

    // Sync arenaOverflow from FrameArena (definitive; explicit sets above are belt+suspenders).
    snap.arenaOverflow = snap.frameArena.overflowed();

    // v4: extends v3 ok gate — adds 5 mech mismatch counters.
    // Mech counters are 0 when MC2_SNAPSHOT_MECH_EXTRACT is unset (gate OFF),
    // so ok is unaffected in default mode.
    // spBuildAttempted/spBuildFallback and mechSnapshotCount/mechMatValid/
    // mechMatSentinel excluded (informational).
    snap.ok = (snap.staticPropValidationFail  == 0u &&
               snap.staticPropPacketRangesFail == 0u &&
               snap.staticPropPacketInvalid    == 0u &&
               !snap.arenaOverflow             &&
               snap.spCountMismatch            == 0u &&
               snap.spSortedSlotMismatch       == 0u &&
               snap.spGlobalPacketMismatch     == 0u &&
               snap.spPipelineMismatch         == 0u &&
               snap.spMaterialIdxMismatch      == 0u &&
               snap.spTexLayerMismatch         == 0u &&
               // spBuild*Mismatch count snapshot-structure failures; zero on a healthy frame — gate stays valid.
               snap.spBuildCountMismatch       == 0u &&
               snap.spBuildPacketMismatch      == 0u &&
               snap.spBuildMetaMismatch        == 0u &&
               snap.mechCountMismatch          == 0u &&
               snap.mechHandleMismatch         == 0u &&
               snap.mechObjectIdMismatch       == 0u &&
               snap.mechTexHandleMismatch      == 0u &&
               snap.mechMaterialIdxMismatch    == 0u) ? 1u : 0u;

    // -----------------------------------------------------------------------
    // Visibility query for log line
    // -----------------------------------------------------------------------
    uint32_t visibilityStaticPropsCount = 0;
    {
        RenderWorld::VisibilityRequest req;
        req.kindMask = RenderWorld::VisibilityKindMask::StaticProp;
        RenderWorld::VisibilityResult vr = RenderWorld::queryVisibility(req);
        visibilityStaticPropsCount = static_cast<uint32_t>(vr.static_props);
    }

    // -----------------------------------------------------------------------
    // Per-frame log line (v1) — gated: MC2_RENDER_SNAPSHOT_LOG=1 to enable
    // -----------------------------------------------------------------------
    static const bool s_logEnabled = false;
    if (s_logEnabled) {
        mc2::fmt::print_err("[RENDER_SNAPSHOT v3+mech-extract] frame={:d} mechs={:d} static_props={:d} lights={:d} bytes={:d} overflow={:d} ok={:d}\n  sp_fail={:d} sp_sentinel_mat={:d} sp_sentinel_cull={:d} sizeof_static_prop={:d}\n  sp_tex_wired={:d} sp_tex_sentinel={:d} sp_mat_wired={:d} sp_mat_sentinel={:d}\n  sp_primary_alpha_on={:d} sp_multi_packet={:d}\n  sp_has_bounds={:d}\n  sp_alpha_on={:d} sp_has_shape_name={:d}\n  sp_packet_ranges_ok={:d} sp_packet_ranges_invalid={:d}\n  sp_packets={:d} sp_packet_invalid={:d}\n  visibility_static_props={:d} sp_vis_delta={:d}\n  [v2.3 compare] snapshot_count={:d} live_count={:d} count_mismatch={:d}\n  sorted_slot_mismatch={:d} global_packet_mismatch={:d} pipeline_mismatch={:d}\n  material_idx_mismatch={:d} instance_count_mismatch={:d} tex_layer_mismatch={:d}\n  [v3 build] attempted={:d} count_mismatch={:d} pkt_mismatch={:d} meta_mismatch={:d} fallback={:d}\n  [v3 arena] used={:d} high_water={:d} allocs={:d} overflow={:d} mechArenaBytes={:d} mechRows={:d}\n  [mech-extract] gate={:d} snapshot={:d} mat_valid={:d} mat_sentinel={:d} count_mismatch={:d} handle_mismatch={:d} objectid_mismatch={:d} tex_mismatch={:d} mat_mismatch={:d}\n", mc2::fmt::as_u64(static_cast<unsigned long long>(snap.frameIndex)), mc2::fmt::as_u32(static_cast<uint32_t>(snap.mechs.size())), mc2::fmt::as_u32(static_cast<uint32_t>(snap.staticProps.size())), mc2::fmt::as_u32(static_cast<uint32_t>(snap.lights.size())), mc2::fmt::as_size(snap.frameArena.stats().usedBytes), mc2::fmt::as_i32(snap.arenaOverflow ? 1 : 0), mc2::fmt::as_u32(snap.ok), mc2::fmt::as_u32(snap.staticPropValidationFail), mc2::fmt::as_u32(snap.staticPropSentinelMat), mc2::fmt::as_u32(snap.staticPropSentinelCull), mc2::fmt::as_size(sizeof(ExtractedStaticProp)), mc2::fmt::as_u32(snap.staticPropTexWired), mc2::fmt::as_u32(snap.staticPropTexSentinel), mc2::fmt::as_u32(snap.staticPropMatWired), mc2::fmt::as_u32(snap.staticPropMatSentinel), mc2::fmt::as_u32(snap.staticPropPrimaryAlphaOn), mc2::fmt::as_u32(snap.staticPropMultiPacket), mc2::fmt::as_u32(snap.staticPropHasBounds), mc2::fmt::as_u32(snap.staticPropAlphaOn), mc2::fmt::as_u32(snap.staticPropHasShapeName), mc2::fmt::as_u32(snap.staticPropPacketRangesOk), mc2::fmt::as_u32(snap.staticPropPacketRangesFail), mc2::fmt::as_u32(snap.staticPropPacketCount), mc2::fmt::as_u32(snap.staticPropPacketInvalid), mc2::fmt::as_u32(visibilityStaticPropsCount), mc2::fmt::as_i32(static_cast<int32_t>(snap.staticProps.size()) -
                static_cast<int32_t>(visibilityStaticPropsCount)), mc2::fmt::as_u32(snap.spCompareSnapshotCount), mc2::fmt::as_u32(snap.spCompareLiveCount), mc2::fmt::as_u32(snap.spCountMismatch), mc2::fmt::as_u32(snap.spSortedSlotMismatch), mc2::fmt::as_u32(snap.spGlobalPacketMismatch), mc2::fmt::as_u32(snap.spPipelineMismatch), mc2::fmt::as_u32(snap.spMaterialIdxMismatch), mc2::fmt::as_u32(snap.spInstanceCountMismatch), mc2::fmt::as_u32(snap.spTexLayerMismatch), mc2::fmt::as_u32(snap.spBuildAttempted), mc2::fmt::as_u32(snap.spBuildCountMismatch), mc2::fmt::as_u32(snap.spBuildPacketMismatch), mc2::fmt::as_u32(snap.spBuildMetaMismatch), mc2::fmt::as_u32(snap.spBuildFallback), mc2::fmt::as_size(snap.frameArena.stats().usedBytes), mc2::fmt::as_size(snap.frameArena.stats().highWaterBytes), mc2::fmt::as_u32(snap.frameArena.stats().allocCount), mc2::fmt::as_u32(snap.frameArena.stats().overflowCount), mc2::fmt::as_size(snap.mechPackets.count * sizeof(ExtractedMechPacket)), mc2::fmt::as_u32(static_cast<uint32_t>(snap.mechPackets.count)), mc2::fmt::as_i32(s_mechExtractEnabled ? 1 : 0), mc2::fmt::as_u32(snap.mechSnapshotCount), mc2::fmt::as_u32(snap.mechMatValid), mc2::fmt::as_u32(snap.mechMatSentinel), mc2::fmt::as_u32(snap.mechCountMismatch), mc2::fmt::as_u32(snap.mechHandleMismatch), mc2::fmt::as_u32(snap.mechObjectIdMismatch), mc2::fmt::as_u32(snap.mechTexHandleMismatch), mc2::fmt::as_u32(snap.mechMaterialIdxMismatch));
    }

    // v2.3: store for getLastRenderSnapshot() before returning.
    // The snapshot's Span pointers are valid until the next ExtractRenderSnapshot() call.
    s_lastSnapshot    = snap;
    s_hasLastSnapshot = true;
    return snap;
}
