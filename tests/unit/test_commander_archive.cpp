#include "save_field_probe.h"
#include "commander_save_data.h"

TEST_CASE("save fields: CommanderData independent census") {
    using G = MoverGroupData;
    auto group = save_probe::patterned<G>(&G::id, &G::numMovers, &G::moverWIDs,
        &G::pointWID, &G::disbandOnNoPoint);
    SAVE_FIELD_SHAPE(group, id, numMovers, moverWIDs, pointWID, disbandOnNoPoint);
    CHECK(save_probe::memberBytes<G>(&G::id, &G::numMovers, &G::moverWIDs,
        &G::pointWID, &G::disbandOnNoPoint) + 3 == sizeof(G));
    CHECK(save_probe::roundtrip(group, [](auto& a, auto& v) { a(v); },
        &G::id, &G::numMovers, &G::moverWIDs, &G::pointWID, &G::disbandOnNoPoint));
    using T = CommanderData;
    T sample{0x01010101, 0x02020202, {}};
    for (auto& item : sample.groups) item = group;
    SAVE_FIELD_SHAPE(sample, id, teamId, groups);
    CHECK(save_probe::memberBytes<T>(&T::id, &T::teamId, &T::groups) == sizeof(T));
    CHECK(save_probe::roundtrip(sample, [](auto& a, auto& v) { a(v); }, &T::id, &T::teamId, &T::groups));
    auto bytes = mc2::save::commanderRecord(sample, mc2::save::commanderDataTag);
    CHECK(bytes.size() == 1248);
    T decoded{};
    REQUIRE(mc2::save::decodeCommanderRecord(bytes, decoded, mc2::save::commanderDataTag));
    CHECK(save_probe::equal(sample.groups, decoded.groups));
    CHECK_FALSE(mc2::save::decodeCommanderRecord(bytes, decoded, mc2::save::commanderStaticTag));
    bytes.pop_back();
    CHECK_FALSE(mc2::save::decodeCommanderRecord(bytes, decoded, mc2::save::commanderDataTag));
}

TEST_CASE("save fields: StaticCommanderData independent census") {
    using T = StaticCommanderData;
    auto sample = save_probe::patterned<T>(&T::numCommanders, &T::homeCommanderId);
    SAVE_FIELD_SHAPE(sample, numCommanders, homeCommanderId);
    CHECK(save_probe::memberBytes<T>(&T::numCommanders, &T::homeCommanderId) == sizeof(T));
    CHECK(save_probe::roundtrip(sample, [](auto& a, auto& v) { a(v); }, &T::numCommanders, &T::homeCommanderId));
    const auto bytes = mc2::save::commanderRecord(sample, mc2::save::commanderStaticTag);
    CHECK(bytes == mc2::save::Bytes{'C','M','S','T',1,0,0,0,1,1,1,1,2,2,2,2});
}
