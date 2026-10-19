// The backend-neutral texture decode (GameOS/gameos/texture_decode.cpp): the bytes and policy every backend uploads.

#include "doctest.h"

#include "texture_decode.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

mc2tex::DecodedTexture rgba(std::vector<uint8_t> px, int w, int h) {
    mc2tex::DecodedTexture t;
    t.width = w;
    t.height = h;
    t.layout = mc2tex::PixelLayout::RGBA8;
    t.pixels = std::move(px);
    return t;
}

// An uncompressed bottom-up TGA (the stock layout) with BGR(A) pixels.
std::vector<uint8_t> tga(int w, int h, int bpp, const std::vector<uint8_t>& bgra) {
    std::vector<uint8_t> f(18, 0);
    f[2] = 2;
    f[12] = (uint8_t)w;
    f[14] = (uint8_t)h;
    f[16] = (uint8_t)bpp;
    f.insert(f.end(), bgra.begin(), bgra.end());
    return f;
}

}  // namespace

TEST_CASE("texture decode: mipmaps are opt-in and DisableMipmap wins") {
    CHECK_FALSE(mc2tex::wantsMipmaps(0));
    CHECK(mc2tex::wantsMipmaps(gosHint_MipmapFilter0));
    CHECK_FALSE(mc2tex::wantsMipmaps(gosHint_MipmapFilter0 | gosHint_DisableMipmap));
}

TEST_CASE("texture decode: Detect resolves from the alpha channel") {
    auto opaque = rgba({1, 2, 3, 255, 4, 5, 6, 255}, 2, 1);
    mc2tex::applyFormatPolicy(opaque, gos_Texture_Detect, false);
    CHECK(opaque.format == gos_Texture_Solid);

    auto translucent = rgba({1, 2, 3, 255, 4, 5, 6, 128}, 2, 1);
    mc2tex::applyFormatPolicy(translucent, gos_Texture_Detect, false);
    CHECK(translucent.format == gos_Texture_Alpha);
    CHECK(translucent.pixels[7] == 128);
}

TEST_CASE("texture decode: a Solid RGBA texture is forced opaque") {
    auto t = rgba({1, 2, 3, 0, 4, 5, 6, 7}, 2, 1);
    mc2tex::applyFormatPolicy(t, gos_Texture_Solid, false);
    CHECK(t.pixels[3] == 255);
    CHECK(t.pixels[7] == 255);
    CHECK(t.pixels[0] == 1);
}

TEST_CASE("texture decode: PNG magenta becomes transparent black only when keyed") {
    auto keyed = rgba({255, 0, 255, 255, 255, 0, 254, 255}, 2, 1);
    mc2tex::applyFormatPolicy(keyed, gos_Texture_Detect, true);
    CHECK(keyed.pixels[0] == 0);
    CHECK(keyed.pixels[3] == 0);
    CHECK(keyed.pixels[6] == 254);   // near-magenta is art, not the key
    CHECK(keyed.format == gos_Texture_Alpha);

    auto unkeyed = rgba({255, 0, 255, 255}, 1, 1);
    mc2tex::applyFormatPolicy(unkeyed, gos_Texture_Detect, false);
    CHECK(unkeyed.pixels[0] == 255);
    CHECK(unkeyed.format == gos_Texture_Solid);
}

TEST_CASE("texture decode: a 24-bit TGA decodes to RGB8 top-down, R first") {
    // Bottom row (first in the file) blue, top row red.
    const auto file = tga(1, 2, 24, {255, 0, 0, /* top: */ 0, 0, 255});
    const auto decoded = mc2tex::decodeMemory(file.data(), file.size(), "unit.tga", gos_Texture_Detect, 0);
    REQUIRE(decoded);
    const mc2tex::DecodedTexture& t = *decoded;
    CHECK(t.width == 1);
    CHECK(t.height == 2);
    CHECK(t.layout == mc2tex::PixelLayout::RGB8);
    CHECK(t.format == gos_Texture_Solid);
    REQUIRE(t.pixels.size() == 6);
    CHECK(t.pixels[0] == 255);   // top row red
    CHECK(t.pixels[5] == 255);   // bottom row blue
}

TEST_CASE("texture decode: malformed sources are rejected, not trusted") {
    auto truncated = tga(4, 4, 32, {1, 2, 3, 4});
    CHECK_FALSE(mc2tex::decodeMemory(truncated.data(), truncated.size(), "short.tga", gos_Texture_Detect, 0));
    auto grey = tga(1, 1, 8, {7});
    CHECK_FALSE(mc2tex::decodeMemory(grey.data(), grey.size(), "grey.tga", gos_Texture_Detect, 0));
    const uint8_t tiny[4] = {0, 0, 2, 0};
    CHECK_FALSE(mc2tex::decodeMemory(tiny, sizeof(tiny), "tiny.tga", gos_Texture_Detect, 0));
    CHECK_FALSE(mc2tex::decodeMemory(nullptr, 0, "none", gos_Texture_Detect, 0));
}

TEST_CASE("texture decode: RLE headers the loader cannot size safely are rejected") {
    // 16-bit RLE: the loader would size 2 bytes per pixel and write 4.
    auto rle16 = tga(64, 64, 16, std::vector<uint8_t>(64, 0x80));
    rle16[2] = 10;
    CHECK_FALSE(mc2tex::decodeMemory(rle16.data(), rle16.size(), "rle16.tga", gos_Texture_Detect, 0));
    // An edge over the cap (its byte count would wrap the loader's int arithmetic near 32768 x 32768).
    auto huge = tga(0, 0, 32, std::vector<uint8_t>(64, 0x80));
    huge[2] = 10;
    huge[12] = 0x20; huge[13] = 0x4E;   // 20000
    huge[14] = 0x20; huge[15] = 0x4E;
    CHECK_FALSE(mc2tex::decodeMemory(huge.data(), huge.size(), "huge.tga", gos_Texture_Detect, 0));
}

TEST_CASE("texture decode: a TGA file goes through the same header check") {
    auto truncated = tga(4, 4, 32, {1, 2, 3, 4});
    const char* path = "test_texture_decode_truncated.tga";
    FILE* f = std::fopen(path, "wb");
    REQUIRE(f);
    std::fwrite(truncated.data(), 1, truncated.size(), f);
    std::fclose(f);
    CHECK_FALSE(mc2tex::decodeFile(path, gos_Texture_Detect, 0));
    std::remove(path);

    const auto good = tga(1, 2, 24, {255, 0, 0, 0, 0, 255});
    const char* goodPath = "test_texture_decode_good.tga";
    f = std::fopen(goodPath, "wb");
    REQUIRE(f);
    std::fwrite(good.data(), 1, good.size(), f);
    std::fclose(f);
    const auto decoded = mc2tex::decodeFile(goodPath, gos_Texture_Detect, 0);
    REQUIRE(decoded);
    CHECK(decoded->pixels.size() == 6);
    std::remove(goodPath);
}

TEST_CASE("texture decode: a placeholder keeps the requested format and refuses a zero size") {
    const auto t = mc2tex::makePlaceholder(2, 1, gos_Texture_Keyed, gosHint_MipmapFilter0);
    REQUIRE(t);
    CHECK(t->format == gos_Texture_Keyed);
    CHECK(t->wantMipmaps);
    CHECK(t->pixels == std::vector<uint8_t>{255, 255, 0, 255, 255, 255, 0, 255});
    CHECK_FALSE(mc2tex::makePlaceholder(0, 4, gos_Texture_Solid, 0));
}

TEST_CASE("texture decode: BC7 blocks decode to RGBA8 only when the byte count matches the size") {
    CHECK(mc2tex::bc7LevelBytes(8, 6) == 64);   // 2 x 2 blocks
    CHECK(mc2tex::bc7LevelBytes(1, 1) == 16);
    // Mode 6 block, all-zero payload: a valid block that decodes to one colour.
    std::vector<uint8_t> block(16, 0);
    block[0] = 0x40;
    const auto rgba = mc2tex::decodeBc7(block, 4, 4);
    CHECK(rgba.size() == 4 * 4 * 4);
    CHECK(mc2tex::decodeBc7(block, 8, 8).empty());   // 4 blocks needed, 1 given
    CHECK(mc2tex::decodeBc7(block, 0, 4).empty());
}

TEST_CASE("mip box filter: a 4x4 RGBA8 level halves to the rounded 2x2 means, then to one texel") {
    // Each 2x2 quadrant has distinct channel values; red and alpha test round-half-up ((sum + 2) / 4).
    std::vector<uint8_t> src(4 * 4 * 4);
    auto put = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        uint8_t* p = &src[(size_t)(y * 4 + x) * 4];
        p[0] = r; p[1] = g; p[2] = b; p[3] = a;
    };
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) put(x, y, 0, 0, 0, 0);
    put(0, 0, 1, 10, 0, 255); put(1, 0, 0, 20, 0, 255); put(0, 1, 0, 30, 0, 255); put(1, 1, 1, 40, 0, 0);    // r 2/4 -> 1, a 765/4 -> 191
    put(2, 0, 255, 0, 0, 255); put(3, 0, 255, 0, 0, 255); put(2, 1, 255, 0, 0, 255); put(3, 1, 255, 0, 0, 255);
    put(0, 2, 0, 0, 7, 0); put(1, 2, 0, 0, 0, 0); put(0, 3, 0, 0, 0, 0); put(1, 3, 0, 0, 0, 0);          // b 7/4 -> 2 (1.75 rounds up)
    put(2, 2, 3, 3, 3, 3); put(3, 2, 3, 3, 3, 3); put(2, 3, 3, 3, 3, 3); put(3, 3, 2, 2, 2, 2);          // 11/4 = 2.75 -> 3

    const auto l1 = mc2tex::boxDownsampleRgba8(src, 4, 4);
    REQUIRE(l1.size() == 2 * 2 * 4);
    const std::vector<uint8_t> want1 = {1, 25, 0, 191,   255, 0, 0, 255,   0, 0, 2, 0,   3, 3, 3, 3};
    CHECK(l1 == want1);

    const auto l2 = mc2tex::boxDownsampleRgba8(l1, 2, 2);
    REQUIRE(l2.size() == 4);
    // r (1+255+0+3+2)/4 = 65, g (25+0+0+3+2)/4 = 7, b (0+0+2+3+2)/4 = 1, a (191+255+0+3+2)/4 = 112
    const std::vector<uint8_t> want2 = {65, 7, 1, 112};
    CHECK(l2 == want2);
}

TEST_CASE("mip box filter: non-square and 1-texel edges, malformed input rejected") {
    std::vector<uint8_t> row = {10, 0, 0, 0, 20, 0, 0, 0, 30, 0, 0, 0, 41, 0, 0, 0};   // 4x1
    const auto half = mc2tex::boxDownsampleRgba8(row, 4, 1);
    REQUIRE(half.size() == 2 * 4);
    CHECK(half[0] == 15);   // (10+20+10+20+2)/4: the single row pairs with itself
    CHECK(half[4] == 36);   // (30+41+30+41+2)/4 = 36
    CHECK(mc2tex::mipExtent(4, 2) == 1);
    CHECK(mc2tex::mipExtent(1, 5) == 1);
    CHECK(mc2tex::mipExtent(256, 3) == 32);
    CHECK(mc2tex::boxDownsampleRgba8(row, 4, 2).empty());   // byte count does not match the size
    CHECK(mc2tex::boxDownsampleRgba8({}, 0, 0).empty());
}

TEST_CASE("texture array level sizes: every layer per level, RGBA8 and BC7, unknown format 0") {
    mc2tex::TextureArrayCpu a;
    a.width = 64; a.height = 32; a.layers = 3; a.vkFormat = mc2tex::kVkFormatRgba8Unorm;
    CHECK(mc2tex::textureArrayLevelBytes(a, 0) == size_t{64} * 32 * 4 * 3);
    CHECK(mc2tex::textureArrayLevelBytes(a, 6) == size_t{1} * 1 * 4 * 3);   // 64x32 -> level 6 is 1x1
    a.vkFormat = mc2tex::kVkFormatBc7Unorm;
    CHECK(mc2tex::textureArrayLevelBytes(a, 0) == size_t{16} * 8 * 16 * 3);
    CHECK(mc2tex::textureArrayLevelBytes(a, 5) == size_t{16} * 3);        // 2x1 still takes one block
    a.vkFormat = 0;
    CHECK(mc2tex::textureArrayLevelBytes(a, 0) == 0);
}

TEST_CASE("texture decode: stored KTX preserves mip colour and alpha policy") {
    std::vector<uint8_t> b(128, 0);
    const uint8_t magic[] = {0xab,0x4b,0x54,0x58,0x20,0x32,0x30,0xbb,0x0d,0x0a,0x1a,0x0a};
    std::copy(std::begin(magic),std::end(magic),b.begin());
    auto put=[&](int at,uint32_t v){ for(int j=0;j<4;++j)b[at+j]=uint8_t(v>>(j*8)); };
    put(12,134); put(16,1); put(20,4); put(24,4); put(36,1); put(40,1);
    put(80,104); put(88,8); put(96,8); b.resize(112);
    std::fill(b.begin()+104,b.end(),255); b[104]=b[105]=0;
    auto t=mc2tex::decodeMemory(b.data(),b.size(),"only.ktx2",gos_Texture_Detect,gosHint_MipmapFilter0);
    REQUIRE(t); CHECK(t->isSrgb); CHECK(t->sourceAlpha==RenderCore::KtxAlpha::Cutout);
    CHECK(t->format==gos_Texture_Alpha); CHECK(t->mipLevels.size()==1); CHECK(t->pixels[3]==0);
    t=mc2tex::decodeMemory(b.data(),b.size(),"only.ktx2",gos_Texture_Solid,0);
    REQUIRE(t); CHECK(t->pixels[3]==255);
}
