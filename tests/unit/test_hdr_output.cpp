#include "doctest.h"
#include <initializer_list>
#include "../../RenderCore/hdr_capture.h"

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
TEST_CASE("HDR capture/paper white preview and premultiplied UI") {
    using namespace mc2vk::hdr;
    for (int code = 0; code <= 255; ++code) {
        CHECK(captureSdr(hdrDecode(code / 255.0), 0, 0) == code);
        CHECK(captureSdr(0.0, static_cast<uint8_t>(code), 255) == code);
    }
    CHECK(captureSdr(0.0, 128, 128) == 128);
    CHECK(captureSdr(1.0, 0, 128) == 127);
    CHECK(captureSdr(8.0, 0, 0) == 255);
    CHECK(captureSdr(-1.0, 0, 0) == 0);
}
TEST_CASE("HDR output/UI composite matches SDR alpha premultiplied additive and opaque") {
    using namespace mc2vk::hdr;
    // Exercise the production GLSL function, not a C++ copy of its equation.
    for (double scene : {0.0, 0.04, 0.25, 0.5, 0.75}) {
        for (double source : {0.0, 0.04, 0.125, 0.25}) {
            for (double alpha : {0.0, 0.125, 0.5, 1.0}) {
                for (int mode = 0; mode < 4; ++mode) {
                    const double coverage = mode == 2 ? 0.0 : mode == 3 ? 1.0 : alpha;
                    const double rgb = mode == 0 ? source * alpha : source;
                    const double sdr = scene * (1.0 - coverage) + rgb;
                    const double hdr = hdrUiComposite(hdrDecode(scene), rgb, coverage);
                    const double decoded = hdrEncode(hdrPqInverse(hdrPq(hdr * 200.0)) / 200.0);
                    CHECK(std::abs(decoded - sdr) <= 1.0 / 255.0);
                    CHECK(std::abs(captureSdr(hdrDecode(scene), quantizeSdr(rgb), quantizeSdr(coverage)) / 255.0 - sdr) <= 1.0 / 255.0);
                }
            }
        }
    }
    // Additive (nonzero source alpha) followed by straight alpha and a
    // premultiplied draw: coverage tracks attenuation, never additive alpha.
    double sdr = 0.25, ui = 0.0, coverage = 0.0;
    for (int mode : {2, 0, 1, 3}) {
        const double a = mode == 2 ? 0.0 : mode == 3 ? 1.0 : 0.5;
        const double rgb = mode == 0 ? 0.125 * a : 0.125;
        sdr = sdr * (1.0 - a) + rgb;
        ui = ui * (1.0 - a) + rgb;
        coverage = coverage * (1.0 - a) + a;
        CHECK(hdrEncode(hdrUiComposite(hdrDecode(0.25), ui, coverage)) == doctest::Approx(sdr));
    }
}
