#include "doctest.h"
#include "logistics_archive.h"

#include <limits>

namespace {
using namespace mc2::save;

LogisticsText text(const char* s) { return LogisticsText(1, s); }

LogisticsPilotRecord pilot(const char* file, float gunnery, float piloting) {
    LogisticsPilotRecord p;
    p.fileName = text(file);
    p.gunnery = gunnery;
    p.piloting = piloting;
    p.rank = logistics_detail::rankOf(gunnery, piloting);
    return p;
}

LogisticsState sample() {
    LogisticsState s;
    s.firstTimeResults = true;
    auto& c = s.campaign;
    c.campaignFile = text("data/campaign/campaign.fit");
    c.campaignName = text("Carver V");
    c.playerName = PlayerNameText(1, "Commander");
    c.cbills = 12500;
    c.currentStage = 2;
    c.hasStage = true;
    c.bigMoviePlayed = true;
    c.missionName = text("Mission 3");
    c.missionFileName = text("mc2_03");
    c.completedMissions = {0, 2};
    c.additionalPurchaseFiles = {text("data/campaign/extra.fit")};

    LogisticsVariantRecord v;
    v.chassisKey = text("Atlas");
    v.variantName = text("Atlas Custom");
    v.components = {{101, 0, 0, 0}, {102, 1, 2, 1}, {103, 2, 5, 2}};
    s.variants = {v};

    auto veteran = pilot("Bishop", 65.0f, 62.0f);
    veteran.mechKills = 4; veteran.vehicleKills = 2; veteran.infantryKills = 9; veteran.missionsCompleted = 3;
    veteran.missionsPlayed[0] = 1; veteran.missionsPlayed[2] = 1;
    veteran.medals[7] = true;
    veteran.specialtySkills[3] = true; veteran.specialtySkills[22] = true;
    auto green = pilot("Hawk", 30.0f, 28.5f);
    green.dead = true;
    s.pilots = {veteran, green};

    LogisticsMechRecord custom;
    custom.variantName = text("Atlas Custom"); custom.chassisKey = text("Atlas");
    custom.pilotFile = text("bishop"); // matched ignoring case
    custom.forceGroup = 1; custom.baseColor = 0xff0000; custom.highlightColor1 = 0x00ff00; custom.highlightColor2 = 0x0000ff;
    LogisticsMechRecord stock;
    stock.variantName = text("Thor-A"); stock.chassisKey = text("Thor"); stock.designer = true;
    s.inventory = {custom, stock};
    return s;
}

bool decodes(const LogisticsState& v) {
    LogisticsState out;
    return decodeTaggedRecord(encodeTaggedRecord(v, tagLogistics), out, tagLogistics);
}
} // namespace

TEST_CASE("logistics archive: roundtrip is exact and re-encodes to the same bytes") {
    const auto state = sample();
    const auto bytes = encodeTaggedRecord(state, tagLogistics);
    REQUIRE_FALSE(bytes.empty());
    LogisticsState out;
    REQUIRE(decodeTaggedRecord(bytes, out, tagLogistics));
    CHECK(out == state);
    CHECK(encodeTaggedRecord(out, tagLogistics) == bytes);
    LogisticsState empty, out2;
    empty.campaign.campaignFile = text("c.fit");
    out2 = sample();
    REQUIRE(decodeTaggedRecord(encodeTaggedRecord(empty, tagLogistics), out2, tagLogistics));
    CHECK(out2 == empty);
}

TEST_CASE("logistics archive: the rank helper matches LogisticsPilot::turnAverageIntoRank") {
    CHECK(logistics_detail::rankOf(50, 50) == 0);
    CHECK(logistics_detail::rankOf(51, 50) == 1);
    CHECK(logistics_detail::rankOf(70, 70) == 2);
    CHECK(logistics_detail::rankOf(70, 71) == 3);
    CHECK(logistics_detail::rankOf(80, 70) == 3);
    CHECK(logistics_detail::rankOf(80, 80) == 4);
    CHECK(logistics_detail::rankOf(100, 100) == logisticsMaxRank);
}

TEST_CASE("logistics archive: wrong tag, version mismatch and truncation are rejected") {
    const auto bytes = encodeTaggedRecord(sample(), tagLogistics);
    LogisticsState out;
    out.version = 7;
    CHECK_FALSE(decodeTaggedRecord(bytes, out, fourcc('O', 'B', 'J', 'S')));
    auto v2 = bytes;
    v2[4] = 2; // version word follows the 4-byte tag
    CHECK_FALSE(decodeTaggedRecord(v2, out, tagLogistics));
    for (std::size_t cut = 0; cut < bytes.size(); cut += 7) {
        auto shorter = std::vector<std::uint8_t>(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(cut));
        CHECK_FALSE(decodeTaggedRecord(shorter, out, tagLogistics));
    }
    CHECK(out.version == 7);
    auto badInner = sample();
    badInner.version = 10003; // the inner SaveGameVersionNumber must be 10004
    CHECK_FALSE(decodes(badInner));
}

TEST_CASE("logistics archive: out-of-range values are rejected") {
    auto s = sample();
    s.pilots[0].rank = 5;
    CHECK_FALSE(decodes(s));
    s = sample();
    s.pilots[0].rank = 0; // stale: the skills say veteran
    CHECK_FALSE(decodes(s));
    s = sample();
    s.pilots[0].gunnery = 101.0f;
    CHECK_FALSE(decodes(s));
    s = sample();
    s.pilots[0].mechKills = -1;
    CHECK_FALSE(decodes(s));
    s = sample();
    s.pilots[0].missionsPlayed[10] = 2;
    CHECK_FALSE(decodes(s));
    s = sample();
    s.variants[0].components[0].location = logisticsLocationCount;
    CHECK_FALSE(decodes(s));
    s = sample();
    s.variants[0].components[1].x = -1;
    CHECK_FALSE(decodes(s));
    s = sample();
    s.variants[0].components[2].y = logisticsMaxCoord + 1;
    CHECK_FALSE(decodes(s));
    s = sample();
    s.variants[0].components.assign(logisticsMaxComponents + 1, LogisticsComponentRecord{});
    CHECK_FALSE(decodes(s));
    s = sample();
    s.variants[0].variantName = text("");
    CHECK_FALSE(decodes(s));
    s = sample();
    s.inventory[0].forceGroup = logisticsMaxForceGroup + 1;
    CHECK_FALSE(decodes(s));
    s = sample();
    s.campaign.currentStage = -1;
    CHECK_FALSE(decodes(s));
    s = sample();
    s.campaign.campaignFile = text("");
    CHECK_FALSE(decodes(s));
    s = sample();
    s.campaign.playerName = PlayerNameText(1, std::string(logisticsMaxPlayerName + 1, 'x'));
    CHECK_FALSE(decodes(s));
    s = sample();
    s.campaign.completedMissions = {2, 2};
    CHECK_FALSE(decodes(s));
    s = sample();
    s.campaign.hasStage = false; // stage fields must be defaults without a stage block
    CHECK_FALSE(decodes(s));
    s.campaign.bigMoviePlayed = false; s.campaign.missionName = text(""); s.campaign.missionFileName = text("");
    s.campaign.completedMissions.clear();
    CHECK(decodes(s));
    s = sample();
    s.pilots[0].gunnery = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(decodes(s));
}

TEST_CASE("logistics archive: dangling and duplicate references are rejected") {
    auto s = sample();
    s.inventory[0].pilotFile = text("Nobody");
    CHECK_FALSE(decodes(s));
    s = sample();
    s.inventory[0].variantName = text("Atlas Missing"); // not designer, so it must be a stored variant
    CHECK_FALSE(decodes(s));
    s = sample();
    s.inventory[0].chassisKey = text("Thor"); // right variant name, wrong chassis
    CHECK_FALSE(decodes(s));
    s = sample();
    s.inventory[1].pilotFile = text("Bishop"); // the same pilot in two mechs
    CHECK_FALSE(decodes(s));
    s = sample();
    s.pilots[1].fileName = text("BISHOP");
    CHECK_FALSE(decodes(s));
    s = sample();
    s.variants.push_back(s.variants[0]);
    s.variants[1].variantName = text("atlas custom");
    CHECK_FALSE(decodes(s));
    s = sample();
    s.inventory[0].pilotFile = text("");
    CHECK_FALSE(decodes(s));
    s = sample();
    s.inventory[0].pilotFile.reset(); // no pilot is valid
    CHECK(decodes(s));
}
