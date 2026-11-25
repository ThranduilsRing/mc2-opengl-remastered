// Trigger paths: code/purchase_catalog.h, code/logisticsdata_availability.cpp,
// code/mechpurchasescreen.cpp. Offline purchase population and section rules.
// Also triggers: mclib/vfs.cpp, code/logisticsmech.cpp, code/logisticsdata_save.cpp.
#include "doctest.h"
#include "purchase_catalog.h"
#include "test_repo_root.h"
#include <filesystem>
#include <fstream>
#include <iterator>

TEST_CASE("purchase_catalog/transition bounds and closed group keyboard traversal") {
    mc2purchase::Group group{"bt2018", {0, 4, 1}, false};
    CHECK(mc2purchase::validItems(group, 0).empty());
    CHECK(mc2purchase::validItems(group, 2) == std::vector<std::size_t>{0, 1});
    group.open = true;
    CHECK(mc2purchase::validItems(group, 0).empty());
    CHECK(mc2purchase::validItems(group, 2) == std::vector<std::size_t>{0, 1});
}

TEST_CASE("purchase_catalog/screen end resets views and begin rebuilds them") {
    const auto root = std::filesystem::path(MC2_REPO_ROOT);
    INFO("__FILE__=" << __FILE__ << ", cwd=" << std::filesystem::current_path().string()
        << ", fixture=" << (root / "code/mechpurchasescreen.cpp").string());
    std::ifstream file(root / "code/mechpurchasescreen.cpp");
    REQUIRE(file.good());
    const std::string source{std::istreambuf_iterator<char>(file), {}};
    const auto beginAt = source.find("void MechPurchaseScreen::begin()");
    const auto endAt = source.find("void MechPurchaseScreen::end()");
    REQUIRE(beginAt != std::string::npos);
    REQUIRE(endAt != std::string::npos);
    const auto begin = source.substr(beginAt, endAt - beginAt);
    const auto end = source.substr(endAt, source.find("void MechPurchaseScreen::setMech", endAt) - endAt);
    for (const auto* reset : {"purchaseMechs.clear();", "purchaseGroups.clear();",
            "purchaseGroupNames.clear();", "purchaseKeyboardGroup = 0;", "pDragMech = nullptr;"})
        CHECK_MESSAGE(end.find(reset) != std::string::npos, std::string(reset));
    CHECK(end.find("endDrag(") == std::string::npos);
    CHECK(begin.find("purchaseGroups = mc2purchase::group(") != std::string::npos);
    CHECK(begin.find("purchaseGroupNames.clear();") != std::string::npos);
    CHECK(begin.find("purchaseKeyboardGroup = 0;") != std::string::npos);
    CHECK(begin.find("purchaseGroupNames.push_back(") != std::string::npos);
    const auto f7 = source.substr(source.find("const auto valid ="), source.find("SDL_SCANCODE_F8") - source.find("const auto valid ="));
    CHECK(f7.find("group.open = true;") != std::string::npos);
    CHECK(f7.find("group.open)") == std::string::npos);
}

TEST_CASE("purchase_catalog/installed assets mount without selecting a campaign") {
    CHECK(mc2purchase::mountInstalled("assets", false, false));
    CHECK(mc2purchase::mountInstalled("", false, true));
    CHECK_FALSE(mc2purchase::mountInstalled("campaign", true, true));
    CHECK_FALSE(mc2purchase::mountInstalled("campaign", false, true));
    CHECK_FALSE(mc2purchase::mountInstalled("dependency", false, true));
    CHECK_FALSE(mc2purchase::mountInstalled("assets", true, true));
    CHECK_FALSE(mc2purchase::mountInstalled("", false, false));
    CHECK(mc2purchase::identityMatches("pack-a", "Atlas", std::nullopt, "ATLAS"));
    CHECK_FALSE(mc2purchase::identityMatches("pack-b", "Atlas", std::string("pack-a"), "Atlas"));
    CHECK_FALSE(mc2purchase::identityMatches("pack-a", "Atlas", std::string("missing"), "Atlas"));
    CHECK_FALSE(mc2purchase::identityMatches("pack-a", "Atlas", std::string(""), "Atlas"));
}

TEST_CASE("purchase_catalog/availability preserves stock progression") {
    const std::vector<mc2purchase::Unlock> table{{"hunter", 1}, {"mw5_atlas", 9}, {"mw5_direwolf", 22}};
    for (int mission = 1; mission <= 24; ++mission) {
        CHECK(mc2purchase::unlocked("hunter", mission, table));
        CHECK(mc2purchase::unlocked("mw5_atlas", mission, table) == (mission >= 9));
        CHECK(mc2purchase::unlocked("mw5_direwolf", mission, table) == (mission >= 22));
        CHECK_FALSE(mc2purchase::unlocked("unknown_pack", mission, table));
        const auto path = "data/missions/purchase" + std::string(mission < 10 ? "0" : "") + std::to_string(mission) + ".fit";
        CHECK(mc2purchase::missionNumber(path) == mission);
    }
    CHECK(mc2purchase::missionNumber("data/missions/purchase_all.fit") == 0);
    CHECK(mc2purchase::missionNumber("purchase00.fit") == 0);
    CHECK(mc2purchase::missionNumber("purchase25.fit") == 0);
    CHECK_FALSE(mc2purchase::unlocked("hunter", 0, table));
    CHECK(mc2purchase::source("bt2018", "skin") == "bt2018");
    CHECK(mc2purchase::source("", "desertfox") == "desertfox");
    CHECK(mc2purchase::source("", "").empty());
}

TEST_CASE("purchase_catalog/vehicles have separate stable dropdowns") {
    const std::vector<mc2purchase::Item> items{{"", "Hunter", 25, true}, {"", "Atlas", 100},
        {"mw5-vehicles", "MW5 Scorpion", 25, true}, {"", "Vedette", 50, true}};
    const auto groups = mc2purchase::group(items, "");
    REQUIRE(groups.size() == 3);
    CHECK_FALSE(groups[0].vehicle);
    CHECK(groups[1].vehicle);
    CHECK(groups[1].items == std::vector<std::size_t>{3, 0});
    CHECK(groups[2].vehicle);
    CHECK(groups[2].source == "mw5-vehicles");
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
