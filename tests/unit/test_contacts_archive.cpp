#include "doctest.h"
#include "contacts_archive.h"

#include <limits>

namespace {
using namespace mc2::save;

ContactsState sample() {
    ContactsState s;
    SensorRecord a;
    a.id = 3; a.masterTeamId = 1; a.masterIndex = 0; a.owner = mc2::sim::ObjectRef{9, 2};
    a.range = 400.0f; a.skill = 60; a.notShutdown = true; a.hasLOSCapability = true;
    a.ecmEffect = 0.25f; a.nextScanUpdate = 12.5f; a.lastScanUpdate = 10.0f;
    a.contacts = {7, 12}; a.numExclusives = 1; a.totalContacts = 5;
    SensorRecord b;
    b.id = 10; b.broken = true;
    s.sensors = {a, b};
    s.freeSensorIds = {11, 4, 5};
    TeamSensorRecord t;
    t.teamId = 1; t.nextContactId = 2; t.numContactUpdatesPerPass = 3; t.curContactUpdate = 1;
    t.contacts = {7, 12}; t.numEnemyContacts = 2; t.sensorIds = {3, 10};
    EcmRecord e; e.owner = mc2::sim::ObjectRef{4, 1}; e.masterId = 3; e.range = 90.0f; e.effect = 0.5f;
    t.ecms = {e};
    s.teamSensors = {t};
    EcmRecord m; m.owner = mc2::sim::ObjectRef{4, 1}; m.range = 90.0f;
    s.ecms = {m};
    s.teamToUpdate = 2; s.scanFrequency = 1.5f; s.updateFrequency = 0.5f;
    s.homeTeamInContact = true; s.enemyInLOS = true;
    ContactInfoRecord c;
    c.moverId = 7; c.contactStatus[1] = 5; c.contactCount[1] = 2; c.teams[1] = 0; c.teamSpotter[1] = 3;
    c.sensors = {{3, 0}, {10, 1}};
    ContactInfoRecord d;
    d.moverId = 12;
    s.contactInfos = {c, d};
    return s;
}

bool decodes(const ContactsState& v) {
    ContactsState out;
    return decodeTaggedRecord(encodeTaggedRecord(v, tagContacts), out, tagContacts);
}
} // namespace

TEST_CASE("contacts archive: roundtrip is exact and re-encodes to the same bytes") {
    const auto state = sample();
    const auto bytes = encodeTaggedRecord(state, tagContacts);
    REQUIRE_FALSE(bytes.empty());
    ContactsState out;
    REQUIRE(decodeTaggedRecord(bytes, out, tagContacts));
    CHECK(out == state);
    CHECK(encodeTaggedRecord(out, tagContacts) == bytes);
    ContactsState empty, out2;
    out2.teamToUpdate = 3;
    REQUIRE(decodeTaggedRecord(encodeTaggedRecord(empty, tagContacts), out2, tagContacts));
    CHECK(out2 == empty);
}

TEST_CASE("contacts archive: wrong tag, version mismatch and truncation are rejected") {
    const auto bytes = encodeTaggedRecord(sample(), tagContacts);
    ContactsState out;
    out.teamToUpdate = 5;
    CHECK_FALSE(decodeTaggedRecord(bytes, out, fourcc('E', 'N', 'V', 'W')));
    auto v2 = bytes;
    v2[4] = 2; // version word follows the 4-byte tag
    CHECK_FALSE(decodeTaggedRecord(v2, out, tagContacts));
    auto cut = bytes;
    cut.pop_back();
    CHECK_FALSE(decodeTaggedRecord(cut, out, tagContacts));
    CHECK(out.teamToUpdate == 5);
}

TEST_CASE("contacts archive: out-of-range ids and counts are rejected") {
    auto s = sample();
    s.sensors[0].id = contactMaxSensors;
    CHECK_FALSE(decodes(s));
    s = sample();
    s.sensors[0].masterTeamId = static_cast<std::int32_t>(contactMaxTeams);
    CHECK_FALSE(decodes(s));
    s = sample();
    s.sensors[0].numExclusives = 3; // more than the two contacts
    CHECK_FALSE(decodes(s));
    s = sample();
    s.sensors[0].contacts[0] = contactMaxMovers;
    CHECK_FALSE(decodes(s));
    s = sample();
    s.teamToUpdate = static_cast<std::int32_t>(contactMaxTeams);
    CHECK_FALSE(decodes(s));
    s = sample();
    s.contactInfos[0].contactStatus[0] = static_cast<std::uint8_t>(contactStatusCount);
    CHECK_FALSE(decodes(s));
    s = sample();
    s.contactInfos[0].moverId = contactMaxMovers;
    CHECK_FALSE(decodes(s));
    s = sample();
    s.teamSensors[0].numEnemyContacts = 3;
    CHECK_FALSE(decodes(s));
    s = sample();
    s.scanFrequency = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(decodes(s));
    s = sample();
    s.updateFrequency = -1.0f;
    CHECK_FALSE(decodes(s));
    s = sample();
    s.ecms.assign(contactMaxEcms + 1, EcmRecord{});
    CHECK_FALSE(decodes(s));
}

TEST_CASE("contacts archive: duplicate, unordered and dangling references are rejected") {
    auto s = sample();
    s.sensors[1].id = 3; // duplicate id
    CHECK_FALSE(decodes(s));
    s = sample();
    std::swap(s.sensors[0], s.sensors[1]); // not ascending
    CHECK_FALSE(decodes(s));
    s = sample();
    s.freeSensorIds = {11, 11}; // duplicate free id
    CHECK_FALSE(decodes(s));
    s = sample();
    s.freeSensorIds = {3}; // live sensor also listed free
    CHECK_FALSE(decodes(s));
    s = sample();
    s.teamSensors[0].sensorIds = {3, 99}; // sensor 99 is not live
    CHECK_FALSE(decodes(s));
    s = sample();
    std::swap(s.contactInfos[0], s.contactInfos[1]);
    CHECK_FALSE(decodes(s));
    s = sample();
    s.contactInfos[0].sensors = {{10, 1}, {3, 0}}; // slots not ascending
    CHECK_FALSE(decodes(s));
}
