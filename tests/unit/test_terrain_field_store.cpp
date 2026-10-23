// The terrain field store (GameOS/gameos/terrain_field_store.cpp): the CPU terrain fields both backends draw from.

#include "doctest.h"

#include "terrain_field_store.h"

#include <algorithm>
#include <vector>

TEST_CASE("terrain fields: a height patch lands row by row and changes the version") {
    mc2terrain::TerrainFieldStore store;
    std::vector<float> map(4 * 4, 0.0f);
    REQUIRE(store.setHeight(map.data(), 4));
    const uint64_t v0 = store.heightVersion();

    const float block[] = {1, 2, 3, 4};   // 2 x 2 at (2, 1)
    REQUIRE(store.patchHeight(block, 2, 1, 1, 1, 4));
    CHECK(store.heightVersion() != v0);
    CHECK(store.height()[1 * 4 + 2] == 1);
    CHECK(store.height()[1 * 4 + 3] == 2);
    CHECK(store.height()[2 * 4 + 2] == 3);
    CHECK(store.height()[2 * 4 + 3] == 4);
    CHECK(store.height()[0] == 0);
}

TEST_CASE("terrain fields: malformed writes are rejected and keep the previous field") {
    mc2terrain::TerrainFieldStore store;
    std::vector<float> map(3 * 3, 5.0f);
    REQUIRE(store.setHeight(map.data(), 3));
    const uint64_t v = store.heightVersion();
    const float block[] = {9, 9, 9, 9};
    CHECK_FALSE(store.patchHeight(block, 2, 0, 1, 1, 3));    // runs off the right edge
    CHECK_FALSE(store.patchHeight(block, 0, 0, 1, 1, 4));    // a different map
    CHECK_FALSE(store.patchHeight(nullptr, 0, 0, 1, 1, 3));
    CHECK_FALSE(store.setHeight(nullptr, 3));
    CHECK_FALSE(store.setHeight(map.data(), 0));
    CHECK(store.heightVersion() == v);
    CHECK(store.height()[8] == 5.0f);

    CHECK_FALSE(store.setCementWords(nullptr, 4));
    CHECK_FALSE(store.setVisualHeight(map.data(), 3, nullptr, 2));   // mips promised, none given
    CHECK(store.cementVersion() == 0);
    CHECK(store.visualHeightVersion() == 0);
}

TEST_CASE("terrain fields: the visual bake is the fine field followed by the mip maxes") {
    mc2terrain::TerrainFieldStore store;
    const float fine[] = {1, 2, 3, 4};
    const float mips[] = {7, 8};
    REQUIRE(store.setVisualHeight(fine, 2, mips, 2));
    CHECK(store.visualSide() == 2);
    CHECK(store.visualMipFloats() == 2);
    CHECK(store.visualHeight() == std::vector<float>{1, 2, 3, 4, 7, 8});

    const uint32_t words[] = {0x80000001u, 0};
    REQUIRE(store.setCementWords(words, 2));
    CHECK(store.cementWords().size() == 2);
    CHECK(store.cementVersion() != 0);
}

TEST_CASE("terrain fields: the shore sidecars keep their bytes and reject a bad size") {
    mc2terrain::TerrainFieldStore store;
    const uint8_t mask[2 * 1 * 4] = {1, 2, 3, 4, 5, 6, 7, 8};
    REQUIRE(store.setShorelineMask(mask, 2, 1));
    CHECK(store.shorelineMask().width == 2);
    CHECK(store.shorelineMask().texels == std::vector<uint8_t>(mask, mask + 8));
    const uint64_t v = store.shorelineMask().version;
    CHECK(v != 0);
    CHECK_FALSE(store.setShorelineMask(mask, 0, 1));
    CHECK_FALSE(store.setShorelineMask(nullptr, 2, 1));
    CHECK(store.shorelineMask().version == v);
    // The VK terrain island re-imports the mask on a version change, so a reload of identical bytes must still bump it.
    REQUIRE(store.setShorelineMask(mask, 2, 1));
    CHECK(store.shorelineMask().version > v);

    const float heights[3] = {1.5f, 2.5f, 3.5f};
    REQUIRE(store.setWaterShoreHeight(heights, 3, 1));
    CHECK(store.waterShoreHeight().texels == std::vector<float>{1.5f, 2.5f, 3.5f});
    CHECK_FALSE(store.setWaterShoreHeight(heights, 1, 70000));
}

TEST_CASE("terrain fields: a recreated store never repeats a version an earlier store handed out") {
    std::vector<float> map(2 * 2, 0.0f);
    uint64_t first = 0;
    {
        mc2terrain::TerrainFieldStore store;
        REQUIRE(store.setHeight(map.data(), 2));
        first = store.heightVersion();
    }
    mc2terrain::TerrainFieldStore again;
    REQUIRE(again.setHeight(map.data(), 2));
    CHECK(again.heightVersion() > first);
}

TEST_CASE("terrain field store: the cement atlas is kept with a fresh version, malformed input keeps the previous one") {
    mc2terrain::TerrainFieldStore store;
    const uint8_t px[2 * 2 * 4] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    REQUIRE(store.setCementAtlas(px, 2, 2));
    const uint64_t v = store.cementAtlas().version;
    CHECK(v != 0);
    CHECK(store.cementAtlas().texels.size() == 16);
    CHECK(store.cementAtlas().texels[15] == 16);
    CHECK_FALSE(store.setCementAtlas(nullptr, 2, 2));
    CHECK_FALSE(store.setCementAtlas(px, 0, 2));
    CHECK(store.cementAtlas().version == v);
    REQUIRE(store.setCementAtlas(px, 2, 2));
    CHECK(store.cementAtlas().version != v);
}

TEST_CASE("terrain field store: the colormap atlas keeps RGBA8 or BC7 level 0 and rejects a byte count that is not one level") {
    mc2terrain::TerrainFieldStore store;
    const std::vector<uint8_t> rgba(4 * 2 * 4, 7);
    CHECK(store.setColormapAtlas(rgba, 4, 2, mc2terrain::ColormapAtlas::kRgba8));
    const uint64_t v = store.colormapAtlas().version;
    CHECK(v != 0);
    const std::vector<uint8_t> bc7(2 * 1 * 16, 9);   // 5x4 texels = 2x1 blocks
    CHECK(store.setColormapAtlas(bc7, 5, 4, mc2terrain::ColormapAtlas::kBc7Srgb));
    CHECK(store.colormapAtlas().version != v);
    CHECK(store.colormapAtlas().vkFormat == mc2terrain::ColormapAtlas::kBc7Srgb);
    const uint64_t v2 = store.colormapAtlas().version;
    CHECK_FALSE(store.setColormapAtlas(std::span<const uint8_t>(bc7).first(bc7.size() - 1), 5, 4, mc2terrain::ColormapAtlas::kBc7Unorm));
    CHECK_FALSE(store.setColormapAtlas(rgba, 4, 2, 99u));
    CHECK_FALSE(store.setColormapAtlas({}, 4, 2, mc2terrain::ColormapAtlas::kRgba8));
    CHECK(store.colormapAtlas().version == v2);
    CHECK(store.colormapAtlas().width == 5);
}

TEST_CASE("terrain fields: source sidecars own their bytes and survive caller reuse") {
    mc2terrain::TerrainFieldStore store;
    std::vector<uint8_t> rgba(16, 37);
    float types[] = {1, 2, 3, 4};
    REQUIRE(store.setControlMap(rgba.data(), 2));
    REQUIRE(store.setOverlay(rgba.data(), 2, 2));
    REQUIRE(store.setTerrainTypes(types, 2));
    rgba.assign(16, 99); types[0] = 99;
    CHECK(store.controlMap().texels[0] == 37);
    CHECK(store.overlay().texels[15] == 37);
    CHECK(store.terrainTypes().texels[0] == 1);
    const auto version = store.overlay().version;
    CHECK_FALSE(store.setOverlay(nullptr, 2, 2));
    CHECK(store.overlay().version == version);
    REQUIRE(store.setOverlay(rgba.data(), 2, 2));
    CHECK(store.overlay().version != version);
    CHECK(store.overlay().texels[0] == 99);
    CHECK(store.controlMap().texels[0] == 37);
    store = mc2terrain::TerrainFieldStore{};
    CHECK(store.controlMap().texels.empty());
    CHECK(store.overlay().texels.empty());
    CHECK(store.terrainTypes().texels.empty());
}

namespace {
// A square BC7 layer of `side` with a full mip chain whose every byte is `fill`.
RenderCore::KtxImage bc7Layer(int side, uint8_t fill) {
    RenderCore::KtxImage l;
    l.width = l.height = side; l.vkFormat = 145; l.isCompressed = true; l.blockSizeBytes = 16;
    for (int s = side; ; s = std::max(1, s / 2)) {
        l.mipByteOffsets.push_back(l.pixels.size());
        const size_t blocks = size_t((s + 3) / 4);
        l.pixels.insert(l.pixels.end(), blocks * blocks * 16, fill);
        ++l.mipCount;
        if (s == 1) break;
    }
    return l;
}
}  // namespace

TEST_CASE("terrain fields: cooked detail-normal layers compose level-major and reject partial chains") {
    mc2terrain::TerrainFieldStore store;
    const RenderCore::KtxImage layers[] = {bc7Layer(8, 1), bc7Layer(8, 2), bc7Layer(8, 3)};
    REQUIRE(store.setMaterialNormals(layers));
    const auto& array = store.materialNormals();
    REQUIRE(array.image);
    CHECK(array.layers == 3);
    CHECK(array.image->mipCount == 4);   // 8, 4, 2, 1
    // Level 0 is 2x2 blocks per layer (64 bytes), then levels 1-3 are one block per layer.
    REQUIRE(array.image->mipByteOffsets == std::vector<uint64_t>{0, 192, 240, 288});
    CHECK(array.image->pixels.size() == 336);
    CHECK(array.image->pixels[0] == 1); CHECK(array.image->pixels[64] == 2); CHECK(array.image->pixels[128] == 3);
    CHECK(array.image->pixels[192] == 1); CHECK(array.image->pixels[208] == 2); CHECK(array.image->pixels[224] == 3);
    const uint64_t v = array.version;

    // One layer without its mip chain (or a different size, or RGBA8) clears the array: the TGA import takes over.
    RenderCore::KtxImage noMips = bc7Layer(8, 4);
    noMips.mipCount = 1; noMips.mipByteOffsets.resize(1); noMips.pixels.resize(64);
    const RenderCore::KtxImage partial[] = {bc7Layer(8, 1), noMips};
    CHECK_FALSE(store.setMaterialNormals(partial));
    CHECK_FALSE(store.materialNormals().image);
    const RenderCore::KtxImage mixed[] = {bc7Layer(8, 1), bc7Layer(16, 1)};
    CHECK_FALSE(store.setMaterialNormals(mixed));
    REQUIRE(store.setMaterialNormals(layers));
    CHECK(store.materialNormals().version != v);
    CHECK_FALSE(store.setMaterialNormals({}));
    CHECK_FALSE(store.materialNormals().image);
}
