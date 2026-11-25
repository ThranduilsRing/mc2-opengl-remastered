// Trigger paths: code/purchase_catalog.h, code/logisticsdata_availability.cpp,
// code/mechpurchasescreen.cpp. Offline purchase population and section rules.
// Also triggers: mclib/vfs.cpp, code/logisticsmech.cpp, code/logisticsdata_save.cpp.
#include "doctest.h"
#include "purchase_catalog.h"
#include "game_rules.h"
#include "inifile.h"
#include "test_repo_root.h"
#include <filesystem>
#include <fstream>
#include <iterator>

TEST_CASE("purchase_catalog/vehicle rule defaults on and filters every vehicle source") {
    mc2::rules::GameRules rules;
    CHECK(rules.purchaseVehicles);
    for (bool vehicle : {false, true}) {
        CHECK(mc2purchase::purchasable(vehicle, rules.purchaseVehicles));
        rules.purchaseVehicles = false;
        CHECK(mc2purchase::purchasable(vehicle, rules.purchaseVehicles) == !vehicle);
        rules.purchaseVehicles = true;
    }
    rules.purchaseVehicles = false;
    mc2::rules::GameRules restored;
    for (const auto& entry : mc2::rules::cfgEntries(rules))
        mc2::rules::loadCfgText(restored, entry.key + "=" + entry.text + "\n");
    CHECK_FALSE(restored.purchaseVehicles);
}

TEST_CASE("purchase_catalog/zero rating vehicle prices use shipped tonnage and role data") {
    FitIniFile table;
    REQUIRE(table.open((std::string(MC2_REPO_ROOT) + "/data/defs/purchase/vehicle_prices.fit").c_str()) == NO_ERR);
    REQUIRE(table.seekBlock("VehiclePrices") == NO_ERR);
    long perTon = 0, minimum = 0;
    REQUIRE(table.readIdLong("PerTon", perTon) == NO_ERR);
    REQUIRE(table.readIdLong("Minimum", minimum) == NO_ERR);
    REQUIRE(perTon > 0);
    REQUIRE(minimum > 0);
    for (float tons : {0.f, 1.f, 5.f, 20.f, 25.f, 30.f, 75.f, 100.f})
        CHECK(mc2purchase::vehiclePrice(0, tons, perTon, minimum) > 0);
    for (const auto* role : {"minelayer", "repairtruck", "mobilehq", "mog"}) {
        long rate = 0;
        REQUIRE(table.readIdLong(role, rate) == NO_ERR);
        CHECK(rate >= perTon);
        CHECK(mc2purchase::vehiclePrice(0, 30.f, rate, minimum) >= 30 * perTon);
    }
    CHECK(mc2purchase::vehiclePrice(2500, 75.f, perTon, minimum) == 2500);
}

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
    CHECK(mc2purchase::missionNumber("data/missions/purchase_all.fit") == 1);
    CHECK(mc2purchase::unlocked("hunter", mc2purchase::missionNumber("data/missions/purchaseKVe.fit"), table));
    CHECK_FALSE(mc2purchase::unlocked("mw5_atlas", mc2purchase::missionNumber("purchaseKVe.fit"), table));
    CHECK(mc2purchase::missionNumber("purchase00.fit") == 0);
    CHECK(mc2purchase::missionNumber("purchase25.fit") == 0);
    CHECK_FALSE(mc2purchase::unlocked("hunter", 0, table));
    CHECK(mc2purchase::source("bt2018", "skin") == "bt2018");
    CHECK(mc2purchase::source("", "desertfox") == "desertfox");
    CHECK(mc2purchase::source("", "").empty());
}

TEST_CASE("purchase_catalog/normal custom campaign gets ten starting MW5 mechs") {
    FitIniFile file;
    REQUIRE(file.open((std::string(MC2_REPO_ROOT) + "/data/defs/purchase/unlocks.fit").c_str()) == NO_ERR);
    REQUIRE(file.seekBlock("Unlocks") == NO_ERR);
    std::vector<mc2purchase::Unlock> table;
    for (int i = 0; i < 48; ++i) {
        char stem[256];
        long mission = 0;
        REQUIRE(file.readIdString(("Unit" + std::to_string(i)).c_str(), stem, sizeof(stem)) == NO_ERR);
        REQUIRE(file.readIdLong(("Mission" + std::to_string(i)).c_str(), mission) == NO_ERR);
        table.push_back({stem, mission});
    }
    for (const auto* purchase : {"purchase01.fit", "purchase02.fit", "purchaseKVe.fit"}) {
        const int mission = mc2purchase::missionNumber(purchase);
        CHECK(std::count_if(table.begin(), table.end(), [&](const auto& row) {
            return mc2purchase::unlocked(row.chassis, mission, table);
        }) == 10);
    }
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

TEST_CASE("purchase_catalog/normal launch exposes installed mech sections") {
    const std::vector<mc2purchase::Item> items = {
        {"bt2018", "Marauder", 75}, {"", "Atlas", 100},
        {"bt2018", "Archer", 70}, {"omnitech", "Raven", 35},
        {"bt2018", "Black Knight", 75}};
    const auto groups = mc2purchase::group(items, "omnitech");
    REQUIRE(groups.size() == 3);
    CHECK(groups[0].source.empty());
    CHECK(groups[0].open);
    CHECK(groups[1].source == "bt2018");
    CHECK(groups[1].open);
    CHECK(groups[1].items == std::vector<std::size_t>{4, 0, 2});
    CHECK(groups[2].open);
    const auto stock = mc2purchase::group(items, "");
    CHECK(stock[0].open);
    CHECK(stock[1].open);
    CHECK(stock[2].open);
    CHECK(mc2purchase::group({}, "").empty());
}

TEST_CASE("purchase_catalog/large packs retain every variant and stable ties") {
    const std::vector<mc2purchase::Item> items(600, {"bt2018", "Marauder", 75});
    const auto groups = mc2purchase::group(items, "");
    REQUIRE(groups.size() == 1);
    REQUIRE(groups[0].items.size() == 600);
    for (std::size_t i = 0; i < 600; ++i) CHECK(groups[0].items[i] == i);
}
