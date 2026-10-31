#include "save_field_probe.h"
#include "carnage_value_codec.h"
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace save_probe {
template<> inline void pattern(Stuff::Vector4D& v, unsigned char seed) {
    pattern(v.x, seed); pattern(v.y, seed); pattern(v.z, seed); pattern(v.w, seed);
}
template<> inline bool equal(const Stuff::Vector4D& a, const Stuff::Vector4D& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}
template<> inline void zero(Stuff::Vector4D& v) { v.x = v.y = v.z = v.w = 0; }
template<> inline void pattern(CarnageInfo& v, unsigned char seed) {
    pattern(v.fire.timeToBurn, seed); pattern(v.fire.radius, seed);
    pattern(v.fire.reallyVisible, seed);
}
template<> inline bool equal(const CarnageInfo& a, const CarnageInfo& b) {
    // Compare union payload bits, not a float interpretation of reallyVisible.
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}
}

// Independent of production serialize(). Base members are enumerated here;
// structured binding catches new members even when they consume old padding.
#define BASE_MEMBERS &T::objectTypeNum, &T::objectClass, &T::handle, &T::partId, \
    &T::watchID, &T::typeHandle, &T::position, &T::cellPositionRow, \
    &T::cellPositionCol, &T::d_vertexNum, &T::flags, &T::debugFlags, &T::status, \
    &T::tonnage, &T::rotation, &T::appearanceTypeID, &T::collisionFreeFromWID, \
    &T::collisionFreeTime, &T::screenPos, &T::windowsVisible, &T::explRadius, \
    &T::explDamage, &T::maxCV, &T::curCV, &T::threatRating, &T::lastFrameTime, \
    &T::blipFrame, &T::numAttackers, &T::drawFlags
#define CARNAGE_MEMBERS &T::carnageType, &T::ownerWID, &T::info, &T::effectId

TEST_CASE("save fields: CarnageData independent base and union census") {
    using T = CarnageData;
    auto sample = save_probe::patterned<T>(BASE_MEMBERS, CARNAGE_MEMBERS);
    sample.objectClass = FIRE;
    sample.windowsVisible = 0; // explicitly derived; tested separately below
    auto& base = static_cast<GameObjectData&>(sample);
    SAVE_FIELD_SHAPE(base, objectTypeNum, objectClass, handle, partId, watchID,
        typeHandle, position, cellPositionRow, cellPositionCol, d_vertexNum,
        flags, debugFlags, status, tonnage, rotation, appearanceTypeID,
        collisionFreeFromWID, collisionFreeTime, screenPos, windowsVisible,
        explRadius, explDamage, maxCV, curCV, threatRating, lastFrameTime,
        blipFrame, numAttackers, drawFlags);
    SAVE_FIELD_SHAPE(sample.info.fire, timeToBurn, radius, reallyVisible);
    SAVE_FIELD_SHAPE(sample.info.explosion, timer, radius, chunkSize);
    const auto codec = [](auto& a, auto& v) {
        mc2::save::CarnageRecord record{v};
        record.serialize(a);
        if (a.reading()) v = record.value;
    };
    for (auto kind : {CARNAGE_NONE, CARNAGE_FIRE, CARNAGE_EXPLOSION}) {
        sample.carnageType = kind;
        REQUIRE(save_probe::roundtrip(sample, codec, BASE_MEMBERS, CARNAGE_MEMBERS));
        const auto bytes = mc2::save::encodeTaggedRecord(
            mc2::save::CarnageRecord{sample}, mc2::save::carnageTag);
        mc2::save::CarnageRecord decoded;
        REQUIRE(mc2::save::decodeTaggedRecord(bytes, decoded, mc2::save::carnageTag));
        CHECK(save_probe::equal(sample.info, decoded.value.info));
        CHECK_FALSE(mc2::save::decodeTaggedRecord(std::span(bytes).first(bytes.size()-1),
            decoded, mc2::save::carnageTag));
        CHECK_FALSE(mc2::save::decodeTaggedRecord(bytes, decoded, mc2::save::tagTeam));
    }
    // Negative controls: the census must observe loss in both the base and leaf.
    CHECK_FALSE(save_probe::roundtrip(sample, [](auto& a, auto& v) {
        mc2::save::CarnageRecord record{v}; record.serialize(a);
        if (a.reading()) { v = record.value; v.watchID = 0; }
    }, BASE_MEMBERS, CARNAGE_MEMBERS));
    CHECK_FALSE(save_probe::roundtrip(sample, [](auto& a, auto& v) {
        mc2::save::CarnageRecord record{v}; record.serialize(a);
        if (a.reading()) { v = record.value; v.info.explosion.chunkSize = 0; }
    }, BASE_MEMBERS, CARNAGE_MEMBERS));
}

TEST_CASE("save fields: CarnageData derived visibility and invalid discriminant") {
    mc2::save::CarnageRecord sample;
    sample.value.screenPos = Stuff::Vector4D(1, 2, 3, 4);
    const auto bytes = mc2::save::encodeTaggedRecord(sample, mc2::save::carnageTag);
    REQUIRE_FALSE(bytes.empty());
    sample.value.windowsVisible = 123;
    CHECK(mc2::save::encodeTaggedRecord(sample, mc2::save::carnageTag) == bytes);
    mc2::save::CarnageRecord decoded;
    decoded.value.windowsVisible = 456;
    REQUIRE(mc2::save::decodeTaggedRecord(bytes, decoded, mc2::save::carnageTag));
    CHECK(decoded.value.windowsVisible == 0);
    sample.value.carnageType = NUM_CARNAGE_TYPES;
    CHECK(mc2::save::encodeTaggedRecord(sample, mc2::save::carnageTag).empty());
}

TEST_CASE("save fields: CarnageData inherited declaration census") {
    // C++ cannot decompose a class with fields in both base and derived class.
    // Pin the complete leaf/union declarations instead; additions inside old
    // padding must fail this test as well as omissions from the member list.
    std::ifstream in(std::filesystem::path(MC2_REPO_ROOT) / "code/carnage.h");
    REQUIRE(in.good());
    std::stringstream stream; stream << in.rdbuf();
    auto source = stream.str();
    std::erase_if(source, [](unsigned char c) { return std::isspace(c); });
    CHECK(source.find("struct_CarnageData:publicGameObjectData{"
        "CarnageEnumTypecarnageType;GameObjectWatchIDownerWID;CarnageInfoinfo;"
        "int32_teffectId;};") != std::string::npos);
    CHECK(source.find("union_CarnageInfo{struct{floattimeToBurn;floatradius;"
        "int32_treallyVisible;}fire;struct{floattimer;floatradius;"
        "floatchunkSize;}explosion;};") != std::string::npos);
}

#undef CARNAGE_MEMBERS
#undef BASE_MEMBERS
