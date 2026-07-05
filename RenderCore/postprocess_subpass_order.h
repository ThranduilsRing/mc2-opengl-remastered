#pragma once
// SUBGRAPH-CONTIGUITY-GUARD-1 — GL-free / Vulkan-free constexpr canonical order of the
// endScene() postprocess SUB-passes, plus a contiguity validator that encodes a standing
// architectural rule:
//
//   ★ RULE — A Vulkan subgraph may ONLY fuse passes that are CONTIGUOUS in canonical
//     GL/frame-graph order. Adding a NON-CONTIGUOUS pass to a subgraph is a REORDER
//     experiment (it moves work across an intervening pass) and must go through the
//     scheduler/oracle + a parity proof as such — it is NOT a subgraph "expansion".
//
// WHY: the shipped Vulkan postprocess subgraph (VULKAN-POSTPROCESS-SUBGRAPH-1, 9f049ce8)
// fuses EdgeFog+OobFog, which ARE adjacent in endScene(). The tempting next step — folding
// CloudShadow into that same subgraph — is NOT contiguous: Shoreline, SSAO and BoxDecals
// all execute between CloudShadow and EdgeFog. Fusing across them silently reorders the
// scene-color writes of those passes relative to the fog blend. The StaticProp<->Mech
// reorder-parity FAIL (mc2_24 pixel diff on a depth-EQUAL tie; candidate rejected, gate
// left default-OFF) is the standing precedent that "no hard resource edge" != "visually
// commutative". So a non-contiguous fusion must be treated as a measured reorder, gated,
// and parity-proven — never adopted as a subgraph growth.
//
// ★ PROOF-ONLY. SCHEMA + VALIDATION. ZERO runtime callers; NOT wired into the renderer;
//   included only by the offline doctest suite (tests/unit/test_subgraph_contiguity.cpp).
//   Header-only, no GL includes, no Vulkan includes, no game-side includes — mirrors
//   vulkan_layout_chain.h discipline exactly.
//
// Canonical order confirmed from GameOS/gameos/gos_postprocess.cpp endScene():
//   ScreenShadow (runScreenShadow, ~L2676) -> CloudShadow (runCloudShadow, ~L2684) ->
//   Shoreline (runShoreline, ~L2690) -> SSAO (runSSAO, ~L2694) -> BoxDecals
//   (drawBoxDecals, ~L2698) -> EdgeFog (runEdgeFog / fused subgraph, ~L2720) ->
//   OobFog (runFogOob, ~L2739; executor island name FogOob) -> Composite (backbuffer
//   bind + pillarbox, ~L2755).

#include <cstdint>

namespace RenderCore { namespace vulkan {

// ---------------------------------------------------------------------------
// Canonical postprocess sub-pass identity, in execution order. The integer value
// is NOT the canonical index by itself — kPostprocessSubPassOrder[] is the source of
// truth (mirrors the vulkan_layout_chain.h "order array is truth" convention).
// ---------------------------------------------------------------------------
enum class PostprocessSubPassId : uint8_t {
    ScreenShadow = 0,   // runScreenShadow  — post-process shadow over terrain/objects/overlays
    CloudShadow,        // runCloudShadow   — procedural multiplicative cloud darkening
    Shoreline,          // runShoreline     — shoreline foam brighten on water-adjacent terrain
    SSAO,               // runSSAO          — multiplicative AO darkening (gated)
    BoxDecals,          // drawBoxDecals    — screen-space box decals (gated)
    EdgeFog,            // runEdgeFog       — map-boundary fog fade (fused w/ OobFog when subgraph on)
    OobFog,             // runFogOob        — out-of-bounds far-plane fog (executor island "FogOob")
    Composite,          // backbuffer bind + pillarbox composite output
};

// Canonical execution order — the single source of truth for "canonical index".
static constexpr PostprocessSubPassId kPostprocessSubPassOrder[] = {
    PostprocessSubPassId::ScreenShadow,
    PostprocessSubPassId::CloudShadow,
    PostprocessSubPassId::Shoreline,
    PostprocessSubPassId::SSAO,
    PostprocessSubPassId::BoxDecals,
    PostprocessSubPassId::EdgeFog,
    PostprocessSubPassId::OobFog,
    PostprocessSubPassId::Composite,
};

static constexpr int kPostprocessSubPassOrderCount =
    sizeof(kPostprocessSubPassOrder) / sizeof(kPostprocessSubPassOrder[0]);

inline const char* postprocessSubPassName(PostprocessSubPassId id) {
    switch (id) {
        case PostprocessSubPassId::ScreenShadow: return "ScreenShadow";
        case PostprocessSubPassId::CloudShadow:  return "CloudShadow";
        case PostprocessSubPassId::Shoreline:    return "Shoreline";
        case PostprocessSubPassId::SSAO:         return "SSAO";
        case PostprocessSubPassId::BoxDecals:    return "BoxDecals";
        case PostprocessSubPassId::EdgeFog:      return "EdgeFog";
        case PostprocessSubPassId::OobFog:       return "OobFog";
        case PostprocessSubPassId::Composite:    return "Composite";
    }
    return "?";
}

// Canonical index of a sub-pass in kPostprocessSubPassOrder (-1 iff absent).
constexpr int postprocessSubPassCanonicalIndex(PostprocessSubPassId id) {
    for (int i = 0; i < kPostprocessSubPassOrderCount; ++i)
        if (kPostprocessSubPassOrder[i] == id) return i;
    return -1;
}

// ---------------------------------------------------------------------------
// vkSubgraphIsContiguous — the RULE, as a constexpr validator.
//
// Returns true iff the `n` fused passes occupy a CONTIGUOUS run in the canonical
// order: their canonical indices, taken as a set, form {min .. min+n-1} with no gaps.
// Equivalently max-min == n-1 AND every index is distinct AND every index is valid.
//
// A fused set that skips over an intervening canonical pass (e.g. {CloudShadow, EdgeFog}
// skipping Shoreline/SSAO/BoxDecals) is NOT contiguous -> the fusion is a REORDER, not a
// subgraph expansion.
// ---------------------------------------------------------------------------
constexpr bool vkSubgraphIsContiguous(const PostprocessSubPassId* fusedSet, int n) {
    if (n <= 0) return false;                 // empty fusion is not a meaningful subgraph
    int lo = kPostprocessSubPassOrderCount;   // sentinel-high
    int hi = -1;
    for (int i = 0; i < n; ++i) {
        const int idx = postprocessSubPassCanonicalIndex(fusedSet[i]);
        if (idx < 0) return false;            // unknown pass -> ill-formed
        // reject duplicates
        for (int j = 0; j < i; ++j)
            if (fusedSet[j] == fusedSet[i]) return false;
        if (idx < lo) lo = idx;
        if (idx > hi) hi = idx;
    }
    // Contiguous iff the span exactly equals the count (no gap swallowed an outside pass).
    return (hi - lo) == (n - 1);
}

// ---------------------------------------------------------------------------
// The CURRENTLY SHIPPED Vulkan postprocess subgraph fuses exactly EdgeFog + OobFog
// (VULKAN-POSTPROCESS-SUBGRAPH-1). Declare it and LOCK its legality at compile time:
// it MUST be contiguous. If a future edit adds a non-contiguous pass here, this
// static_assert fails — forcing the change through the reorder/parity path instead.
// ---------------------------------------------------------------------------
static constexpr PostprocessSubPassId kShippedPostprocessSubgraph[] = {
    PostprocessSubPassId::EdgeFog,
    PostprocessSubPassId::OobFog,
};
static constexpr int kShippedPostprocessSubgraphCount =
    sizeof(kShippedPostprocessSubgraph) / sizeof(kShippedPostprocessSubgraph[0]);

static_assert(vkSubgraphIsContiguous(kShippedPostprocessSubgraph,
                                     kShippedPostprocessSubgraphCount),
              "SUBGRAPH-CONTIGUITY-GUARD-1: the shipped Vulkan postprocess subgraph "
              "(EdgeFog+OobFog) must be CONTIGUOUS in canonical endScene() order. If this "
              "fires, a non-contiguous pass was added to the subgraph -- that is a REORDER "
              "experiment (scheduler/oracle + parity proof), not a subgraph expansion.");

// Order-array well-formedness (pure constexpr data -> static_assert).
static_assert(kPostprocessSubPassOrderCount == 8,
              "kPostprocessSubPassOrder row count changed; re-confirm against endScene() "
              "before adjusting (ScreenShadow..Composite).");

// The canonical index of every enumerator must equal its position (enum value == order idx).
constexpr bool postprocessSubPassOrderMatchesEnum() {
    for (int i = 0; i < kPostprocessSubPassOrderCount; ++i)
        if (static_cast<int>(kPostprocessSubPassOrder[i]) != i) return false;
    return true;
}
static_assert(postprocessSubPassOrderMatchesEnum(),
              "kPostprocessSubPassOrder must list enumerators in declaration order");

}} // namespace RenderCore::vulkan
