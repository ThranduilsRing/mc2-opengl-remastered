// The KTX2 container parser (RenderCore/KtxLoader.cpp ktxParse): every sidecar texture goes through it, and a mod or a
// truncated cache file must be rejected rather than trusted.

#include "doctest.h"

#include "KtxLoader.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
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

    // A Basis payload (vkFormat 0) that is not a real Basis file goes to the transcoder and is rejected there, never trusted.
    auto basisZstd = makeKtx2(0, 4, 4, 16);
    putU32(basisZstd, 44, 2);
    const auto basisResult = parse(basisZstd.data(), basisZstd.size());
    CHECK_FALSE(basisResult.image);
    CHECK(basisResult.error != nullptr);

    const auto unknown = makeKtx2(100, 4, 4, 16);
    CHECK_FALSE(parse(unknown.data(), unknown.size()).image);
}

TEST_CASE("ktx2: a level offset past the end is rejected even when its length is right") {
    auto file = makeKtx2(kBc7Srgb, 4, 4, 16);
    putU64(file, 80, ~0ull - 4);   // offset + length would wrap; the offset alone is already past the file
    CHECK_FALSE(parse(file.data(), file.size()).image);
    putU64(file, 80, file.size() + 1);
    CHECK_FALSE(parse(file.data(), file.size()).image);
}

TEST_CASE("ktx2: an RGBA8 level is returned as 4 bytes a pixel") {
    constexpr uint32_t kRgba8Unorm = 37;
    const auto file = makeKtx2(kRgba8Unorm, 3, 2, 3 * 2 * 4);
    const auto r = parse(file.data(), file.size());
    REQUIRE(r.image);
    CHECK_FALSE(r.image->isCompressed);
    CHECK_FALSE(r.image->isSrgb);
    CHECK(r.image->blockSizeBytes == 0);
    CHECK(r.image->pixels.size() == 24);
}

TEST_CASE("ktx2: a full BC7 chain packs the levels in order with their byte offsets") {
    // 8x8 -> 4x4 -> 2x2 -> 1x1: 4 blocks, then one block for each smaller level (a partial block is still a whole block).
    const uint64_t lens[4] = {64, 16, 16, 16};
    const size_t dataAt = 80 + 4 * 24;
    std::vector<uint8_t> file = makeKtx2(kBc7Srgb, 8, 8, 0);
    file.resize(dataAt + 112, 0);
    putU32(file, 40, 4);
    uint64_t at = dataAt;
    for (int lvl = 0; lvl < 4; ++lvl) {
        putU64(file, 80 + lvl * 24, at);
        putU64(file, 88 + lvl * 24, lens[lvl]);
        putU64(file, 96 + lvl * 24, lens[lvl]);
        std::memset(file.data() + at, 0x10 + lvl, (size_t)lens[lvl]);
        at += lens[lvl];
    }
    const auto r = parse(file.data(), file.size());
    REQUIRE(r.image);
    CHECK(r.image->mipCount == 4);
    REQUIRE(r.image->mipByteOffsets.size() == 4);
    CHECK(r.image->mipByteOffsets[0] == 0);
    CHECK(r.image->mipByteOffsets[1] == 64);
    CHECK(r.image->mipByteOffsets[2] == 80);
    CHECK(r.image->mipByteOffsets[3] == 96);
    REQUIRE(r.image->pixels.size() == 112);
    CHECK(r.image->pixels[0] == 0x10);
    CHECK(r.image->pixels[64] == 0x11);
    CHECK(r.image->pixels[80] == 0x12);
    CHECK(r.image->pixels[111] == 0x13);
}

TEST_CASE("ktx2: BC1 BC3 BC5 metadata and bounded CPU decode") {
    for (uint32_t fmt : {131u,132u,133u,134u,137u,138u,141u}) {
        auto bytes = makeKtx2(fmt, 3, 2, fmt <= 134 ? 8 : 16);
        auto parsed = RenderCore::ktxParse(bytes);
        REQUIRE(parsed.image);
        CHECK(parsed.image->isSrgb == (fmt == 132 || fmt == 134 || fmt == 138));
        std::vector<uint8_t> rgba;
        REQUIRE(RenderCore::ktxDecodeBlocksToRgba8(*parsed.image, 0, rgba));
        CHECK(rgba.size() == 24);
        if (fmt == 141) { CHECK_FALSE(parsed.image->isSrgb); CHECK(rgba[2] == 0); CHECK(rgba[3] == 255); }
        bytes.pop_back(); CHECK_FALSE(RenderCore::ktxParse(bytes).image);
    }
    auto bytes = makeKtx2(141,4,4,16);
    putU64(bytes,96,15); CHECK_FALSE(RenderCore::ktxParse(bytes).image);
    putU64(bytes,96,16); putU64(bytes,80,80); CHECK_FALSE(RenderCore::ktxParse(bytes).image);
}
TEST_CASE("ktx2: BC1 alpha and BC5 channel fixtures") {
    auto bytes = makeKtx2(133,4,4,8);
    std::fill(bytes.begin()+104,bytes.end(),0xff);
    bytes[104]=bytes[105]=0; // colour0 < colour1, selector3 -> transparent
    auto img=RenderCore::ktxParse(bytes); REQUIRE(img.image);
    std::vector<uint8_t> rgba; REQUIRE(RenderCore::ktxDecodeBlocksToRgba8(*img.image,0,rgba));
    CHECK(rgba[3]==0);
    bytes=makeKtx2(141,4,4,16); std::fill(bytes.begin()+104,bytes.end(),0);
    bytes[104]=255; bytes[112]=128;
    img=RenderCore::ktxParse(bytes); REQUIRE(img.image);
    REQUIRE(RenderCore::ktxDecodeBlocksToRgba8(*img.image,0,rgba));
    CHECK(rgba[0]==255); CHECK(rgba[1]==128); CHECK(rgba[3]==255);
}

namespace {

// A full BC7 chain for a square `edge` texture, every byte `fill`.
std::vector<uint8_t> makeBc7Chain(uint32_t edge, uint8_t fill) {
    std::vector<uint64_t> lens;
    for (uint32_t e = edge;; e >>= 1) {
        lens.push_back(uint64_t((e + 3) / 4) * ((e + 3) / 4) * 16);
        if (e == 1) break;
    }
    const size_t dataAt = 80 + lens.size() * 24;
    uint64_t total = 0;
    for (auto l : lens) total += l;
    std::vector<uint8_t> file = makeKtx2(kBc7Srgb, edge, edge, 0);
    file.resize(dataAt + (size_t)total, fill);
    putU32(file, 40, (uint32_t)lens.size());
    uint64_t at = dataAt;
    for (size_t lvl = 0; lvl < lens.size(); ++lvl) {
        putU64(file, 80 + lvl * 24, at);
        putU64(file, 88 + lvl * 24, lens[lvl]);
        putU64(file, 96 + lvl * 24, lens[lvl]);
        at += lens[lvl];
    }
    return file;
}

bool writeFile(const std::filesystem::path& p, const std::vector<uint8_t>& b) {
    std::FILE* f = std::fopen(p.string().c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(b.data(), 1, b.size(), f) == b.size();
    std::fclose(f);
    return ok;
}

}  // namespace

// The static-prop BC7 group build (gos_static_prop_batcher_finalize_textures.cpp) loads each unique sidecar with
// ktxLoadRgba8, requires isCompressed + vkFormat 145/146 + matching dims and mip count, then copies
// ((w+3)/4)*((h+3)/4)*16 bytes per level from pixels + mipByteOffsets[lvl]. This replays that contract on a valid
// 2-layer set (branch taken) and a truncated one (loader refuses, so the group falls back to RGBA8).
TEST_CASE("static-prop BC7: a valid 2-layer 128px set satisfies the group-build contract; a truncated one is refused") {
    const auto dir = std::filesystem::temp_directory_path() / "mc2_bc7_group_test";
    std::filesystem::create_directories(dir);
    const auto a = dir / "a.ktx2", b = dir / "b.ktx2", bad = dir / "bad.ktx2";
    auto fb = makeBc7Chain(128, 0x22);
    REQUIRE(writeFile(a, makeBc7Chain(128, 0x11)));
    REQUIRE(writeFile(b, fb));
    fb.resize(fb.size() - 7);
    REQUIRE(writeFile(bad, fb));

    RenderCore::KtxImage imgs[2];
    REQUIRE(RenderCore::ktxLoadRgba8(a.string().c_str(), imgs[0]));
    REQUIRE(RenderCore::ktxLoadRgba8(b.string().c_str(), imgs[1]));
    const int levels = imgs[0].mipCount;
    CHECK(levels == 8);
    std::vector<std::vector<uint8_t>> out((size_t)levels);
    for (auto& img : imgs) {
        CHECK(img.isCompressed);
        CHECK(img.vkFormat == kBc7Srgb);
        CHECK(img.width == 128);
        CHECK(img.height == 128);
        CHECK(img.mipCount == levels);
        for (int lvl = 0; lvl < levels; ++lvl) {
            const int lw = (128 >> lvl) ? (128 >> lvl) : 1;
            const size_t imageSize = size_t((lw + 3) / 4) * size_t((lw + 3) / 4) * 16;
            REQUIRE(img.mipByteOffsets[(size_t)lvl] + imageSize <= img.pixels.size());
            const uint8_t* blocks = img.pixels.data() + img.mipByteOffsets[(size_t)lvl];
            out[(size_t)lvl].insert(out[(size_t)lvl].end(), blocks, blocks + imageSize);
        }
    }
    CHECK(out[0].size() == 2u * 32u * 32u * 16u);
    CHECK(out[0].front() == 0x11);
    CHECK(out[0].back() == 0x22);

    RenderCore::KtxImage badImg;
    CHECK_FALSE(RenderCore::ktxLoadRgba8(bad.string().c_str(), badImg));
    CHECK_FALSE(RenderCore::ktxLoadRgba8((dir / "absent.ktx2").string().c_str(), badImg));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
