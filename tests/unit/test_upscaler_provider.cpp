// Upscaler provider interface (RenderCore/UpscalerProvider.h): registry, fallback order, preferred name,
// quality-mode render scales, motion-vector convention and jitter phase math. Offline, no Vulkan.
// Uses local registries only, so the process-wide registry and the temporal consumer count stay untouched.
#include "doctest.h"
#include "RenderCore/UpscalerProvider.h"
#include "RenderCore/TemporalInputs.h"
#include <cmath>
#include <iterator>

using namespace RenderCore;

namespace {
constexpr UpscalerQualityMode kModes[] = {
    {UpscalerQuality::NativeAA, 1.0f},
    {UpscalerQuality::Quality, 1.0f / 1.5f},
    {UpscalerQuality::Balanced, 1.0f / 1.7f},
    {UpscalerQuality::Performance, 0.5f},
};

struct MockUpscaler final : IUpscalerProvider {
    std::string_view nm;
    bool avail;
    UpscalerCaps c{kModes, static_cast<int>(std::size(kModes))};
    int released = 0;
    MockUpscaler(std::string_view n, bool a) : nm(n), avail(a) {}
    std::string_view name() const override { return nm; }
    bool available() const override { return avail; }
    const UpscalerCaps& caps() const override { return c; }
    bool prepare(const UpscalerFrame&) override { return avail; }
    bool dispatch(const UpscalerFrame&) override { return avail; }
    UpscalerImage output() const override { return {}; }
    void resetHistory() override {}
    void releaseResources() override { ++released; }
};
}  // namespace

TEST_CASE("upscaler registry: registration rejects null, duplicates and overflow") {
    UpscalerRegistry r;
    MockUpscaler a("a", true), a2("a", true), b("b", true), c("c", true), d("d", true), e("e", true);
    CHECK_FALSE(registerUpscaler(r, nullptr));
    CHECK(registerUpscaler(r, &a));
    CHECK_FALSE(registerUpscaler(r, &a));    // same pointer
    CHECK_FALSE(registerUpscaler(r, &a2));   // same name
    CHECK(registerUpscaler(r, &b));
    CHECK(registerUpscaler(r, &c));
    CHECK(registerUpscaler(r, &d));
    CHECK(r.count == kMaxUpscalers);
    CHECK_FALSE(registerUpscaler(r, &e));    // full
    releaseUpscalers(r);
    CHECK(a.released == 1);
    CHECK(d.released == 1);
}

TEST_CASE("upscaler registry: fallback order and preferred name") {
    UpscalerRegistry r;
    CHECK(effectiveUpscaler(r) == nullptr);   // nothing registered: native
    MockUpscaler dlss("dlss", false), xess("xess", true), fsr("fsr", true);
    registerUpscaler(r, &dlss);
    registerUpscaler(r, &xess);
    registerUpscaler(r, &fsr);
    CHECK(effectiveUpscaler(r) == &xess);            // first available in registration order
    CHECK(effectiveUpscaler(r, "fsr") == &fsr);      // preferred and available
    CHECK(effectiveUpscaler(r, "dlss") == &xess);    // preferred but unavailable: fallback order
    CHECK(effectiveUpscaler(r, "nope") == &xess);
    xess.avail = false;
    fsr.avail = false;
    CHECK(effectiveUpscaler(r) == nullptr);          // none available: native
}

TEST_CASE("upscaler quality modes: render scale and extent math") {
    MockUpscaler m("m", true);
    float s = 0;
    REQUIRE(renderScaleFor(m.caps(), UpscalerQuality::Quality, s));
    uint32_t w = 0, h = 0;
    renderExtentFor(3840, 2160, s, w, h);
    CHECK(w == 2560);
    CHECK(h == 1440);
    REQUIRE(renderScaleFor(m.caps(), UpscalerQuality::Performance, s));
    renderExtentFor(1920, 1080, s, w, h);
    CHECK(w == 960);
    CHECK(h == 540);
    REQUIRE(renderScaleFor(m.caps(), UpscalerQuality::NativeAA, s));
    renderExtentFor(1920, 1080, s, w, h);
    CHECK(w == 1920);
    CHECK(h == 1080);
    CHECK_FALSE(renderScaleFor(m.caps(), UpscalerQuality::UltraPerformance, s));   // not offered
    renderExtentFor(1, 1, 0.01f, w, h);
    CHECK(w == 1);   // never 0
    renderExtentFor(100, 100, 2.0f, w, h);
    CHECK(w == 100);   // never above display
}

TEST_CASE("upscaler caps: motion-vector scale and sign convention") {
    UpscalerCaps px{};
    float o[2];
    motionVectorScaleFor(px, 1280, 720, o);   // engine UV -> render pixels
    CHECK(o[0] == 1280.0f);
    CHECK(o[1] == 720.0f);
    UpscalerCaps uv{};
    uv.mvUnits = MotionVectorUnits::Uv;
    uv.mvSign[1] = -1.0f;
    motionVectorScaleFor(uv, 1280, 720, o);
    CHECK(o[0] == 1.0f);
    CHECK(o[1] == -1.0f);
}

TEST_CASE("upscaler defaults: jitter phase count and mip bias") {
    MockUpscaler m("m", true);
    CHECK(m.jitterPhaseCount(1920, 1920) == 8);
    CHECK(m.jitterPhaseCount(1280, 1920) == 18);
    CHECK(m.jitterPhaseCount(960, 1920) == 32);
    CHECK(m.mipLodBias(1.0f) == doctest::Approx(-1.0f));
    CHECK(m.mipLodBias(0.5f) == doctest::Approx(-2.0f));
    CHECK(temporal::consumerCount().load() == 0);   // registry use never registers a temporal consumer
}
