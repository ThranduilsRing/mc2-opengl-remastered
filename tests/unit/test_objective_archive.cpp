#include "doctest.h"
#include "objective_archive.h"

#include <limits>

namespace {
using namespace mc2::save;

ObjectiveText text(const char* s) { return ObjectiveText(1, s); }

ObjectiveCondition cond(std::uint8_t species) {
    ObjectiveCondition c;
    c.species = species;
    return c;
}

// One condition per payload kind, one action per payload kind, in two teams.
ObjectiveSetState sample() {
    ObjectiveSetState s;
    TeamObjectives t0;
    t0.alignment = 0;
    t0.flags = {{text("flag0"), true}, {text("Gate"), false}};
    ObjectiveRecord o;
    o.title = text("Destroy the base");
    o.description = text("Level it.");
    o.titleUseResourceString = true; o.titleResourceStringId = 4001;
    o.priority = 1; o.resourcePoints = 500;
    o.previousPrimaryMustBeComplete = true;
    o.displayMarker = 2; o.markerX = 128.5f; o.markerY = -64.0f;
    o.activateOnFlag = true; o.activateFlagId = text("Gate");
    o.modelName = text("radar"); o.modelType = 1; o.modelBaseColor = 0x00ff00; o.modelHighlightColor = 7; o.modelHighlightColor2 = 9;
    o.modelScale = 1.5f;
    o.isActive = true; o.activationTime = 12.25; o.resolved = true; o.resolvedStatus = 1; o.changedStatus = true;
    auto num = cond(1); num.num = 6;
    auto unit = cond(13); unit.watchId = 42;
    auto structure = cond(4); structure.watchId = 77;
    auto area = cond(19); area.centerX = 10; area.centerY = 20; area.radius = 30;
    auto flag = cond(22); flag.flagId = text("flag0"); flag.flagValue = true;
    auto time = cond(23); time.time = 90.0f;
    o.conditions = {cond(0), num, unit, structure, area, flag, time};
    o.failureConditions = {cond(10)};
    ObjectiveAction bik; bik.species = 0; bik.text = text("data/movies/intro.bik");
    ObjectiveAction msg; msg.species = 2; msg.text = text("Well done");
    ObjectiveAction res; res.species = 3; res.resourceStringId = 55;
    ObjectiveAction setFlag; setFlag.species = 4; setFlag.text = text("Gate"); setFlag.flagValue = true;
    ObjectiveAction buy; buy.species = 5; buy.text = text("data/campaign/purchase.fit");
    ObjectiveAction rem; rem.species = 6; rem.watchId = 91;
    o.actions = {bik, msg, res, setFlag, buy, rem};
    ObjectiveAction wav; wav.species = 1; wav.text = text("sounds/fail.wav");
    o.failureActions = {wav};
    ObjectiveRecord secondary;
    secondary.priority = 2;
    t0.objectives = {o, secondary};
    TeamObjectives t3;
    t3.alignment = 3;
    s.teams = {t0, t3};
    return s;
}

bool decodes(const ObjectiveSetState& v) {
    ObjectiveSetState out;
    return decodeTaggedRecord(encodeTaggedRecord(v, tagObjectives), out, tagObjectives);
}
} // namespace

TEST_CASE("objective archive: every species has a payload kind and the group species are invalid") {
    for (std::uint8_t i = 0; i < conditionSpeciesCount; ++i) {
        const bool group = i == 2 || i == 7 || i == 12;
        CHECK((conditionPayloadOf(i) == ConditionPayload::Invalid) == group);
    }
    CHECK(conditionPayloadOf(conditionSpeciesCount) == ConditionPayload::Invalid);
    for (std::uint8_t i = 0; i < actionSpeciesCount; ++i) CHECK(actionPayloadOf(i) != ActionPayload::Invalid);
    CHECK(actionPayloadOf(actionSpeciesCount) == ActionPayload::Invalid);
}

TEST_CASE("objective archive: roundtrip is exact and re-encodes to the same bytes") {
    const auto state = sample();
    const auto bytes = encodeTaggedRecord(state, tagObjectives);
    REQUIRE_FALSE(bytes.empty());
    ObjectiveSetState out;
    REQUIRE(decodeTaggedRecord(bytes, out, tagObjectives));
    CHECK(out == state);
    CHECK(encodeTaggedRecord(out, tagObjectives) == bytes);
    ObjectiveSetState empty, out2;
    out2.teams.resize(2);
    REQUIRE(decodeTaggedRecord(encodeTaggedRecord(empty, tagObjectives), out2, tagObjectives));
    CHECK(out2 == empty);
}

TEST_CASE("objective archive: wrong tag, version mismatch and truncation are rejected") {
    const auto bytes = encodeTaggedRecord(sample(), tagObjectives);
    ObjectiveSetState out;
    out.version = 9;
    CHECK_FALSE(decodeTaggedRecord(bytes, out, fourcc('C', 'N', 'T', 'C')));
    auto v2 = bytes;
    v2[4] = 2; // version word follows the 4-byte tag
    CHECK_FALSE(decodeTaggedRecord(v2, out, tagObjectives));
    for (std::size_t cut = 0; cut < bytes.size(); cut += 7) {
        auto shorter = std::vector<std::uint8_t>(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(cut));
        CHECK_FALSE(decodeTaggedRecord(shorter, out, tagObjectives));
    }
    CHECK(out.version == 9);
    auto badInner = sample();
    badInner.version = 4; // the inner CObjectives version must be 3
    CHECK_FALSE(decodes(badInner));
}

TEST_CASE("objective archive: out-of-range species, ids and values are rejected") {
    auto s = sample();
    s.teams[0].objectives[0].conditions[0].species = conditionSpeciesCount;
    CHECK_FALSE(decodes(s));
    s = sample();
    s.teams[0].objectives[0].conditions[0].species = 7; // CAPTURE_OR_DESTROY_ENEMY_UNIT_GROUP: no factory case
    CHECK_FALSE(decodes(s));
    s = sample();
    s.teams[0].objectives[0].actions[0].species = actionSpeciesCount;
    CHECK_FALSE(decodes(s));
    s = sample();
    s.teams[0].alignment = objectiveMaxTeams;
    CHECK_FALSE(decodes(s));
    s = sample();
    s.teams[0].objectives[0].priority = 3;
    CHECK_FALSE(decodes(s));
    s = sample();
    s.teams[0].objectives[0].resolvedStatus = 3;
    CHECK_FALSE(decodes(s));
    s = sample();
    s.teams[0].objectives[0].displayMarker = 3;
    CHECK_FALSE(decodes(s));
    s = sample();
    s.teams[0].objectives[0].conditions[4].radius = -1.0f;
    CHECK_FALSE(decodes(s));
    s = sample();
    s.teams[0].objectives[0].conditions[6].time = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(decodes(s));
    s = sample();
    s.teams[0].objectives[0].activationTime = -1.0;
    CHECK_FALSE(decodes(s));
    s = sample();
    s.teams[0].objectives[0].conditions.assign(objectiveMaxParts + 1, cond(0));
    CHECK_FALSE(decodes(s));
    s = sample();
    s.teams[0].objectives[0].title = ObjectiveText(1, std::string(objectiveMaxText + 1, 'x'));
    CHECK_FALSE(decodes(s));
}

TEST_CASE("objective archive: dangling, duplicate, unordered and stray fields are rejected") {
    auto s = sample();
    s.teams[0].objectives[0].conditions[2].watchId = 0; // specific unit with no target
    CHECK_FALSE(decodes(s));
    s = sample();
    s.teams[0].objectives[0].conditions[3].watchId = -5;
    CHECK_FALSE(decodes(s));
    s = sample();
    s.teams[0].objectives[0].actions[5].watchId = 0; // _REMOVE_STRUCTURE with no target
    CHECK_FALSE(decodes(s));
    s = sample();
    s.teams[0].flags.push_back({text("GATE"), true}); // duplicate of "Gate" ignoring case
    CHECK_FALSE(decodes(s));
    s = sample();
    s.teams[0].flags[0].id = text(""); // empty flag id
    CHECK_FALSE(decodes(s));
    s = sample();
    s.teams[0].objectives[0].activateFlagId = text(""); // trigger set but no flag id
    CHECK_FALSE(decodes(s));
    s = sample();
    s.teams[0].objectives[1].resetStatusFlagId = text("flag0"); // flag id without the trigger
    CHECK_FALSE(decodes(s));
    s = sample();
    std::swap(s.teams[0], s.teams[1]); // not ascending
    CHECK_FALSE(decodes(s));
    s = sample();
    s.teams[1].alignment = 0; // duplicate alignment
    CHECK_FALSE(decodes(s));
    s = sample();
    s.teams[0].objectives[0].conditions[0].num = 5; // payload foreign to the species
    CHECK_FALSE(decodes(s));
    s = sample();
    s.teams[0].objectives[0].actions[2].text = text("x"); // text on a resource-string action
    CHECK_FALSE(decodes(s));
}
