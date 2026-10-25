// LOGGING-FACADE-P0: mc2::log facade (GameOS/gameos/utils/mc2log.{h,cpp}).
// Level filtering must not evaluate args, brace escaping, runtime-format fail-soft,
// MC2_EVENT byte-exactness vs legacy printf, MC2_LOG spec parsing (incl. legacy "1").
#include "doctest.h"
#include "msvc_printf_reference.h"
#include "../../GameOS/gameos/utils/mc2log.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

int g_fmtCalls = 0;
struct Counted { int v; };

struct Capture {
    std::FILE* f;
    mc2::log::Stream s;
    explicit Capture(mc2::log::Stream st) : f(std::tmpfile()), s(st) {
        mc2::log::setStreamOverrideForTest(s, f);
    }
    ~Capture() {
        mc2::log::setStreamOverrideForTest(s, nullptr);
        if (f) std::fclose(f);
    }
    std::string str() {
        std::fflush(f);
        std::rewind(f);
        std::string out;
        char buf[256];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
        return out;
    }
};

void resetSpec() { mc2::log::setSpec("*:info"); }

// Runs after the main thread's thread_local objects are destroyed. A wrong byte, or a crash, ends the test binary with status 3 / a fault.
void logAtExit()
{
    char got[32] = {};
    if (std::FILE* f = std::tmpfile()) {
        mc2::log::setSpec("*:info");
        mc2::log::setStreamOverrideForTest(mc2::log::Stream::Err, f);
        MC2_LOG(Core, Error, "exit:{}", 7);
        std::fflush(f);
        std::rewind(f);
        std::fread(got, 1, sizeof got - 1, f);
        mc2::log::setStreamOverrideForTest(mc2::log::Stream::Err, nullptr);
        std::fclose(f);
    }
    if (std::strcmp(got, "exit:7\n") != 0)
        std::_Exit(3);
}

}  // namespace

template <>
struct std::formatter<Counted> : std::formatter<int> {
    auto format(const Counted& c, std::format_context& ctx) const {
        ++g_fmtCalls;
        return std::formatter<int>::format(c.v, ctx);
    }
};

TEST_CASE("mc2log: disabled level evaluates neither format nor arguments") {
    resetSpec();
    Capture cap(mc2::log::Stream::Err);
    mc2::log::setSpec("render:warn");
    int evals = 0;
    g_fmtCalls = 0;
    auto arg = [&]() { ++evals; return Counted{7}; };
    MC2_LOG(Render, Debug, "dbg {}", arg());
    MC2_LOG(Render, Info, "inf {}", arg());
    CHECK(evals == 0);
    CHECK(g_fmtCalls == 0);
    CHECK(cap.str().empty());
    MC2_LOG(Render, Warn, "warn {}", arg());
    CHECK(evals == 1);
    CHECK(g_fmtCalls == 1);
    CHECK(cap.str() == "warn 7\n");
    // other categories keep the legacy Info default
    MC2_LOG(Terrain, Info, "t{}", 1);
    CHECK(cap.str() == "warn 7\nt1\n");
    resetSpec();
}

TEST_CASE("mc2log: ONCE and EVERY gate before evaluating args") {
    resetSpec();
    Capture cap(mc2::log::Stream::Err);
    int evals = 0;
    for (int i = 0; i < 3; ++i) MC2_LOG_ONCE(Core, Info, "once {}", ++evals);
    CHECK(evals == 1);
    CHECK(cap.str() == "once 1\n");
    mc2::log::setSpec("core:error");
    int e2 = 0;
    MC2_LOG_ONCE(Core, Info, "x {}", ++e2);
    MC2_LOG_EVERY(Core, Info, 10.0, "y {}", ++e2);
    MC2_LOG_TOPIC(Core, "nope", Info, "z {}", ++e2);
    CHECK(e2 == 0);
    resetSpec();
}

TEST_CASE("mc2log: a line logged from an atexit handler uses a buffer that outlives the thread_local destructors") {
    resetSpec();
    Capture c(mc2::log::Stream::Err);
    MC2_LOG(Core, Error, "warm the main thread's buffer");
    CHECK(std::atexit(logAtExit) == 0);
}

TEST_CASE("mc2log: brace escaping and plain text") {
    resetSpec();
    Capture cap(mc2::log::Stream::Err);
    MC2_LOG(Core, Info, "{{\"k\":{}}}", 5);
    MC2_LOG(Core, Info, "100%% done {}%", 3);   // '%' is literal in std::format
    CHECK(cap.str() == "{\"k\":5}\n100%% done 3%\n");
}

TEST_CASE("mc2log: runtime format strings never throw") {
    resetSpec();
    Capture cap(mc2::log::Stream::Err);
    std::string badTmpl = "bad {";
    CHECK_NOTHROW(MC2_LOGV(Core, Error, std::string_view(badTmpl), 1));
    std::string out = cap.str();
    CHECK(out.find("bad {") == 0);
    CHECK(out.find("[LOG FORMAT ERROR]") != std::string::npos);
    CHECK_NOTHROW(MC2_LOGV(Core, Error, std::string_view("{} {}"), 1));   // too few args
    CHECK(cap.str().find("[LOG FORMAT ERROR]", out.size()) != std::string::npos);
    std::string ok = "v={} w={:.2f}";
    MC2_LOGV(Core, Error, std::string_view(ok), 3, 1.5);
    const std::string all = cap.str();
    CHECK(all.size() >= 11);
    CHECK(all.substr(all.size() - 11) == "v=3 w=1.50\n");
}

TEST_CASE("mc2log: MC2_EVENT bytes equal the legacy printf bytes") {
    resetSpec();
    {
        Capture cap(mc2::log::Stream::Err);
        char legacy[256];
        std::snprintf(legacy, sizeof legacy,
            "[PATCH_STREAM v1] event=overflow slot=%u kind=%s count=%u cap=%u\n",
            2u, "bucket_count", 4096u, 4096u);
        MC2_EVENT(Terrain, "PATCH_STREAM", 1, "event=overflow slot={} kind={} count={} cap={}",
            2u, "bucket_count", 4096u, 4096u);
        CHECK(cap.str() == legacy);
    }
    {
        Capture cap(mc2::log::Stream::Err);
        char legacy[256];
        std::snprintf(legacy, sizeof legacy,
            "[BUCKET_CENSUS v1] frame=%llu raw=%u avg=%.1f neg=%d hex=0x%llx\n",
            (unsigned long long)123456789012ull, 7u, 2.25, -5, (unsigned long long)0xdeadbeefull);
        MC2_EVENT(Terrain, "BUCKET_CENSUS", 1, "frame={} raw={} avg={:.1f} neg={} hex=0x{:x}",
            (unsigned long long)123456789012ull, 7u, 2.25, -5, (unsigned long long)0xdeadbeefull);
        CHECK(cap.str() == legacy);
    }
    {   // stdout variant + unversioned tag; streams stay separate
        Capture o(mc2::log::Stream::Out);
        Capture e(mc2::log::Stream::Err);
        MC2_EVENT_OUT(Perf, "PERF", 1, "avg_fps={:.1f} p50_ms={:.2f}", 59.96, 16.667);
        MC2_EVENT_NOVER(Perf, "VEG_SUMMARY", "n={}", 3);
        CHECK(o.str() == "[PERF v1] avg_fps=60.0 p50_ms=16.67\n");
        CHECK(e.str() == "[VEG_SUMMARY] n=3\n");
    }
    {   // events bypass the level filter, obey the category toggle only
        Capture cap(mc2::log::Stream::Err);
        mc2::log::setSpec("*:error");
        MC2_EVENT(Core, "X", 2, "a={}", 1);
        CHECK(cap.str() == "[X v2] a=1\n");
        mc2::log::setSpec("core:off");
        MC2_EVENT(Core, "X", 2, "a={}", 2);
        CHECK(cap.str() == "[X v2] a=1\n");
        resetSpec();
    }
}

TEST_CASE("mc2log: MC2_LOG spec parsing") {
    using namespace mc2::log;
    // legacy values: not specs, no overrides, streams live iff variable set
    for (const char* v : {"1", "", "0", "yes"}) {
        ParsedSpec p = parseSpec(v);
        CHECK_FALSE(p.isSpec);
        CHECK(p.errors == 0);
        for (size_t i = 0; i < kCatCount; ++i) CHECK_FALSE(p.levelSet[i]);
        CHECK(streamsLiveFor(true));
    }
    CHECK_FALSE(streamsLiveFor(false));

    ParsedSpec p = parseSpec("render:debug, vk=trace ,*:warn");
    CHECK(p.isSpec);
    CHECK(p.errors == 0);
    CHECK(p.level[size_t(Cat::Render)] == Level::Warn);   // later '*' overrides earlier tokens
    CHECK(p.level[size_t(Cat::Vk)] == Level::Warn);
    CHECK(p.level[size_t(Cat::Terrain)] == Level::Warn);

    p = parseSpec("*:warn,render:debug,vk=trace");
    CHECK(p.level[size_t(Cat::Render)] == Level::Debug);
    CHECK(p.level[size_t(Cat::Vk)] == Level::Trace);
    CHECK(p.level[size_t(Cat::Gl)] == Level::Warn);

    p = parseSpec("terrain=indirect+surface,bogus:info,render:loud");
    CHECK(p.isSpec);
    CHECK(p.errors == 2);
    REQUIRE(p.topics.size() == 2);
    CHECK(p.topics[0].cat == Cat::Terrain);
    CHECK(p.topics[0].name == "indirect");
    CHECK(p.topics[1].name == "surface");

    p = parseSpec("water:off");
    CHECK(p.eventOff[size_t(Cat::Water)]);

    setSpec("terrain=indirect+surface");
    CHECK(topicEnabled(Cat::Terrain, "indirect"));
    CHECK_FALSE(topicEnabled(Cat::Terrain, "lod"));
    CHECK_FALSE(topicEnabled(Cat::Water, "indirect"));
}

TEST_CASE("mc2log: rate limit and once") {
    using namespace mc2::log;
    CHECK(allowRateAt(0x1111, 10.0, 1'000'000'000));       // first call passes
    CHECK_FALSE(allowRateAt(0x1111, 10.0, 1'050'000'000)); // 50ms later, 100ms interval
    CHECK(allowRateAt(0x1111, 10.0, 1'110'000'000));
    CHECK(allowOnce(0x2222));
    CHECK_FALSE(allowOnce(0x2222));
    CHECK(allowOnce(0x3333));
}

TEST_CASE("mc2log: gos_terrain_lighting events equal the legacy printf bytes") {
    resetSpec();
    // Same arguments, same C types as the call sites (frame counters unsigned long long / long long,
    // owner pointer printed as 16 upper-case hex digits like MSVC %p).
    const void* owners[] = {nullptr, reinterpret_cast<const void*>(0x24DB7D100F0ull),
                            reinterpret_cast<const void*>(0x7FF612345678ull)};
    for (const void* owner : owners) {
        Capture cap(mc2::log::Stream::Err);
        char legacy[512];
        corpusSnprintf(legacy, sizeof legacy,
            "[LIGHT_POOL_DUMP v1] idx=%ld slot=%ld type=%ld active=%d ownerEnabled=%d "
            "pos=(%.1f,%.1f,%.1f) argb=%08lx far=%.0f class=%s owner=%p packed=%d\n",
            5L, -1L, 3L, 1, 0, -3288.34f, 2739.96f, 0.0f, 0xff746438ul, 80.0f, "bldg", owner, 1);
        MC2_EVENT(Terrain, "LIGHT_POOL_DUMP", 1,
            "idx={} slot={} type={} active={} ownerEnabled={} "
            "pos=({:.1f},{:.1f},{:.1f}) argb={:08x} far={:.0f} class={} owner={:016X} packed={}",
            5L, -1L, 3L, 1, 0, -3288.34f, 2739.96f, 0.0f, 0xff746438ul, 80.0f, "bldg",
            reinterpret_cast<std::uintptr_t>(owner), 1);
        CHECK(cap.str() == legacy);
    }
    {
        Capture cap(mc2::log::Stream::Err);
        char legacy[256];
        std::snprintf(legacy, sizeof legacy, "[LIGHT_POOL_DUMP v1] frame_tick=%lu isNight=%d nTerrainLights=%ld\n",
                      241ul, 1, 2L);
        MC2_EVENT(Terrain, "LIGHT_POOL_DUMP", 1, "frame_tick={} isNight={} nTerrainLights={}", 241ul, 1, 2L);
        CHECK(cap.str() == legacy);
    }
    {
        Capture cap(mc2::log::Stream::Err);
        char legacy[256];
        std::snprintf(legacy, sizeof legacy,
            "[TERRAIN_LIGHTING_PARITY v1] event=mismatch frame=%llu vertex=%d field=%s legacy=0x%08X gpu=0x%08X\n",
            1234567890123ull, 42, "lightRGB", 0xff00aa11u, 0x0u);
        MC2_EVENT(Terrain, "TERRAIN_LIGHTING_PARITY", 1,
                  "event=mismatch frame={} vertex={} field={} legacy=0x{:08X} gpu=0x{:08X}",
                  1234567890123ull, 42, "lightRGB", 0xff00aa11u, 0x0u);
        CHECK(cap.str() == legacy);
    }
    {
        Capture cap(mc2::log::Stream::Err);
        char legacy[256];
        std::snprintf(legacy, sizeof legacy,
            "[TERRAIN_LIGHTING_PARITY v1] event=summary frames=%lld verts_checked=%lld total_mismatches=%lld\n",
            600ll, 3123456789ll, 0ll);
        MC2_EVENT(Terrain, "TERRAIN_LIGHTING_PARITY", 1,
                  "event=summary frames={} verts_checked={} total_mismatches={}", 600ll, 3123456789ll, 0ll);
        CHECK(cap.str() == legacy);
    }
    for (double d : {0.0, 1.5, 0.25, 1e-5, 123456.789, 1e10}) {
        Capture o(mc2::log::Stream::Out);
        char legacy[256];
        std::snprintf(legacy, sizeof legacy, "[HAZE_PARITY v1] event=summary frames=%llu max_abs_delta=%g violations=%llu\n",
                      600ull, d, 7ull);
        MC2_EVENT_OUT(Terrain, "HAZE_PARITY", 1, "event=summary frames={} max_abs_delta={:g} violations={}", 600ull, d, 7ull);
        CHECK(o.str() == legacy);
    }
}
