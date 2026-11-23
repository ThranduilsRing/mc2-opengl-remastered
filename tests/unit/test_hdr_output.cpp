#include "doctest.h"
#include <initializer_list>
#include "../../shaders/vulkan/hdr_transfer.glsl"

TEST_CASE("HDR output/PQ luminance anchors and extended decode") {
    using namespace mc2vk::hdr;
    CHECK(hdrPq(100.0) == doctest::Approx(0.5081).epsilon(0.0001));
    CHECK(hdrPq(1000.0) == doctest::Approx(0.7518).epsilon(0.0001));
    CHECK(hdrPq(10000.0) == doctest::Approx(1.0));
    CHECK(hdrDecode(1.0) == doctest::Approx(1.0));
    CHECK(hdrDecode(2.0) > 4.0);
}
TEST_CASE("HDR output/EETF preserves paper white and is monotone bounded") {
    using namespace mc2vk::hdr;
    for (double peak : {1.0, 1.1, 5.0, 7.495, 50.0}) {
        double previous = 0.0;
        for (int i = 0; i <= 10000; ++i) {
            const double y = i * peak / 1000.0;
            const double out = hdrEetf(y, peak, 200.0);
            CHECK(out >= previous);
            CHECK(out <= peak);
            if (y <= 1.0 || y <= 0.75 * peak) CHECK(out == y);
            previous = out;
        }
    }
}
