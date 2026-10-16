// The tick-to-millisecond step of the VK pass timer: a TIMESTAMP counter keeps only its valid bits, so a pass that
// crosses the counter's wrap must still read as its short forward duration.
#include "doctest.h"

#include "vk_render_pass_timer.h"
#include "vk_render_pass_timer_ring.h"

#include <set>
#include <vector>

using vk_render_pass_timer::TicksToMs;
using vk_render_pass_timer::ReadResult;
using TestRing = vk_render_pass_timer::Ring<3, 4>;

namespace {
// A fake GPU: a query pair is ready once its bracket's frame has been "finished"; every read is recorded.
struct FakeGpu {
    std::set<uint32_t> ready;            // first query of each finished bracket
    std::set<uint32_t> failing;          // first query whose read is a hard error
    std::vector<uint32_t> reads;
    std::vector<std::vector<double>> published;
    ReadResult read(const TestRing::Bracket& b, double* ms) {
        reads.push_back(b.query);
        if (failing.count(b.query)) return ReadResult::Error;
        if (!ready.count(b.query)) return ReadResult::NotReady;
        *ms = 1.0 + b.pass;
        return ReadResult::Ready;
    }
    TestRing::FrameEndCounts frameEnd(TestRing& r) {
        return r.frameEnd([this](const TestRing::Bracket& b, double* ms) { return read(b, ms); },
                          [this](const double* ms, const bool* has) {
                              std::vector<double> row;
                              for (int p = 0; p < 3; ++p) row.push_back(has[p] ? ms[p] : -1.0);
                              published.push_back(row);
                          });
    }
};
}  // namespace

TEST_CASE("vk pass timer ring: a finished frame publishes each pass once") {
    TestRing r(8);
    FakeGpu gpu;
    const auto q0 = r.begin(0);
    REQUIRE(q0 == 0);
    CHECK(r.end(0) == 1);
    gpu.ready.insert(static_cast<uint32_t>(q0));
    const auto c = gpu.frameEnd(r);
    CHECK(c.published == 1);
    CHECK(c.dropped == 0);
    REQUIRE(gpu.published.size() == 1);
    CHECK(gpu.published[0] == std::vector<double>{1.0, -1.0, -1.0});
}

TEST_CASE("vk pass timer ring: a bracket on an unsubmitted command buffer is never read (stale result after the wrap)") {
    TestRing r(8);
    FakeGpu gpu;
    // Five frames: pass 1 (the VFX shape) records, closes and is cancelled every frame; pass 0 is submitted and finishes.
    for (int f = 0; f < 5; ++f) {
        const auto q0 = r.begin(0);
        r.end(0);
        gpu.ready.insert(static_cast<uint32_t>(q0));
        const auto q1 = r.begin(1);
        r.end(1);
        gpu.ready.insert(static_cast<uint32_t>(q1));   // an older frame left "available" data in these queries
        r.cancel(1);
        const auto c = gpu.frameEnd(r);
        CHECK(c.dropped == 0);
        for (uint32_t q : gpu.reads) CHECK(q != static_cast<uint32_t>(q1));
    }
    REQUIRE(!gpu.published.empty());
    for (const auto& row : gpu.published) {
        CHECK(row[0] == 1.0);
        CHECK(row[1] == -1.0);   // the cancelled pass is absent, not a stale value
    }
}

TEST_CASE("vk pass timer: the submit guard cancels on every exit that did not submit") {
    int cancels = 0;
    auto cancel = [&] { ++cancels; };
    {
        vk_render_pass_timer::SubmitGuard<decltype(cancel)> g(cancel);   // an error return before the submit
    }
    CHECK(cancels == 1);
    {
        vk_render_pass_timer::SubmitGuard<decltype(cancel)> g(cancel);
        g.submitted();                                                   // the queue accepted the buffer
    }
    CHECK(cancels == 1);
    auto early = [&](bool fail) {
        vk_render_pass_timer::SubmitGuard<decltype(cancel)> g(cancel);
        if (fail) return false;
        g.submitted();
        return true;
    };
    CHECK(!early(true));
    CHECK(cancels == 2);
    CHECK(early(false));
    CHECK(cancels == 2);
}

TEST_CASE("vk pass timer ring: a frame whose brackets were all cancelled publishes nothing") {
    TestRing r(8);
    FakeGpu gpu;
    r.begin(0);
    r.end(0);
    r.cancel(0);
    r.begin(1);
    r.cancel(1);   // cancelled while open
    const auto c = gpu.frameEnd(r);
    CHECK(c.published == 0);
    CHECK(c.dropped == 0);
    CHECK(c.errors == 0);
    CHECK(gpu.published.empty());
    CHECK(gpu.reads.empty());
}

TEST_CASE("vk pass timer ring: cancelling a still-open bracket, or twice, never publishes it") {
    TestRing r(8);
    FakeGpu gpu;
    const auto q = r.begin(1);
    gpu.ready.insert(static_cast<uint32_t>(q));
    r.cancel(1);   // cancelled before End
    r.cancel(1);   // and again
    gpu.frameEnd(r);
    for (uint32_t read : gpu.reads) CHECK(read != static_cast<uint32_t>(q));
}

TEST_CASE("vk pass timer ring: the slot about to be reused is dropped while unfinished; younger slots wait") {
    TestRing r(8);
    FakeGpu gpu;
    unsigned dropped = 0, published = 0;
    for (int f = 0; f < 4; ++f) {   // four frames the GPU never finishes
        r.begin(2);
        r.end(2);
        const auto c = gpu.frameEnd(r);
        dropped += c.dropped;
        published += c.published;
    }
    CHECK(published == 0);
    CHECK(dropped == 1);   // only the oldest, at the wrap; the three younger ones are still waiting
}

TEST_CASE("vk pass timer ring: a hard read error discards the frame and is reported apart from drops") {
    TestRing r(8);
    FakeGpu gpu;
    const auto q = r.begin(0);
    r.end(0);
    gpu.failing.insert(static_cast<uint32_t>(q));
    const auto c = gpu.frameEnd(r);
    CHECK(c.errors == 1);
    CHECK(c.dropped == 0);
    CHECK(c.published == 0);
}

TEST_CASE("vk pass timer ring: brackets past the per-frame limit are refused and counted") {
    TestRing r(2);
    CHECK(r.begin(0) >= 0);
    r.end(0);
    CHECK(r.begin(1) >= 0);
    r.end(1);
    CHECK(r.begin(2) == -1);
    CHECK(r.takeOverflow() == 1);
}

TEST_CASE("vk pass timer: ticks scale by the device period") {
    CHECK(TicksToMs(1000, 3000, ~0ull, 1.0) == doctest::Approx(0.002));     // 2,000 ticks of 1 ns
    CHECK(TicksToMs(0, 1000000, ~0ull, 40.0) == doctest::Approx(40.0));      // 1e6 ticks of 40 ns = 40 ms
}

TEST_CASE("vk pass timer: an end stamp past the counter wrap reads as the forward distance") {
    const uint64_t mask36 = (1ull << 36) - 1ull;           // a 36-bit counter (timestampValidBits = 36)
    const uint64_t begin = mask36 - 99;                     // 100 ticks before the wrap
    const uint64_t end = 400;                               // 400 ticks after it
    CHECK(TicksToMs(begin, end, mask36, 1.0) == doctest::Approx(500.0 / 1.0e6));
}

TEST_CASE("vk pass timer: bits above the valid range are ignored") {
    const uint64_t mask32 = 0xFFFFFFFFull;
    CHECK(TicksToMs(0xABC00000010ull, 0xDEF00000110ull, mask32, 1.0) == doctest::Approx(256.0 / 1.0e6));
}
