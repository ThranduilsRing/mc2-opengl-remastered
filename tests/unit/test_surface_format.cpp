#include "doctest.h"
#include "vk_surface_format.h"
#include "Gates/render_options.h"
#include <algorithm>
#include <array>

namespace {
bool same(VkSurfaceFormatKHR a, VkSurfaceFormatKHR b) { return a.format == b.format && a.colorSpace == b.colorSpace; }
const VkSurfaceFormatKHR sdr{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
const VkSurfaceFormatKHR hdr{VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT};
const VkSurfaceFormatKHR scrgb{VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT};
}
TEST_CASE("HDR surface chooser preserves SDR choice for every ordered subset") {
    const std::vector<VkSurfaceFormatKHR> pool{sdr, hdr, scrgb,
        {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        {VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}};
    std::array<int, 5> order{0,1,2,3,4};
    do {
        for (int count = 0; count <= 5; ++count) {
            std::vector<VkSurfaceFormatKHR> list;
            for (int i = 0; i < count; ++i) list.push_back(pool[order[i]]);
            auto old = list.empty() ? sdr : list.front();
            for (auto f : list) if (same(f, sdr)) { old = f; break; }
            CHECK(same(mc2vk::chooseSurfaceFormat(list, false), old));
        }
    } while (std::next_permutation(order.begin(), order.end()));
}
TEST_CASE("HDR surface chooser prefers HDR10 then scRGB then historical SDR") {
    const VkSurfaceFormatKHR hdrRed{VK_FORMAT_A2R10G10B10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT};
    CHECK(same(mc2vk::chooseSurfaceFormat({sdr,scrgb,hdrRed,hdr}, true), hdr));
    CHECK(same(mc2vk::chooseSurfaceFormat({scrgb,hdrRed}, true), hdrRed));
    CHECK(same(mc2vk::chooseSurfaceFormat({sdr,scrgb}, true), scrgb));
    CHECK(same(mc2vk::chooseSurfaceFormat({sdr}, true), sdr));
    CHECK(same(mc2vk::chooseSurfaceFormat({}, true), sdr));
    CHECK(same(mc2vk::chooseSurfaceFormat({{hdr.format,sdr.colorSpace},sdr}, true), sdr));
}
TEST_CASE("HDR options default Auto and validate display calibration") {
    using namespace mc2::options;
    RenderOptions o;
    CHECK(o.hdrOutput == 0);
    CHECK(o.hdrPaperWhiteNits == 200.0f);
    CHECK(o.hdrPeakNits == 0.0f);
    std::vector<Reject> rejects;
    CHECK(applyOverrides(o, "HdrOutput=Off,HdrPaperWhiteNits=350,HdrPeakNits=1200", rejects) == 3);
    CHECK(rejects.empty());
    CHECK(o.hdrOutput == 1);
    CHECK(o.hdrPaperWhiteNits == 350.0f);
    CHECK(o.hdrPeakNits == 1200.0f);
    CHECK(applyOverrides(o, "HdrOutput=Auto", rejects) == 1);
    CHECK(o.hdrOutput == 0);
    CHECK(applyOverrides(o, "HdrOutput=On,HdrPaperWhiteNits=79,HdrPeakNits=-1", rejects) == 0);
    CHECK(rejects.size() == 3);
}
