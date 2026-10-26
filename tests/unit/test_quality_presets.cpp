#include "doctest.h"
#include "../../GuiRuntime/QualityPresets.h"

TEST_CASE("launcher presets scale shadow quality with overall quality") {
    for (int quality = 0; quality < 3; ++quality) {
        CHECK(k_gfxPresets[quality].shadQual == quality);
        CHECK(k_gfxPresets[quality].aa == (quality == 2));
    }
}
