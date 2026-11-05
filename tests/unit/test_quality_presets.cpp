#include "doctest.h"
#include "../../GuiRuntime/QualityPresets.h"
#include "Gates/render_options.h"
#include "../../GuiRuntime/PlayerSettings.h"
#include <cstring>

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
