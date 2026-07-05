// VULKAN-LAYOUT-CHAIN-VALIDATOR-2 offline validation.
//
// Proves the per-sampled-image Vulkan layout chain (vulkan_layout_chain.h) is COMPLETE
// (I3) and every sampled image has a PRODUCER (I4). Every CombinedImageSampler read in
// the shipped pass order carries a layout-chain row with a real sampled-as layout, and is
// either produced upstream in kFramePassOrder or has a Mission/Persistent/External
// lifetime. All offline, no GL/engine/smoke. Table-shape guards are already proven at
// compile time by the header's static_asserts; these doctest cases make the
// completeness/producer invariants visible and named at run time, plus a negative check.
#include "doctest.h"
#include "RenderCore/vulkan_layout_chain.h"

using namespace RenderCore;
using namespace RenderCore::vulkan;

TEST_SUITE("VulkanLayoutChain") {

TEST_CASE("row count is pinned and every row is well-formed (matches header static_asserts)") {
    CHECK(kVkImageLayoutChainCount == 4);
    CHECK(vkLayoutChainRowsWellFormed());
    CHECK(vkLayoutChainNoDuplicateIds());
    for (int i = 0; i < kVkImageLayoutChainCount; ++i) {
        const VkImageLayoutChain& r = kVkImageLayoutChains[i];
        INFO("row ", i, " id=", static_cast<unsigned>(r.id),
             " layout=", vkImageLayoutReadName(r.sampledAsLayout));
        CHECK(vkIsCombinedImageSampler(r.id));
        CHECK(r.sampledAsLayout != VkImageLayoutRead::Undefined);
    }
}

TEST_CASE("I3: every CombinedImageSampler pass-read has a real layout-chain row") {
    RenderResourceId offending = RenderResourceId::Unknown;
    CHECK(vkLayoutChainCompleteForAllSampledImages(&offending));
    CHECK(static_cast<unsigned>(offending) == 0u);   // Unknown == untouched
}

TEST_CASE("I4: every sampled image has an upstream producer or an external lifetime") {
    RenderResourceId offending = RenderResourceId::Unknown;
    CHECK(vkLayoutChainAllSampledImagesHaveProducers(&offending));
    CHECK(static_cast<unsigned>(offending) == 0u);
}

TEST_CASE("layout derivation: shadow is a compare sampler, color is shader-read, depth is depth-read") {
    const VkImageLayoutChain* shadow = vkFindLayoutChain(RenderResourceId::ShadowDynamicMap);
    REQUIRE(shadow != nullptr);
    CHECK(shadow->sampledAsLayout == VkImageLayoutRead::DepthStencilReadOnly);
    CHECK(shadow->producerPass == RenderPassId::Shadow);

    const VkImageLayoutChain* color = vkFindLayoutChain(RenderResourceId::MainColor);
    REQUIRE(color != nullptr);
    CHECK(color->sampledAsLayout == VkImageLayoutRead::ShaderReadOnly);

    const VkImageLayoutChain* depth = vkFindLayoutChain(RenderResourceId::MainDepth);
    REQUIRE(depth != nullptr);
    CHECK(depth->sampledAsLayout == VkImageLayoutRead::DepthReadOnly);
    CHECK(depth->firstConsumerPass == RenderPassId::Water);   // Water precedes Veg/VFX/PP

    const VkImageLayoutChain* normal = vkFindLayoutChain(RenderResourceId::MainNormal);
    REQUIRE(normal != nullptr);
    CHECK(normal->sampledAsLayout == VkImageLayoutRead::ShaderReadOnly);
}

TEST_CASE("negative: a non-sampler resource has no layout-chain row") {
    // Buffers are never CombinedImageSampler, so they are absent from the layout chain.
    CHECK_FALSE(vkIsCombinedImageSampler(RenderResourceId::LightDataSsbo));
    CHECK(vkFindLayoutChain(RenderResourceId::LightDataSsbo) == nullptr);
}

// --- POSTPROCESS-VK-IMAGE-OWNERSHIP-1: ordered transition-chain layer ---

TEST_CASE("transition-chain: row count pinned and both subgraph chains valid") {
    CHECK(kVkImageTransitionChainCount == 2);
    RenderResourceId offending = RenderResourceId::Unknown;
    CHECK(vkAllTransitionChainsValid(&offending));
    CHECK(static_cast<unsigned>(offending) == 0u);   // Unknown == untouched
    CHECK(vkTransitionChainsNoDuplicateIds());
}

TEST_CASE("transition-chain: subgraph COLOR is copy-in/attachment/copy-out ending in TransferSrc") {
    const VkImageTransitionChain* color =
        vkFindTransitionChain(RenderResourceId::PostprocessSubgraphColor);
    REQUIRE(color != nullptr);
    CHECK(color->stateCount == 4);
    CHECK(color->copyOutColor);
    CHECK(color->states[0] == VkImageLayoutState::Undefined);
    CHECK(color->states[1] == VkImageLayoutState::TransferDst);
    CHECK(color->states[2] == VkImageLayoutState::ColorAttachment);
    CHECK(color->states[3] == VkImageLayoutState::TransferSrc);   // copy-out capable terminal
    CHECK(vkTransitionChainValid(*color));
}

TEST_CASE("transition-chain: subgraph DEPTH is copy-in then shader-read (no copy-out)") {
    const VkImageTransitionChain* depth =
        vkFindTransitionChain(RenderResourceId::PostprocessSubgraphDepth);
    REQUIRE(depth != nullptr);
    CHECK(depth->stateCount == 3);
    CHECK_FALSE(depth->copyOutColor);
    CHECK(depth->states[0] == VkImageLayoutState::Undefined);
    CHECK(depth->states[1] == VkImageLayoutState::TransferDst);
    CHECK(depth->states[2] == VkImageLayoutState::ShaderReadOnly);
    CHECK(vkTransitionChainValid(*depth));
}

TEST_CASE("transition-chain NEGATIVE: illegal chains trip the validator") {
    // (1) Does NOT start at Undefined (starts straight at ColorAttachment).
    VkImageTransitionChain noUndefStart = {
        RenderResourceId::PostprocessSubgraphColor,
        { VkImageLayoutState::ColorAttachment, VkImageLayoutState::TransferSrc,
          VkImageLayoutState::Undefined, VkImageLayoutState::Undefined,
          VkImageLayoutState::Undefined, VkImageLayoutState::Undefined },
        2, true, "illegal: missing Undefined start" };
    CHECK_FALSE(vkTransitionChainValid(noUndefStart));

    // (2) copyOutColor chain that does NOT end in TransferSrc.
    VkImageTransitionChain colorNoCopyOut = {
        RenderResourceId::PostprocessSubgraphColor,
        { VkImageLayoutState::Undefined, VkImageLayoutState::TransferDst,
          VkImageLayoutState::ColorAttachment,
          VkImageLayoutState::Undefined, VkImageLayoutState::Undefined,
          VkImageLayoutState::Undefined },
        3, /*copyOutColor=*/true, "illegal: color chain not ending in TransferSrc" };
    CHECK_FALSE(vkTransitionChainValid(colorNoCopyOut));

    // (3) Undefined reappearing after index 0.
    VkImageTransitionChain undefTwice = {
        RenderResourceId::PostprocessSubgraphDepth,
        { VkImageLayoutState::Undefined, VkImageLayoutState::Undefined,
          VkImageLayoutState::ShaderReadOnly,
          VkImageLayoutState::Undefined, VkImageLayoutState::Undefined,
          VkImageLayoutState::Undefined },
        3, false, "illegal: Undefined after index 0" };
    CHECK_FALSE(vkTransitionChainValid(undefTwice));

    // (4) Transition OUT of terminal TransferSrc.
    VkImageTransitionChain afterTerminal = {
        RenderResourceId::PostprocessSubgraphColor,
        { VkImageLayoutState::Undefined, VkImageLayoutState::TransferSrc,
          VkImageLayoutState::ColorAttachment,
          VkImageLayoutState::Undefined, VkImageLayoutState::Undefined,
          VkImageLayoutState::Undefined },
        3, false, "illegal: state after terminal TransferSrc" };
    CHECK_FALSE(vkTransitionChainValid(afterTerminal));

    // (5) Empty chain.
    VkImageTransitionChain empty = {
        RenderResourceId::PostprocessSubgraphDepth,
        { VkImageLayoutState::Undefined, VkImageLayoutState::Undefined,
          VkImageLayoutState::Undefined, VkImageLayoutState::Undefined,
          VkImageLayoutState::Undefined, VkImageLayoutState::Undefined },
        0, false, "illegal: empty chain" };
    CHECK_FALSE(vkTransitionChainValid(empty));

    // Sanity: the transition predicate itself.
    CHECK(vkTransitionLegal(VkImageLayoutState::TransferDst,
                            VkImageLayoutState::ColorAttachment));
    CHECK_FALSE(vkTransitionLegal(VkImageLayoutState::ColorAttachment,
                                  VkImageLayoutState::Undefined));   // into Undefined
    CHECK_FALSE(vkTransitionLegal(VkImageLayoutState::TransferSrc,
                                  VkImageLayoutState::General));      // out of terminal
    CHECK_FALSE(vkTransitionLegal(VkImageLayoutState::General,
                                  VkImageLayoutState::General));      // no-op
}

} // TEST_SUITE("VulkanLayoutChain")
