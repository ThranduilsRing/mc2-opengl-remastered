// The backend-neutral texture decode (GameOS/gameos/texture_decode.cpp): the bytes and policy every backend uploads.

#include "doctest.h"

#include "texture_decode.h"

#include <cstdint>
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
    mc2tex::DecodedTexture t;
    REQUIRE(mc2tex::decodeMemory(file.data(), file.size(), "unit.tga", gos_Texture_Detect, 0, t));
    CHECK(t.width == 1);
    CHECK(t.height == 2);
    CHECK(t.layout == mc2tex::PixelLayout::RGB8);
    CHECK(t.format == gos_Texture_Solid);
    REQUIRE(t.pixels.size() == 6);
    CHECK(t.pixels[0] == 255);   // top row red
    CHECK(t.pixels[5] == 255);   // bottom row blue
}

TEST_CASE("texture decode: malformed sources are rejected, not trusted") {
    mc2tex::DecodedTexture t;
    auto truncated = tga(4, 4, 32, {1, 2, 3, 4});
    CHECK_FALSE(mc2tex::decodeMemory(truncated.data(), truncated.size(), "short.tga", gos_Texture_Detect, 0, t));
    auto grey = tga(1, 1, 8, {7});
    CHECK_FALSE(mc2tex::decodeMemory(grey.data(), grey.size(), "grey.tga", gos_Texture_Detect, 0, t));
    const uint8_t tiny[4] = {0, 0, 2, 0};
    CHECK_FALSE(mc2tex::decodeMemory(tiny, sizeof(tiny), "tiny.tga", gos_Texture_Detect, 0, t));
    CHECK_FALSE(mc2tex::decodeMemory(nullptr, 0, "none", gos_Texture_Detect, 0, t));
}

TEST_CASE("texture decode: a placeholder keeps the requested format and refuses a zero size") {
    mc2tex::DecodedTexture t;
    REQUIRE(mc2tex::makePlaceholder(2, 1, gos_Texture_Keyed, gosHint_MipmapFilter0, t));
    CHECK(t.format == gos_Texture_Keyed);
    CHECK(t.wantMipmaps);
    CHECK(t.pixels == std::vector<uint8_t>{255, 255, 0, 255, 255, 255, 0, 255});
    CHECK_FALSE(mc2tex::makePlaceholder(0, 4, gos_Texture_Solid, 0, t));
}
