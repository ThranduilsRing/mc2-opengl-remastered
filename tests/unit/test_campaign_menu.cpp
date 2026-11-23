#include "doctest.h"
#include "../../GuiRuntime/PlayerFrontEnd.h"
#include "../../GuiRuntime/PlayerSettings.h"
#include <filesystem>
#include <fstream>

namespace {
namespace fs = std::filesystem;
void put(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}
void campaignMod(const fs::path& root, const std::string& id, const std::string& title, const std::string& type = "campaign") {
    put(root / id / "mod.json", "{\"id\":\"" + id + "\",\"type\":\"" + type + "\"}");
    put(root / id / "data" / "campaign" / "campaign.fit", "[Campaign]\nst CampaignName = \"" + title + "\"\n[Group0Mission0]\n");
}
}

// Trigger: GuiRuntime/PlayerSettings.h, GuiRuntime/PlayerFrontEnd.h, code/logisticsdialog.cpp campaign list, code/mainmenu.cpp relaunch.
TEST_CASE("campaign menu/lists stock and every installed mod campaign, tagged with its mod") {
    using namespace PlayerFrontEnd;
    const fs::path root = fs::temp_directory_path() / "mc2_campaign_menu_fixture" / "mods";
    fs::remove_all(root.parent_path());
    campaignMod(root, "alpha", "Alpha Campaign");
    campaignMod(root, "beta", "Beta Campaign");
    campaignMod(root, "tools", "Not a campaign", "assets");
    put(root / "wrongid" / "mod.json", "{\"id\":\"other\",\"type\":\"campaign\"}");
    fs::create_directories(root / "wrongid" / "data" / "campaign");
    put(root / ".campaign-auto.json", "{}");

    const auto mods = scanModCampaigns(root);
    SUBCASE("stock mounted: both mods are offered, nothing else") {
        const auto entries = modCampaignEntries(mods, "");
        REQUIRE(entries.size() == 2);
        CHECK(entries[0].key == "alpha::campaign");
        CHECK(entries[0].text == "Alpha Campaign [alpha]");
        CHECK(entries[1].key == "beta::campaign");
        CHECK(entries[1].text == "Beta Campaign [beta]");
    }
    SUBCASE("a mounted mod's own campaigns are not repeated") {
        const auto entries = modCampaignEntries(mods, "alpha");
        REQUIRE(entries.size() == 1);
        CHECK(entries[0].key == "beta::campaign");
    }
    SUBCASE("import sources are hidden") {
        const auto entries = modCampaignEntries(mods, "", [](std::string_view id) { return id == "beta"; });
        REQUIRE(entries.size() == 1);
        CHECK(entries[0].key == "alpha::campaign");
    }
    SUBCASE("keys round trip and stock with no mod layer is the empty-mod key") {
        std::string mod, fit;
        REQUIRE(splitCampaignKey("alpha::campaign", mod, fit));
        CHECK((mod == "alpha" && fit == "campaign"));
        REQUIRE(splitCampaignKey(std::string(kStockNoModKey), mod, fit));
        CHECK((mod.empty() && fit == "campaign"));
        CHECK_FALSE(splitCampaignKey("campaign", mod, fit));
        CHECK(isCrossModKey("beta::campaign"));
        CHECK_FALSE(isCrossModKey("campaign"));
    }
    fs::remove_all(root.parent_path());
}

TEST_CASE("campaign menu/a save reloads under the mod it was made under") {
    using namespace PlayerFrontEnd;
    // The record survives the FIT text round trip ("-" stands for none) ...
    for (const char* value : {"", "alpha", "mco-compat,mc2-vehicle-kit"})
        CHECK(loadModField(saveModField(value)) == value);
    CHECK(saveModField("") == "-");
    // ... and only a recorded save made under other layers relaunches; an old save with no record loads as before.
    CHECK(saveNeedsRelaunch(true, "alpha", "", "", ""));
    CHECK(saveNeedsRelaunch(true, "", "", "alpha", ""));
    CHECK(saveNeedsRelaunch(true, "alpha", "mco-compat", "alpha", ""));
    CHECK_FALSE(saveNeedsRelaunch(true, "alpha", "mco-compat", "alpha", "mco-compat"));
    CHECK_FALSE(saveNeedsRelaunch(true, "", "", "", ""));
    CHECK_FALSE(saveNeedsRelaunch(false, "alpha", "", "", ""));
}
