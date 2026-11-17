#include "doctest.h"
#include "icon_registry.h"
#include "icon_decode_cache.h"
#include <filesystem>
#include <format>
#include <fstream>
#include <numeric>
#include <set>

using namespace unit_icons;

TEST_CASE("icon_atlas_harness/decode_cache_identity_and_rejection") {
    const auto key = iconDecodeKey(iconDecodeVersion, "data/art/pilots/00.png", 123, 10);
    CHECK(key != iconDecodeKey(iconDecodeVersion, "data/art/pilots/01.png", 123, 10));
    CHECK(key != iconDecodeKey(iconDecodeVersion, "data/art/pilots/00.png", 124, 10));
    CHECK(key != iconDecodeKey(iconDecodeVersion, "data/art/pilots/00.png", 123, 9));
    CHECK(key != iconDecodeKey(iconDecodeVersion + 1, "data/art/pilots/00.png", 123, 10));
    Registry source, restored;
    REQUIRE(source.add("pilot/00", 2, 2, std::vector<uint32_t>{0x00123456, 0xffabcdef, 0xff505050, 0}));
    std::map<std::string, int> ranks{{"pilot/00", 10}}, decoded;
    const auto bytes = encodeIcons(source, ranks, key);
    REQUIRE(decodeIcons(bytes, key, restored, decoded));
    CHECK(restored.find("pilot/00")->pixels == source.find("pilot/00")->pixels);
    CHECK(decoded == ranks);
    REQUIRE(source.pack());
    REQUIRE(restored.pack());
    CHECK(source.atlas == restored.atlas);
    CHECK_FALSE(decodeIcons(bytes, key + 1, restored, decoded));
    for (size_t n = 0; n < bytes.size(); ++n)
        CHECK_FALSE(decodeIcons(std::span(bytes).first(n), key, restored, decoded));
    for (size_t i = 0; i < bytes.size(); ++i) {
        auto corrupt = bytes;
        corrupt[i] ^= 1;
        CHECK_FALSE(decodeIcons(corrupt, key, restored, decoded));
    }
    auto malformed = bytes;
    std::fill_n(malformed.begin() + 20, 4, 255); // oversized name, with a valid checksum
    malformed.resize(malformed.size() - 8);
    mc2cache::putLE(malformed, mc2::content::fastHash(malformed), 8);
    CHECK_FALSE(decodeIcons(malformed, key, restored, decoded));
    CHECK(restored.find("pilot/00")->pixels == source.find("pilot/00")->pixels);
}

TEST_CASE("icon_atlas_harness/drawable_source_detail_shared_with_logistics") {
    for (int width : {1920, 3840}) {
        const auto mech = hudIconSource(width, 800);
        CHECK(std::string_view(mech.sheet) == "data/art/mcui_high7.tga");
        CHECK(mech.width == 40);
        CHECK(mech.height == 48);
        CHECK(std::string_view(hudIconSource(width, 800, true).sheet) == "data/art/mcui_high8.tga");
    }
    CHECK(hudIconSource(640, 800).width == 25);
    CHECK(hudIconSource(800, 640).height == 38);
    CHECK(hudIconSource(0, 800).width == 32);
    CHECK(hudIconSource(0, 1920).height == 48);
    CHECK(logisticsIconColor(true, 0xa0123456) == 0xa0ffffff);
    CHECK(logisticsIconColor(false, 0xa0123456) == 0xa0123456);
}

TEST_CASE("icon_atlas_harness/named_image_preferred_with_mod_atlas_fallback") {
    Registry r;
    REQUIRE(r.add("mech/starslayer", 2, 2, std::vector<uint32_t>(4, 0xff123456)));
    r.sheet("stock", std::vector<uint32_t>(4, 0xffabcdef), 2, 2, 2, 2);
    CHECK(r.lookup("mech/Starslayer", "stock:0", 10, 10) == r.find("mech/starslayer"));
    CHECK(r.lookup("mech/hollander", "stock:0", 10, 10) == r.find("stock:0"));
    CHECK(r.lookup("mech/starslayer", "stock:0", 10, 1) == r.find("stock:0"));
    REQUIRE(r.add("hud/mcui_high7/back/starslayer", 2, 2, std::vector<uint32_t>(4, 0xff505050)));
    CHECK(r.lookup("hud/mcui_high7/back/starslayer", "stock:0", 1, 10)->pixels.front() == 0xff505050);
}

TEST_CASE("icon_atlas_harness/source_keys_survive_packing") {
    Registry r;
    std::vector<uint32_t> stock(256 * 256);
    std::iota(stock.begin(), stock.end(), 0u);
    r.sheet("DATA\\ART/stock.tga", stock, 256, 256, 25, 30);
    REQUIRE(r.icons.size() == 80);
    REQUIRE(r.pack());
    for (int index = 0; index < 80; ++index) {
        const Icon* icon = r.find(legacyKey("data/art/stock.tga", index));
        REQUIRE(icon);
        for (int y = 0; y < 30; ++y)
            for (int x = 0; x < 25; ++x) {
                const auto expected = stock[(index / 10 * 30 + y) * 256 + index % 10 * 25 + x];
                CHECK(icon->pixels[y * 25 + x] == expected);
                CHECK(r.atlas[(icon->y + y) * r.edge + icon->x + x] == expected);
            }
    }
    // The old UV bias can sample this neighbor at the final screen pixel.
    const auto* first = r.find("data/art/stock.tga:0");
    CHECK(r.atlas[size_t(first->y + 30) * r.edge + first->x + 25] == stock[30 * 256 + 25]);
    CHECK_FALSE(r.find("data/art/stock.tga:80"));
    CHECK_FALSE(r.find("data/art/stock.tga:-1"));
}

TEST_CASE("icon_atlas_harness/reap3_152_and_mco_floor_non_square") {
    Registry r;
    std::vector<uint32_t> reap(512 * 512);
    std::iota(reap.begin(), reap.end(), 0u);
    r.sheet("reap", reap, 512, 512, 25, 30);
    const Icon* transport = r.find("reap:152");
    REQUIRE(transport);
    CHECK(transport->pixels.front() == reap[(152 / 20 * 30) * 512 + 152 % 20 * 25]);
    std::vector<uint32_t> mco(1024 * 512);
    std::iota(mco.begin(), mco.end(), 0u);
    r.sheet("mco", mco, 1024, 512, 40, 40);
    REQUIRE(r.find("mco:75"));
    CHECK(r.find("mco:75")->pixels.front() == mco[120 * 1024]);
    CHECK_FALSE(r.find("mco:300"));
    std::vector<uint32_t> tall(256 * 512);
    r.sheet("tall", tall, 256, 512, 25, 30);
    CHECK(r.find("tall:152"));
    CHECK(r.find("tall:169"));
    CHECK_FALSE(r.find("tall:170"));
    REQUIRE(r.pack());
}

TEST_CASE("icon_atlas_harness/all_strip_columns_front_and_back") {
    Registry r;
    std::vector<uint32_t> strip(85 * 40 * 96);
    std::iota(strip.begin(), strip.end(), 0u);
    r.sheet("hud", strip, 3400, 96, 40, 48, true);
    CHECK(r.icons.size() == 170);
    REQUIRE(r.find("hud/back:76"));
    CHECK(r.find("hud/back:76")->pixels.front() == strip[48 * 3400 + 76 * 40]);
    CHECK_FALSE(r.find("hud:85"));
}

TEST_CASE("icon_atlas_harness/loose_images_and_sheets_do_not_collide") {
    Registry r;
    r.add("mech/Marauder", 64, 64, std::vector<uint32_t>(4096, 0xff123456));
    r.add("stock:152", 25, 30, std::vector<uint32_t>(750, 0xff654321));
    r.add("mod:152", 40, 48, std::vector<uint32_t>(1920, 0xffabcdef));
    REQUIRE(r.pack());
    std::set<std::pair<int, int>> occupied;
    for (const auto& [key, icon] : r.icons)
        for (int y = -1; y <= icon.height; ++y)
            for (int x = -1; x <= icon.width; ++x) {
                CHECK(occupied.insert({icon.x + x, icon.y + y}).second);
                CHECK(r.atlas[(icon.y + y) * r.edge + icon.x + x] == icon.pixels.front());
            }
    CHECK(r.find("MECH/MARAUDER"));
    CHECK_FALSE(r.add("bad", 0, 0, {}));
    CHECK_FALSE(r.add("bad", 25, 30, {}));
}

TEST_CASE("icon_atlas_harness/single_source_hud_wins_and_tiered_mod_sheet_still_resolves") {
    Registry r;
    r.add("hud/front/anubis", 42, 50, std::vector<uint32_t>(2100, 0xff111111));
    r.sheet("data/art/mcui_low4.tga", std::vector<uint32_t>(25 * 30 * 2, 0xff222222), 25, 60, 25, 30, true);
    const auto legacy = legacyKey("data/art/mcui_low4.tga", 0);
    // Stock: the single-source icon resolves at every resolution; no tier lookup is needed.
    for (const char* tier : {"hud/mcui_low4/front/anubis", "hud/mcui_med4/front/anubis", "hud/mcui_high7/front/anubis"})
        CHECK(r.lookupHud("hud/front/anubis", tier, 1000, 1000001, legacy, 1000) == r.find("hud/front/anubis"));
    // A mod tiered file with a better VFS rank wins; a worse rank loses to the single source.
    r.add("hud/mcui_low4/front/anubis", 25, 30, std::vector<uint32_t>(750, 0xff333333));
    CHECK(r.lookupHud("hud/front/anubis", "hud/mcui_low4/front/anubis", 1000, 10, legacy, 1000)->width == 25);
    CHECK(r.lookupHud("hud/front/anubis", "hud/mcui_low4/front/anubis", 10, 1000, legacy, 1000)->width == 42);
    // A mech with no single file falls back to the tiered mod sheet.
    CHECK(r.lookupHud("hud/front/ghost", "hud/mcui_low4/front/ghost", 1000001, 1000001, legacy, 1000) == r.find(legacy));
}

TEST_CASE("icon_atlas_harness/exact_size_hud_art_beats_resampling") {
    CHECK(hudSizedKey("front", "anubis", 34, 40) == "hud/front/anubis@34x40");
    // Stock: the exact-size file shares the single source's rank and wins.
    auto pick = pickHudSingle("hud/front/anubis", 1000, "hud/front/anubis@34x40", 1000);
    CHECK(pick.key == "hud/front/anubis@34x40");
    // No art for this size (rank 1000001): the single source is resampled.
    pick = pickHudSingle("hud/front/anubis", 1000, "hud/front/anubis@30x36", 1000001);
    CHECK(pick.key == "hud/front/anubis");
    // A mod's better-ranked single source is not shadowed by stock exact-size art.
    pick = pickHudSingle("hud/front/anubis", 10, "hud/front/anubis@34x40", 1000);
    CHECK(pick.key == "hud/front/anubis");
    // Through the registry: the exact asset resolves with its own dimensions, untouched.
    Registry r;
    r.add("hud/front/anubis", 42, 50, std::vector<uint32_t>(2100, 0xff111111));
    r.add("hud/front/anubis@34x40", 34, 40, std::vector<uint32_t>(1360, 0xff444444));
    const auto legacy = legacyKey("data/art/mcui_low4.tga", 0);
    pick = pickHudSingle("hud/front/anubis", 1000, "hud/front/anubis@34x40", 1000);
    const Icon* icon = r.lookupHud(pick.key, "hud/mcui_med4/front/anubis", pick.rank, 1000001, legacy, 1000);
    REQUIRE(icon);
    CHECK((icon->width == 34 && icon->height == 40));
}

TEST_CASE("icon_atlas_harness/stock_chassis_have_art_for_every_hud_size") {
    namespace fs = std::filesystem;
    const fs::path hud = fs::path(__FILE__).parent_path().parent_path().parent_path() / "data" / "art" / "mechicons" / "hud";
    if (!fs::exists(hud / "front")) return; // source tree without data/
    struct Size { int w, h; };
    const auto pngSize = [](const fs::path& p, int& w, int& h) {
        std::ifstream f(p, std::ios::binary);
        unsigned char b[24] = {};
        f.read(reinterpret_cast<char*>(b), 24);
        w = (b[16] << 24) | (b[17] << 16) | (b[18] << 8) | b[19];
        h = (b[20] << 24) | (b[21] << 16) | (b[22] << 8) | b[23];
    };
    size_t stems = 0;
    for (const char* side : {"front", "back"})
        for (const auto& e : fs::directory_iterator(hud / side)) {
            const std::string stem = e.path().stem().string();
            if (e.path().extension() != ".png" || stem.find('@') != std::string::npos) continue;
            ++stems;
            int w = 0, h = 0;
            pngSize(e.path(), w, h);
            CHECK_MESSAGE((w == 42 && h == 50), stem); // the high7 source is itself the 42x50 tier
            // The HUD cell sizes (ForceGroupIcon::setIconVariables): 1024+, 800 and 640 wide screens.
            for (Size s : {Size{40, 48}, Size{32, 38}, Size{25, 30}}) {
                const fs::path sized = hud / side / (stem + std::format("@{}x{}.png", s.w, s.h));
                CHECK_MESSAGE(fs::exists(sized), sized.string());
                if (fs::exists(sized)) {
                    pngSize(sized, w, h);
                    CHECK_MESSAGE((w == s.w && h == s.h), sized.string());
                }
            }
        }
    CHECK(stems >= 160);
}

TEST_CASE("icon_atlas_harness/stock_data_one_file_per_mech_side_and_pilot") {
    namespace fs = std::filesystem;
    const fs::path art = fs::path(__FILE__).parent_path().parent_path().parent_path() / "data" / "art";
    if (!fs::exists(art / "mechicons")) return; // source tree without data/
    CHECK_FALSE(fs::exists(art / "mechicons" / "hud" / "mcui_low4"));
    CHECK_FALSE(fs::exists(art / "mechicons" / "hud" / "mcui_med4"));
    CHECK_FALSE(fs::exists(art / "mechicons" / "hud" / "mcui_high7"));
    size_t front = 0, back = 0;
    for (const auto& e : fs::directory_iterator(art / "mechicons" / "hud" / "front"))
        if (e.path().extension() == ".png") {
            ++front;
            CHECK_MESSAGE(fs::exists(art / "mechicons" / "hud" / "back" / e.path().filename()), e.path().filename().string());
        }
    for (const auto& e : fs::directory_iterator(art / "mechicons" / "hud" / "back")) back += e.path().extension() == ".png";
    CHECK(front == back);
    CHECK(front >= 80);
    for (int i = 0; i < 28; ++i)
        CHECK_MESSAGE(fs::exists(art / "pilots" / std::format("{:02}.png", i)), i);
}

TEST_CASE("icon_atlas_harness/hud_shrink_averages_instead_of_skipping_pixels") {
    // 4x4 source drawn at 2x2: a one-pixel-wide bright column that nearest sampling would drop.
    Icon icon{4, 4, 0, 0, std::vector<uint32_t>(16, 0xff000000)};
    for (int y = 0; y < 4; ++y) icon.pixels[static_cast<size_t>(y) * 4 + 1] = 0xffffffff;
    uint32_t out[4] = {};
    resampleIcon(icon, out, 2, 2, 2);
    CHECK(out[0] == 0xff808080); // (0 + 255) / 2, rounded up
    CHECK(out[1] == 0xff000000);
    // Enlarging and same-size draws are exact copies.
    uint32_t same[16] = {};
    resampleIcon(icon, same, 4, 4, 4);
    CHECK(std::equal(icon.pixels.begin(), icon.pixels.end(), same));
    // Transparent pixels do not bleed their colour into the average.
    Icon edge{2, 1, 0, 0, {0xff0000ff, 0x00ffffff}};
    uint32_t one[1] = {};
    resampleIcon(edge, one, 1, 1, 1);
    CHECK((one[0] & 0x00ffffff) == 0x0000ff);
    CHECK((one[0] >> 24) == 128);
}
