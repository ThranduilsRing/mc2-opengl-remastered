// SUBGRAPH-CONTIGUITY-GUARD-1 offline validation.
//
// Proves the standing rule encoded in RenderCore/postprocess_subpass_order.h: a Vulkan
// postprocess subgraph may ONLY fuse passes contiguous in canonical endScene() order.
// Positive cases (adjacent runs) must be contiguous; negative cases (fusions that skip an
// intervening pass) must be REJECTED. All offline, no GL/Vulkan/engine/smoke. The shipped
// {EdgeFog,OobFog} fusion is additionally locked by a header static_assert.
#include "doctest.h"
#include "RenderCore/postprocess_subpass_order.h"

using namespace RenderCore::vulkan;

TEST_SUITE("SubgraphContiguity") {

TEST_CASE("canonical order matches endScene() and the enum") {
    CHECK(kPostprocessSubPassOrderCount == 8);
    CHECK(postprocessSubPassOrderMatchesEnum());
    CHECK(kPostprocessSubPassOrder[0] == PostprocessSubPassId::ScreenShadow);
    CHECK(kPostprocessSubPassOrder[1] == PostprocessSubPassId::CloudShadow);
    CHECK(kPostprocessSubPassOrder[5] == PostprocessSubPassId::EdgeFog);
    CHECK(kPostprocessSubPassOrder[6] == PostprocessSubPassId::OobFog);
    CHECK(kPostprocessSubPassOrder[7] == PostprocessSubPassId::Composite);
}

TEST_CASE("POSITIVE: shipped EdgeFog+OobFog fusion is contiguous") {
    const PostprocessSubPassId fused[] = {
        PostprocessSubPassId::EdgeFog, PostprocessSubPassId::OobFog };
    CHECK(vkSubgraphIsContiguous(fused, 2));
    // and the header locked the shipped set at compile time:
    CHECK(vkSubgraphIsContiguous(kShippedPostprocessSubgraph,
                                 kShippedPostprocessSubgraphCount));
}

TEST_CASE("POSITIVE: ScreenShadow+CloudShadow (adjacent head) is contiguous") {
    const PostprocessSubPassId fused[] = {
        PostprocessSubPassId::ScreenShadow, PostprocessSubPassId::CloudShadow };
    CHECK(vkSubgraphIsContiguous(fused, 2));
}

TEST_CASE("POSITIVE: order-insensitive (OobFog listed before EdgeFog) still contiguous") {
    const PostprocessSubPassId fused[] = {
        PostprocessSubPassId::OobFog, PostprocessSubPassId::EdgeFog };
    CHECK(vkSubgraphIsContiguous(fused, 2));
}

TEST_CASE("NEGATIVE: CloudShadow+EdgeFog is NOT contiguous (Shoreline/SSAO/BoxDecals intervene)") {
    // The Cloud-Shadow fork: fusing CloudShadow into the fog subgraph skips 3 passes.
    const PostprocessSubPassId fused[] = {
        PostprocessSubPassId::CloudShadow, PostprocessSubPassId::EdgeFog };
    CHECK_FALSE(vkSubgraphIsContiguous(fused, 2));
}

TEST_CASE("NEGATIVE: ScreenShadow+Shoreline is NOT contiguous (CloudShadow intervenes)") {
    const PostprocessSubPassId fused[] = {
        PostprocessSubPassId::ScreenShadow, PostprocessSubPassId::Shoreline };
    CHECK_FALSE(vkSubgraphIsContiguous(fused, 2));
}

TEST_CASE("NEGATIVE: three passes skipping one (Shoreline..EdgeFog missing BoxDecals) rejected") {
    // Shoreline(2), SSAO(3), EdgeFog(5) -> skips BoxDecals(4): span 3 != count 3-1.
    const PostprocessSubPassId fused[] = {
        PostprocessSubPassId::Shoreline, PostprocessSubPassId::SSAO,
        PostprocessSubPassId::EdgeFog };
    CHECK_FALSE(vkSubgraphIsContiguous(fused, 3));
}

TEST_CASE("POSITIVE: three fully-adjacent passes (SSAO..EdgeFog) contiguous") {
    const PostprocessSubPassId fused[] = {
        PostprocessSubPassId::SSAO, PostprocessSubPassId::BoxDecals,
        PostprocessSubPassId::EdgeFog };
    CHECK(vkSubgraphIsContiguous(fused, 3));
}

TEST_CASE("degenerate inputs rejected (empty set, duplicate pass)") {
    CHECK_FALSE(vkSubgraphIsContiguous(nullptr, 0));   // empty fusion
    const PostprocessSubPassId dup[] = {
        PostprocessSubPassId::EdgeFog, PostprocessSubPassId::EdgeFog };
    CHECK_FALSE(vkSubgraphIsContiguous(dup, 2));        // duplicate -> ill-formed
}

} // TEST_SUITE
