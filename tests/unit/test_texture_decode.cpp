// The backend-neutral texture decode (GameOS/gameos/texture_decode.cpp): the bytes and policy every backend uploads.

#include <algorithm>
#include <span>
#include "doctest.h"
#include "Gates/gate.h"
#include "Gates/gate_cold.h"

#include "texture_decode.h"
#include "hud_texture_mips.h"
#include "gos_prop_material_cpu.h"
#include "StockPbr.h"
#include "TextureService.h"
#include "../../RenderCore/PropMaterialRows.h"
#include "utils/Image.h"
#include <fstream>
#include <filesystem>

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

TEST_CASE("StockPbr: every shipped actor atlas resolves and decodes through the texture service") {
    auto root = std::filesystem::current_path();
    while (!std::filesystem::exists(root / "RenderCore/StockPbr.h") && root != root.parent_path())
        root = root.parent_path();
    REQUIRE(std::filesystem::exists(root / "RenderCore/StockPbr.h"));
    const std::vector<std::string> mechs = {
        "anubis", "atlas", "bloodasp", "bushwacker", "catapult", "couger", "cyclops", "enfield",
        "fireant", "ghoul", "highlander", "hollander", "hunchback", "jagermech", "lauhu", "madcat",
        "menshen", "raven", "ryoken", "shadowcat", "shayu", "shootist", "starslayer", "thor",
        "uller", "urbanmech", "vutlure", "werewolf", "wolfhound", "zeus"};
    const std::vector<std::string> vehicles = {
        "aerospotter", "ambulance", "armoredcar", "drillson", "hetzer", "hunter", "jeep", "mobilehq",
        "monsoon", "partisan", "smalltruckmap", "srmc", "storm", "trooptransport", "vedette", "vtolrgb"};
    RenderCore::TextureService service;
    uint32_t node = 0;
    for (const bool vehicle : {false, true}) {
        const auto& stems = vehicle ? vehicles : mechs;
        size_t shipped = 0;
        const auto directory = root / "data/textures" / (vehicle ? "vehicle_pbr" : "mech_pbr");
        for (const auto& entry : std::filesystem::directory_iterator(directory))
            if (entry.path().filename().string().ends_with("_albedo.png")) ++shipped;
        REQUIRE(shipped == stems.size());
        for (const auto& stem : stems) {
            const auto legacy = "data/tgl/128/" + stem + (vehicle ? ".tga" : "RGB.tga");
            const auto base = RenderCore::stockPbrBase(legacy, vehicle);
            REQUIRE(base == "data/textures/" + std::string(vehicle ? "vehicle_pbr/" : "mech_pbr/") + stem);
            for (const auto* suffix : {"_albedo.png", "_normal.png", "_orm.png"}) {
                const auto name = base + suffix;
                INFO(name);
                std::ifstream input(root / name, std::ios::binary);
                REQUIRE(input.is_open());
                const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
                REQUIRE(RenderCore::preferStockPbr(!bytes.empty(), false, 100, 100));
                const auto decoded = mc2tex::decodeMemory(bytes.data(), bytes.size(), name.c_str(),
                    gos_Texture_Solid, gosHint_MipmapFilter0 | gosHint_DontShrink);
                REQUIRE(decoded);
                // Shipped tiers: scripts/tests/test_stock_pbr_assets.py (ghoul has no 2048 set).
                const int edge = vehicle ? (stem == "jeep" ? 256 : 512) : (stem == "ghoul" ? 512 : 2048);
                CHECK(decoded->width == edge);
                CHECK(decoded->height == edge);
                CHECK(decoded->pixels.size() == static_cast<size_t>(edge * edge * 4));
                CHECK(decoded->wantMipmaps);
                RenderCore::TextureKey key;
                key.source = RenderCore::makeTextureSourceId("base", name,
                    RenderCore::textureContentHash(bytes.data(), bytes.size()));
                key.format = gos_Texture_Solid;
                key.hints = gosHint_MipmapFilter0 | gosHint_DontShrink;
                const auto handle = service.acquire(key, ++node);
                CHECK(service.nodeOf(handle) == node);
                CHECK(service.acquire(key, node) == handle);
                auto modKey = key;
                modKey.source = RenderCore::makeTextureSourceId("mod:own-art", name,
                    RenderCore::textureContentHash(bytes.data(), bytes.size()));
                CHECK(service.acquire(modKey, ++node) != handle);
                CHECK_FALSE(RenderCore::preferStockPbr(true, true, 0, 100));
                CHECK_FALSE(RenderCore::preferStockPbr(false, false, 100, 100));
            }
            std::printf("StockPbr offline: %s/%s albedo normal orm decoded\n",
                vehicle ? "vehicle" : "mech", stem.c_str());
        }
    }
    CHECK(service.liveCount() == 2 * 3 * (30 + 16));
}

TEST_CASE("texture decode: mipmaps are opt-in and DisableMipmap wins") {
    CHECK_FALSE(mc2tex::wantsMipmaps(0));
    CHECK(mc2tex::wantsMipmaps(gosHint_MipmapFilter0));
    CHECK_FALSE(mc2tex::wantsMipmaps(gosHint_MipmapFilter0 | gosHint_DisableMipmap));
}

TEST_CASE("cursor image: keyed TGA decode clears magenta for RGB and RGBA art") {
    for (const int bits : {24, 32}) {
        const std::vector<uint8_t> pixels = bits == 24 ?
            std::vector<uint8_t>{255,0,255, 30,20,10} :
            std::vector<uint8_t>{255,0,255,255, 30,20,10,128};
        const auto file = tga(2, 1, bits, pixels);
        const auto decoded = mc2tex::decodeMemory(file.data(), file.size(), "cursor.tga", gos_Texture_Keyed, 0);
        REQUIRE(decoded);
        CHECK(decoded->layout == mc2tex::PixelLayout::RGBA8);
        CHECK(decoded->pixels == std::vector<uint8_t>{0,0,0,0, 10,20,30,static_cast<uint8_t>(bits == 24 ? 255 : 128)});
    }
}

// Trigger: GameOS/gameos/texture_decode.cpp (font coverage upload policy).
TEST_CASE("texture decode: stock font red coverage remains byte-exact") {
    const auto install = mc2::gate::str<"MC2_GATE_INSTALL">();
    if (install.empty()) { MESSAGE("installed corpus unavailable; MC2_GATE_INSTALL unset"); return; }
    const auto path = std::filesystem::path(install) / "assets/graphics/arial8.bmp";
    if (!std::filesystem::exists(path)) { MESSAGE("installed stock font unavailable: " << path.string()); return; }
    Image stock;
    REQUIRE(stock.loadFromFile(path.string().c_str()));
    REQUIRE(stock.getFormat() == FORMAT_RGBA8);
    const auto decoded = mc2tex::decodeFile(path.string().c_str(), gos_Texture_Alpha, 0);
    REQUIRE(decoded);
    REQUIRE(decoded->width == stock.getWidth());
    REQUIRE(decoded->height == stock.getHeight());
    size_t changed = 0, clear = 0, ink = 0;
    for (size_t i = 0; i < decoded->pixels.size(); i += 4) {
        changed += decoded->pixels[i] != stock.getPixels()[i];
        clear += stock.getPixels()[i] == 0;
        ink += stock.getPixels()[i] == 255;
    }
    INFO("changed=" << changed << " clear=" << clear << " ink=" << ink);
    CHECK(changed == 0);
    CHECK(clear > 0);
    CHECK(ink > 0);
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

TEST_CASE("campaign marker: production decode keeps black padding transparent") {
    for (const char* name : {"mcl_cm_5.tga", "mcl_mn_4.tga"}) {
    CAPTURE(name);
    const auto path = std::filesystem::path(MC2_REPO_ROOT)
        / "data/art/gui" / name;
    if (!std::filesystem::exists(path)) {
        MESSAGE("stock campaign marker absent; skipping ", path.string());
        continue;
    }
    const auto decoded = mc2tex::decodeFile(path.string().c_str(), gos_Texture_Alpha, 0);
    REQUIRE(decoded);
    REQUIRE(decoded->width == 128);
    REQUIRE(decoded->height == 128);
    REQUIRE(decoded->layout == mc2tex::PixelLayout::RGBA8);
    size_t padding = 0;
    size_t ink = 0;
    for (size_t i = 0; i < decoded->pixels.size(); i += 4) {
        const auto* p = decoded->pixels.data() + i;
        if (p[0] == 0 && p[1] == 0 && p[2] == 0) {
            CHECK(p[3] == 0);
            ++padding;
        } else if (p[0] == 255 && p[1] == 255 && p[2] == 255 && p[3] == 255) {
            ++ink;
        }
    }
    CHECK(padding > 1000);
    CHECK(ink > 100);
    }
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

// Trigger paths: GameOS/gameos/{hud_texture_mips.h,engine_present_vk.cpp}.
TEST_CASE("HUD atlas mips preserve premultiplication rounding and immutable source") {
    const std::vector<uint8_t> source = {
        255, 0, 0, 255, 0, 255, 0, 128,
        0, 0, 255, 0,   3, 7, 11, 255};
    const auto original = source;
    const auto chain = mc2hud::premultipliedMips(source, 2);
    const std::vector<uint8_t> expected = {
        255, 0, 0, 255, 0, 128, 0, 128,
        0, 0, 0, 0,     3, 7, 11, 255,
        65, 34, 3, 160};
    CHECK(chain == expected);
    CHECK(source == original);
    const auto single = mc2hud::premultipliedMips({255, 127, 1, 128}, 1);
    CHECK(single == std::vector<uint8_t>{128, 64, 1, 128});
    const auto full = mc2hud::premultipliedMips(std::vector<uint8_t>(2048 * 2048 * 4, 255), 2048);
    CHECK(full.size() == (2048 * 2048 * 4 * 4 - 4) / 3);
    CHECK(std::all_of(full.begin(), full.end(), [](uint8_t value) { return value == 255; }));
}

TEST_CASE("HUD atlas mips reject invalid dimensions and short buffers") {
    CHECK(mc2hud::premultipliedMips({}, 0).empty());
    CHECK(mc2hud::premultipliedMips(std::vector<uint8_t>(3 * 3 * 4), 3).empty());
    CHECK(mc2hud::premultipliedMips(std::vector<uint8_t>(6 * 6 * 4), 6).empty());
    CHECK(mc2hud::premultipliedMips(std::vector<uint8_t>(2 * 2 * 4 - 1), 2).empty());
}

TEST_CASE("HUD atlas SIMD mips match scalar bytes") {
    const auto reference = [](const std::vector<uint8_t>& rgba, size_t side) {
        const size_t base = side * side * 4;
        auto pixels = rgba;
        pixels.resize((base * 4 - 4) / 3);
        for (size_t i = 0; i < base; i += 4)
            for (size_t c = 0; c < 3; ++c)
                pixels[i+c] = (static_cast<uint32_t>(pixels[i+c]) * pixels[i+3] + 127u) / 255u;
        size_t offset = 0, next = base;
        for (size_t w = side; w > 1; w /= 2) {
            for (size_t y = 0; y < w/2; ++y) for (size_t x = 0; x < w/2; ++x)
                for (size_t c = 0; c < 4; ++c) {
                    const size_t p = offset + (2*y*w + 2*x)*4 + c;
                    pixels[next + (y*(w/2)+x)*4+c] = (static_cast<uint32_t>(pixels[p]) + pixels[p+4] + pixels[p+w*4] + pixels[p+w*4+4] + 2u) / 4u;
                }
            offset = next; next += (w/2)*(w/2)*4;
        }
        return pixels;
    };
    uint32_t seed = 0x12345678;
    for (size_t side : {1u, 2u, 4u, 8u, 32u, 256u, 2048u}) {
        std::vector<uint8_t> pixels(side * side * 4);
        for (auto& value : pixels) {
            seed = seed * 1664525u + 1013904223u;
            value = static_cast<uint8_t>(seed >> 24);
        }
        const auto original = pixels;
        CHECK(mc2hud::premultipliedMips(pixels, side) == reference(pixels, side));
        CHECK(pixels == original);
    }
    // Exhaust every color/alpha product, including the division rounding boundaries.
    std::vector<uint8_t> products(256 * 256 * 4);
    for (size_t a = 0; a < 256; ++a) for (size_t c = 0; c < 256; ++c) {
        const auto i = (a*256+c)*4;
        products[i] = products[i+1] = products[i+2] = static_cast<uint8_t>(c);
        products[i+3] = static_cast<uint8_t>(a);
    }
    CHECK(mc2hud::premultipliedMips(products, 256) == reference(products, 256));
    // Exercise real decoded artwork as well as synthetic rounding boundaries.
    auto root = std::filesystem::current_path();
    while (!std::filesystem::exists(root / "RenderCore/StockPbr.h") && root != root.parent_path())
        root = root.parent_path();
    const auto path = root / "data/textures/mech_pbr/anubis_albedo.png";
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.is_open());
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
    const auto decoded = mc2tex::decodeMemory(bytes.data(), bytes.size(), "anubis_albedo.png", gos_Texture_Alpha, gosHint_DontShrink);
    REQUIRE(decoded);
    REQUIRE(decoded->width == 2048);
    REQUIRE(decoded->height == decoded->width);
    CHECK(mc2hud::premultipliedMips(decoded->pixels, 2048) == reference(decoded->pixels, 2048));
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
    const char* path="test_texture_decode_stored.ktx2";
    FILE* file=std::fopen(path,"wb"); REQUIRE(file);
    REQUIRE(std::fwrite(b.data(),1,b.size(),file)==b.size()); std::fclose(file);
    t=mc2tex::decodeFile(path,gos_Texture_Detect,0);
    REQUIRE(t); CHECK(t->sourceVkFormat==134); CHECK(t->pixels[3]==0);
    std::remove(path);
    b.resize(144); std::fill(b.begin()+104,b.begin()+128,0); put(40,2); put(80,128); put(104,136); put(112,8); put(120,8);
    std::fill(b.begin()+128,b.end(),255); b[136]=0; b[137]=0xf8; b[138]=b[139]=0;
    std::fill(b.begin()+140,b.end(),0);
    t=mc2tex::decodeMemory(b.data(),b.size(),"mips.ktx2",gos_Texture_Detect,gosHint_MipmapFilter0);
    REQUIRE(t); REQUIRE(t->mipLevels.size()==2); CHECK(t->mipLevels[1].size()==16); CHECK(t->mipLevels[1][0]==255);
    t=mc2tex::decodeMemory(b.data(),b.size(),"mips.ktx2",gos_Texture_Detect,gosHint_MipmapFilter0|gosHint_DisableMipmap);
    REQUIRE(t); CHECK(t->mipLevels.size()==1);
}

TEST_CASE("mod-sized RGBA mip chains preserve constant colour through rectangular and thin levels") {
    for (const auto shape : {std::pair<uint32_t,uint32_t>{64,64},{128,64},{1,64},{64,1},{65,33}}) {
        uint32_t w=shape.first,h=shape.second;
        std::vector<uint8_t> level(size_t(w)*h*4,127);
        while (w>1 || h>1) {
            level=mc2tex::boxDownsampleRgba8(level,w,h);
            w=std::max(1u,w/2);h=std::max(1u,h/2);
            REQUIRE(level.size()==size_t(w)*h*4);
            for (auto pixel:level) CHECK(pixel==127);
        }
    }
}

TEST_CASE("authored prop detail: linear ORM mips keep channels and normal mips normalize") {
    mc2tex::TextureArrayCpu orm;
    const std::vector<uint8_t> texels = {0,40,200,255, 100,80,100,255, 200,120,0,255, 100,160,100,255};
    mc2prop::appendLayer(orm,2,2,texels,false);
    REQUIRE(orm.levels.size()==2);
    CHECK(orm.levels[0]==texels);
    CHECK(orm.levels[1]==std::vector<uint8_t>{100,100,100,255});
    mc2tex::TextureArrayCpu normals;
    std::vector<uint8_t> sloped = {218,128,218,255, 128,218,218,255, 218,128,218,255, 128,218,218,255};
    mc2prop::appendLayer(normals,2,2,sloped,true);
    REQUIRE(normals.levels.size()==2);
    float length2=0;
    for(int c=0;c<3;++c) { float v=normals.levels[1][c]/127.5f-1; length2+=v*v; }
    CHECK(length2==doctest::Approx(1.0f).epsilon(0.02));
    CHECK(normals.levels[0]==sloped);
}

TEST_CASE("authored prop detail: failed map never creates a material, absent channel is neutral") {
    mc2prop::MaterialSources sources;
    ModelOverrideRecord owner;
    owner.manifestDir=std::filesystem::temp_directory_path().generic_string();
    ModelOverrideMaterial input;
    input.normalPath="mc2-pbr-test-definitely-missing.tga";
    CHECK_FALSE(mc2prop::addMaterial(sources,owner,input));
    CHECK(sources.table.empty());
    CHECK(sources.normal.layers==0);
    const auto file=std::filesystem::path(owner.manifestDir)/"mc2-pbr-doctest-orm.tga";
    const auto bytes=tga(2,2,32,{20,100,220,255,20,100,220,255,20,100,220,255,20,100,220,255});
    { std::ofstream out(file,std::ios::binary); out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size()); }
    input.normalPath.clear(); input.ormPath=file.filename().generic_string();
    const bool loaded=mc2prop::addMaterial(sources,owner,input);
    std::filesystem::remove(file);
    REQUIRE(loaded);
    REQUIRE(sources.table.size()==1);
    CHECK(sources.table[0].material.normalTex==RenderCore::kMaterialTexAbsent);
    CHECK(sources.table[0].material.metallicRoughnessTex==0);
    CHECK(sources.normal.levels[0][0]==128);
    CHECK(sources.normal.levels[0][2]==255);
    CHECK(sources.orm.levels[0][0]==220);
    CHECK(sources.orm.levels[0][1]==100);
    CHECK(sources.orm.levels[0][2]==20);
    // Failed second entry must leave all arrays/table at their previous size.
    input.normalPath="mc2-pbr-test-definitely-missing.tga";
    CHECK_FALSE(mc2prop::addMaterial(sources,owner,input));
    CHECK(sources.table.size()==1);
    CHECK(sources.normal.layers==1);
}

TEST_CASE("vehicle paint: unmasked object maps share the registered shader body") {
    ModelOverrideRecord vehicle;
    CHECK_FALSE(vehicle.usesShaderTeamPaint());
    ModelOverrideMaterial material;
    material.normalPath = "lrmc/detail/0_normal.ktx2";
    material.ormPath = "lrmc/detail/0_orm.ktx2";
    vehicle.materials.push_back(material);
    CHECK(vehicle.usesShaderTeamPaint());
    vehicle.materials[0].albedoPath = "lrmc/albedo.ktx2";
    CHECK(vehicle.usesShaderTeamPaint());
    vehicle.materials[0].teamMaskPath = "lrmc/mask.ktx2";
    CHECK(vehicle.usesShaderTeamPaint());
}

TEST_CASE("content cook contract: class-scalar slot, BC5 Z rebuild, explicit alpha class") {
    mc2prop::MaterialSources sources;
    ModelOverrideRecord owner;
    owner.manifestDir = std::filesystem::temp_directory_path().generic_string();
    ModelOverrideMaterial bare;
    bare.materialClass = static_cast<uint32_t>(RenderCore::SurfaceClass::BareMetal);
    bare.metallic = 1.0f; bare.roughness = 0.45f;
    REQUIRE(mc2prop::addMaterial(sources, owner, bare));          // no maps: class scalars, no layers
    CHECK(sources.table.size() == 1);
    CHECK(sources.normal.layers == 0);
    CHECK(sources.table[0].material.metallicRoughnessTex == RenderCore::kMaterialTexAbsent);
    CHECK(sources.table[0].material.metallicFactor == 1.0f);
    CHECK(sources.table[0].materialClass == 3u);

    std::vector<uint8_t> xy = {218,128,0,255, 128,128,0,255};       // BC5 decodes X/Y with B = 0
    mc2prop::rebuildNormalZ(xy);
    CHECK(xy[2] > 200);                                            // tilted: z = sqrt(1 - x^2)
    CHECK(xy[6] == 255);                                           // flat: z = 1

    owner.materials = {bare};
    owner.materials[0].slot = 3;
    owner.materials[0].alphaMode = static_cast<uint32_t>(RenderCore::SurfaceAlpha::Mask);
    CHECK(mc2prop::packetAlphaClass(true, &owner, 3) == 2u);
    owner.materials[0].alphaMode = static_cast<uint32_t>(RenderCore::SurfaceAlpha::Stock);
    CHECK(mc2prop::packetAlphaClass(true, &owner, 3) == 1u);
    owner.materials[0].alphaMode = static_cast<uint32_t>(RenderCore::SurfaceAlpha::Opaque);
    CHECK(mc2prop::packetAlphaClass(true, &owner, 3) == 0u);
    CHECK(mc2prop::packetAlphaClass(true, &owner, 7) == 1u);       // unmapped slot of a package: stock coverage
    ModelOverrideRecord bare_override;
    CHECK(mc2prop::packetAlphaClass(true, &bare_override, 0) == 1u);  // provenance alone never selects the late fringe
    bare_override.overrideClass = "tree";
    CHECK(mc2prop::packetAlphaClass(true, &bare_override, 0) == 2u);  // override trees: canopy fringe after the water
    bare_override.overrideClass = "staticprop";
    bare_override.lateCoverageFringe = true;
    CHECK(mc2prop::packetAlphaClass(true, &bare_override, 0) == 2u);  // explicit opt-in (procedural fences)
    owner.lateCoverageFringe = true;
    CHECK(mc2prop::packetAlphaClass(true, &owner, 7) == 2u);       // opt-in applies to a package's unmapped slots too
    owner.materials[0].alphaMode = static_cast<uint32_t>(RenderCore::SurfaceAlpha::Opaque);
    CHECK(mc2prop::packetAlphaClass(true, &owner, 3) == 0u);       // but never to an opaque slot
    CHECK(mc2prop::packetAlphaClass(false, nullptr, 0) == 1u);     // stock: coverage policy
}

TEST_CASE("content cook contract: a cooked KTX2 detail chain is used as stored") {
    // 2x2 RGBA8 KTX2 with two levels; level 1 deliberately differs from the box filter of level 0.
    const std::vector<uint8_t> l0 = {10,20,30,255, 10,20,30,255, 10,20,30,255, 10,20,30,255};
    const std::vector<uint8_t> l1 = {200,100,50,255};
    std::vector<uint8_t> f = {0xAB,0x4B,0x54,0x58,0x20,0x32,0x30,0xBB,0x0D,0x0A,0x1A,0x0A};
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) f.push_back(uint8_t(v >> (8*i))); };
    auto u64 = [&](uint64_t v) { for (int i = 0; i < 8; ++i) f.push_back(uint8_t(v >> (8*i))); };
    for (uint32_t v : {37u, 1u, 2u, 2u, 0u, 0u, 1u, 2u, 0u}) u32(v);
    for (int i = 0; i < 4; ++i) u32(0);
    u64(0); u64(0);
    const uint64_t data = 80 + 2 * 24;
    u64(data + 4); u64(16); u64(16);       // level 0 stored after level 1 (smallest first)
    u64(data); u64(4); u64(4);
    f.insert(f.end(), l1.begin(), l1.end());
    f.insert(f.end(), l0.begin(), l0.end());
    ModelOverrideRecord owner;
    owner.manifestDir = std::filesystem::temp_directory_path().generic_string();
    const auto file = std::filesystem::path(owner.manifestDir) / "mc2-content-doctest-orm.ktx2";
    { std::ofstream out(file, std::ios::binary); out.write(reinterpret_cast<const char*>(f.data()), f.size()); }
    mc2prop::MaterialSources sources;
    ModelOverrideMaterial input;
    input.ormPath = file.filename().generic_string();
    const bool loaded = mc2prop::addMaterial(sources, owner, input);
    std::filesystem::remove(file);
    REQUIRE(loaded);
    REQUIRE(sources.orm.levels.size() == 2);
    CHECK(sources.orm.levels[0] == l0);
    CHECK(sources.orm.levels[1] == l1);
}

TEST_CASE("authored prop detail: GPU blocks share layers and mixed mods remain valid") {
    const auto root = std::filesystem::temp_directory_path();
    auto write = [&](const char* name, uint32_t format) {
        std::vector<uint8_t> f = {0xAB,0x4B,0x54,0x58,0x20,0x32,0x30,0xBB,0x0D,0x0A,0x1A,0x0A};
        auto u32 = [&](uint32_t v) { for (int i=0;i<4;++i) f.push_back(uint8_t(v>>(8*i))); };
        auto u64 = [&](uint64_t v) { for (int i=0;i<8;++i) f.push_back(uint8_t(v>>(8*i))); };
        for (uint32_t v : {format,1u,4u,4u,0u,0u,1u,3u,0u}) u32(v);
        for (int i=0;i<4;++i) u32(0);
        u64(0); u64(0);
        for (int level=0;level<3;++level) { u64(152+level*16); u64(16); u64(16); }
        f.resize(200,0);
        std::ofstream out(root/name,std::ios::binary);
        out.write(reinterpret_cast<const char*>(f.data()),f.size());
    };
    write("mc2-detail-blocks-normal.ktx2",141);
    write("mc2-detail-blocks-orm.ktx2",145);
    ModelOverrideRecord owner; owner.manifestDir=root.generic_string();
    ModelOverrideMaterial input;
    input.normalPath="mc2-detail-blocks-normal.ktx2"; input.ormPath="mc2-detail-blocks-orm.ktx2";
    mc2prop::MaterialSources dst;
    REQUIRE(mc2prop::addMaterial(dst,owner,input));
    input.roughness=0.6f;
    REQUIRE(mc2prop::addMaterial(dst,owner,input));
    CHECK(dst.table.size()==2);
    // Cooked block maps are per-object textures: own image each, deduplicated by file, no shared array layer.
    CHECK(dst.normal.layers==0); CHECK(dst.orm.layers==0);
    REQUIRE(dst.textures.size()==2);
    CHECK(dst.textures[0].vkFormat==141); CHECK(dst.textures[1].vkFormat==145);
    CHECK(dst.textures[0].mipCount==3);
    CHECK((dst.table[1].material.flags & RenderCore::MaterialFlags::kPerTexture)!=0);
    CHECK(dst.table[1].material.normalTex==0); CHECK(dst.table[1].material.metallicRoughnessTex==1);
    CHECK(dst.table[1].reserved[1]==1);
    const auto rgbaFile=root/"mc2-detail-blocks-mixed.tga";
    const auto bytes=tga(4,4,32,std::vector<uint8_t>(64,255));
    { std::ofstream out(rgbaFile,std::ios::binary); out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size()); }
    input.normalPath=rgbaFile.filename().generic_string();
    REQUIRE(mc2prop::addMaterial(dst,owner,input));
    CHECK(dst.normal.vkFormat==37); CHECK(dst.orm.vkFormat==37);
    CHECK(dst.normal.layers==1); CHECK(dst.orm.layers==1);   // a mixed row keeps the legacy array route
    CHECK(dst.table[0].reserved[1]==1);                       // per-object rows keep their BC5 flag
    std::filesystem::remove(root/"mc2-detail-blocks-normal.ktx2");
    std::filesystem::remove(root/"mc2-detail-blocks-orm.ktx2");
    std::filesystem::remove(rgbaFile);
}

TEST_CASE("prop material rows: per-object rows without resident textures lose the flag and indices") {
    using namespace RenderCore;
    PropMaterialGpu rows[3]{};
    rows[0].material.flags = MaterialFlags::kPerTexture | MaterialFlags::kNormalMap | MaterialFlags::kMetallicRoughness;
    rows[0].material.normalTex = 4; rows[0].material.metallicRoughnessTex = 5; rows[0].material.albedoTex = 6;
    rows[0].reserved[1] = 1;
    rows[1].material.flags = MaterialFlags::kNormalMap;   // legacy detail-array row: untouched
    rows[1].material.normalTex = 2; rows[1].reserved[1] = 1;
    rows[2].material.flags = MaterialFlags::kPerTexture;
    rows[2].material.normalTex = 0;
    CHECK(dropPerTextureRows(std::span<PropMaterialGpu>(rows, 3)) == 2);
    CHECK((rows[0].material.flags & MaterialFlags::kPerTexture) == 0);
    CHECK((rows[0].material.flags & (MaterialFlags::kNormalMap | MaterialFlags::kMetallicRoughness)) == 0);
    CHECK(rows[0].material.normalTex == kMaterialTexAbsent);
    CHECK(rows[0].material.metallicRoughnessTex == kMaterialTexAbsent);
    CHECK(rows[0].material.albedoTex == kMaterialTexAbsent);
    CHECK(rows[0].reserved[1] == 0);
    CHECK(rows[1].material.normalTex == 2); CHECK(rows[1].reserved[1] == 1);
    CHECK((rows[1].material.flags & MaterialFlags::kNormalMap) != 0);
    CHECK(rows[2].material.normalTex == kMaterialTexAbsent);
    CHECK(dropPerTextureRows(std::span<PropMaterialGpu>(rows, 3)) == 0);   // idempotent
}

TEST_CASE("texture decode: PNG memory and file preserve every RGBA byte") {
    const unsigned char png[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x20, 0x08, 0x06, 0x00, 0x00, 0x00, 0x73, 0x7a, 0x7a,
        0xf4, 0x00, 0x00, 0x00, 0x31, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0xed, 0xce, 0x21, 0x01, 0x00,
        0x00, 0x08, 0x03, 0x30, 0x2a, 0xbc, 0x02, 0x15, 0x28, 0x48, 0x74, 0x62, 0x60, 0x26, 0xe6, 0x57,
        0xe9, 0xd9, 0x4f, 0x25, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
        0x20, 0x20, 0x90, 0xe7, 0xc0, 0x01, 0x86, 0xa8, 0x98, 0x2e, 0x36, 0x46, 0x88, 0x56, 0x00, 0x00,
        0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82
    };
    const std::vector<uint8_t> expected = [] {
        std::vector<uint8_t> bytes;
        for (int i = 0; i < 32 * 32; ++i)
            bytes.insert(bytes.end(), {17, 34, 51, 128});
        return bytes;
    }();
    const auto memory = mc2tex::decodeMemory(png, sizeof(png), "unit.png", gos_Texture_Detect, 0);
    REQUIRE(memory);
    CHECK(memory->width == 32);
    CHECK(memory->height == 32);
    CHECK(memory->pixels == expected);
    const auto path = std::filesystem::temp_directory_path() / "mc2-texture-decode.png";
    struct Cleanup { std::filesystem::path p; ~Cleanup() { std::filesystem::remove(p); } } cleanup{path};
    { std::ofstream out(path, std::ios::binary); out.write(reinterpret_cast<const char*>(png), sizeof(png)); }
    const auto file = mc2tex::decodeFile(path.string().c_str(), gos_Texture_Detect, 0);
    REQUIRE(file);
    CHECK(file->pixels == expected);
    Image image;
#ifndef _WIN32
    auto alias = path.string();
    alias.replace(alias.size() - 3, 3, "PNG");
    REQUIRE(image.loadPNG(alias.c_str()));
#else
    REQUIRE(image.loadPNG(path.string().c_str()));
#endif
    CHECK_FALSE(image.loadPNG(static_cast<const unsigned char*>(nullptr), 0));
    CHECK(image.getPixels() == nullptr);
    CHECK(image.getWidth() == 0);
    REQUIRE(image.loadPNG(png, sizeof(png)));
    CHECK_FALSE(image.loadPNG(static_cast<const char*>(nullptr)));
    CHECK(image.getPixels() == nullptr);
}
