// GameOS/gameos/render_snapshot.h
//
// Per-frame RenderSnapshot: immutable frame-lifetime view of render
// state populated by ExtractRenderSnapshot() from engine layers.
//
// Extraction v1 (2026-05-24): static-prop snapshot populated alongside
// v0 mech and light records. Arena capacity follows native prop-row size.
// v1.1 (2026-05-25): texArrayLayer/materialIdx wired; material cache counters added.
// FRAMEARENA-L2-1: RenderFrameArena replaced by RenderCore::FrameArena (typed,
//   stats-tracked, alignment-safe). Three module-static buffers in
//   render_snapshot.cpp rotate by extraction sequence. Spans survive two later extractions.

#pragma once

#include "../RenderCore/FrameArena.h"
#include "../RenderCore/Handle.h"
#include <cstdint>
#include <cstdlib>

// Span<T> alias: consumers use the unqualified name; type is RenderCore::Span<T>.
// uint32_t count (was size_t); size()/begin()/end()/empty()/operator[] all present.
template <typename T>
using Span = RenderCore::Span<T>;

// --- v0: mech extraction record (MECH-EXTRACTION-0) ---
// Populated when MC2_SNAPSHOT_MECH_EXTRACT=1 (default OFF).
// materialIdx is a 0xFFFFFFFFu sentinel in v0 — wired in Mech-1 follow-up.
struct ExtractedMechPacket {
    uint32_t objectIdRaw;    // GpuMechSubmitDesc::objectIdRaw (RenderObjectHandle.raw())
    uint32_t instanceIdx;    // index i in this-frame s_pendingSubmits
    uint32_t materialIdx;    // 0xFFFFFFFFu sentinel (not available pre-flush in v0)
    uint32_t texHandle;      // GpuMechSubmitDesc::slot0TexHandle (mc2::textures::ready() slot index)
    uint32_t typeLodIdx;     // PendingSubmit::typeLodIdx (type x LOD record index)
    uint32_t renderFlags;    // GpuMechSubmitDesc::renderFlags (bit0=ALPHA_TEST, bit1=lightsOut, bit2=highlighted)
    // GAMEADAPTERS-VISUAL-STATE-BRIDGE-1: per-mech visual state (debug dump).
    float    heat01;         // GpuMechSubmitDesc::heat01 (always 0; USEHEAT off)
    float    damage01;       // GpuMechSubmitDesc::damage01 ([0,1] composite damage)
    uint32_t visualFlags;    // GpuMechSubmitDesc::visualFlags (RenderCore::MechVisualFlagBits)
};
static_assert(sizeof(ExtractedMechPacket) == 36,
    "ExtractedMechPacket size changed — update extraction consumers");

// Kept as a zero-size placeholder for code that still references ExtractedMech
// (light-record TBD; rename ExtractedMech when lights get real fields).
struct ExtractedMech {
    // Placeholder — do not add fields; use ExtractedMechPacket for mech data.
};

// --- v0: light record ---
struct LightRecord {
    // Placeholder for v0 fields; implementation deferred.
    // Expected fields: position, color, falloff, etc.
};

// --- v1: static-prop snapshot record ---
// Extraction v1: static-prop snapshot record.
// Fields marked "sentinel v1" are typed for v1.1/v2 but not authoritative here.
// Sentinel values mean "not yet extracted" — callers must not treat them as valid data.
struct ExtractedStaticProp {
    // --- identity (authoritative v1) ---
    RenderCore::RenderObjectHandle rwHandle;      // validated alive+generation by fillStaticPropSlots
    int32_t            recipeIndex;   // == rwHandle.index() by construction
    uint32_t           typeId;        // from GpuStaticPropInstance.typeID

    // --- transform (authoritative v1) ---
    // Row-major matrix from GpuStaticPropInstance.modelMatrix.
    // Translation is at [3],[7],[11] with Stuff-space axis swap:
    //   worldCenterX = -worldMatrix[3]   (MC2 east)
    //   worldCenterY =  worldMatrix[11]  (MC2 north)
    //   worldCenterZ =  worldMatrix[7]   (MC2 elevation)
    float worldMatrix[16];
    float worldCenterX;
    float worldCenterY;
    float worldCenterZ;
    // RecipeRange::extentRadius from previous frame's markVisible(); 0.0f if never visible.
    float boundingRadius;

    // --- material / lighting (sentinel v1) ---
    uint32_t materialIdx;    // sentinel 0xFFFFFFFFu — MaterialGpu wiring is v1.1
    int32_t  texArrayLayer;  // sentinel -1
    // RecipeRange::lightDataIndex; 0xFFFFFFFFu if getter failed.
    uint32_t lightDataIndex;

    // --- metadata (v2) ---
    uint8_t  alphaClass;    // 0=alpha-off, 1=alpha-on; from s_types[typeID].alphaClass
    uint32_t instanceFlags; // GpuStaticPropInstance.flags: bit0=lightsOut, bit1=isWindow, bit2=isSpotlight
    uint32_t packetCount;   // s_types[typeID].packetCount; 0 if not found
    uint32_t firstPacket;   // s_types[typeID].firstPacket; use with batcher_getPacketTexArrayLayer
    char     shapeName[64]; // AppearanceType name; '\0' for bulk-registered props
};

static_assert(sizeof(ExtractedStaticProp) <= 256,
    "ExtractedStaticProp exceeded 256-byte budget; adjust arena sizing");

// --- v2.2: per-draw-slot packet snapshot record ---
// One entry per draw slot in s_sortedPacketOrder (geometry-stable after finalizeGeometry).
// sortedSlot: index i in the batcher's sorted draw command array.
// globalPacketIdx: s_sortedPacketOrder[sortedSlot] — index into s_packets[].
// pipelineId: RenderCore::PipelineId cast to uint32_t (StaticPropOpaque or StaticPropAlphaTest).
// materialIdx: s_packetMaterialIdx[sortedSlot]; 0xFFFFFFFFu if MC2_MATERIAL_GPU sidecar invalid.
// instanceCount: previous-frame per-type instance count from flush(); 0 on frame 1.
// texArrayLayer: packet's albedo tex-array layer (v2.2); -1 sentinel if not set.
struct ExtractedStaticPropPacket {
    uint32_t sortedSlot;       // draw-slot index in s_sortedPacketOrder
    uint32_t globalPacketIdx;  // s_sortedPacketOrder[sortedSlot]
    uint32_t typeId;           // s_packets[globalPacketIdx].owningTypeID
    uint32_t pipelineId;       // RenderCore::PipelineId cast to uint32_t (StaticPropOpaque or StaticPropAlphaTest)
    uint32_t materialIdx;      // s_packetMaterialIdx[slot]; 0xFFFFFFFFu sentinel if sidecar invalid
    uint32_t instanceCount;    // previous-frame; 0 on frame 1 or no visible instances
    int32_t  texArrayLayer;    // packet's albedo tex-array layer (v2.2); -1 sentinel if not set
};

static_assert(sizeof(ExtractedStaticPropPacket) == 28,
    "ExtractedStaticPropPacket layout changed — update v2.2 extraction consumers");

// --- TERRAIN-PASS-PACKET-0: pass-level (not per-tile) terrain render facts ---
// Populated once per frame in gameosmain after ExtractRenderSnapshot(), from
// already-existing accessors. NO new counters added to terrain code; if a
// field has no existing source, it is omitted (e.g. visibleBlockCount).
// Pass-level only — no per-tile identity, no draw-order info.
// NOT included in render_snapshot::ok gate (inspection-only for v1).
struct TerrainPassFacts {
    // --- flags bitfield (bit positions are stable) ---
    static constexpr uint32_t kFlagTessellationOn   = 1u << 0;
    static constexpr uint32_t kFlagViewUniformsBound = 1u << 1;
    static constexpr uint32_t kFlagOverflow         = 1u << 2;

    uint32_t viewId           = 0u;  // RenderCore::getCurrentView().id
    uint32_t drawCallCount    = 0u;  // TerrainPatchStream::getLastFlushBucketCount() (prev-flush)
    uint32_t flags            = 0u;  // OR of kFlag* above
    // Note: visibleBlockCount intentionally omitted — no existing counter to source.
};
static_assert(sizeof(TerrainPassFacts) == 12,
    "TerrainPassFacts layout changed — update inspector consumers");

// Per-frame render snapshot: immutable view of extracted engine state.
struct RenderSnapshot {
    // Frame identity
    uint64_t frameIndex = 0;

    // --- v0: mech + light records ---
    Span<ExtractedMech> mechs;                    // kept for light-record placeholder (zero-size struct)
    Span<ExtractedMechPacket> mechPackets;         // MECH-EXTRACTION-0 (gate: MC2_SNAPSHOT_MECH_EXTRACT=1)
    Span<LightRecord> lights;

    // --- v0/v1: mech compare counters (gate: MC2_SNAPSHOT_MECH_EXTRACT=1) ---
    // All informational — NOT included in ok gate.
    uint32_t mechSnapshotCount       = 0u;  // entries captured this frame
    uint32_t mechCountMismatch       = 0u;  // 1 if snapshot count != live pending count
    uint32_t mechHandleMismatch      = 0u;  // rows where typeLodIdx differs from live
    uint32_t mechObjectIdMismatch    = 0u;  // rows where objectIdRaw differs from live
    // v1: independent live compare via s_mechHandleToMaterialIdx; 0 = mismatch free
    uint32_t mechMaterialIdxMismatch = 0u;  // rows where snap materialIdx != live materialIdx
    uint32_t mechTexHandleMismatch   = 0u;  // rows where texHandle differs from live
    // v1: materialIdx coverage counters (populated during extraction loop)
    uint32_t mechMatValid            = 0u;  // rows where materialIdx != 0xFFFFFFFFu
    uint32_t mechMatSentinel         = 0u;  // rows where materialIdx == 0xFFFFFFFFu

    // --- v1: static-prop snapshot ---
    Span<ExtractedStaticProp> staticProps;           // populated by ExtractRenderSnapshot v1
    uint32_t staticPropValidationFail = 0;           // hard gate: must be 0
    uint32_t staticPropSentinelMat    = 0;           // v1: expected == staticProps.size(); v1.1: < size when MC2_MATERIAL_GPU=1
    uint32_t staticPropSentinelCull   = 0;           // expected == staticProps.size() in v1

    // v1.1 material/texture wiring counters
    uint32_t staticPropTexWired       = 0;  // texArrayLayer != -1
    uint32_t staticPropTexSentinel    = 0;  // texArrayLayer == -1 (no valid primary)
    uint32_t staticPropMatWired       = 0;  // materialIdx != 0xFFFFFFFF (MC2_MATERIAL_GPU=1 only)
    uint32_t staticPropMatSentinel    = 0;  // materialIdx == 0xFFFFFFFF
    uint32_t staticPropPrimaryAlphaOn = 0;  // primary came from alpha-on fallback
    uint32_t staticPropMultiPacket    = 0;  // type has > 1 packet

    // v2 metadata counters
    uint32_t staticPropHasBounds          = 0;  // boundingRadius > 0
    uint32_t staticPropAlphaOn            = 0;  // alphaClass == 1
    uint32_t staticPropHasShapeName       = 0;  // shapeName[0] != '\0'
    uint32_t staticPropPacketRangesOk     = 0;  // firstPacket + packetCount within sidecar
    uint32_t staticPropPacketRangesFail   = 0;  // firstPacket + packetCount out of range

    // --- v2.1: per-draw-slot packet snapshot ---
    Span<ExtractedStaticPropPacket> staticPropPackets; // one per draw slot
    uint32_t staticPropPacketCount   = 0;  // packets successfully captured
    uint32_t staticPropPacketInvalid = 0;  // batcher_getDrawSlotEntry failures

    // v4 hard gate — extends v3: adds 5 mech mismatch counters when
    //   MC2_SNAPSHOT_MECH_EXTRACT=1; trivially zero (safe) when gate OFF.
    // v3 gate: adds spBuildCountMismatch, spBuildPacketMismatch, spBuildMetaMismatch.
    // v2.3 gate: staticPropValidationFail==0, staticPropPacketRangesFail==0,
    //   staticPropPacketInvalid==0, !arenaOverflow,
    //   spCountMismatch==0, spSortedSlotMismatch==0, spGlobalPacketMismatch==0,
    //   spPipelineMismatch==0, spMaterialIdxMismatch==0, spTexLayerMismatch==0.
    // Informational (excluded from ok): spInstanceCountMismatch,
    //   spBuildAttempted, spBuildFallback,
    //   mechSnapshotCount, mechMatValid, mechMatSentinel.
    uint32_t ok = 0u;

    // --- v2.2: dispatch-fact compare results (filled by batcher_compareSnapshotPackets) ---
    // Called from ExtractRenderSnapshot() after v2.1 packet capture, before ok computation.
    // spInstanceCountMismatch is informational only — different-frame authority by design.
    // All other counters are included in the ok gate.
    uint32_t spCompareSnapshotCount  = 0u;  // snap->staticPropPackets.count at compare time
    uint32_t spCompareLiveCount      = 0u;  // batcher_getDrawSlotCount() at compare time
    uint32_t spCountMismatch         = 0u;  // 1 if snapshot_count != live_count
    uint32_t spSortedSlotMismatch    = 0u;  // rows where snap[i].sortedSlot != i
    uint32_t spGlobalPacketMismatch  = 0u;  // rows where globalPacketIdx != s_sortedPacketOrder[i]
    uint32_t spPipelineMismatch      = 0u;  // rows where pipelineId diverges from live alpha split
    uint32_t spMaterialIdxMismatch   = 0u;  // rows where materialIdx != s_packetMaterialIdx[i]
    uint32_t spInstanceCountMismatch = 0u;  // rows where prev-frame count != current-frame count
    uint32_t spTexLayerMismatch      = 0u;  // rows where MaterialGpu.albedoTex != texArrayLayer

    // --- v3: snapshot build stats (previous-flush) ---
    // Written by batcher_getSnapshotBuildStats(); read by ExtractRenderSnapshot().
    // spBuildAttempted and spBuildFallback are informational — excluded from ok gate.
    // spBuildCountMismatch, spBuildPacketMismatch, spBuildMetaMismatch participate in ok gate.
    uint32_t spBuildAttempted      = 0u;  // 1 if gate check ran this flush
    uint32_t spBuildCountMismatch  = 0u;  // snap.count != totalCmds
    uint32_t spBuildPacketMismatch = 0u;  // DrawPacket field divergence (accumulated)
    uint32_t spBuildMetaMismatch   = 0u;  // DispatchMeta field divergence (accumulated)
    uint32_t spBuildFallback       = 0u;  // gate enabled/attempted but snapshot arrays NOT dispatched


    // --- TERRAIN-PASS-PACKET-0: pass-level terrain facts (inspection-only) ---
    // Populated in gameosmain after ExtractRenderSnapshot() returns; NOT touched
    // by ExtractRenderSnapshot(). Not part of ok gate.
    TerrainPassFacts terrainPass;

    // L2 frame-lifetime arena. Backed by one of three module-static buffers
    // in render_snapshot.cpp. FrameArena is non-owning: base_ points into the
    // slot buffer; CPU recording must retire before this slot can recycle.
    RenderCore::FrameArena frameArena;
    bool arenaOverflow = false;
};

// Extraction entry point — implemented in render_snapshot.cpp.
// Called once per frame between DoGameLogic() and draw_screen().
// Returns a snapshot backed by a frame-lifetime arena (snap.arena).
// A nonzero sequence retains the arena until RetireRenderSnapshot; zero keeps
// synchronous inline tool behavior. CPU completion does not replace GPU tickets.
RenderSnapshot ExtractRenderSnapshot(uint64_t packetSequence = 0);
void RetireRenderSnapshot(uint64_t packetSequence);

// v2.3: returns a non-owning pointer to the most recently produced snapshot.
// Valid until the next ExtractRenderSnapshot() call (same frame lifetime as the arena).
// Returns nullptr before the first extraction — callers must null-check before use.
const RenderSnapshot* getLastRenderSnapshot();
