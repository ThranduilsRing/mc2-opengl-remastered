#include "doctest.h"
#include "mission_init_frame.h"
#include "save_archive.h"
#include "objtype.h"

#include <bit>
#include <cstring>
#include <limits>

// The FIT/heap spine is real; these are the surrounding session services used by the init phase.
UserHeapPtr missionHeap = nullptr;
MultiPlayer* MPlayer = nullptr;
long GameDifficulty = 0;
float loadProgress = 0;
CPrefs::CPrefs() {}
CPrefs prefs;
long RandomNumber(long) { return 0; }
MechWarrior* MechWarrior::warriorList[MAX_WARRIORS] = {};
void MechWarrior::init(bool) {}
void MechWarrior::destroy() {}
void TacticalOrder::init() {}
void TacticalOrder::destroy() {}
void TargetPriorityList::init() {}
void TargetPriorityList::destroy() {}
void GameObjectManager::init() {}
void GameObjectManager::destroy() {}
void ObjectTypeManager::destroy() {}
void ObjectType::operator delete(void*) {}
long ObjectType::init(FilePtr, unsigned long) { return NO_ERR; }
void ObjectType::destroy() {}
GameObjectPtr ObjectType::createInstance() { return nullptr; }
bool ObjectType::handleCollision(GameObjectPtr, GameObjectPtr) { return false; }
bool ObjectType::handleDestruction(GameObjectPtr, GameObjectPtr) { return false; }

// Object-table fixtures provide session services; archive validation remains production code.
void GameObject::init(bool) {}
void GameObject::destroy() {}
void GameObject::operator delete(void*) {}
void GameObject::getBlockAndVertexNumber(int&, int&) {}
ObjectTypePtr GameObject::getObjectType() { return nullptr; }
void GameObject::init(bool, ObjectTypePtr) {}
Stuff::Vector3D GameObject::relativePosition(float, float, unsigned long) { return {}; }
void GameObject::setPosition(const Stuff::Vector3D&, bool) {}
float GameObject::distanceFrom(Stuff::Vector3D) { return 0; }
long GameObject::cellDistanceFrom(Stuff::Vector3D) { return 0; }
long GameObject::cellDistanceFrom(GameObjectPtr) { return 0; }
long GameObject::getLineOfSightNodes(long, long, long*) { return 0; }
bool GameObject::lineOfSight(Stuff::Vector3D) { return false; }
bool GameObject::lineOfSight(GameObjectPtr, float) { return false; }
float GameObject::relFacingTo(Stuff::Vector3D, long) { return 0; }
float GameObject::getExtentRadius() { return 0; }
void GameObject::setExtentRadius(float) {}
bool GameObject::canBeSeen_instr() const { return false; }
long GameObject::getCaptureBlocker(GameObjectPtr, GameObjectPtr*) { return 0; }
long GameObject::kill() { return 0; }
bool GameObject::onScreen() { return false; }
MechClass GameObject::getMechClass() { return {}; }
bool GameObject::isFriendly(GameObjectPtr) { return false; }
bool GameObject::isEnemy(GameObjectPtr) { return false; }
bool GameObject::isNeutral(GameObjectPtr) { return false; }
void GameObject::Save(PacketFilePtr, long) {}
namespace mc2verify {
bool Fail(const char*, int, const char*, const char*, ...) {
    FAIL("production Part initialization rejected the fixture FIT");
    return false;
}
}

struct MissionPartArchiveTest {
    static void initialize(Mission& mission, FitIniFile& file, unsigned long count) {
        mission.missionFile = &file;
        MissionInitFrame frame{};
        mission.initPartsHeader(frame);
        REQUIRE(mission.numParts == count);
        mission.initPartsLoad(MISSION_LOAD_SP_QUICKSTART, -1, nullptr, nullptr, 0, frame);
    }
    static void release(Mission& mission) {
        if (mission.parts) missionHeap->Free(mission.parts);
        mission.parts = nullptr;
        mission.numParts = 0;
    }
    static Part& reserved(Mission& mission) { return mission.parts[0]; }
};
void Mission::destroy(bool) { MissionPartArchiveTest::release(*this); }
// The test never creates the mission's appearance types; the owner's destructor only needs the symbol.
void AppearanceTypeList::destroy(void) {}

namespace {
struct PilotFixture : MechWarrior {
    void setLive(bool live) { used = live; }
};
struct WatchFixture : GameObject {
    MechWarrior* pilot = nullptr;
    MechWarriorPtr getPilot() override { return pilot; }
    Stuff::Vector3D getRotationVector() override { return {}; }
};
struct LiveParts {
    UserHeap heap;
    FitIniFile fit;
    Mission mission;
    std::array<PilotFixture, 2> pilots;
    std::array<WatchFixture, 2> movers;
    GameObjectManager objects;
    ObjectTypeManager types;
    std::array<ObjectType, 2> objectTypes;
    std::array<ObjectTypePtr, 64> typeTable{};
    std::array<GameObjectPtr, 3> watches{};
    LiveParts() {
        missionHeap = &heap;
        ObjectManager = &objects;
        objects.objTypeManager = &types;
        types.numObjectTypes = static_cast<long>(typeTable.size());
        types.table = typeTable.data();
        objects.nextWatchID = static_cast<unsigned long>(watches.size());
        objects.watchList = watches.data();
        for (int i = 0; i < 2; ++i) {
            pilots[i].setLive(true);
            pilots[i].setIndex(i + 1);
            MechWarrior::warriorList[i + 1] = &pilots[i];
            movers[i].pilot = &pilots[i];
            movers[i].objectClass = MOVER;
            movers[i].typeHandle = 41 + i;
            watches[i + 1] = &movers[i];
            objectTypes[i].setObjTypeNum(41 + i);
            typeTable[41 + i] = &objectTypes[i];
        }
        std::string content = "FITini\n[Parts]\nul NumParts = 2\n";
        for (int i = 1; i <= 2; ++i) {
            content += "[Part" + std::to_string(i) + "]\n"
                "ul squadNum = 1\nl[1] IndicesOfAlternatives = -1\n"
                "ul ObjectNumber = " + std::to_string(40 + i) + "\nul ControlType = 2\nul ControlDataType = 1\n"
                "st ObjectProfile = \"profile\"\nst CSVFile = \"modmech\"\n"
                "ul VariantNumber = 3\nul Pilot = " + std::to_string(i) + "\nf PositionX = -0.0\nf PositionY = " + std::to_string(i * 12.5f) + "\n"
                "f Rotation = 90\nc TeamId = 2\nl CommanderId = 0\nul BaseColor = 287454020\n"
                "ul HighlightColor1 = 7\nul HighlightColor2 = 8\nl Active = 1\nl Exists = 1\n"
                "c MyIcon = 1\nb Captureable = TRUE\n";
        }
        content += "FITend\n";
        REQUIRE(fit.open(content.data(), static_cast<int>(content.size())) == NO_ERR);
        MissionPartArchiveTest::initialize(mission, fit, 2);
    }
    ~LiveParts() {
        MissionPartArchiveTest::release(mission);
        missionHeap = nullptr;
        ObjectManager = nullptr;
        MechWarrior::warriorList[1] = nullptr;
        MechWarrior::warriorList[2] = nullptr;
    }
    mc2::save::Section section() {
        mc2::save::development::PartSection snapshot;
        for (int i = 1; i <= 2; ++i) snapshot.parts.push_back(mission.getPart(i)->saveRecord());
        mc2::save::Archive writer;
        writer(snapshot);
        REQUIRE(writer.finish());
        return {10, true, writer.bytes()};
    }
};

mc2::save::Part expectedPart(int index) {
    mc2::save::Part expected;
    expected.objectWID = index;
    expected.objNumber = 40 + index;
    expected.baseColor = 287454020u;
    expected.highlightColor1 = 7;
    expected.highlightColor2 = 8;
    expected.active = 1;
    expected.exists = 1;
    expected.destroyed = false;
    expected.position = {-0.0f, index * 12.5f, -1.0f};
    expected.velocity = 0;
    expected.rotation = 90;
    expected.gestureId = 2;
    expected.alignment = 0;
    expected.teamId = 0;
    expected.commanderID = 0;
    expected.squadId = 1;
    expected.myIcon = 1;
    expected.controlType = 2;
    expected.controlDataType = 1;
    expected.profileName = {2, "profile"};
    expected.pilot = index;
    expected.captureable = true;
    expected.variantNum = 3;
    return expected;
}

void poison(Part& part) {
    part.objectWID = 99;
    part.objNumber = 99;
    part.baseColor = 1;
    part.highlightColor1 = 2;
    part.highlightColor2 = 3;
    part.active = 0;
    part.exists = 0;
    part.destroyed = true;
    part.position.x = 4;
    part.position.y = 5;
    part.position.z = 6;
    part.velocity = 7;
    part.rotation = 8;
    part.gestureId = 9;
    part.alignment = -1;
    part.teamId = -1;
    part.commanderID = -1;
    part.squadId = -1;
    part.myIcon = 0;
    part.controlType = 0;
    part.controlDataType = 0;
    std::memset(part.profileName, 'X', sizeof(part.profileName));
    std::strcpy(part.csvFile, "destination_hint");
    part.pilot = 99;
    part.captureable = false;
    part.variantNum = 99;
}

void checkExpected(const Part& part, int index) {
    const auto expected = expectedPart(index);
    CHECK(part.objectWID == expected.objectWID);
    CHECK(part.objNumber == expected.objNumber);
    CHECK(part.baseColor == expected.baseColor);
    CHECK(part.highlightColor1 == expected.highlightColor1);
    CHECK(part.highlightColor2 == expected.highlightColor2);
    CHECK(part.active == expected.active);
    CHECK(part.exists == expected.exists);
    CHECK(part.destroyed == expected.destroyed);
    CHECK(std::bit_cast<std::uint32_t>(part.position.x) == 0x80000000u);
    CHECK(part.position.y == expected.position.y);
    CHECK(part.position.z == expected.position.z);
    CHECK(part.velocity == expected.velocity);
    CHECK(part.rotation == expected.rotation);
    CHECK(part.gestureId == expected.gestureId);
    CHECK(part.alignment == expected.alignment);
    CHECK(part.teamId == expected.teamId);
    CHECK(part.commanderID == expected.commanderID);
    CHECK(part.squadId == expected.squadId);
    CHECK(part.myIcon == expected.myIcon);
    CHECK(part.controlType == expected.controlType);
    CHECK(part.controlDataType == expected.controlDataType);
    CHECK(std::string(part.profileName) == expected.profileName.bytes);
    CHECK(part.profileName[8] == 0);
    CHECK(part.pilot == expected.pilot);
    CHECK(part.captureable == expected.captureable);
    CHECK(part.variantNum == expected.variantNum);
    CHECK(std::string(part.csvFile) == "destination_hint");
}
}

TEST_SUITE("LivePartArchive") {
TEST_CASE("real fresh mission Part init retains identity and omitted CSV hints") {
    LiveParts live;
    auto* part = live.mission.getPart(1);
    CHECK(part->teamId == 0);
    CHECK(part->gestureId == 2);
    CHECK(part->position.z == -1.0f);
    CHECK(std::string(part->csvFile) == "modmech");
    auto original = part->saveRecord();
    original.objectWID = 1;
    CHECK(original == expectedPart(1));
    mc2::save::Archive writer;
    writer(original);
    REQUIRE(writer.finish());
    mc2::save::Part decoded;
    REQUIRE(mc2::save::decodeRecord(writer.bytes(), decoded));
    poison(*part);
    REQUIRE(part->restoreRecord(decoded));
    checkExpected(*part, 1);
    mc2::save::Archive resave;
    auto restored = part->saveRecord();
    resave(restored);
    CHECK(resave.bytes() == writer.bytes());
    CHECK(std::bit_cast<std::uint32_t>(part->position.x) == std::bit_cast<std::uint32_t>(original.position.x));
    CHECK(std::string(part->csvFile) == "destination_hint");
}

TEST_CASE("Part restore validates the whole record and narrowing before publishing") {
    LiveParts live;
    auto* part = live.mission.getPart(1);
    auto original = part->saveRecord();
    for (int defect = 0; defect < 8; ++defect) {
        auto bad = original;
        if (defect == 0) bad.position.y = std::numeric_limits<float>::infinity();
        if (defect == 1) bad.controlType = 4;
        if (defect == 2) bad.profileName.bytes = "123456789";
        if (defect == 3) bad.profileName.bytes = "../asset";
        if (defect == 4) bad.squadId = 128;
        if (defect == 5) bad.squadId = -129;
        if (defect == 6) bad.profileName.encoding = 3;
        if (defect == 7) bad.profileName.encoding = 1;
        std::vector<std::string> logs;
        CHECK_FALSE(part->restoreRecord(bad, [&](std::string_view line) { logs.emplace_back(line); }));
        CHECK(logs.size() == 1);
        CHECK(part->saveRecord() == original);
        CHECK(std::string(part->csvFile) == "modmech");
    }
}

TEST_CASE("developer section import is transactional including the final Part") {
    LiveParts live;
    live.mission.getPart(1)->objectWID = 1;
    live.mission.getPart(2)->objectWID = 2;
    const auto original = live.mission.getPart(1)->saveRecord();
    const auto second = live.mission.getPart(2)->saveRecord();
    const auto section = live.section();
    poison(MissionPartArchiveTest::reserved(live.mission));
    const auto reserved = MissionPartArchiveTest::reserved(live.mission).saveRecord();
    for (std::size_t n = 0; n < section.payload.size(); ++n) {
        auto truncated = section;
        truncated.payload.resize(n);
        std::vector<std::string> logs;
        CHECK_FALSE(live.mission.loadParts(truncated, 1, [&](std::string_view line) { logs.emplace_back(line); }));
        CHECK(logs.size() == 1);
        CHECK(live.mission.getPart(1)->saveRecord() == original);
        CHECK(live.mission.getPart(2)->saveRecord() == second);
    }
    poison(*live.mission.getPart(1));
    poison(*live.mission.getPart(2));
    REQUIRE(live.mission.loadParts(section, 1));
    checkExpected(*live.mission.getPart(1), 1);
    checkExpected(*live.mission.getPart(2), 2);
    CHECK(MissionPartArchiveTest::reserved(live.mission).saveRecord() == reserved);
    for (int defect = 0; defect < 23; ++defect) {
        mc2::save::development::PartSection snapshot;
        snapshot.parts = {original, second};
        snapshot.parts[0].position.y = 33;
        if (defect == 0) snapshot.parts[1].squadId = 128;
        if (defect == 1) snapshot.parts[1].pilot = MAX_WARRIORS;
        if (defect == 2) snapshot.parts[1].objectWID = MAX_WATCHES;
        if (defect == 3) snapshot.parts.pop_back();
        if (defect == 4) snapshot.parts.push_back(original);
        if (defect == 5) snapshot.parts[1].objectWID = 3;
        if (defect == 8) {
            snapshot.parts[1].objectWID = 1;
            snapshot.parts[1].pilot = 1;
            snapshot.parts[1].objNumber = 41;
        }
        if (defect == 9) snapshot.parts[1].pilot = 1;
        if (defect == 10) live.pilots[1].setLive(false);
        if (defect == 11) snapshot.parts[1].teamId = 127;
        if (defect == 12) snapshot.parts[1].teamId = -2;
        if (defect == 13) snapshot.parts[1].commanderID = MAX_COMMANDERS;
        if (defect == 14) snapshot.parts[1].commanderID = -2;
        if (defect == 15) snapshot.parts[1].objNumber = 64;
        if (defect == 16) snapshot.parts[1].objNumber = 43;
        if (defect == 17) snapshot.parts[1].objNumber = 41;
        if (defect == 18) snapshot.parts[1].pilot = 0;
        if (defect == 19) live.movers[1].objectClass = BUILDING;
        if (defect == 20) MechWarrior::warriorList[2] = nullptr;
        if (defect == 21) snapshot.parts[1].objectWID = -1;
        if (defect == 22) snapshot.parts[1].objNumber = 0;
        mc2::save::Archive writer;
        writer(snapshot);
        REQUIRE(writer.finish());
        mc2::save::Section bad{defect == 6 ? 9u : 10u, true, writer.bytes()};
        std::vector<std::string> logs;
        CHECK_FALSE(live.mission.loadParts(bad, defect == 7 ? 2 : 1,
            [&](std::string_view line) { logs.emplace_back(line); }));
        CHECK(logs.size() == 1);
        if (defect == 8 && !logs.empty()) CHECK(logs[0].find("part_watch_duplicate") != std::string::npos);
        if (defect == 9 && !logs.empty()) CHECK(logs[0].find("part_watch_reference") != std::string::npos);
        CHECK(live.mission.getPart(1)->saveRecord() == original);
        CHECK(live.mission.getPart(2)->saveRecord() == second);
        CHECK(MissionPartArchiveTest::reserved(live.mission).saveRecord() == reserved);
        CHECK(std::string(live.mission.getPart(1)->csvFile) == "destination_hint");
        CHECK(std::string(live.mission.getPart(2)->csvFile) == "destination_hint");
        live.pilots[1].setLive(true);
        live.movers[1].objectClass = MOVER;
        MechWarrior::warriorList[2] = &live.pilots[1];
    }
}

TEST_CASE("unassigned references and neutral owner sentinels remain valid") {
    LiveParts live;
    mc2::save::development::PartSection snapshot;
    snapshot.parts = {expectedPart(1), expectedPart(2)};
    for (auto& record : snapshot.parts) {
        record.pilot = 0;
        record.objectWID = 0;
        record.teamId = -1;
        record.commanderID = -1;
    }
    mc2::save::Archive writer;
    writer(snapshot);
    REQUIRE(writer.finish());
    REQUIRE(live.mission.loadParts({10, true, writer.bytes()}, 1));
    CHECK(live.mission.getPart(1)->saveRecord() == snapshot.parts[0]);
    CHECK(live.mission.getPart(2)->saveRecord() == snapshot.parts[1]);
    snapshot.parts[0].teamId = MAX_TEAMS - 1;
    snapshot.parts[0].commanderID = MAX_COMMANDERS - 1;
    mc2::save::Archive boundary;
    boundary(snapshot);
    REQUIRE(boundary.finish());
    REQUIRE(live.mission.loadParts({10, true, boundary.bytes()}, 1));
    CHECK(live.mission.getPart(1)->saveRecord() == snapshot.parts[0]);
    snapshot.parts[1] = {};
    mc2::save::Archive omitted;
    omitted(snapshot);
    REQUIRE(omitted.finish());
    REQUIRE(live.mission.loadParts({10, true, omitted.bytes()}, 1));
    CHECK(live.mission.getPart(2)->saveRecord() == snapshot.parts[1]);
}
}
