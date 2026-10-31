#include "save_field_probe.h"
#include "warrior_value_codec.h"

TEST_CASE("save fields: StaticMechWarriorData independent census") {
    using T = StaticMechWarriorData;
    auto sample = save_probe::patterned<T>(&T::numWarriors, &T::numWarriorsInCombat,
        &T::brainsEnabled, &T::minSkill, &T::maxSkill, &T::increaseCap,
        &T::maxVisualRadius, &T::curEventID, &T::curEventTrigger);
    SAVE_FIELD_SHAPE(sample, numWarriors, numWarriorsInCombat, brainsEnabled,
        minSkill, maxSkill, increaseCap, maxVisualRadius, curEventID, curEventTrigger);
    CHECK(save_probe::memberBytes<T>(&T::numWarriors, &T::numWarriorsInCombat,
        &T::brainsEnabled, &T::minSkill, &T::maxSkill, &T::increaseCap,
        &T::maxVisualRadius, &T::curEventID, &T::curEventTrigger) == sizeof(T));
    CHECK(save_probe::roundtrip(sample, [](auto& a, auto& v) {
        mc2::save::WarriorStaticRecord record{v};
        record.serialize(a);
        if (a.reading()) v = record.value;
    }, &T::numWarriors, &T::numWarriorsInCombat, &T::brainsEnabled, &T::minSkill,
        &T::maxSkill, &T::increaseCap, &T::maxVisualRadius, &T::curEventID, &T::curEventTrigger));
    const auto bytes = mc2::save::encodeTaggedRecord(
        mc2::save::WarriorStaticRecord{sample}, mc2::save::warriorStaticTag);
    CHECK(bytes.size() == 48);
    mc2::save::WarriorStaticRecord decoded;
    REQUIRE(mc2::save::decodeTaggedRecord(bytes, decoded, mc2::save::warriorStaticTag));
    CHECK(decoded.value.curEventTrigger == sample.curEventTrigger);
    CHECK_FALSE(mc2::save::decodeTaggedRecord(bytes, decoded, mc2::save::tagTeam));
    CHECK_FALSE(mc2::save::decodeTaggedRecord(std::span(bytes).first(bytes.size()-1),
        decoded, mc2::save::warriorStaticTag));
}
