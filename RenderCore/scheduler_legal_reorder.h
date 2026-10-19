#pragma once
// SCHEDULER-EDGE-CLASSIFY-1 — the FIRST tier-C artifact.
//
// A GL-free, OFFLINE edge classifier that TYPES every ordering edge implied by
// kFramePassOrder[] and PROVES the shipped order is legal (a no-op identity
// baseline). It is PROOF INFRASTRUCTURE, NOT A SCHEDULER:
//
//   - No pass is moved. No runtime change. No GL. No engine includes.
//   - Byte-identical by construction: this header has ZERO runtime callers; it is
//     included only by the offline doctest suite (tests/unit/test_frame_graph.cpp).
//   - The arbitrary-permutation oracle isReorderLegal() is SLICE 2 — deliberately
//     NOT built here. This slice = classify + identity baseline only.
//
// WHY classify edges at all? The resource validator (frame_graph_validate.h)
// answers "is every read produced before it is consumed?" but it cannot tell you
// WHICH edges a reorderer must preserve vs which are free. A legal-reorder oracle
// needs every ordering constraint TYPED:
//
//   HardResource      — a real producer->consumer data edge in the FrameLocal DAG
//                       (last in-frame writer of a read). MUST be preserved.
//   SoftState         — ambient GL-state handshake the resource DAG cannot see
//                       (e.g. terrain re-asserts glColorMask(TRUE) for following
//                       passes). MUST be preserved; not derivable from reads/writes.
//   LegacyLatch       — cross-phase CPU latch (markTerrainDrawn / sceneHasTerrain_):
//                       terrain SETS it, post sub-passes CONSUME it. MUST be preserved.
//   KnownEarly        — a config-variable draw-site that legitimately fires BEFORE its
//                       kFramePassOrder slot (LOD-chunk terrain draws pre-renderLists).
//                       A SUPPRESSION rule, NOT a violation (mirrors frame_pass_trace.h
//                       knownEarlyDrawSite).
//   ContentConditional— an ordering relationship that only exists when specific scene
//                       CONTENT is present (water-on-water-map, decals-on-cement, etc.).
//                       Content presence is NOT in the resource DAG.
//   ExternalNonEdge   — a read EXCLUDED from the HardResource set because the resource's
//                       lifetime is Mission/Persistent/External (seeded / persistent /
//                       temporal N-1). Emitted VISIBLY so the lifetime exclusion shows up
//                       in the report instead of being silently dropped.
//
// Firewall: header-only, no GL includes, no game-side includes. Pure POD + constexpr +
// inline. Mirrors the style of frame_graph_validate.h / frame_pass_trace.h.

#include <cstdint>
#include "RenderPassContract.h"        // RenderPassId, kRenderPassContracts, kFramePassOrder
#include "RenderResourceRegistry.h"    // RenderResourceId, RenderResourceLifetime
#include "frame_graph_validate.h"      // findContract, kExternalResources (GL-free external set)
#include "ambient_contract.h"          // AmbientContract: colorMask reassert + terrain latch
#include "terrain_subpass_contract.h"  // TerrainDrawSite (LOD-chunk knownEarly draw-site)

namespace RenderCore { namespace framegraph {

// ---------------------------------------------------------------------------
// Edge classification vocabulary
// ---------------------------------------------------------------------------
enum class EdgeClass : uint8_t {
    HardResource = 0,   // producer->consumer FrameLocal data edge; must preserve
    SoftState,          // ambient GL-state handshake (colorMask reassert)
    LegacyLatch,        // cross-phase CPU latch (sceneHasTerrain_)
    KnownEarly,         // config-variable draw-site fires early; suppression, not violation
    ContentConditional, // edge only exists when specific scene content is present
    ExternalNonEdge,    // read excluded because lifetime in {Mission,Persistent,External}
};

inline const char* edgeClassName(EdgeClass c) {
    switch (c) {
        case EdgeClass::HardResource:       return "HardResource";
        case EdgeClass::SoftState:          return "SoftState";
        case EdgeClass::LegacyLatch:        return "LegacyLatch";
        case EdgeClass::KnownEarly:         return "KnownEarly";
        case EdgeClass::ContentConditional: return "ContentConditional";
        case EdgeClass::ExternalNonEdge:    return "ExternalNonEdge";
    }
    return "?";
}

// A single classified ordering edge (or non-edge, for ExternalNonEdge / KnownEarly).
//   from/to    — the two passes the relationship is between. For ExternalNonEdge and
//                KnownEarly, `to` is the pass the note attaches to and `from` is None.
//   cls        — the classification above.
//   via        — the resource the edge flows through (Unknown for pure SoftState/Latch).
//   forbidden  — true iff a reorderer MUST keep pos(from) < pos(to). HardResource,
//                SoftState, and LegacyLatch edges are forbidden-to-reorder. KnownEarly,
//                ContentConditional, and ExternalNonEdge are NOT forbidden here (KnownEarly
//                is a suppression; ContentConditional is informational for the oracle;
//                ExternalNonEdge is a deliberately-excluded non-edge).
//   contentGated — true for ContentConditional edges (and only those).
//   note       — human-readable description.
struct PassEdge {
    RenderPassId     from        = RenderPassId::None;
    RenderPassId     to          = RenderPassId::None;
    EdgeClass        cls         = EdgeClass::HardResource;
    RenderResourceId via         = RenderResourceId::Unknown;
    bool             forbidden   = false;
    bool             contentGated= false;
    const char*      note        = "";
};

// ---------------------------------------------------------------------------
// Static (GL-free) lifetime map.
//
// RenderResourceRegistry's lifetime field is populated at RUNTIME (registerOrUpdate...),
// so it is unavailable offline. This static map encodes the same vocabulary from the
// authoritative comments in RenderResourceRegistry.h, for the resources that the
// shipped top-level contract + terrain sub-pass tables actually read. FrameLocal is the
// default for anything not listed (the pass DAG's transient color/depth/normal/shadow-
// dynamic targets). A resource whose lifetime is Mission/Persistent/External is a read
// that must be classified ExternalNonEdge (excluded from the HardResource set), exactly
// mirroring kExternalResources in frame_graph_validate.h (which is the broader
// "readable without an in-frame producer" set).
// ---------------------------------------------------------------------------
inline RenderResourceLifetime staticResourceLifetime(RenderResourceId id) {
    switch (id) {
        // Mission: seeded/rebuilt per mission load.
        case RenderResourceId::TerrainHeightTexture:
        case RenderResourceId::TerrainRecipeBuffer:
        case RenderResourceId::TerrainThinBuffer:
        case RenderResourceId::CementAtlas:
        case RenderResourceId::TransitionMaskArray:
        case RenderResourceId::TerrainHeightSsbo:
            return RenderResourceLifetime::Mission;
        // Persistent: process/long-lived; built outside the dynamic frame order.
        case RenderResourceId::MaterialGpuBuffer:
            return RenderResourceLifetime::Persistent;
        // External: temporal / cross-frame N-1 / externally owned.
        case RenderResourceId::WaterReflectionColor:
        case RenderResourceId::WaterReflectionDepth:
            return RenderResourceLifetime::External;
        // Everything else read by a pass is a FrameLocal transient produced this frame
        // (MainColor, MainDepth, MainNormal, scene copies, ...).
        default:
            return RenderResourceLifetime::FrameLocal;
    }
}

// A read is excluded from the HardResource DAG iff its resource lifetime is
// Mission/Persistent/External (seeded / persistent / temporal). FrameLocal reads ARE
// real in-frame data edges. This is the offline analog of the kExternalResources
// exclusion in validateReadsSatisfied().
inline bool isExternalLifetime(RenderResourceId id) {
    const RenderResourceLifetime lt = staticResourceLifetime(id);
    return lt == RenderResourceLifetime::Mission
        || lt == RenderResourceLifetime::Persistent
        || lt == RenderResourceLifetime::External;
}

// ===========================================================================
// HAND-DECLARED TABLES (reviewed-at-change-time, like kExternalResources).
// ===========================================================================

// (1) kContentConditionalEdges[] — ordering relationships that only exist when specific
//     scene CONTENT is present. Content presence is NOT visible in the resource DAG, so a
//     pure producer-walk cannot derive these. They are NOT forbidden-to-reorder edges in
//     the resource sense; the oracle (slice 2) treats them as "present only on maps with
//     this content". Reviewed when a content-gated pass is added/changed.
static constexpr PassEdge kContentConditionalEdges[] = {
    // Water only draws on maps that HAVE water; it overlays the opaque scene depth.
    { RenderPassId::Terrain, RenderPassId::Water, EdgeClass::ContentConditional,
      RenderResourceId::MainDepth, /*forbidden*/ false, /*contentGated*/ true,
      "water-on-water-map: Water present only when the map contains water" },
    // Terrain overlay (cement/transitions) only when the map has cement/perimeter content.
    { RenderPassId::Terrain, RenderPassId::TerrainOverlay, EdgeClass::ContentConditional,
      RenderResourceId::MainColor, /*forbidden*/ false, /*contentGated*/ true,
      "overlay-on-cement: TerrainOverlay quads present only with residual cement content" },
    // Terrain decals (craters/scorch) only when craters/footprints exist this frame.
    { RenderPassId::Terrain, RenderPassId::TerrainDecal, EdgeClass::ContentConditional,
      RenderResourceId::MainColor, /*forbidden*/ false, /*contentGated*/ true,
      "decal-on-crater: TerrainDecal present only when craters/footprints exist" },
    // SceneObjectId writes happen only when the object-id MRT buffer is enabled.
    { RenderPassId::MechOpaque, RenderPassId::PostProcess, EdgeClass::ContentConditional,
      RenderResourceId::SceneObjectId, /*forbidden*/ false, /*contentGated*/ true,
      "objectId-when-IsObjectIdBufferEnabled: SceneObjectId MRT only on the 3-entry draw-buffer set" },
    // Box decals (post-process) only under MC2_DYNAMIC_DECALS.
    // Scene-color copy for the compute blur substrate only under MC2_VFX_SCENECOLOR_GRAB.
    { RenderPassId::VFX, RenderPassId::PostProcess, EdgeClass::ContentConditional,
      RenderResourceId::SceneColorCopy, /*forbidden*/ false, /*contentGated*/ true,
      "SceneColorCopy-when-VFX_SCENECOLOR_GRAB: copySceneColorForVfx gated; consumed by compute blur" },
};
static constexpr int kContentConditionalEdgeCount =
    sizeof(kContentConditionalEdges) / sizeof(kContentConditionalEdges[0]);

// (2) kForbiddenReorderEdges[] — belt-and-suspenders list of must-not-move edges the
//     resource model alone can't fully derive (soft handshakes, latches, screen-space
//     ordering). The classifier ORs the derived HardResource edges WITH this set so a
//     soft edge the model misses is STILL caught as forbidden. Every entry's from/to
//     must be a real RenderPassId (asserted by a doctest). Reviewed at change time.
static constexpr PassEdge kForbiddenReorderEdges[] = {
    // Native shadow preparation must precede color consumers. Vulkan owns
    // the shadow images; these ordering latches need no retired GL resource id.
    { RenderPassId::Shadow, RenderPassId::StaticPropOpaque, EdgeClass::LegacyLatch,
      RenderResourceId::Unknown, /*forbidden*/ true, /*contentGated*/ false,
      "native shadow inputs must be ready before StaticPropOpaque" },
    { RenderPassId::Shadow, RenderPassId::MechOpaque, EdgeClass::LegacyLatch,
      RenderResourceId::Unknown, /*forbidden*/ true, /*contentGated*/ false,
      "native shadow inputs must be ready before MechOpaque" },
    { RenderPassId::Shadow, RenderPassId::Terrain, EdgeClass::LegacyLatch,
      RenderResourceId::Unknown, /*forbidden*/ true, /*contentGated*/ false,
      "native shadow inputs must be ready before Terrain" },
    // Terrain re-asserts glColorMask(TRUE) (after shadow disabled color write) for ALL
    // following color-writing passes. Soft GL-state edge, invisible to the resource DAG.
    { RenderPassId::Terrain, RenderPassId::PostProcess, EdgeClass::SoftState,
      RenderResourceId::Unknown, /*forbidden*/ true, /*contentGated*/ false,
      "Terrain colorMask-reassert: re-enables RGBA write that following passes depend on" },
    // Terrain sets sceneHasTerrain_; PostProcess sub-passes bail if unset. Cross-phase latch.
    { RenderPassId::Terrain, RenderPassId::PostProcess, EdgeClass::LegacyLatch,
      RenderResourceId::Unknown, /*forbidden*/ true, /*contentGated*/ false,
      "sceneHasTerrain latch: terrain sets markTerrainDrawn; 5 post sub-passes consume it" },
    // Opaque scene must be drawn before VFX / vegetation / post composite read or overlay it.
    { RenderPassId::StaticPropOpaque, RenderPassId::VFX, EdgeClass::HardResource,
      RenderResourceId::MainDepth, /*forbidden*/ true, /*contentGated*/ false,
      "opaque->VFX: VFX reads scene depth produced by opaque geometry" },
    { RenderPassId::StaticPropOpaque, RenderPassId::VegetationCards, EdgeClass::HardResource,
      RenderResourceId::MainDepth, /*forbidden*/ true, /*contentGated*/ false,
      "opaque->Veg: vegetation depth-tests against the opaque scene depth" },
    { RenderPassId::StaticPropOpaque, RenderPassId::PostProcess, EdgeClass::HardResource,
      RenderResourceId::MainColor, /*forbidden*/ true, /*contentGated*/ false,
      "opaque->PP: post-process composites the opaque scene color" },
    // UI is drawn before the post composite presents the final frame.
    { RenderPassId::UI, RenderPassId::PostProcess, EdgeClass::SoftState,
      RenderResourceId::MainColor, /*forbidden*/ true, /*contentGated*/ false,
      "UI-before-composite: HUD/text must be present before the final composite" },
};
static constexpr int kForbiddenReorderEdgeCount =
    sizeof(kForbiddenReorderEdges) / sizeof(kForbiddenReorderEdges[0]);

// (3) kDeferredSoftStateEdges[] — soft-state hazards with NO stable per-pass value
//     (ambient_contract.h:54-56: "deferred axes with no stable per-pass declarable
//     value"): tex-unit latches, bound FBO, clip-control/scissor ambiguities. The oracle
//     (slice 2) uses these to say "resource-wise legal BUT blocked by a deferred
//     soft-state edge" — so it does NOT overclaim legality. These are NOT classified as
//     forbidden here (no stable from/to ordering value yet); they are an explicit
//     "unknown hazard" ledger. Reviewed at change time.
//     ★ GENERAL PRINCIPLE (ORACLE-STRENGTHEN-1): an entry here may also carry a CONCRETE
//     from/to pass pair when the deferred hazard is a *visual-commutativity* hazard between
//     two specific passes — i.e. the resource DAG sees no hard edge, but the two passes can
//     overlap in SCREEN SPACE and their fragments resolve to an order-dependent winner under
//     depth-EQUAL / coplanar ties (last-writer-wins is order-sensitive). This is NOT derivable
//     from reads/writes: two opaque passes that both write MainColor/MainDepth are "resource-
//     legal to swap" yet NOT visually commutative. Any future candidate pair of overlapping
//     opaque passes (both writing MainColor+MainDepth, both able to occupy the same pixels)
//     must get the SAME scrutiny — do not assume "no hard resource edge" == "free to reorder".
static constexpr PassEdge kDeferredSoftStateEdges[] = {
    { RenderPassId::None, RenderPassId::None, EdgeClass::SoftState,
      RenderResourceId::Unknown, /*forbidden*/ false, /*contentGated*/ false,
      "tex-unit latch hazard: NVIDIA tex-unit leak; no stable per-pass declarable value" },
    { RenderPassId::None, RenderPassId::None, EdgeClass::SoftState,
      RenderResourceId::Unknown, /*forbidden*/ false, /*contentGated*/ false,
      "bound-FBO hazard: draw-FBO is a dynamic GLuint; not a per-pass enum" },
    { RenderPassId::None, RenderPassId::None, EdgeClass::SoftState,
      RenderResourceId::Unknown, /*forbidden*/ false, /*contentGated*/ false,
      "clip-control/scissor hazard: frame-global / inherited; ambiguous per-pass" },
    // StaticProp<->Mech depth-tie / visual-commutativity hazard (ORACLE-STRENGTHEN-1).
    // Concrete from/to: these two opaque passes BOTH write MainColor+MainDepth and overlap
    // in screen space (a mech can stand in front of / inside a static prop). Resource-wise
    // there is NO hard edge between them, so the producer-walk reports the swap as legal.
    // BUT the gated reorder experiment MEASURED-REORDER-SPMECH-1 (2461d37e) PROVED parity
    // FAILS: pixel diff 3-6x above the noise floor, because overlapping fragments resolve to
    // a DIFFERENT winner under depth-EQUAL ties when draw order swaps (last-writer-wins is
    // order-dependent). "No hard resource edge" != "visually commutative". The oracle must
    // therefore report this swap as BlockedByDeferredSoftState, NOT Legal.
    { RenderPassId::StaticPropOpaque, RenderPassId::MechOpaque, EdgeClass::SoftState,
      RenderResourceId::MainColor, /*forbidden*/ false, /*contentGated*/ false,
      "StaticProp<->Mech depth-tie: overlapping opaque passes; depth-equal ties resolve "
      "order-dependently — resource-legal but NOT visually commutative "
      "(proven by MEASURED-REORDER-SPMECH-1 @2461d37e: parity 3-6x above noise)" },
};
static constexpr int kDeferredSoftStateEdgeCount =
    sizeof(kDeferredSoftStateEdges) / sizeof(kDeferredSoftStateEdges[0]);

// ---------------------------------------------------------------------------
// classifyEdges — derive HardResource edges via the producer-walk, then fold in the
// SoftState/LegacyLatch (from ambient_contract), KnownEarly suppression, ExternalNonEdge
// visibility, and the three hand tables. Pure; writes into a caller-provided buffer.
//
// `out` must have room for at least classifyEdgesCapacity() entries; `count` is set to
// the number written. Generic over a contract table + order so a doctest can feed a
// synthetic table (e.g. to exercise an ExternalNonEdge read).
// ---------------------------------------------------------------------------
inline int classifyEdgesCapacity() {
    // Worst case: every (pass, read-slot) pair is an edge, plus all three hand tables.
    return kRenderPassIdCount * 4
         + kContentConditionalEdgeCount
         + kForbiddenReorderEdgeCount
         + kDeferredSoftStateEdgeCount
         + kRenderPassIdCount /* one KnownEarly suppression entry budget */;
}

inline void classifyEdgesFor(const RenderPassContract* contracts, int contractCount,
                             const RenderPassId* order, int orderCount,
                             PassEdge* out, int& count) {
    count = 0;

    // --- 1. Producer-walk: for each read, find the LAST earlier in-frame FrameLocal
    //        writer. FrameLocal reads -> HardResource edge. External-lifetime reads ->
    //        ExternalNonEdge (visible, not dropped). ---
    for (int o = 0; o < orderCount; ++o) {
        const RenderPassContract* c = findContract(contracts, contractCount, order[o]);
        if (!c) continue;
        for (int r = 0; r < 4; ++r) {
            const RenderResourceId rid = c->reads[r];
            if (rid == RenderResourceId::Unknown) break;

            if (isExternalLifetime(rid)) {
                // Lifetime-excluded read: emit a VISIBLE non-edge so the exclusion shows
                // up in the report rather than silently vanishing.
                PassEdge e;
                e.from = RenderPassId::None;
                e.to   = order[o];
                e.cls  = EdgeClass::ExternalNonEdge;
                e.via  = rid;
                e.forbidden    = false;
                e.contentGated = false;
                e.note = "read excluded: lifetime Mission/Persistent/External (no in-frame edge)";
                out[count++] = e;
                continue;
            }

            // FrameLocal read: find the last earlier writer of rid in the order.
            RenderPassId producer = RenderPassId::None;
            for (int p = o - 1; p >= 0; --p) {
                const RenderPassContract* pc = findContract(contracts, contractCount, order[p]);
                if (!pc) continue;
                bool writes = false;
                for (int w = 0; w < 4; ++w) {
                    if (pc->writes[w] == RenderResourceId::Unknown) break;
                    if (pc->writes[w] == rid) { writes = true; break; }
                }
                if (writes) { producer = order[p]; break; }
            }
            if (producer == RenderPassId::None) continue; // no in-frame producer (e.g. pre-seeded)

            PassEdge e;
            e.from = producer;
            e.to   = order[o];
            e.cls  = EdgeClass::HardResource;
            e.via  = rid;
            e.forbidden    = true;    // a real producer->consumer data edge must be preserved
            e.contentGated = false;
            e.note = "FrameLocal producer->consumer (last in-frame writer of the read)";
            out[count++] = e;
        }
    }

    // --- 2. SoftState (colorMask reassert) + LegacyLatch (sceneHasTerrain_) from the
    //        ambient contract. The producer is the pass that REASSERTS/PRODUCES; the
    //        consumer is each pass that depends on it. We model the canonical pairs:
    //        terrain-reassert -> PostProcess, terrain-latch -> PostProcess. ---
    {
        const AmbientContract* terrain = findAmbient(RenderPassId::Terrain);
        const AmbientContract* post    = findAmbient(RenderPassId::PostProcess);
        if (terrain && post) {
            if (terrain->reassertsColorMaskAllOn) {
                PassEdge e;
                e.from = RenderPassId::Terrain;
                e.to   = RenderPassId::PostProcess;
                e.cls  = EdgeClass::SoftState;
                e.via  = RenderResourceId::Unknown;
                e.forbidden    = true;
                e.contentGated = false;
                e.note = "ambient: terrain re-asserts glColorMask(TRUE) for following passes";
                out[count++] = e;
            }
            if (terrain->producesTerrainLatch && post->consumesTerrainLatch) {
                PassEdge e;
                e.from = RenderPassId::Terrain;
                e.to   = RenderPassId::PostProcess;
                e.cls  = EdgeClass::LegacyLatch;
                e.via  = RenderResourceId::Unknown;
                e.forbidden    = true;
                e.contentGated = false;
                e.note = "ambient: sceneHasTerrain_ latch (terrain sets, post consumes)";
                out[count++] = e;
            }
        }
    }

    // --- 3. KnownEarly suppression: the LOD-chunk terrain branch draws at the Gamecam
    //        site (pre-renderLists), legitimately BEFORE its kFramePassOrder slot. This is
    //        a suppression rule, NOT a violation — emit it as a KnownEarly non-forbidden
    //        marker so isCurrentOrderLegal() (and the slice-2 oracle) knows to suppress an
    //        apparent out-of-order for Terrain. ---
    {
        const TerrainSubPass* lod = findTerrainSubPass(TerrainPath::LODChunk);
        if (lod && lod->drawSite == TerrainDrawSite::Gamecam) {
            PassEdge e;
            e.from = RenderPassId::None;
            e.to   = RenderPassId::Terrain;
            e.cls  = EdgeClass::KnownEarly;
            e.via  = RenderResourceId::Unknown;
            e.forbidden    = false;   // suppression, not a constraint
            e.contentGated = false;
            e.note = "LOD-chunk terrain draws at Gamecam site (pre-renderLists); apparent-early is suppressed";
            out[count++] = e;
        }
    }

    // --- 4. Fold in the three hand tables (belt-and-suspenders + content + deferred). ---
    for (int i = 0; i < kForbiddenReorderEdgeCount; ++i)    out[count++] = kForbiddenReorderEdges[i];
    for (int i = 0; i < kContentConditionalEdgeCount; ++i)  out[count++] = kContentConditionalEdges[i];
    for (int i = 0; i < kDeferredSoftStateEdgeCount; ++i)   out[count++] = kDeferredSoftStateEdges[i];
}

// Convenience: classify the SHIPPED top-level table against the shipped order.
inline void classifyEdges(PassEdge* out, int& count) {
    classifyEdgesFor(kRenderPassContracts, kRenderPassIdCount,
                     kFramePassOrder, kFramePassOrderCount, out, count);
}

// ---------------------------------------------------------------------------
// isCurrentOrderLegal — the no-op identity baseline. Asserts kFramePassOrder satisfies
// every `forbidden` edge (pos(from) < pos(to)), honoring KnownEarly suppression. Returns
// true for the shipped order, mirroring validateShippedFrameGraph(). The arbitrary-
// permutation oracle isReorderLegal() is SLICE 2 — deliberately NOT here.
// ---------------------------------------------------------------------------
inline int orderIndexOf(RenderPassId id, const RenderPassId* order, int orderCount) {
    for (int i = 0; i < orderCount; ++i)
        if (order[i] == id) return i;
    return -1;
}

inline bool isCurrentOrderLegalFor(const RenderPassContract* contracts, int contractCount,
                                   const RenderPassId* order, int orderCount) {
    PassEdge edges[/* generous */ 256];
    int n = 0;
    classifyEdgesFor(contracts, contractCount, order, orderCount, edges, n);

    // Build the KnownEarly suppression set (passes whose apparent-early is allowed).
    bool knownEarly[static_cast<int>(RenderPassId::_SentinelLast)] = { false };
    for (int i = 0; i < n; ++i)
        if (edges[i].cls == EdgeClass::KnownEarly && edges[i].to != RenderPassId::None)
            knownEarly[static_cast<int>(edges[i].to)] = true;

    for (int i = 0; i < n; ++i) {
        const PassEdge& e = edges[i];
        if (!e.forbidden) continue;
        if (e.from == RenderPassId::None || e.to == RenderPassId::None) continue;
        const int pf = orderIndexOf(e.from, order, orderCount);
        const int pt = orderIndexOf(e.to,   order, orderCount);
        if (pf < 0 || pt < 0) continue;   // edge references a pass not in this order
        if (pf < pt) continue;            // satisfied: producer precedes consumer
        // Apparent violation. Suppress iff the CONSUMER has a known-early draw-site
        // (e.g. Terrain LOD-chunk firing before a pass it is listed after).
        if (knownEarly[static_cast<int>(e.to)]) continue;
        return false;
    }
    return true;
}

// Convenience: identity baseline over the shipped table + order.
inline bool isCurrentOrderLegal() {
    return isCurrentOrderLegalFor(kRenderPassContracts, kRenderPassIdCount,
                                  kFramePassOrder, kFramePassOrderCount);
}

// ===========================================================================
// SCHEDULER-REORDER-ORACLE-1 — the SLICE-2 permutation-legality ORACLE.
//
// ★ PROOF-ONLY. This oracle answers "would this reorder be legal?" for an
//   ARBITRARY candidate permutation of the frame passes. It MOVES NOTHING and
//   EXECUTES NOTHING. A `Legal` verdict is NOT a license to reorder — it means
//   only "eligible for a future MEASURED reorder experiment". Byte-identical by
//   construction: header + doctests, ZERO runtime callers.
//
// Three-state verdict (deliberately NOT collapsed to a boolean): a candidate can
// be resource-wise legal yet STILL unsafe to actually move because it crosses a
// deferred soft-state hazard (tex-unit latch / bound FBO / clip-control) that the
// resource model cannot fully see. Reporting that case DISTINCTLY is the whole
// point — the advisor must not overclaim "safe to reorder".
// ---------------------------------------------------------------------------
enum class ReorderVerdict : uint8_t {
    Legal = 0,                  // every forbidden edge held AND no deferred hazard crossed
    ForbiddenEdgeViolated,      // a HardResource/SoftState/LegacyLatch edge runs backwards
    BlockedByDeferredSoftState, // forbidden edges hold, but the move crosses a deferred hazard
};

inline const char* reorderVerdictName(ReorderVerdict v) {
    switch (v) {
        case ReorderVerdict::Legal:                      return "Legal";
        case ReorderVerdict::ForbiddenEdgeViolated:      return "ForbiddenEdgeViolated";
        case ReorderVerdict::BlockedByDeferredSoftState: return "BlockedByDeferredSoftState";
    }
    return "?";
}

// The deferred soft-state hazards (kDeferredSoftStateEdges) are real GL-state
// latches with NO stable per-pass declarable value — they carry None/None pass
// ids precisely because the resource model cannot pin them to a from/to pair.
// What the corpus DOES tell us is WHERE they live: the colorMask-reassert / FBO /
// clip-control / tex-unit hazards all bracket the Terrain..PostProcess region of
// the shipped order (terrain re-asserts colorMask, post sub-passes consume the
// latch, FBO/clip-control churn across the overlay/decal/water/veg/vfx/ui block).
// Passes STRICTLY BEFORE Terrain in the shipped order (Shadow, the two opaque
// geometry passes) sit OUTSIDE that hazard region for the FBO/clip/tex-unit
// hazards — they are z-tested opaque draws that do not touch that deferred
// soft-state. So a swap is region-clean iff BOTH moved passes sit before the
// deferred-hazard boundary (= Terrain's shipped slot). This is the conservative
// analog of KnownEarly suppression: it is the ONE region the resource DAG
// provably does not cover.
//   ★ ORACLE-STRENGTHEN-1: region-clean is NECESSARY but NOT SUFFICIENT. Two opaque
//   passes can both precede Terrain (region-clean) yet still be NOT visually
//   commutative because they overlap in screen space and resolve depth-equal ties
//   order-dependently (StaticProp<->Mech, proven by MEASURED-REORDER-SPMECH-1). Such
//   pairs are listed CONCRETELY in kDeferredSoftStateEdges and blocked additionally
//   below, so a region-clean-but-visually-order-sensitive swap is NOT reported Legal.
inline int deferredHazardBoundaryIndex() {
    return orderIndexOf(RenderPassId::Terrain, kFramePassOrder, kFramePassOrderCount);
}

// crossesDeferredSoftState — true iff the candidate permutation moves a pass into
// or out of the deferred-hazard region relative to the shipped order. Concretely:
// a pass participates in the hazard iff its SHIPPED slot is >= the boundary; if a
// candidate changes the relative order of ANY hazard-region pass against ANY other
// pass, the resource model cannot prove the deferred soft-state still holds, so we
// must block (refuse to overclaim). For an adjacent swap this reduces to: at least
// one of the two swapped passes is in the hazard region.
inline bool crossesDeferredSoftState(const RenderPassId* candidate, int n,
                                     PassEdge* outDeferred) {
    const int boundary = deferredHazardBoundaryIndex();
    if (boundary < 0) return false;
    // The shipped slot of every pass; a pass at shipped slot >= boundary is "in" the
    // deferred-hazard region. Detect any pair whose relative order differs between the
    // shipped order and the candidate AND where at least one member is in the region.
    for (int i = 0; i < n; ++i) {
        const int si = orderIndexOf(candidate[i], kFramePassOrder, kFramePassOrderCount);
        if (si < 0) continue;
        for (int j = i + 1; j < n; ++j) {
            const int sj = orderIndexOf(candidate[j], kFramePassOrder, kFramePassOrderCount);
            if (sj < 0) continue;
            // candidate has i before j; shipped has them in (si,sj) order.
            const bool reorderedPair = (si > sj); // they appear swapped vs shipped
            if (!reorderedPair) continue;
            const bool inRegion = (si >= boundary) || (sj >= boundary);
            if (inRegion) {
                if (outDeferred && kDeferredSoftStateEdgeCount > 0)
                    *outDeferred = kDeferredSoftStateEdges[0];
                return true;
            }
        }
    }
    // ORACLE-STRENGTHEN-1: also block any candidate that REORDERS a CONCRETE-named
    // deferred-soft-state pair (e.g. StaticProp<->Mech depth-tie). These pairs can sit
    // ENTIRELY BEFORE the deferred-hazard boundary (both opaque geometry passes precede
    // Terrain), so the region scan above misses them — but they are still not visually
    // commutative (proven by MEASURED-REORDER-SPMECH-1). If the candidate has the pair in
    // a different relative order than the shipped order, refuse to overclaim.
    for (int k = 0; k < kDeferredSoftStateEdgeCount; ++k) {
        const PassEdge& d = kDeferredSoftStateEdges[k];
        if (d.from == RenderPassId::None || d.to == RenderPassId::None) continue;
        const int sf = orderIndexOf(d.from, kFramePassOrder, kFramePassOrderCount);
        const int st = orderIndexOf(d.to,   kFramePassOrder, kFramePassOrderCount);
        const int cf = orderIndexOf(d.from, candidate, n);
        const int ct = orderIndexOf(d.to,   candidate, n);
        if (sf < 0 || st < 0 || cf < 0 || ct < 0) continue;
        // shipped relative order vs candidate relative order: a flip means the pair moved.
        if ((sf < st) != (cf < ct)) {
            if (outDeferred) *outDeferred = d;
            return true;
        }
    }
    return false;
}

// isReorderLegal — the oracle. For a candidate permutation:
//   1. Classify edges over the SHIPPED contract table against the CANDIDATE order
//      (so the producer-walk resolves last-writer relative to the candidate).
//   2. For every forbidden edge (HardResource/SoftState/LegacyLatch), assert
//      pos(from) < pos(to) in the candidate, honoring KnownEarly suppression. The
//      forbidden edges are evaluated against the SHIPPED classification so a swap
//      cannot "hide" an edge by relabelling the last writer: we OR the candidate-
//      derived forbidden edges WITH the shipped hand tables (already folded in by
//      classifyEdgesFor). First backwards edge -> ForbiddenEdgeViolated.
//   3. If all forbidden edges hold, check crossesDeferredSoftState; if so ->
//      BlockedByDeferredSoftState (resource-wise legal but not safe to move).
//   4. Else Legal (a CANDIDATE legal adjacent swap; eligible for a future MEASURED
//      reorder experiment — NOT an approved/scheduled reorder).
//
// outFirstViolated (optional) receives the first forbidden edge that runs
// backwards (when ForbiddenEdgeViolated) or the deferred hazard edge (when
// BlockedByDeferredSoftState); left untouched on Legal.
inline ReorderVerdict isReorderLegal(const RenderPassId* candidate, int n,
                                     PassEdge* outFirstViolated) {
    // Classify the shipped contract table against the candidate order. forbidden
    // edges (derived producer->consumer + the shipped hand tables) come back folded.
    PassEdge edges[256];
    int ec = 0;
    classifyEdgesFor(kRenderPassContracts, kRenderPassIdCount, candidate, n, edges, ec);

    // KnownEarly suppression set (same rule as isCurrentOrderLegalFor).
    bool knownEarly[static_cast<int>(RenderPassId::_SentinelLast)] = { false };
    for (int i = 0; i < ec; ++i)
        if (edges[i].cls == EdgeClass::KnownEarly && edges[i].to != RenderPassId::None)
            knownEarly[static_cast<int>(edges[i].to)] = true;

    for (int i = 0; i < ec; ++i) {
        const PassEdge& e = edges[i];
        if (!e.forbidden) continue;
        if (e.from == RenderPassId::None || e.to == RenderPassId::None) continue;
        const int pf = orderIndexOf(e.from, candidate, n);
        const int pt = orderIndexOf(e.to,   candidate, n);
        if (pf < 0 || pt < 0) continue;      // edge references a pass absent from candidate
        if (pf < pt) continue;               // satisfied: producer precedes consumer
        if (knownEarly[static_cast<int>(e.to)]) continue; // suppressed apparent-early
        if (outFirstViolated) *outFirstViolated = e;
        return ReorderVerdict::ForbiddenEdgeViolated;
    }

    // Forbidden edges all hold. Now the conservative deferred-soft-state gate.
    PassEdge deferred;
    if (crossesDeferredSoftState(candidate, n, &deferred)) {
        if (outFirstViolated) *outFirstViolated = deferred;
        return ReorderVerdict::BlockedByDeferredSoftState;
    }
    return ReorderVerdict::Legal;
}

// legalAdjacentSwaps — for each adjacent pair (i, i+1) in kFramePassOrder, build
// the swapped permutation and run the oracle. out[i] = true IFF the verdict is
// Legal (NOT merely "forbidden-edges-hold"; a deferred-blocked swap is out[i]=
// false). ORACLE-STRENGTHEN-1: on the content-free shipped baseline there are now
// ZERO true entries. The StaticPropOpaque<->MechOpaque swap was once the single
// candidate (E6: two opaque z-tested passes with no hard edge between them, both
// before the deferred-hazard boundary), but MEASURED-REORDER-SPMECH-1 (@2461d37e)
// PROVED it fails parity (depth-equal ties resolve order-dependently), so it is now
// a CONCRETE deferred-soft-state pair -> BlockedByDeferredSoftState, not Legal. The
// honest post-experiment state of the shipped order is: NO clean legal swaps exist.
// Everything is forbidden or deferred-blocked. `out` must hold (n-1) bools.
inline void legalAdjacentSwaps(bool* out, int n) {
    for (int i = 0; i + 1 < n; ++i) {
        RenderPassId cand[static_cast<int>(RenderPassId::_SentinelLast)];
        for (int k = 0; k < n; ++k) cand[k] = kFramePassOrder[k];
        const RenderPassId tmp = cand[i];
        cand[i] = cand[i + 1];
        cand[i + 1] = tmp;
        const ReorderVerdict v = isReorderLegal(cand, n, nullptr);
        out[i] = (v == ReorderVerdict::Legal);
    }
}

// reportReorderVerdict — format a candidate's verdict + (if not Legal) the first
// blocking edge and its class, into a caller-provided buffer.
//
// ★ WORDING CONTRACT: a Legal verdict is reported as a "candidate legal adjacent
//   swap" / "eligible for a future MEASURED reorder experiment" — NEVER as
//   "approved reorder", "safe to reorder", or "scheduled". The oracle proves
//   eligibility, not sanction. Do not soften this wording.
inline void reportReorderVerdict(ReorderVerdict v, const PassEdge* blocking,
                                 char* buf, int bufLen) {
    if (!buf || bufLen <= 0) return;
    const char* head =
        (v == ReorderVerdict::Legal)
            ? "candidate legal adjacent swap (eligible for a future MEASURED reorder experiment)"
        : (v == ReorderVerdict::ForbiddenEdgeViolated)
            ? "NOT a candidate: forbidden edge violated"
        :   "NOT a candidate: resource-wise legal BUT blocked by a deferred soft-state edge";

    // Tiny GL-free formatter (no <cstdio> dependency contract beyond what doctest
    // already pulls in is assumed; use a manual append to stay self-contained).
    int p = 0;
    auto put = [&](const char* s) {
        for (int k = 0; s[k] && p < bufLen - 1; ++k) buf[p++] = s[k];
    };
    put(head);
    if (v != ReorderVerdict::Legal && blocking) {
        put(" [");
        put(edgeClassName(blocking->cls));
        put("] ");
        put(blocking->note ? blocking->note : "");
    }
    buf[p] = '\0';
}

}} // namespace RenderCore::framegraph
