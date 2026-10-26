#include "doctest.h"
#include "../../tools/mc2_launcher/quality_presets.h"
#include "Gates/render_options.h"
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
