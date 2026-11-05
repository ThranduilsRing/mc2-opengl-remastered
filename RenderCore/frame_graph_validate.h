#pragma once
// FRAME-GRAPH-SKELETON-1 — pure, GL-free validation kernel over the existing
// descriptive pass table (RenderPassContract.h). NOT a scheduler/executor: it does
// not run passes or emit barriers. It VALIDATES the declared resource DAG so a bad
// pass order or a missing producer is caught offline (tests/unit/test_frame_graph.cpp)
// and in CI, instead of as an invisible scene at runtime.
//
// The "declaration" already exists: kRenderPassContracts[] (reads[]/writes[] per pass)
// + kFramePassOrder[] (execution order). This kernel adds the missing "validation"
// layer the recon (docs/render-backend-seams/frame-graph-recon-1.md) called for.
//
// NOTE (recon executor blocker): this validates the RESOURCE edges only. AMBIENT GL
// state (colorMask handshake, markTerrainDrawn/sceneHasTerrain_ latches, viewport,
// reverse-Z) is NOT modeled here and remains the gating blocker before any executor.
#include "RenderPassContract.h"
#include "RenderResourceRegistry.h"

namespace RenderCore { namespace framegraph {

inline const RenderPassContract* findContract(const RenderPassContract* contracts,
                                              int count, RenderPassId id) {
    for (int i = 0; i < count; ++i)
        if (contracts[i].id == id) return &contracts[i];
    return nullptr;
}

struct ValidationResult {
    bool             ok              = true;
    RenderPassId     offendingPass   = RenderPassId::None;
    RenderResourceId missingResource = RenderResourceId::Unknown;
    bool             unknownPass     = false;  // order referenced an id with no contract row
};

// A pass's read is satisfied iff the resource was WRITTEN by an earlier pass in the
// execution order, OR it is EXTERNALLY provided. External = legitimately readable
// without an in-frame producer this frame: static uploads (TerrainHeightTexture), the
// shadow STATIC map (built outside the dynamic order), and temporal N-1 resources
// (water reflection). Listing them explicitly means a genuinely missing producer is a
// real error, not silently excused.
inline ValidationResult validateReadsSatisfied(
        const RenderPassContract* contracts, int contractCount,
        const RenderPassId* order, int orderCount,
        const RenderResourceId* external, int externalCount) {
    bool produced[static_cast<int>(RenderResourceId::Count)] = { false };
    for (int e = 0; e < externalCount; ++e)
        produced[static_cast<int>(external[e])] = true;

    for (int o = 0; o < orderCount; ++o) {
        const RenderPassContract* c = findContract(contracts, contractCount, order[o]);
        if (!c) return { false, order[o], RenderResourceId::Unknown, true };

        for (int r = 0; r < 4; ++r) {
            const RenderResourceId rid = c->reads[r];
            if (rid == RenderResourceId::Unknown) break;
            if (!produced[static_cast<int>(rid)])
                return { false, order[o], rid, false };
        }
        for (int w = 0; w < 4; ++w) {
            const RenderResourceId wid = c->writes[w];
            if (wid == RenderResourceId::Unknown) break;
            produced[static_cast<int>(wid)] = true;
        }
    }
    return { true, RenderPassId::None, RenderResourceId::Unknown, false };
}

// The resources that are legitimately readable without an in-frame producer.
// Keep in sync with the recon ledger; adding one here is an explicit assertion that
// the resource is seeded/persistent/temporal, reviewed at change time.
//
// REGISTRY-LIFETIME-CLASS-1 — relationship to RenderResourceLifetime:
//   This frame-graph "external" set (readable WITHOUT an in-frame producer) is
//   BROADER than the registry lifetime==External class. Lifetime::External means
//   strictly temporal / cross-frame N-1 / externally-owned (the two water
//   reflection targets). But a resource can also be readable without an in-frame
//   producer because it is SEEDED (Mission: TerrainHeightTexture) or built
//   outside the dynamic order (Persistent: MaterialGpuBuffer).
//   So lifetime==External is a SUBSET of kExternalResources, not equal to it.
//   The single-source-of-truth invariant we CAN enforce (and a doctest in
//   tests/unit/test_rendercore.cpp does): every resource registered with
//   lifetime==External MUST appear in kExternalResources, so the two cannot
//   drift. A full migration (deriving the broader frame-graph-external set from
//   the registry once Mission/Persistent-but-seeded is a queryable property) is
//   a follow-up — the two concepts genuinely differ today.
static constexpr RenderResourceId kExternalResources[] = {
    RenderResourceId::TerrainHeightTexture,  // static height upload (lifetime Mission)
    // built outside the dynamic frame order (lifetime Persistent)
    RenderResourceId::WaterReflectionColor,  // temporal N-1 (lifetime External)
    RenderResourceId::WaterReflectionDepth,  // temporal N-1 (lifetime External)
    RenderResourceId::MaterialGpuBuffer,     // persistent material table, not a pass output (lifetime Persistent)
};
static constexpr int kExternalResourceCount =
    sizeof(kExternalResources) / sizeof(kExternalResources[0]);

// REGISTRY-LIFETIME-CLASS-1: compile-time membership query so a doctest can
// assert every lifetime==External resource is listed above (drift guard).
inline bool isInExternalResourceList(RenderResourceId id) {
    for (auto kExternalResource : kExternalResources)
        if (kExternalResource == id) return true;
    return false;
}

// Convenience: validate the SHIPPED table against the shipped order.
inline ValidationResult validateShippedFrameGraph() {
    return validateReadsSatisfied(kRenderPassContracts, kRenderPassIdCount,
                                  kFramePassOrder, kFramePassOrderCount,
                                  kExternalResources, kExternalResourceCount);
}

}} // namespace RenderCore::framegraph
