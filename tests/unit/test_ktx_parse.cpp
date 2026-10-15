// The KTX2 container parser (RenderCore/KtxLoader.cpp ktxParse): every sidecar texture goes through it, and a mod or a
// truncated cache file must be rejected rather than trusted.

#include "doctest.h"

#include "KtxLoader.h"

#include <cstring>
#include <vector>

namespace {

void putU32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + i] = (uint8_t)(v >> (8 * i));
}

void putU64(std::vector<uint8_t>& b, size_t at, uint64_t v) {
    for (int i = 0; i < 8; ++i) b[at + i] = (uint8_t)(v >> (8 * i));
}

// A well-formed single-level KTX2 file: header, one level-index entry, then `dataBytes` of payload filled with 0x5A.
std::vector<uint8_t> makeKtx2(uint32_t vkFormat, uint32_t w, uint32_t h, uint64_t dataBytes) {
    static const uint8_t magic[12] = {0xab, 0x4b, 0x54, 0x58, 0x20, 0x32, 0x30, 0xbb, 0x0d, 0x0a, 0x1a, 0x0a};
    const size_t dataAt = 80 + 24;
    std::vector<uint8_t> b(dataAt + (size_t)dataBytes, 0);
    std::memcpy(b.data(), magic, sizeof(magic));
    putU32(b, 12, vkFormat);
    putU32(b, 16, 1);   // typeSize
    putU32(b, 20, w);
    putU32(b, 24, h);
    putU32(b, 28, 0);   // depth
    putU32(b, 32, 0);   // layers (0 = not an array)
    putU32(b, 36, 1);   // faces
    putU32(b, 40, 1);   // levels
    putU32(b, 44, 0);   // supercompression
    putU64(b, 80, dataAt);
    putU64(b, 88, dataBytes);
    putU64(b, 96, dataBytes);
    std::memset(b.data() + dataAt, 0x5A, (size_t)dataBytes);
    return b;
}

constexpr uint32_t kBc7Srgb = 146;

RenderCore::KtxParseResult parse(const uint8_t* bytes, size_t size) {
    return RenderCore::ktxParse(std::span<const uint8_t>(bytes, size));
}

}  // namespace

TEST_CASE("ktx2: a stored BC7 level is returned as its blocks, untouched") {
    const auto file = makeKtx2(kBc7Srgb, 8, 6, 2 * 2 * 16);   // 8x6 = 2x2 blocks
    const auto r = parse(file.data(), file.size());
    REQUIRE(r.image);
    CHECK(r.error == nullptr);
    CHECK(r.image->width == 8);
    CHECK(r.image->height == 6);
    CHECK(r.image->isCompressed);
    CHECK(r.image->isSrgb);
    CHECK(r.image->vkFormat == kBc7Srgb);
    CHECK(r.image->mipCount == 1);
    REQUIRE(r.image->pixels.size() == 64);
    CHECK(r.image->pixels[0] == 0x5A);
    CHECK(r.image->pixels[63] == 0x5A);
}

TEST_CASE("ktx2: a truncated file is rejected") {
    auto file = makeKtx2(kBc7Srgb, 8, 8, 64);
    file.resize(file.size() - 1);   // the level now runs past the end
    CHECK_FALSE(parse(file.data(), file.size()).image);
    file.resize(90);                // the level index itself is cut
    CHECK_FALSE(parse(file.data(), file.size()).image);
    file.resize(40);                // shorter than the header
    CHECK_FALSE(parse(file.data(), file.size()).image);
    CHECK_FALSE(parse(nullptr, 0).image);
}

TEST_CASE("ktx2: an oversized or empty edge is rejected before anything is allocated") {
    // A complete, correctly sized level: only the edge bound rejects it.
    const auto file = makeKtx2(kBc7Srgb, 20000, 4, (20000ull / 4) * 16);
    CHECK_FALSE(parse(file.data(), file.size()).image);
    const auto empty = makeKtx2(kBc7Srgb, 0, 4, 16);
    CHECK_FALSE(parse(empty.data(), empty.size()).image);
}

TEST_CASE("ktx2: a level whose length is not the format's block size is rejected") {
    const auto shortLevel = makeKtx2(kBc7Srgb, 8, 8, 63);         // 4 blocks need 64 bytes
    CHECK_FALSE(parse(shortLevel.data(), shortLevel.size()).image);
    const auto rgbaSized = makeKtx2(kBc7Srgb, 8, 8, 8 * 8 * 4);   // RGBA8 bytes under a BC7 format
    CHECK_FALSE(parse(rgbaSized.data(), rgbaSized.size()).image);
}

TEST_CASE("ktx2: more levels than a full chain, arrays, cubes and supercompression are rejected") {
    auto levels = makeKtx2(kBc7Srgb, 4, 4, 16);
    putU32(levels, 40, 4);   // a 4x4 image has at most 3 levels
    CHECK_FALSE(parse(levels.data(), levels.size()).image);

    auto array = makeKtx2(kBc7Srgb, 4, 4, 16);
    putU32(array, 32, 2);
    CHECK_FALSE(parse(array.data(), array.size()).image);

    auto cube = makeKtx2(kBc7Srgb, 4, 4, 16);
    putU32(cube, 36, 6);
    CHECK_FALSE(parse(cube.data(), cube.size()).image);

    auto basis = makeKtx2(kBc7Srgb, 4, 4, 16);
    putU32(basis, 44, 1);
    CHECK_FALSE(parse(basis.data(), basis.size()).image);

    const auto unknown = makeKtx2(100, 4, 4, 16);
    CHECK_FALSE(parse(unknown.data(), unknown.size()).image);
}
