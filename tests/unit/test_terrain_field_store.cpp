// The terrain field store (GameOS/gameos/terrain_field_store.cpp): the CPU terrain fields both backends draw from.

#include "doctest.h"

#include "terrain_field_store.h"

#include <algorithm>
#include <vector>
#include <cmath>
#include <random>
#include "water_shore_cells.h"

namespace shore_reference {
#include "../../shaders/vulkan/include/terrain_cliff_surface.glsl"

float smooth(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
template<class Raw> float height(Raw raw, int side, float wx, float wy, float half) {
    const float gx = std::clamp((wx + half) / 32.0f, 0.0f, float(side - 1));
    const float gy = std::clamp((half - wy) / 32.0f, 0.0f, float(side - 1));
    const int x = (std::min)(int(std::floor(gx)), side - 2), y = (std::min)(int(std::floor(gy)), side - 2);
    const float u = gx - x, v = gy - y;
    const float tl = cliffVisualHeight(raw, x, y), tr = cliffVisualHeight(raw, x + 1, y);
    const float bl = cliffVisualHeight(raw, x, y + 1), br = cliffVisualHeight(raw, x + 1, y + 1);
    if (std::abs(raw(x + 1, y) - raw(x, y + 1)) < std::abs(raw(x, y) - raw(x + 1, y + 1))) {
        if (u + v <= 1.0f) return tl + (tr - tl) * u + (bl - tl) * v;
        return br + (bl - br) * (1.0f - u) + (tr - br) * (1.0f - v);
    }
    const float b = v >= u ? bl : tr;
    return v >= u ? tl + (br - b) * u + (b - tl) * v : tl + (b - tl) * u + (br - b) * v;
}
template<class Raw> float nearShore(Raw raw, int side, float wx, float wy, float half, float elev) {
    const float h00 = height(raw, side, wx - 32.0f, wy - 32.0f, half);
    const float h10 = height(raw, side, wx, wy - 32.0f, half);
    const float h20 = height(raw, side, wx + 32.0f, wy - 32.0f, half);
    const float h01 = height(raw, side, wx - 32.0f, wy, half);
    const float h11 = height(raw, side, wx, wy, half);
    const float h21 = height(raw, side, wx + 32.0f, wy, half);
    const float h02 = height(raw, side, wx - 32.0f, wy + 32.0f, half);
    const float h12 = height(raw, side, wx, wy + 32.0f, half);
    const float h22 = height(raw, side, wx + 32.0f, wy + 32.0f, half);
    const float depth = elev - (h00 + 2.0f*h10 + h20 + 2.0f*h01 + 4.0f*h11 + 2.0f*h21 + h02 + 2.0f*h12 + h22)/16.0f;
    const float gx = -((h20 + 2.0f*h21 + h22) - (h00 + 2.0f*h01 + h02))/256.0f;
    const float gy = -((h02 + 2.0f*h12 + h22) - (h00 + 2.0f*h10 + h20))/256.0f;
    const float g = std::sqrt(gx*gx + gy*gy);
    return (1.0f - smooth(36.0f, 100.0f, (std::max)(depth, 0.0f)/(std::max)(g, 0.08f))) * smooth(0.04f, 0.08f, g);
}
} // namespace shore_reference

TEST_CASE("water shore cells: conservative against B nine taps and exact flat vertices") {
    constexpr int side = 49;
    const float inf = (std::numeric_limits<float>::max)();
    std::mt19937 rng(0x5a07);
    std::uniform_real_distribution<float> random(-120.0f, 120.0f);
    size_t finite = 0, flat = 0, skipped = 0;
    for (int field = 0; field < 80; ++field) {
        std::vector<float> fine(side * side);
        const float base = random(rng), sx = random(rng) * 0.05f, sy = random(rng) * 0.05f;
        for (int y = 0; y < side; ++y) for (int x = 0; x < side; ++x) {
            float h = base;
            if (field % 4 == 1) h += sx * x + sy * y;
            if (field % 4 == 2) h += random(rng);
            if (field % 4 == 3) h += (x > 24 ? 160.0f : 0.0f) + sy * y;
            fine[x + y * side] = h;
        }
        const auto raw = [&](int x, int y) {
            return fine[std::clamp(x, 0, side - 1) + std::clamp(y, 0, side - 1) * side];
        };
        const auto records = mc2terrain::bakeWaterShoreCells(fine.data(), side);
        REQUIRE(records.size() == 2 * fine.size());
        for (int y = 0; y < side; ++y) for (int x = 0; x < side; ++x) {
            const size_t i = 2 * (x + y * side);
            if (x == side - 1 || y == side - 1) {
                REQUIRE(records[i] == inf); REQUIRE(records[i + 1] == 0.0f);
            }
            if (records[i + 1] == 1.0f) {
                ++flat;
                for (int v = 0; v < 4; ++v) for (int u = 0; u < 4; ++u)
                    REQUIRE(shore_reference::cliffVisualHeight(raw, x + u, y + v) == raw(x + u, y + v));
            }
        }
        // Include boundaries and their immediate binary32 neighbours, both axes.
        const float fractions[] = {0.0f, 0.25f, 0.5f, 0.99999994f, 1.0f};
        const float half = (field & 1) ? 32768.0f : 768.0f;
        for (int y = 0; y < side - 1; ++y) for (int x = 0; x < side - 1; ++x)
            for (float f : fractions) {
                const float wx = (x + f) * 32.0f - half, wy = half - (y + f) * 32.0f;
                const int ox = (std::min)(int(std::floor(std::clamp((wx - 32.0f + half)/32.0f, 0.0f, float(side-1)))), side-2);
                const int oy = (std::min)(int(std::floor(std::clamp((half - (wy + 32.0f))/32.0f, 0.0f, float(side-1)))), side-2);
                const float threshold = records[2 * (ox + oy * side)];
                if (threshold == inf) continue; // always tent, including coarse border blend
                ++finite;
                const float elev = threshold == -inf ? base + random(rng) : threshold;
                CAPTURE(field); CAPTURE(x); CAPTURE(y); CAPTURE(f); CAPTURE(threshold); CAPTURE(elev);
                REQUIRE(shore_reference::nearShore(raw, side, wx, wy, half, elev) == 0.0f);
                REQUIRE(shore_reference::nearShore(raw, side, wx, wy, half, elev + 32.0f) == 0.0f);
                if (threshold == -inf) ++skipped;
            }
    }
    CHECK(finite > 10000); CHECK(flat > 1000); CHECK(skipped > 1000);
}

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
    CHECK(store.waterShoreCells().size() == 8);
    CHECK(store.waterShoreCells()[0] == (std::numeric_limits<float>::max)());
    CHECK(store.waterShoreCells()[1] == 0.0f);

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

TEST_CASE("terrain fields: shore edits and mission clear invalidate both consumers") {
    mc2terrain::TerrainFieldStore store;
    const uint8_t mask[4] = {1, 2, 3, 255};
    const float heights[1] = {-7.0f};
    REQUIRE(store.setShorelineMask(mask, 1, 1));
    const auto maskVersion = store.shorelineMask().version;
    REQUIRE(store.setWaterShoreHeight(heights, 1, 1, mc2terrain::WaterShoreHeightKind::ShoreSdf));
    const auto heightVersion = store.waterShoreHeight().version;
    CHECK(store.shorelineMask().version == maskVersion);
    store.clearShoreFields();
    CHECK(store.shorelineMask().version > maskVersion);
    CHECK(store.waterShoreHeight().version > heightVersion);
    CHECK(store.shorelineMask().texels.empty());
    CHECK(store.waterShoreHeight().texels.empty());
    CHECK(store.shorelineMask().width == 0);
    CHECK(store.waterShoreHeightKind() == mc2terrain::WaterShoreHeightKind::RawHeight);
    const auto clearedVersion = store.shorelineMask().version;
    REQUIRE(store.setShorelineMask(mask, 1, 1));
    CHECK(store.shorelineMask().version > clearedVersion);
    CHECK(store.waterShoreHeight().texels.empty());
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
    REQUIRE(static_cast<bool>(array.image));
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
    CHECK_FALSE(static_cast<bool>(store.materialNormals().image));
    const RenderCore::KtxImage mixed[] = {bc7Layer(8, 1), bc7Layer(16, 1)};
    CHECK_FALSE(store.setMaterialNormals(mixed));
    REQUIRE(store.setMaterialNormals(layers));
    CHECK(store.materialNormals().version != v);
    CHECK_FALSE(store.setMaterialNormals({}));
    CHECK_FALSE(static_cast<bool>(store.materialNormals().image));
}

TEST_CASE("terrain fields: lossless RGBA8 detail-normal layers compose level-major and never mix with BC7") {
    mc2terrain::TerrainFieldStore store;
    const auto rgba = [](int side, uint8_t fill) {
        RenderCore::KtxImage l;
        l.width = l.height = side; l.vkFormat = 37;
        for (int s = side; ; s = std::max(1, s / 2)) {
            l.mipByteOffsets.push_back(l.pixels.size());
            l.pixels.insert(l.pixels.end(), size_t(s) * size_t(s) * 4u, fill);
            ++l.mipCount;
            if (s == 1) break;
        }
        return l;
    };
    const RenderCore::KtxImage layers[] = {rgba(4, 1), rgba(4, 2)};
    REQUIRE(store.setMaterialNormals(layers));
    const auto& array = store.materialNormals();
    CHECK(array.image->vkFormat == 37); CHECK_FALSE(array.image->isCompressed);
    CHECK(array.image->mipCount == 3);   // 4, 2, 1
    REQUIRE(array.image->mipByteOffsets == std::vector<uint64_t>{0, 128, 160});
    CHECK(array.image->pixels.size() == 168);
    CHECK(array.image->pixels[0] == 1); CHECK(array.image->pixels[64] == 2); CHECK(array.image->pixels[128] == 1); CHECK(array.image->pixels[136] == 1); CHECK(array.image->pixels[144] == 2);
    const RenderCore::KtxImage mixed[] = {rgba(4, 1), bc7Layer(4, 1)};
    CHECK_FALSE(store.setMaterialNormals(mixed));
}

TEST_CASE("terrain fields: a one-plane cement write keeps the road-art plane") {
    mc2terrain::TerrainFieldStore store;
    const uint32_t two[] = {1, 2, 3, 4, 0x80010007u, 0x80010008u, 0, 0x80010009u};   // 2 x 2 map, 2 planes
    REQUIRE(store.setCementWords(two, 8));
    const uint32_t one[] = {9, 9, 9, 9};   // repopulate (lighting recalc)
    REQUIRE(store.setCementWords(one, 4));
    REQUIRE(store.cementWords().size() == 8);
    CHECK(store.cementWords()[0] == 9);
    CHECK(store.cementWords()[4] == 0x80010007u);
    CHECK(store.cementWords()[7] == 0x80010009u);
}

TEST_CASE("terrain fields: clearing the visual bake leaves no relief for the next mission") {
    mc2terrain::TerrainFieldStore store;
    const float fine[] = {1, 2, 3, 4};
    const float mips[] = {7, 8};
    REQUIRE(store.setVisualHeight(fine, 2, mips, 2));   // mission A: accepted bake
    const uint64_t accepted = store.visualHeightVersion();
    store.clearVisualHeight();                          // mission B: rejected or missing bake
    CHECK(store.visualHeight().empty());
    CHECK(store.visualSide() == 0);
    CHECK(store.visualMipFloats() == 0);
    CHECK(store.visualHeightVersion() != accepted);
}

#include "cement_layers.h"

// CEMENT-ARRAY-1: the cement catalog atlas (grid x grid cells) becomes one texture-array layer per cell.
TEST_CASE("cement layers: layer count is the catalog tile count and layer k is atlas cell k") {
    const int grid = 4, tile = 4, side = grid * tile, tiles = 10;   // 10 cells in a 4x4 grid: 6 grid cells are unused
    std::vector<uint8_t> atlas(static_cast<size_t>(side) * side * 4, 0);
    for (int k = 0; k < tiles; ++k)
        for (int y = 0; y < tile; ++y)
            for (int x = 0; x < tile; ++x) {
                uint8_t* p = &atlas[((static_cast<size_t>((k / grid) * tile + y)) * side + (k % grid) * tile + x) * 4];
                p[0] = static_cast<uint8_t>(k); p[1] = static_cast<uint8_t>(x); p[2] = static_cast<uint8_t>(y); p[3] = 255;
            }
    const std::vector<uint8_t> layers = mc2terrain::cementAtlasToLayers(atlas.data(), side, grid, tiles);
    REQUIRE(layers.size() == static_cast<size_t>(tiles) * tile * tile * 4);   // layers == tiles, not grid^2
    for (int k = 0; k < tiles; ++k)
        for (int y = 0; y < tile; ++y)
            for (int x = 0; x < tile; ++x) {
                const uint8_t* p = &layers[(static_cast<size_t>(k) * tile * tile + static_cast<size_t>(y) * tile + x) * 4];
                CHECK(p[0] == k); CHECK(p[1] == x); CHECK(p[2] == y); CHECK(p[3] == 255);
            }
}

TEST_CASE("cement layers: texels (including magenta-keyed ones) are copied bit-identically per layer") {
    // The catalog build keeps the magenta key texels as authored (it only counts them), so a layer must
    // carry exactly the atlas cell's bytes: nothing is zeroed, dilated or blended across layers.
    const int grid = 2, tile = 2, side = 4;
    std::vector<uint8_t> atlas(static_cast<size_t>(side) * side * 4);
    for (size_t i = 0; i < atlas.size(); ++i) atlas[i] = static_cast<uint8_t>(i * 7 + 3);
    atlas[0] = 255; atlas[1] = 0; atlas[2] = 255; atlas[3] = 255;   // a magenta texel at cell 0
    const std::vector<uint8_t> layers = mc2terrain::cementAtlasToLayers(atlas.data(), side, grid, 4);
    REQUIRE(layers.size() == 4u * tile * tile * 4);
    CHECK(layers[0] == 255); CHECK(layers[1] == 0); CHECK(layers[2] == 255); CHECK(layers[3] == 255);
    for (int k = 0; k < 4; ++k)
        for (int y = 0; y < tile; ++y)
            CHECK(std::equal(&layers[(static_cast<size_t>(k) * tile * tile + static_cast<size_t>(y) * tile) * 4],
                             &layers[(static_cast<size_t>(k) * tile * tile + static_cast<size_t>(y) * tile) * 4 + tile * 4],
                             &atlas[(static_cast<size_t>((k / grid) * tile + y) * side + (k % grid) * tile) * 4]));
}

TEST_CASE("cement layers: arguments that do not describe the atlas give no layers") {
    std::vector<uint8_t> atlas(8 * 8 * 4, 1);
    CHECK(mc2terrain::cementAtlasToLayers(nullptr, 8, 2, 4).empty());
    CHECK(mc2terrain::cementAtlasToLayers(atlas.data(), 8, 0, 4).empty());
    CHECK(mc2terrain::cementAtlasToLayers(atlas.data(), 8, 3, 4).empty());   // 8 is not a multiple of 3
    CHECK(mc2terrain::cementAtlasToLayers(atlas.data(), 8, 2, 5).empty());   // more layers than grid cells
    CHECK(mc2terrain::cementAtlasToLayers(atlas.data(), 8, 2, 0).empty());
}

TEST_CASE("fog floor: the regional low ground under each vertex, rebuilt after a height patch") {
    // A V valley down the middle column of a 33 x 33 map (128 wu cells): floor 0 at x = 16, walls rising 40 wu a cell.
    constexpr int side = 33, mid = 16;
    std::vector<float> h(static_cast<size_t>(side) * side);
    for (int y = 0; y < side; ++y)
        for (int x = 0; x < side; ++x) h[static_cast<size_t>(y) * side + x] = 40.0f * float(std::abs(x - mid));
    mc2terrain::TerrainFieldStore store;
    CHECK(store.fogFloor().empty());   // no height field yet
    REQUIRE(store.setHeight(h.data(), side));
    const auto floorAt = [&](int x, int y) { return store.fogFloor()[static_cast<size_t>(y) * side + x]; };

    // Inside the 1024 wu minimum radius the floor is the valley minimum (the blur of a flat zero band stays zero).
    for (int x = mid - 6; x <= mid + 6; ++x) CHECK(floorAt(x, mid) == doctest::Approx(0.0f));
    // The ridge wall rises above floor + 220 wu (the fog thickness), so the fog top stays below it.
    CHECK(h[static_cast<size_t>(mid) * side + 32] - floorAt(32, mid) >= 220.0f);
    CHECK(floorAt(32, mid) > 0.0f);
    CHECK(store.fogFloorMax() == doctest::Approx(floorAt(32, 0)));   // every row is the same valley

    // A crater at the valley floor lowers the floor within the 8-cell radius and no further.
    const float rows[1] = {-100.0f};
    const auto version = store.heightVersion();
    REQUIRE(store.patchHeight(rows, mid, mid, 0, 0, side));
    CHECK(store.heightVersion() != version);
    CHECK(floorAt(mid, mid) == doctest::Approx(-100.0f));
    CHECK(floorAt(mid + 9, mid) == doctest::Approx(8.0f));   // x = 25: mean of -100, -100, 40, 80, 120 (x = 16 is 9 cells away)

    // A flat map is its own floor.
    std::vector<float> flat(static_cast<size_t>(side) * side, 100.0f);
    REQUIRE(store.setHeight(flat.data(), side));
    CHECK(floorAt(0, 0) == doctest::Approx(100.0f));
    CHECK(store.fogFloorMax() == doctest::Approx(100.0f));
}
