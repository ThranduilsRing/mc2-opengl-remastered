#include "doctest.h"
#include "environment_archive.h"
#include "media_cue_archive.h"
#include "support_craft_archive.h"

#include <limits>

namespace {
using namespace mc2::save;

EnvironmentState envSample() {
    EnvironmentState e;
    e.weatherActive = true;
    e.rainLevel = 2.5f; e.lightningLevel = 1.0f; e.rainTrend = -1.0f; e.rainFactor = 0.07f; e.rainLightLevel = 2.0f;
    e.baseRainChance = 40; e.baseLightningChance = 10;
    e.nextRainCheckTick = 600; e.nextLightningCheckTick = 900; e.thunderDueTick = 1234u;
    e.scheduleSystemId = 3; e.nextCueSequence = 17;
    return e;
}

SupportCraftState supportSample() {
    SupportCraftState s;
    SupportCraft a;
    a.commanderId = 1; a.vehicleId = 77; a.position = {10, 20, 30};
    a.salvageMechRef = mc2::sim::ObjectRef{5, 2};
    a.paintingVtol = true; a.vTolTime = 3.5f; a.mechRecovered = true;
    a.currentFrame = 12; a.currentGestureId = 4;
    SupportCraft b;
    b.commanderId = 2; b.vehicleDropped = true;
    s.craft = {a, b};
    return s;
}

template<class T> bool decodes(const T& v, std::uint32_t tag) {
    T out;
    return decodeTaggedRecord(encodeTaggedRecord(v, tag), out, tag);
}
} // namespace

TEST_CASE("environment archive: roundtrip is exact and re-encodes to the same bytes") {
    const auto state = envSample();
    const auto bytes = encodeTaggedRecord(state, tagEnvironment);
    REQUIRE_FALSE(bytes.empty());
    EnvironmentState out;
    REQUIRE(decodeTaggedRecord(bytes, out, tagEnvironment));
    CHECK(out == state);
    CHECK(encodeTaggedRecord(out, tagEnvironment) == bytes);
}

TEST_CASE("environment archive: default (no thunder pending) roundtrips") {
    EnvironmentState empty, out;
    out.rainLevel = 3.0f;
    REQUIRE(decodeTaggedRecord(encodeTaggedRecord(empty, tagEnvironment), out, tagEnvironment));
    CHECK(out == empty);
    CHECK_FALSE(out.thunderDueTick.has_value());
}

TEST_CASE("environment archive: wrong tag, truncation and invariants are rejected") {
    const auto bytes = encodeTaggedRecord(envSample(), tagEnvironment);
    EnvironmentState out;
    out.nextCueSequence = 5;
    CHECK_FALSE(decodeTaggedRecord(bytes, out, tagMediaCue));
    auto cut = bytes;
    cut.pop_back();
    CHECK_FALSE(decodeTaggedRecord(cut, out, tagEnvironment));
    CHECK(out.nextCueSequence == 5);
    auto e = envSample();
    e.rainLevel = 5.0f;
    CHECK_FALSE(decodes(e, tagEnvironment));
    e = envSample();
    e.baseRainChance = 101;
    CHECK_FALSE(decodes(e, tagEnvironment));
    e = envSample();
    e.rainTrend = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(decodes(e, tagEnvironment));
}

TEST_CASE("support craft archive: roundtrip is exact and re-encodes to the same bytes") {
    const auto state = supportSample();
    const auto bytes = encodeTaggedRecord(state, tagSupportCraft);
    REQUIRE_FALSE(bytes.empty());
    SupportCraftState out;
    REQUIRE(decodeTaggedRecord(bytes, out, tagSupportCraft));
    CHECK(out == state);
    CHECK(encodeTaggedRecord(out, tagSupportCraft) == bytes);
    SupportCraftState empty, out2;
    out2.craft.push_back({});
    REQUIRE(decodeTaggedRecord(encodeTaggedRecord(empty, tagSupportCraft), out2, tagSupportCraft));
    CHECK(out2 == empty);
}

TEST_CASE("support craft archive: wrong tag, truncation and invariants are rejected") {
    const auto bytes = encodeTaggedRecord(supportSample(), tagSupportCraft);
    SupportCraftState out;
    out.craft.push_back({});
    CHECK_FALSE(decodeTaggedRecord(bytes, out, tagEnvironment));
    auto cut = bytes;
    cut.pop_back();
    CHECK_FALSE(decodeTaggedRecord(cut, out, tagSupportCraft));
    CHECK(out.craft.size() == 1);
    auto s = supportSample();
    s.craft[1].commanderId = 1; // not ascending
    CHECK_FALSE(decodes(s, tagSupportCraft));
    s = supportSample();
    s.craft[0].vTolTime = -1.0f;
    CHECK_FALSE(decodes(s, tagSupportCraft));
    s = supportSample();
    s.craft.resize(maxSupportCraft + 1);
    for (std::uint32_t i = 0; i < s.craft.size(); ++i) s.craft[i].commanderId = i;
    CHECK_FALSE(decodes(s, tagSupportCraft));
}
