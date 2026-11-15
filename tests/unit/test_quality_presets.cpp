#include "doctest.h"
#include "../../GuiRuntime/QualityPresets.h"
#include "Gates/render_options.h"
#include "../../GuiRuntime/PlayerSettings.h"
#include <cstring>

TEST_CASE("campaign picker/title precedence and unrecognised folders") {
    using namespace PlayerFrontEnd;
    CHECK(campaignTitle("Manifest title", "Desert Fox", "installer") == "Manifest title");
    CHECK(campaignTitle("", "Desert Fox", "installer") == "Desert Fox");
    CHECK(campaignTitle("", "", "carver_probe") == "carver_probe");
    CHECK(unrecognisedCampaign("", false));
    CHECK_FALSE(unrecognisedCampaign("Named mod", false));
    CHECK_FALSE(unrecognisedCampaign("", true));
    const auto fit = campaignInfo("FITini\r\n[Campaign]\r\nst CampaignName = \"Exodus\"\r\n"
        "[Group0]\nl MissionCount = 2\n[Group0Mission0]\nst FileName = \"m1\"\n[Group0Mission1]\n");
    CHECK(fit.title == "Exodus");
    CHECK(fit.missions == 2);
    CHECK(campaignInfo("; st CampaignName = \"Comment\"\n[Other]\nst CampaignName = \"Wrong\"\n").title.empty());
    CHECK(campaignInfo("[Campaign]\nst CampaignNameID = \"Wrong\"\nst CampaignName = 123\n").title.empty());
    CHECK(campaignInfo("[Campaign]\nst CampaignName = \"Unterminated\n").title.empty());
}

TEST_CASE("launcher presets scale shadow quality with overall quality") {
    for (int quality = 0; quality < 3; ++quality) {
        CHECK(k_gfxPresets[quality].shadQual == quality);
        CHECK(k_gfxPresets[quality].aa == (quality == 2));
    }
}

TEST_CASE("base presets retain stock vegetation and reserve detail for Medium and High") {
    CHECK(k_gfxPresets[0].baseQuality == 1);
    CHECK(k_gfxPresets[1].baseQuality == 2);
    CHECK(k_gfxPresets[2].baseQuality == 2);
    const auto& potato = k_gfxPresets[3];
    CHECK(std::strcmp(potato.label, "Potato") == 0);
    CHECK(potato.baseQuality == 0);
    CHECK(potato.shadQual == 0);
    CHECK(potato.aa == 0);
    for (int i : {0, 3}) {
        CHECK(std::strcmp(k_gfxPresets[i].values[1], "0") == 0);
        CHECK(std::strcmp(k_gfxPresets[i].values[5], "0.5") == 0);
    }
    auto saved = mc2::options::g_current;
    mc2::options::g_current = {};
    CHECK_FALSE(mc2::options::lowCost());
    mc2::options::g_current.shadowQuality = 0;
    CHECK(mc2::options::lowCost());
    mc2::options::g_current.shadowQuality = 2;
    mc2::options::g_current.baseQuality = 0;
    CHECK(mc2::options::lowCost());
    mc2::options::g_current = saved;
}

TEST_CASE("player settings/preset application replaces every quality field") {
    mc2::options::RenderOptions draft;
    for (const auto& from : k_gfxPresets) for (const auto& to : k_gfxPresets) {
        from.apply(draft);
        to.apply(draft);
        CHECK(draft.baseQuality == to.baseQuality);
        CHECK(draft.shadowQuality == to.shadQual);
        CHECK(draft.spatialAA == to.aa);
    }
}

TEST_CASE("player settings/legacy migration consumes invalid and unknown entries") {
    for (const auto& key : mc2::options::legacy::kLegacyKeys) {
        for (const auto& old : {"1", "0", "banana", ""}) {
            nlohmann::json settings = {{key.envName, old}, {"last_campaign", "community"},
                {mc2::gate::name<"MC2_RENDER_SCALE">(), "banana"}};
            std::string cfg = "[MechCommander2]\n";
            PlayerFrontEnd::migrateLegacySettings(settings, cfg);
            CHECK_FALSE(settings.contains(key.envName));
            CHECK(settings["last_campaign"] == "community");
            CHECK(settings[mc2::gate::name<"MC2_RENDER_SCALE">()] == "banana");
            mc2::options::RenderOptions expected, actual;
            const auto* option = mc2::options::find(key.option);
            mc2::options::setFromText(expected, *option, old);
            mc2::options::cfg::loadFromText(actual, cfg);
            if (option->kind == mc2::options::Kind::Bool) CHECK(actual.*option->b == expected.*option->b);
            else if (option->kind == mc2::options::Kind::Long) CHECK(actual.*option->l == expected.*option->l);
            else CHECK(actual.*option->f == expected.*option->f);
            auto migrated = settings;
            const auto once = cfg;
            PlayerFrontEnd::migrateLegacySettings(settings, cfg);
            CHECK(settings == migrated);
            CHECK(cfg == once);
        }
    }
}

TEST_CASE("player settings/named legacy values win and malformed types disappear") {
    for (const auto& key : mc2::options::legacy::kLegacyKeys) {
        nlohmann::json settings = {{key.envName, "1"}, {key.option, "0"}};
        std::string cfg = "[MechCommander2]\n";
        PlayerFrontEnd::migrateLegacySettings(settings, cfg);
        mc2::options::RenderOptions options;
        mc2::options::cfg::loadFromText(options, cfg);
        const auto* option = mc2::options::find(key.option);
        mc2::options::withValue(options, *option, [](auto& value) { CHECK(value == 0); });   // not option->b: some legacy keys are float options (ASan stack-buffer-overflow)
        CHECK(settings.empty());
        settings = {{key.envName, 42}};
        PlayerFrontEnd::migrateLegacySettings(settings, cfg);
        CHECK(settings.empty());
    }
}

TEST_CASE("player settings/object numbers stay signed and on their assignment line") {
    CHECK(PlayerFrontEnd::hasHighObjectNumber("ul objectnumber = 2000\n"));
    CHECK_FALSE(PlayerFrontEnd::hasHighObjectNumber("objectnumber = -2000\n"));
    CHECK_FALSE(PlayerFrontEnd::hasHighObjectNumber("objectnumber = -1\nother = 2000\n"));
    CHECK_FALSE(PlayerFrontEnd::hasHighObjectNumber("// objectnumber = 2000\n"));
    CHECK_FALSE(PlayerFrontEnd::hasHighObjectNumber("objectnumber\nother = 2000\n"));
    CHECK_FALSE(PlayerFrontEnd::hasHighObjectNumber("myobjectnumber = 2000\n"));
}

TEST_CASE("texture tier resolves explicit options and Auto by device VRAM") {
    using mc2::options::resolveTextureTier;
    constexpr unsigned long long MiB = 1ull << 20;
    for (int t = 0; t <= 3; ++t) CHECK(resolveTextureTier(t, 0) == t);
    CHECK(resolveTextureTier(-1, 0) == 0);
    CHECK(resolveTextureTier(-1, 512 * MiB) == 0);
    CHECK(resolveTextureTier(-1, 513 * MiB) == 1);
    CHECK(resolveTextureTier(-1, 2047 * MiB) == 1);
    CHECK(resolveTextureTier(-1, 2048 * MiB) == 2);
    CHECK(resolveTextureTier(-1, 6143 * MiB) == 2);
    CHECK(resolveTextureTier(-1, 6144 * MiB) == 3);
    CHECK(resolveTextureTier(-1, 24576 * MiB) == 3);
}

TEST_CASE("TextureQuality option row: range -1..3, default Auto, text form validated") {
    using namespace mc2::options;
    const Option* opt = find("texturequality");
    REQUIRE(opt != nullptr);
    CHECK(opt->kind == Kind::Long);
    CHECK(opt->lo == -1);
    CHECK(opt->hi == 3);
    RenderOptions o;
    CHECK(o.textureQuality == -1);
    CHECK(setFromText(o, *opt, "2") == Verdict::Ok);
    CHECK(o.textureQuality == 2);
    CHECK(setFromText(o, *opt, "-1") == Verdict::Ok);
    CHECK(o.textureQuality == -1);
    CHECK(setFromText(o, *opt, "5") != Verdict::Ok);
    CHECK(o.textureQuality == -1);
    CHECK(setFromText(o, *opt, "high") == Verdict::Garbage);
}
