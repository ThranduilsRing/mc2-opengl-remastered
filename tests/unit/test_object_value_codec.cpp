#include "save_field_probe.h"
#include "carnage_value_codec.h"
#include "artillery_value_codec.h"
#include "weaponbolt_value_codec.h"
#include "gate_value_codec.h"
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <regex>

namespace {
std::string compactSource(const char* file) {
    std::ifstream in(std::filesystem::path(MC2_REPO_ROOT) / file);
    REQUIRE(in.good());
    std::stringstream stream; stream << in.rdbuf();
    auto source = std::regex_replace(stream.str(), std::regex("//[^\n]*"), "");
    std::erase_if(source, [](unsigned char c) { return std::isspace(c); });
    return source;
}
}

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
template<> inline void pattern(ArtilleryInfo& v, unsigned char seed) {
    using T = decltype(v.strike);
    v.strike = patterned<T>(&T::timeToImpact, &T::timeToLaunch, &T::sensorRange,
        &T::timeToBlind, &T::sensorSystemIndex, &T::contactUpdate,
        &T::timeString, &T::explosionOffFlags);
}
template<> inline bool equal(const ArtilleryInfo& a, const ArtilleryInfo& b) {
    const auto& x = a.strike; const auto& y = b.strike;
    return std::tie(x.timeToImpact, x.timeToLaunch, x.sensorRange, x.timeToBlind,
        x.sensorSystemIndex, x.contactUpdate, x.explosionOffFlags) ==
        std::tie(y.timeToImpact, y.timeToLaunch, y.sensorRange, y.timeToBlind,
        y.sensorSystemIndex, y.contactUpdate, y.explosionOffFlags) &&
        equal(x.timeString, y.timeString);
}
template<> inline void pattern(WeaponShotInfo& v, unsigned char seed) {
    pattern(v.attackerWID, seed); pattern(v.masterId, seed); pattern(v.damage, seed);
    pattern(v.hitLocation, seed); pattern(v.entryAngle, seed);
}
template<> inline void zero(WeaponShotInfo& v) {
    v.attackerWID = 0; v.masterId = 0; v.damage = 0; v.hitLocation = 0; v.entryAngle = 0;
}
template<> inline bool equal(const WeaponShotInfo& a, const WeaponShotInfo& b) {
    return std::tie(a.attackerWID, a.masterId, a.damage, a.hitLocation, a.entryAngle) ==
        std::tie(b.attackerWID, b.masterId, b.damage, b.hitLocation, b.entryAngle);
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

#define TERRAIN_MEMBERS &T::damage, &T::vertexNumber, &T::blockNumber, \
    &T::pitchAngle, &T::fallRate, &T::powerSupply, &T::cellFootprint, \
    &T::vectorFootprint, &T::numSubAreas0, &T::numSubAreas1, &T::subAreas0, \
    &T::subAreas1, &T::listID, &T::numCellsCovered, &T::cellsCovered
#define GATE_MEMBERS &T::teamId, &T::lockedOpen, &T::lockedClose, &T::reasonToOpen, \
    &T::opened, &T::opening, &T::closed, &T::closing, &T::justDestroyed, \
    &T::lastMarkedOpen, &T::parentId, &T::parent, &T::buildingDescriptionID

TEST_CASE("save fields: GateData inherited census and legacy values") {
    using T = GateData;
    auto sample = save_probe::patterned<T>(BASE_MEMBERS, TERRAIN_MEMBERS, GATE_MEMBERS);
    sample.objectClass = GATE;
    sample.windowsVisible = 0;
    sample.numSubAreas0 = sample.numSubAreas1 = MAX_SPECIAL_SUB_AREAS;
    sample.numCellsCovered = 40;
    sample.cellsCovered[80] = 0;
    const auto codec = [](auto& a, auto& v) {
        mc2::save::GateRecord record{v}; record.serialize(a);
        if (a.reading()) v = record.value;
    };
    REQUIRE(save_probe::roundtrip(sample, codec, BASE_MEMBERS, TERRAIN_MEMBERS, GATE_MEMBERS));
    CHECK_FALSE(save_probe::roundtrip(sample, [](auto& a, auto& v) {
        mc2::save::GateRecord record{v}; record.serialize(a);
        if (a.reading()) { v = record.value; v.parent = 0; }
    }, BASE_MEMBERS, TERRAIN_MEMBERS, GATE_MEMBERS));
    CHECK_FALSE(save_probe::roundtrip(sample, [](auto& a, auto& v) {
        mc2::save::GateRecord record{v}; record.serialize(a);
        if (a.reading()) { v = record.value; v.cellsCovered[79] = 0; }
    }, BASE_MEMBERS, TERRAIN_MEMBERS, GATE_MEMBERS));
    const auto bytes = mc2::save::encodeTaggedRecord(mc2::save::GateRecord{sample}, mc2::save::gateTag);
    mc2::save::GateRecord decoded;
    REQUIRE(mc2::save::decodeTaggedRecord(bytes, decoded, mc2::save::gateTag));
    CHECK_FALSE(mc2::save::decodeTaggedRecord(std::span(bytes).first(bytes.size()-1), decoded, mc2::save::gateTag));
    CHECK_FALSE(mc2::save::decodeTaggedRecord(bytes, decoded, mc2::save::artilleryTag));
    const auto source = compactSource("code/gate.h");
    CHECK(source.find("struct_GateData:publicTerrainObjectData{charteamId;"
        "boollockedOpen;boollockedClose;boolreasonToOpen;boolopened;boolopening;"
        "boolclosed;boolclosing;booljustDestroyed;boollastMarkedOpen;MoverPtrclosestObject;"
        "DWORDparentId;GameObjectWatchIDparent;longbuildingDescriptionID;longupdatedTurn;};") != std::string::npos);
    const auto terrain = compactSource("code/terrobj.h");
    CHECK(terrain.find("struct_TerrainObjectData:publicGameObjectData{floatdamage;"
        "int32_tvertexNumber;int32_tblockNumber;floatpitchAngle;floatfallRate;"
        "GameObjectWatchIDpowerSupply;shortcellFootprint[4];glm::vec3vectorFootprint[4];"
        "shortnumSubAreas0;shortnumSubAreas1;shortsubAreas0[MAX_SPECIAL_SUB_AREAS];"
        "shortsubAreas1[MAX_SPECIAL_SUB_AREAS];unsignedcharlistID;"
        "unsignedcharnumCellsCovered;shortcellsCovered[81];};") != std::string::npos);
}

TEST_CASE("save fields: GateData derived values and terrain bounds") {
    mc2::save::GateRecord sample;
    const auto bytes = mc2::save::encodeTaggedRecord(sample, mc2::save::gateTag);
    REQUIRE_FALSE(bytes.empty());
    int blocker = 0;
    // An opaque non-null pointer is never dereferenced or serialized.
    sample.value.closestObject = reinterpret_cast<MoverPtr>(&blocker);
    sample.value.updatedTurn = 99;
    sample.value.subAreas0[0] = sample.value.subAreas1[0] = 15;
    sample.value.cellsCovered[80] = 12;
    CHECK(mc2::save::encodeTaggedRecord(sample, mc2::save::gateTag) == bytes);
    mc2::save::GateRecord decoded = sample;
    REQUIRE(mc2::save::decodeTaggedRecord(bytes, decoded, mc2::save::gateTag));
    CHECK(decoded.value.closestObject == nullptr);
    CHECK(decoded.value.updatedTurn == 0);
    CHECK(decoded.value.subAreas0[0] == 0);
    CHECK(decoded.value.subAreas1[0] == 0);
    CHECK(decoded.value.cellsCovered[80] == 0);
    sample.value.numCellsCovered = 41;
    CHECK(mc2::save::encodeTaggedRecord(sample, mc2::save::gateTag).empty());
    sample.value.numCellsCovered = 0;
    sample.value.numSubAreas0 = -1;
    CHECK(mc2::save::encodeTaggedRecord(sample, mc2::save::gateTag).empty());
    sample.value.numSubAreas0 = 0;
    sample.value.numSubAreas1 = MAX_SPECIAL_SUB_AREAS + 1;
    CHECK(mc2::save::encodeTaggedRecord(sample, mc2::save::gateTag).empty());
}

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
    const auto source = compactSource("code/carnage.h");
    CHECK(source.find("struct_CarnageData:publicGameObjectData{"
        "CarnageEnumTypecarnageType;GameObjectWatchIDownerWID;CarnageInfoinfo;"
        "int32_teffectId;};") != std::string::npos);
    CHECK(source.find("union_CarnageInfo{struct{floattimeToBurn;floatradius;"
        "int32_treallyVisible;}fire;struct{floattimer;floatradius;"
        "floatchunkSize;}explosion;};") != std::string::npos);
}

#define ARTILLERY_MEMBERS &T::artilleryType, &T::teamId, &T::commanderId, \
    &T::info, &T::effectId, &T::bombRunStarted, &T::inView, &T::iFacePosition

TEST_CASE("save fields: ArtilleryData independent inherited census") {
    using T = ArtilleryData;
    auto sample = save_probe::patterned<T>(BASE_MEMBERS, ARTILLERY_MEMBERS);
    sample.objectClass = ARTILLERY;
    sample.windowsVisible = 0;
    SAVE_FIELD_SHAPE(sample.info.strike, timeToImpact, timeToLaunch,
        sensorRange, timeToBlind, sensorSystemIndex, contactUpdate,
        timeString, explosionOffFlags);
    const auto codec = [](auto& a, auto& v) {
        mc2::save::ArtilleryRecord record{v}; record.serialize(a);
        if (a.reading()) v = record.value;
    };
    for (char kind = 0; kind < NUM_ARTILLERY_TYPES; ++kind) {
        sample.artilleryType = kind;
        REQUIRE(save_probe::roundtrip(sample, codec, BASE_MEMBERS, ARTILLERY_MEMBERS));
        const auto bytes = mc2::save::encodeTaggedRecord(
            mc2::save::ArtilleryRecord{sample}, mc2::save::artilleryTag);
        mc2::save::ArtilleryRecord decoded;
        REQUIRE(mc2::save::decodeTaggedRecord(bytes, decoded, mc2::save::artilleryTag));
        CHECK(save_probe::equal(decoded.value.info, sample.info));
        CHECK_FALSE(mc2::save::decodeTaggedRecord(std::span(bytes).first(bytes.size()-1),
            decoded, mc2::save::artilleryTag));
    }
    CHECK_FALSE(save_probe::roundtrip(sample, [](auto& a, auto& v) {
        mc2::save::ArtilleryRecord record{v}; record.serialize(a);
        if (a.reading()) { v = record.value; v.info.strike.explosionOffFlags = 0; }
    }, BASE_MEMBERS, ARTILLERY_MEMBERS));
    sample.artilleryType = NUM_ARTILLERY_TYPES;
    CHECK(mc2::save::encodeTaggedRecord(mc2::save::ArtilleryRecord{sample},
        mc2::save::artilleryTag).empty());
    const auto source = compactSource("code/artlry.h");
    CHECK(source.find("struct_ArtilleryData:publicGameObjectData{charartilleryType;"
        "charteamId;charcommanderId;ArtilleryInfoinfo;int32_teffectId;"
        "boolbombRunStarted;boolinView;glm::vec3iFacePosition;};") != std::string::npos);
    CHECK(source.find("union_ArtilleryInfo{struct{floattimeToImpact;floattimeToLaunch;"
        "floatsensorRange;floattimeToBlind;shortsensorSystemIndex;floatcontactUpdate;"
        "chartimeString[5];uint32_texplosionOffFlags;}strike;};") != std::string::npos);
}

#define BOLT_MEMBERS &T::ownerWID, &T::hotSpotNumber, &T::targetWID, &T::targetHotSpot, \
    &T::targetPosition, &T::distanceToTarget, &T::halfDistanceToTarget, &T::weaponShot, \
    &T::laserPosition, &T::laserVertices, &T::laserSide, &T::effectId, &T::hitTarget, \
    &T::timeLeft, &T::hsPos, &T::hitLeft, &T::mcTextureHandle, &T::gosTextureHandle, \
    &T::startUV, &T::goalHeight, &T::retireDeadlineTick, &T::retireDeadlineState

TEST_CASE("save fields: WeaponBoltData independent inherited census") {
    using T = WeaponBoltData;
    auto sample = save_probe::patterned<T>(BASE_MEMBERS, BOLT_MEMBERS);
    sample.objectClass = WEAPONBOLT;
    sample.windowsVisible = 0;
    sample.mcTextureHandle = sample.gosTextureHandle = 0; // explicitly derived
    sample.retireDeadlineState = mc2::combat::SavedDeadlineState::armed;
    SAVE_FIELD_SHAPE(sample.weaponShot, attackerWID, masterId, damage, hitLocation, entryAngle);
    const auto codec = [](auto& a, auto& v) {
        mc2::save::WeaponBoltRecord record{v}; record.serialize(a);
    };
    REQUIRE(save_probe::roundtrip(sample, codec, BASE_MEMBERS, BOLT_MEMBERS));
    auto bytes = mc2::save::encodeTaggedRecord(
        mc2::save::WeaponBoltRecord{sample}, mc2::save::weaponBoltTag);
    REQUIRE_FALSE(bytes.empty());
    auto decoded = mc2::save::decodeWeaponBoltRecord(bytes);
    REQUIRE(decoded.has_value());
    CHECK(save_probe::equal(sample.weaponShot, decoded->weaponShot));
    CHECK_FALSE(mc2::save::decodeWeaponBoltRecord(std::span(bytes).first(bytes.size()-1)));
    sample.mcTextureHandle = 123; sample.gosTextureHandle = 456;
    CHECK(mc2::save::encodeTaggedRecord(
        mc2::save::WeaponBoltRecord{sample}, mc2::save::weaponBoltTag) == bytes);
    sample.mcTextureHandle = sample.gosTextureHandle = 0;
    CHECK_FALSE(save_probe::roundtrip(sample, [](auto& a, auto& v) {
        mc2::save::WeaponBoltRecord record{v}; record.serialize(a);
        if (a.reading()) v.laserVertices[3].z = 0;
    }, BASE_MEMBERS, BOLT_MEMBERS));
    CHECK_FALSE(save_probe::roundtrip(sample, [](auto& a, auto& v) {
        mc2::save::WeaponBoltRecord record{v}; record.serialize(a);
        if (a.reading()) v.retireDeadlineTick = 0;
    }, BASE_MEMBERS, BOLT_MEMBERS));
    bytes.back() = 255; // unknown SavedDeadlineState
    CHECK_FALSE(mc2::save::decodeWeaponBoltRecord(bytes));
    sample.retireDeadlineTick = mc2::combat::invalidTick;
    CHECK(mc2::save::encodeTaggedRecord(
        mc2::save::WeaponBoltRecord{sample}, mc2::save::weaponBoltTag).empty());
    const auto source = compactSource("code/weaponbolt.h");
    CHECK(source.find("struct_WeaponBoltData:publicGameObjectData{GameObjectWatchIDownerWID;"
        "int32_thotSpotNumber;GameObjectWatchIDtargetWID;int32_ttargetHotSpot;"
        "glm::vec3targetPosition;floatdistanceToTarget;floathalfDistanceToTarget;"
        "WeaponShotInfoweaponShot;glm::vec3laserPosition;glm::vec3laserVertices[4];"
        "glm::vec3laserSide[4];int32_teffectId;boolhitTarget;floattimeLeft;glm::vec3hsPos;"
        "floathitLeft;DWORDmcTextureHandle;DWORDgosTextureHandle;floatstartUV;floatgoalHeight;"
        "std::uint64_tretireDeadlineTick;mc2::combat::SavedDeadlineStateretireDeadlineState;};")
        != std::string::npos);
}

#undef BOLT_MEMBERS
#undef ARTILLERY_MEMBERS
#undef CARNAGE_MEMBERS
#undef BASE_MEMBERS
