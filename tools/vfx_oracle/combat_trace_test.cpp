#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "code/combat_trace.h"
#include <limits>
#include <string_view>
#include <vector>

namespace {
bool enabled = true;
std::vector<std::string>* borrowedPayloads = nullptr;
struct Vector { float x, y, z; };
struct Shot {
    float damage;
    int hitLocation, masterId, attackerWID;
    float entryAngle;
};
struct Target {
    long getHandle() const { return 9; }
    long getWatchID(bool create) const { CHECK_FALSE(create); return 10; }
    Vector getPosition() const { return {4.0f, 8.0f, 16.0f}; }
};
struct Capture {
    std::vector<std::string> payloads;
    Capture() { enabled = true; borrowedPayloads = &payloads; }
    ~Capture() { borrowedPayloads = nullptr; }
};
}

namespace mc2_diag {
bool tagEnabled(const char* tag) { CHECK(std::string_view(tag) == "VFX_COMBAT"); return ::enabled; }
bool writeEvent(const char* tag, int version, std::uint64_t frame, const char* data)
{
    CHECK(std::string_view(tag) == "VFX_COMBAT");
    CHECK(version == 1);
    CHECK(frame == 0);
    REQUIRE(borrowedPayloads != nullptr);
    borrowedPayloads->emplace_back(data);
    return true;
}
}

TEST_CASE_FIXTURE(Capture, "vfx_trace/full bolt and carnage payloads retain field order and bit patterns")
{
    mc2::CombatTrace trace;
    trace.begin(12, 8);
    const Vector position{1.0f, -2.0f, -0.0f}, flight{0.5f, 2.0f, 0.0f};
    Target target;
    const Shot shot{1.0f, -1, 11, 10, -0.0f};
    unsigned flightReads = 0;
    auto getFlight = [&] { ++flightReads; return &flight; };
    trace.write("bolt", 3, 0, "birth", "", position, getFlight, &target, static_cast<const Shot*>(nullptr));
    REQUIRE(payloads.size() == 1);
    CHECK(payloads.back() == R"({"kind":"bolt","slot":3,"generation":1,"seq":0,"tick":0,"event":"birth","reason":"","effect_id":8,"type":12})");
    CHECK(flightReads == 0);
    trace.write("bolt", 3, 7, "damage", "direct", position, getFlight, &target, &shot);
    CHECK(payloads.back() == R"({"kind":"bolt","slot":3,"generation":1,"seq":1,"tick":7,"event":"damage","reason":"direct","effect_id":8,"type":12,"position_bits":[1065353216,3221225472,2147483648],"flight_bits":[1056964608,1073741824,0],"target":9,"target_wid":10,"damage_bits":1065353216,"location":-1,"master":11,"attacker":10,"entry_bits":2147483648,"target_position_bits":[1082130432,1090519040,1098907648]})");
    CHECK(flightReads == 1);
    auto noFlight = [] { return static_cast<const Vector*>(nullptr); };
    trace.write("carnage", 4, 8, "damage", "collision", position, noFlight, &target, &shot);
    CHECK(payloads.back() == R"({"kind":"carnage","slot":4,"generation":1,"seq":2,"tick":8,"event":"damage","reason":"collision","effect_id":8,"type":12,"position_bits":[1065353216,3221225472,2147483648],"target":9,"target_wid":10,"damage_bits":1065353216,"location":-1,"master":11,"attacker":10,"entry_bits":2147483648,"target_position_bits":[1082130432,1090519040,1098907648]})");
}

TEST_CASE_FIXTURE(Capture, "vfx_trace/disabled events and pool reuse retain object-owned sequencing")
{
    mc2::CombatTrace a, b;
    const Vector position{};
    auto noFlight = [] { return static_cast<const Vector*>(nullptr); };
    auto emit = [&](mc2::CombatTrace& trace) {
        trace.write("bolt", 3, 0, "birth", "", position, noFlight,
                    static_cast<Target*>(nullptr), static_cast<const Shot*>(nullptr));
    };
    emit(a);
    CHECK(payloads.empty());
    a.begin(12, 8);
    enabled = false;
    emit(a);
    CHECK(a.sequence == 0);
    enabled = true;
    emit(a);
    CHECK(a.sequence == 1);
    a.live = false;
    emit(a);
    CHECK(a.sequence == 1);
    a.begin(13, 9);
    emit(a);
    CHECK(a.generation == 2);
    CHECK(a.sequence == 1);
    b.begin(12, 8);
    emit(b);
    CHECK(b.generation == 1);
    CHECK(b.sequence == 1);
    CHECK(a.generation == 2);
    CHECK(payloads.size() == 3);
}

TEST_CASE_FIXTURE(Capture, "vfx_trace/formatting is complete beyond the former fixed buffer")
{
    mc2::CombatTrace trace;
    trace.begin(std::numeric_limits<long>::min(), std::numeric_limits<long>::max());
    const Vector position{};
    const std::string reason(4096, 'x');
    trace.write("bolt", 3, 0, "retire", reason.c_str(), position,
                [] { return static_cast<const Vector*>(nullptr); },
                static_cast<Target*>(nullptr), static_cast<const Shot*>(nullptr));
    REQUIRE(payloads.size() == 1);
    CHECK(payloads.front().find(reason) != std::string::npos);
    CHECK(payloads.front().ends_with(",\"position_bits\":[0,0,0]}"));
}
