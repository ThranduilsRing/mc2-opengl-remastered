#include "save_field_probe.h"
#include "team_save_data.h"

TEST_CASE("save fields: TeamData independent census") {
    using T = TeamData;
    auto sample = save_probe::patterned<T>(&T::id, &T::rosterSize, &T::roster);
    SAVE_FIELD_SHAPE(sample, id, rosterSize, roster);
    CHECK(save_probe::memberBytes<T>(&T::id, &T::rosterSize, &T::roster) == sizeof(T));
    CHECK(save_probe::roundtrip(sample, [](auto& a, auto& v) { a(v); }, &T::id, &T::rosterSize, &T::roster));
    auto bytes = mc2::save::encodeTaggedRecord(sample, mc2::save::teamDataTag);
    CHECK(bytes.size() == 496);
    T decoded{};
    REQUIRE(mc2::save::decodeTaggedRecord(bytes, decoded, mc2::save::teamDataTag));
    CHECK(save_probe::equal(sample.roster, decoded.roster));
    bytes.push_back(0);
    CHECK_FALSE(mc2::save::decodeTaggedRecord(bytes, decoded, mc2::save::teamDataTag));
}

TEST_CASE("save fields: TeamStaticData independent census") {
    using T = TeamStaticData;
    auto sample = save_probe::patterned<T>(&T::numTeams, &T::homeTeamId, &T::relations, &T::noPain);
    SAVE_FIELD_SHAPE(sample, numTeams, homeTeamId, relations, noPain);
    CHECK(save_probe::memberBytes<T>(&T::numTeams, &T::homeTeamId, &T::relations, &T::noPain) == sizeof(T));
    CHECK(save_probe::roundtrip(sample, [](auto& a, auto& v) { a(v); },
        &T::numTeams, &T::homeTeamId, &T::relations, &T::noPain));
    auto bytes = mc2::save::encodeTaggedRecord(sample, mc2::save::teamStaticTag);
    CHECK(bytes.size() == 88);
    bytes.back() = 2;
    T decoded{};
    CHECK_FALSE(mc2::save::decodeTaggedRecord(bytes, decoded, mc2::save::teamStaticTag));
}
