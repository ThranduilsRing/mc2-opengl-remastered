#pragma once
// VULKAN-LAYOUT-CHAIN-VALIDATOR-2 — GL-free, OFFLINE image-layout-chain contract + validators.
//
// This is the FIRST image-layout-chain validator (slice 2 of the Vulkan-descriptor
// substrate: slice 1 = descriptor-CLASS coverage in vulkan_contract.h; slice 2 = this,
// the per-sampled-image LAYOUT chain). For every FBO-backed image that vulkan_contract.h
// classifies as a CombinedImageSampler, it records the Vulkan image LAYOUT that image
// must be in when it is sampled downstream (VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL /
// DEPTH_STENCIL_READ_ONLY / DEPTH_READ_ONLY / GENERAL), plus the producing pass and the
// first consuming pass (both verified against kRenderPassContracts reads[]/writes[] and
// kFramePassOrder). Two constexpr validators then prove:
//
//   I3 (layout completeness) — every pass READ that is a CombinedImageSampler (per
//        kVkResourceContracts) has a layout-chain row whose sampledAsLayout != Undefined.
//   I4 (producer presence)   — every such sampled read is either WRITTEN by an upstream
//        pass in kFramePassOrder, OR its lifetime (staticResourceLifetime) is
//        Mission/Persistent/External (seeded/persistent/temporal — no in-frame producer
//        needed). A sampled image that is neither is a genuine modeling gap.
//
// ★ PROOF-ONLY. SCHEMA + VALIDATION, NOT authoritative, NOT wired into the renderer.
//   ZERO runtime callers; included only by the offline doctest suite
//   (tests/unit/test_vulkan_layout_chain.cpp). Byte-identical by construction — exactly
//   the discipline of vulkan_contract.h / scheduler_legal_reorder.h.
//
// Firewall: header-only, no GL includes, no game-side includes. Includes ONLY the GL-free
// substrate headers (registry enum, pass contract, descriptor-class contract, static
// lifetime map).

#include <cstdint>
#include "RenderResourceRegistry.h"       // RenderResourceId, RenderResourceKind, RenderResourceLifetime
#include "RenderPassContract.h"           // RenderPassId, kRenderPassContracts, kFramePassOrder
#include "vulkan_contract.h"              // kVkResourceContracts, VkDescriptorClass
#include "scheduler_legal_reorder.h"      // framegraph::staticResourceLifetime, isExternalLifetime

namespace RenderCore { namespace vulkan {

// ---------------------------------------------------------------------------
// Vulkan image layout a sampled image must be in when read downstream.
// (Subset of VkImageLayout relevant to the sampled-read side of the chain; the
// attachment-write layouts already live in RenderPassContract.h's ImageLayout.)
// ---------------------------------------------------------------------------
enum class VkImageLayoutRead : uint8_t {
    Undefined = 0,           // VK_IMAGE_LAYOUT_UNDEFINED — sentinel; a row at this is a bug (I3)
    ShaderReadOnly,          // VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL (color sampled)
    DepthStencilReadOnly,    // VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL (compare sampler)
    DepthReadOnly,           // VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL (plain depth sample)
    General,                 // VK_IMAGE_LAYOUT_GENERAL (compute/transfer copy target sampled)
};

inline const char* vkImageLayoutReadName(VkImageLayoutRead l) {
    switch (l) {
        case VkImageLayoutRead::Undefined:            return "Undefined";
        case VkImageLayoutRead::ShaderReadOnly:       return "ShaderReadOnly";
        case VkImageLayoutRead::DepthStencilReadOnly: return "DepthStencilReadOnly";
        case VkImageLayoutRead::DepthReadOnly:        return "DepthReadOnly";
        case VkImageLayoutRead::General:              return "General";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// A single sampled-image layout-chain row. HAND-DECLARED, reviewed at change time.
//   id                — the CombinedImageSampler resource (must be a sampler row in
//                       kVkResourceContracts).
//   sampledAsLayout   — layout the image must be in when a downstream pass samples it.
//   producerPass      — first pass in kFramePassOrder that WRITES id (RenderPassId::None
//                       iff the resource is seeded/persistent and has no in-frame writer).
//   firstConsumerPass — first pass in kFramePassOrder that READS id.
//   note              — human-readable derivation.
// ---------------------------------------------------------------------------
struct VkImageLayoutChain {
    RenderResourceId id;
    VkImageLayoutRead sampledAsLayout;
    RenderPassId     producerPass;
    RenderPassId     firstConsumerPass;
    const char*      note;
};

// ---------------------------------------------------------------------------
// kVkImageLayoutChains[] — one row per CombinedImageSampler image in
// kVkResourceContracts whose producer/consumer passes are verifiable from
// kRenderPassContracts reads[]/writes[]. There are exactly 4 sampler rows in
// vulkan_contract.h: MainColor, MainDepth, MainNormal, ShadowDynamicMap.
//
// Producer/firstConsumer derived from kFramePassOrder against the shipped
// reads[]/writes[]:
//   ShadowDynamicMap: written by Shadow (order[0]); first read by StaticPropOpaque.
//   MainColor:        written by StaticPropOpaque; first read by PostProcess.
//   MainDepth:        written by StaticPropOpaque; first read by Water (Water precedes
//                     VegetationCards/VFX/PostProcess in kFramePassOrder).
//   MainNormal:       written by StaticPropOpaque; first read by PostProcess.
//
// Layout derivation from resource semantics:
//   ShadowDynamicMap — depth compare-sampler (shadow PCF) -> DepthStencilReadOnly.
//   MainDepth        — plain depth sampled for soft-depth reject -> DepthReadOnly.
//   MainColor / MainNormal — color / GBuffer sampled -> ShaderReadOnly.
// ---------------------------------------------------------------------------
static constexpr VkImageLayoutChain kVkImageLayoutChains[] = {
    { RenderResourceId::ShadowDynamicMap, VkImageLayoutRead::DepthStencilReadOnly,
      RenderPassId::Shadow, RenderPassId::StaticPropOpaque,
      "dynamic shadow depth; compare-sampled by StaticProp/Terrain/Mech/PostProcess "
      "-> DEPTH_STENCIL_READ_ONLY" },
    { RenderResourceId::MainColor, VkImageLayoutRead::ShaderReadOnly,
      RenderPassId::StaticPropOpaque, RenderPassId::PostProcess,
      "scene HDR color (sceneFBO attach0); sampled by PostProcess -> SHADER_READ_ONLY" },
    { RenderResourceId::MainDepth, VkImageLayoutRead::DepthReadOnly,
      RenderPassId::StaticPropOpaque, RenderPassId::Water,
      "scene depth; sampled by Water/Veg/VFX/PostProcess (soft depth) -> DEPTH_READ_ONLY" },
    { RenderResourceId::MainNormal, VkImageLayoutRead::ShaderReadOnly,
      RenderPassId::StaticPropOpaque, RenderPassId::PostProcess,
      "GBuffer1 normal (attach1); sampled by PostProcess SSAO/Shoreline/ScreenShadow "
      "-> SHADER_READ_ONLY" },
};

static constexpr int kVkImageLayoutChainCount =
    sizeof(kVkImageLayoutChains) / sizeof(kVkImageLayoutChains[0]);

// ---------------------------------------------------------------------------
// Helpers.
// ---------------------------------------------------------------------------

// Is `id` a CombinedImageSampler per the descriptor-class contract?
constexpr bool vkIsCombinedImageSampler(RenderResourceId id) {
    for (int i = 0; i < kVkResourceContractCount; ++i)
        if (kVkResourceContracts[i].id == id
            && kVkResourceContracts[i].cls == VkDescriptorClass::CombinedImageSampler)
            return true;
    return false;
}

// Find the layout-chain row for `id` (nullptr if absent).
constexpr const VkImageLayoutChain* vkFindLayoutChain(RenderResourceId id) {
    for (int i = 0; i < kVkImageLayoutChainCount; ++i)
        if (kVkImageLayoutChains[i].id == id) return &kVkImageLayoutChains[i];
    return nullptr;
}

// Does any pass BEFORE `order[consumerIdx]` in `order` write `id`?
// NOTE: inline (not constexpr) — framegraph::findContract is an inline helper, so this
// walk is a RUNTIME check proven by the doctest CHECKs, not a static_assert. The table
// well-formedness guards below remain constexpr/static_assert.
inline bool vkHasUpstreamProducer(RenderResourceId id, int consumerIdx) {
    for (int p = 0; p < consumerIdx; ++p) {
        const RenderPassContract* pc =
            framegraph::findContract(kRenderPassContracts, kRenderPassIdCount, kFramePassOrder[p]);
        if (!pc) continue;
        for (int w = 0; w < 4; ++w) {
            if (pc->writes[w] == RenderResourceId::Unknown) break;
            if (pc->writes[w] == id) return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// I3 — layout completeness. For every pass READ (in kFramePassOrder) that is a
// CombinedImageSampler, assert a layout-chain row exists with sampledAsLayout != Undefined.
// On failure, *offending (if non-null) receives the first offending resource id.
// inline (runtime) — walks the pass table via framegraph::findContract; proven by doctest.
// ---------------------------------------------------------------------------
inline bool vkLayoutChainCompleteForAllSampledImages(RenderResourceId* offending = nullptr) {
    for (int o = 0; o < kFramePassOrderCount; ++o) {
        const RenderPassContract* c =
            framegraph::findContract(kRenderPassContracts, kRenderPassIdCount, kFramePassOrder[o]);
        if (!c) continue;
        for (int r = 0; r < 4; ++r) {
            const RenderResourceId rid = c->reads[r];
            if (rid == RenderResourceId::Unknown) break;
            if (!vkIsCombinedImageSampler(rid)) continue;
            const VkImageLayoutChain* row = vkFindLayoutChain(rid);
            if (!row || row->sampledAsLayout == VkImageLayoutRead::Undefined) {
                if (offending) *offending = rid;
                return false;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// I4 — producer presence. For every sampled read, assert an upstream pass writes it OR
// its lifetime is Mission/Persistent/External (framegraph::isExternalLifetime). A sampled
// image with neither is a real modeling gap. On failure *offending receives the id.
// inline (runtime) — walks the pass table via framegraph::findContract; proven by doctest.
// ---------------------------------------------------------------------------
inline bool vkLayoutChainAllSampledImagesHaveProducers(RenderResourceId* offending = nullptr) {
    for (int o = 0; o < kFramePassOrderCount; ++o) {
        const RenderPassContract* c =
            framegraph::findContract(kRenderPassContracts, kRenderPassIdCount, kFramePassOrder[o]);
        if (!c) continue;
        for (int r = 0; r < 4; ++r) {
            const RenderResourceId rid = c->reads[r];
            if (rid == RenderResourceId::Unknown) break;
            if (!vkIsCombinedImageSampler(rid)) continue;
            if (vkHasUpstreamProducer(rid, o)) continue;
            if (framegraph::isExternalLifetime(rid)) continue;
            if (offending) *offending = rid;
            return false;
        }
    }
    return true;
}

// No duplicate ids in the layout-chain table.
constexpr bool vkLayoutChainNoDuplicateIds() {
    for (int i = 0; i < kVkImageLayoutChainCount; ++i)
        for (int j = i + 1; j < kVkImageLayoutChainCount; ++j)
            if (kVkImageLayoutChains[i].id == kVkImageLayoutChains[j].id)
                return false;
    return true;
}

// Every layout-chain row keys on a real CombinedImageSampler resource with a real layout.
constexpr bool vkLayoutChainRowsWellFormed() {
    for (int i = 0; i < kVkImageLayoutChainCount; ++i) {
        const VkImageLayoutChain& r = kVkImageLayoutChains[i];
        if (!vkIsCombinedImageSampler(r.id)) return false;
        if (r.sampledAsLayout == VkImageLayoutRead::Undefined) return false;
        if (r.firstConsumerPass == RenderPassId::None) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Compile-time guards. Table-shape invariants that touch ONLY constexpr data
// (my own table + the constexpr kVkResourceContracts scan) are static_asserts and
// fire at build time, mirroring vulkan_contract.h. I3/I4 walk the pass table via the
// inline framegraph::findContract helper, so they are RUNTIME validators proven by the
// doctest CHECKs (test_vulkan_layout_chain.cpp) rather than static_asserts.
// ---------------------------------------------------------------------------
static_assert(kVkImageLayoutChainCount == 4,
              "kVkImageLayoutChains row count changed; this sampled-read chain covers the 4 "
              "FBO-backed sampler images consumed by passes in kFramePassOrder "
              "(MainColor/MainDepth/MainNormal/ShadowDynamicMap). The subgraph-internal "
              "PostprocessSubgraphDepth is NOT a kFramePassOrder read — its layout is modeled by "
              "the transition-chain table below, not this sampled-read table.");
static_assert(vkLayoutChainRowsWellFormed(),
              "a layout-chain row keys on a non-sampler id, has Undefined layout, or None consumer");
static_assert(vkLayoutChainNoDuplicateIds(),
              "vulkan_layout_chain: duplicate RenderResourceId in the table");

// ===========================================================================
// POSTPROCESS-VK-IMAGE-OWNERSHIP-1 — ORDERED MULTI-STATE LAYOUT-TRANSITION CHAINS.
//
// The sampled-read model above records the SINGLE layout an FBO image must be in when a
// downstream pass samples it. The Layer-4 postprocess subgraph is different: it OWNS its
// intermediate images and moves each through an ORDERED SEQUENCE of layouts inside one
// render-pass boundary (copy-in -> render -> copy-out). To fuse EdgeFog+OOB-fog into a
// native Vulkan subgraph we must model that whole transition CHAIN, not just one read
// layout. This is the real Layer-4 extension: it ADDS a transition-chain layer; it does
// NOT replace the I3/I4 sampled-read validators above.
//
// Owned subgraph images (from the slice contract):
//   PostprocessSubgraphColor (R16G16B16A16_SFLOAT): copied-in from GL sceneColor, blended by
//     BOTH fog passes (edge then oob) in ONE render pass, copied-out to GL.
//       UNDEFINED -> TRANSFER_DST -> COLOR_ATTACHMENT -> TRANSFER_SRC
//   PostprocessSubgraphDepth (D32_SFLOAT): copied-in from GL sceneDepth, SAMPLED by both passes.
//       UNDEFINED -> TRANSFER_DST -> SHADER_READ_ONLY
// ===========================================================================

// GL-free / Vulkan-free stand-in for VkImageLayout (a plain enum, NOT the real type).
enum class VkImageLayoutState : uint8_t {
    Undefined = 0,            // VK_IMAGE_LAYOUT_UNDEFINED — legal ONLY as the first state.
    TransferDst,              // VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL (copy-in target)
    TransferSrc,              // VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL (copy-out source; terminal-ish)
    ColorAttachment,          // VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
    ShaderReadOnly,           // VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    DepthStencilAttachment,   // VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
    General,                  // VK_IMAGE_LAYOUT_GENERAL
};

inline const char* vkImageLayoutStateName(VkImageLayoutState s) {
    switch (s) {
        case VkImageLayoutState::Undefined:              return "Undefined";
        case VkImageLayoutState::TransferDst:            return "TransferDst";
        case VkImageLayoutState::TransferSrc:            return "TransferSrc";
        case VkImageLayoutState::ColorAttachment:        return "ColorAttachment";
        case VkImageLayoutState::ShaderReadOnly:         return "ShaderReadOnly";
        case VkImageLayoutState::DepthStencilAttachment: return "DepthStencilAttachment";
        case VkImageLayoutState::General:                return "General";
    }
    return "?";
}

// Max states in a modeled chain (copy-in/render/copy-out is 4). Small fixed cap keeps the
// row a constexpr aggregate with no heap.
static constexpr int kVkMaxTransitionStates = 6;

// One owned-image transition chain. HAND-DECLARED, reviewed at change time.
//   id           — the subgraph-owned image (a real RenderResourceId).
//   states[]     — ordered layout sequence; unused tail entries stay Undefined and are
//                  ignored by walking only [0, stateCount).
//   stateCount   — number of live entries in states[].
//   copyOutColor — true iff this image is copied back OUT to GL after the subgraph (i.e. it
//                  must END in a transfer-src-capable state). Depth (sampled-only) is false.
//   note         — human-readable derivation.
struct VkImageTransitionChain {
    RenderResourceId  id;
    VkImageLayoutState states[kVkMaxTransitionStates];
    int               stateCount;
    bool              copyOutColor;
    const char*       note;
};

static constexpr VkImageTransitionChain kVkImageTransitionChains[] = {
    { RenderResourceId::PostprocessSubgraphColor,
      { VkImageLayoutState::Undefined, VkImageLayoutState::TransferDst,
        VkImageLayoutState::ColorAttachment, VkImageLayoutState::TransferSrc,
        VkImageLayoutState::Undefined, VkImageLayoutState::Undefined },
      4, /*copyOutColor=*/true,
      "subgraph COLOR (R16G16B16A16_SFLOAT): copy-in -> both fog draws (edge,oob) in one "
      "render pass -> copy-out. UNDEFINED->TRANSFER_DST->COLOR_ATTACHMENT->TRANSFER_SRC" },
    { RenderResourceId::PostprocessSubgraphDepth,
      { VkImageLayoutState::Undefined, VkImageLayoutState::TransferDst,
        VkImageLayoutState::ShaderReadOnly,
        VkImageLayoutState::Undefined, VkImageLayoutState::Undefined,
        VkImageLayoutState::Undefined },
      3, /*copyOutColor=*/false,
      "subgraph DEPTH (D32_SFLOAT): copy-in from GL sceneDepth, sampled read-only by both "
      "fog passes. UNDEFINED->TRANSFER_DST->SHADER_READ_ONLY (no copy-out)" },
};

static constexpr int kVkImageTransitionChainCount =
    sizeof(kVkImageTransitionChains) / sizeof(kVkImageTransitionChains[0]);

// A transition FROM `a` TO `b` is legal iff:
//   - `b` is never Undefined (Undefined is a start-only sentinel; you cannot transition INTO it);
//   - you cannot leave a terminal TransferSrc (copy-out source is chain-terminal);
//   - identity (a==b) is NOT a transition and is rejected as a degenerate step.
constexpr bool vkTransitionLegal(VkImageLayoutState a, VkImageLayoutState b) {
    if (b == VkImageLayoutState::Undefined) return false;   // can't transition into Undefined
    if (a == VkImageLayoutState::TransferSrc) return false;  // TransferSrc is terminal-ish
    if (a == b) return false;                                // no-op step is a modeling bug
    return true;
}

// Validate ONE chain:
//   - non-empty;
//   - starts at Undefined, and Undefined appears ONLY at index 0;
//   - every adjacent transition is legal (vkTransitionLegal);
//   - every listed state is distinct (each state reachable exactly once — no cycles);
//   - if copyOutColor, the chain ENDS in TransferSrc (copy-out capable).
constexpr bool vkTransitionChainValid(const VkImageTransitionChain& c) {
    if (c.stateCount <= 0 || c.stateCount > kVkMaxTransitionStates) return false;
    if (c.states[0] != VkImageLayoutState::Undefined) return false;
    for (int i = 0; i < c.stateCount; ++i) {
        // Undefined only at index 0.
        if (i > 0 && c.states[i] == VkImageLayoutState::Undefined) return false;
        // Distinctness (every state reachable, no repeat/cycle).
        for (int j = i + 1; j < c.stateCount; ++j)
            if (c.states[i] == c.states[j]) return false;
        // Legal adjacent transition.
        if (i > 0 && !vkTransitionLegal(c.states[i - 1], c.states[i])) return false;
    }
    if (c.copyOutColor && c.states[c.stateCount - 1] != VkImageLayoutState::TransferSrc)
        return false;
    return true;
}

// All chains valid.
constexpr bool vkAllTransitionChainsValid(RenderResourceId* offending = nullptr) {
    for (int i = 0; i < kVkImageTransitionChainCount; ++i)
        if (!vkTransitionChainValid(kVkImageTransitionChains[i])) {
            if (offending) *offending = kVkImageTransitionChains[i].id;
            return false;
        }
    return true;
}

// No duplicate ids in the transition-chain table.
constexpr bool vkTransitionChainsNoDuplicateIds() {
    for (int i = 0; i < kVkImageTransitionChainCount; ++i)
        for (int j = i + 1; j < kVkImageTransitionChainCount; ++j)
            if (kVkImageTransitionChains[i].id == kVkImageTransitionChains[j].id)
                return false;
    return true;
}

// Find a transition chain by id (nullptr if absent).
constexpr const VkImageTransitionChain* vkFindTransitionChain(RenderResourceId id) {
    for (int i = 0; i < kVkImageTransitionChainCount; ++i)
        if (kVkImageTransitionChains[i].id == id) return &kVkImageTransitionChains[i];
    return nullptr;
}

// Compile-time guards for the transition-chain layer (pure constexpr data -> static_assert).
static_assert(kVkImageTransitionChainCount == 2,
              "kVkImageTransitionChains row count changed; the Layer-4 subgraph owns exactly "
              "two intermediate images (PostprocessSubgraphColor + PostprocessSubgraphDepth)");
static_assert(vkAllTransitionChainsValid(),
              "a subgraph image transition chain is malformed (bad start/transition/terminal "
              "or a color chain not ending in TransferSrc)");
static_assert(vkTransitionChainsNoDuplicateIds(),
              "vulkan_layout_chain: duplicate RenderResourceId in the transition-chain table");

}} // namespace RenderCore::vulkan
