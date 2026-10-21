#include "doctest.h"
#include "terrain_field_store.h"

#include <bit>
#include <limits>

namespace {
mc2terrain::SurfaceAsset sampleSurface() {
    return {2, 1, 0x123456789abcdef0ull, {{0.1f, 0.2f, 0.3f, 1}, {0.9f, 0.8f, 0.7f, 1}}};
}
void putWord(std::vector<uint8_t>& bytes, size_t at, uint64_t value, unsigned count = 4) {
    for (unsigned i = 0; i < count; ++i) bytes[at + i] = uint8_t(value >> (8 * i));
}
}

TEST_CASE("surface asset: explicit wire roundtrip and unique store versions") {
    const auto asset = sampleSurface();
    const auto bytes = mc2terrain::encodeSurfaceAsset(asset);
    REQUIRE(bytes.size() == 48 + 2 * 16);
    CHECK(bytes[0] == 'T'); CHECK(bytes[1] == 'S'); CHECK(bytes[2] == 'F'); CHECK(bytes[3] == '1');
    mc2terrain::TerrainFieldStore first, second;
    REQUIRE(first.loadSurfaceAsset(bytes, asset.sourceHash));
    CHECK(first.surfaceAsset().macroColor == asset.macroColor);
    CHECK(first.surfaceAsset().width == 2);
    REQUIRE(second.loadSurfaceAsset(bytes, asset.sourceHash));
    CHECK(second.surfaceVersion() > first.surfaceVersion());
    const auto version = first.surfaceVersion();
    first.clearSurfaceAsset();
    CHECK(first.surfaceAsset().macroColor.empty());
    CHECK(first.surfaceVersion() > version);
}

TEST_CASE("surface asset: stale truncated oversized corrupt replacements preserve active bytes and version") {
    const auto asset = sampleSurface();
    const auto good = mc2terrain::encodeSurfaceAsset(asset);
    mc2terrain::TerrainFieldStore store;
    REQUIRE(store.loadSurfaceAsset(good, asset.sourceHash));
    const auto version = store.surfaceVersion();
    auto rejects = [&](const auto& bytes, uint64_t source) {
        CHECK_FALSE(store.loadSurfaceAsset(bytes, source));
        CHECK(store.surfaceVersion() == version);
        CHECK(mc2terrain::encodeSurfaceAsset(store.surfaceAsset()) == good);
    };
    rejects(good, asset.sourceHash + 1);
    for (size_t size = 0; size < good.size(); ++size)
        rejects(std::span<const uint8_t>(good).first(size), asset.sourceHash);
    for (const size_t offset : {size_t(0), size_t(4), size_t(8), size_t(20), size_t(32), size_t(48)}) {
        auto bytes = good; bytes[offset] ^= 0x40; rejects(bytes, asset.sourceHash);
    }
    for (uint32_t width : {0u, mc2terrain::SurfaceAsset::kMaxSide + 1, 0xffffffffu}) {
        auto bytes = good; putWord(bytes, 12, width); rejects(bytes, asset.sourceHash);
    }
    auto bytes = good; bytes.push_back(0); rejects(bytes, asset.sourceHash);
    bytes = good; putWord(bytes, 40, 0xffffffffffffffffull, 8); rejects(bytes, asset.sourceHash);
    for (float bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -0.1f, 1.1f}) {
        bytes = good; putWord(bytes, 48, std::bit_cast<uint32_t>(bad));
        putWord(bytes, 32, mc2terrain::surfaceHash(std::span<const uint8_t>(bytes).subspan(48)), 8);
        rejects(bytes, asset.sourceHash);
        auto candidate = asset; candidate.macroColor[0][0] = bad;
        CHECK_FALSE(store.setSurfaceAsset(candidate, asset.sourceHash));
        CHECK(mc2terrain::encodeSurfaceAsset(candidate).empty());
        CHECK(store.surfaceVersion() == version);
    }
}
