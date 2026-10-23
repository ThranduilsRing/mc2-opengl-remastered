#include "doctest.h"
#include "icon_registry.h"
#include <numeric>
#include <set>

using namespace unit_icons;

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
