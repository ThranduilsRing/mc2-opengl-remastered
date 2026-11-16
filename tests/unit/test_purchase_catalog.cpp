// Trigger paths: code/purchase_catalog.h, code/logisticsdata_availability.cpp,
// code/mechpurchasescreen.cpp. Offline purchase population and section rules.
// Also triggers: mclib/vfs.cpp, code/logisticsmech.cpp, code/logisticsdata_save.cpp.
#include "doctest.h"
#include "purchase_catalog.h"

TEST_CASE("purchase_catalog/installed assets mount without selecting a campaign") {
    CHECK(mc2purchase::mountInstalled("assets", false, false));
    CHECK(mc2purchase::mountInstalled("", false, true));
    CHECK_FALSE(mc2purchase::mountInstalled("campaign", true, true));
    CHECK_FALSE(mc2purchase::mountInstalled("campaign", false, true));
    CHECK_FALSE(mc2purchase::mountInstalled("assets", true, true));
    CHECK_FALSE(mc2purchase::mountInstalled("", false, false));
    CHECK(mc2purchase::identityMatches("pack-a", "Atlas", std::nullopt, "ATLAS"));
    CHECK_FALSE(mc2purchase::identityMatches("pack-b", "Atlas", std::string("pack-a"), "Atlas"));
    CHECK_FALSE(mc2purchase::identityMatches("pack-a", "Atlas", std::string("missing"), "Atlas"));
    CHECK_FALSE(mc2purchase::identityMatches("pack-a", "Atlas", std::string(""), "Atlas"));
}

TEST_CASE("purchase_catalog/availability preserves stock progression") {
    CHECK_FALSE(mc2purchase::available(false, ""));
    CHECK(mc2purchase::available(true, ""));
    CHECK(mc2purchase::available(false, "bt2018"));
    CHECK(mc2purchase::available(false, "omnitech"));
    CHECK(mc2purchase::source("bt2018", "skin") == "bt2018");
    CHECK(mc2purchase::source("", "desertfox") == "desertfox");
    CHECK(mc2purchase::source("", "").empty());
}

TEST_CASE("purchase_catalog/sections count sort and open only campaign source") {
    const std::vector<mc2purchase::Item> items = {
        {"bt2018", "Marauder", 75}, {"", "Atlas", 100},
        {"bt2018", "Archer", 70}, {"omnitech", "Raven", 35},
        {"bt2018", "Black Knight", 75}};
    const auto groups = mc2purchase::group(items, "omnitech");
    REQUIRE(groups.size() == 3);
    CHECK(groups[0].source.empty());
    CHECK_FALSE(groups[0].open);
    CHECK(groups[1].source == "bt2018");
    CHECK_FALSE(groups[1].open);
    CHECK(groups[1].items == std::vector<std::size_t>{4, 0, 2});
    CHECK(groups[2].open);
    const auto stock = mc2purchase::group(items, "");
    CHECK(stock[0].open);
    CHECK_FALSE(stock[1].open);
    CHECK_FALSE(stock[2].open);
    CHECK(mc2purchase::group({}, "").empty());
}

TEST_CASE("purchase_catalog/large packs retain every variant and stable ties") {
    const std::vector<mc2purchase::Item> items(600, {"bt2018", "Marauder", 75});
    const auto groups = mc2purchase::group(items, "");
    REQUIRE(groups.size() == 1);
    REQUIRE(groups[0].items.size() == 600);
    for (std::size_t i = 0; i < 600; ++i) CHECK(groups[0].items[i] == i);
}
