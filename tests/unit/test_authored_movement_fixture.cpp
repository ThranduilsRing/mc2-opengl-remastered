#include "doctest.h"
#include "path_work.h"
#include "mission.h"
#include "mech.h"
#include "movemgr.h"
#include "objtype.h"
#include "heap.h"
#include "mapdata.h"
#include "team.h"
#include "quad.h"
#include "move_internal.h"
#include "mover_internal.h"
#include "comndr.h"
#include "objective.h"
#include "afont.h"
#include "inifile.h"
#include "timing.h"
#include "Gates/gate_cold.h"
#include "GameOS/gameos/diagnostic_trace.h"
#include "gamesound.h"
#include "logisticsdata.h"
#include "paths.h"
#include "mc2video.h"
#include "multplyr.h"
#include "save_fix_section.h"
#include <chrono>
#include <fstream>
#include <thread>

#include <array>
#include <cstring>
#include <cmath>
#include <functional>
#include <memory>
#include <type_traits>
#include <vector>
#include <cstdlib>
#include <cstdio>
#include <io.h>
#include <filesystem>

extern long LastMoveCalcErr;

struct ObjectiveObservationFixture {
    std::unique_ptr<aFont> previous = std::move(CObjective::s_markerFont);
    ObjectiveObservationFixture() { CObjective::s_markerFont = std::make_unique<aFont>(); }
    ~ObjectiveObservationFixture() { CObjective::s_markerFont = std::move(previous); }
};

TEST_CASE("ObjectiveObservation/replay preserves causes; decoded frames emit once; stale movies and completed availability emit nothing") {
    struct CountedCondition : CObjectiveCondition {
        int& count;
        explicit CountedCondition(int& value) : CObjectiveCondition(0), count(value) {}
        condition_species_type Species() override { return ELAPSED_MISSION_TIME; }
        bool Save(FitIniFile*) override { return true; }
        objective_status_type Status() override { ++count; return OS_SUCCESSFUL; }
    };
    struct SinkAction : CObjectiveAction {
        bool movie;
        VideoObjectiveReceipt receipt;
        explicit SinkAction(bool value) : CObjectiveAction(0), movie(value) {
            observationOrdinal = movie ? 5 : 4;
            observationPayload = movie ? "\"briefing.bik\"" : "\"ready\"";
        }
        action_species_type Species() override { return movie ? PLAY_BIK : DISPLAY_TEXT_MESSAGE; }
        bool Init() override { return true; }
        bool Read(FitIniFile*) override { return true; }
        bool Save(FitIniFile*) override { return true; }
        int Execute() override {
            if (movie) { receipt.capture("./briefing.mp4"); }
            objectiveObservationSink(movie ? "movie" : "chat", movie ? "gui" : "stored",
                                     movie ? "boot_to_bay" : "accepted", movie ? "./briefing.bik" : "ready");
            return true;
        }
    };
    const std::string fitText =
        "FITini\r\n[Objectives Version]\r\nul Version = 3\r\n"
        "[Team0Objectives]\r\nul NumObjectives = 2\r\n"
        "[Team0BooleanFlags]\r\nst BooleanString0 = \"ready\"\r\nb BooleanValue0 = true\r\n"
        "[Team0Objective0]\r\nst Title = \"malformed\"\r\nb DisplayMarker = false\r\n"
        "[Team0Objective1]\r\nst Title = \"fixture\"\r\nb DisplayMarker = false\r\n"
        "b HiddenTrigger = true\r\nb ActivateOnFlag = true\r\nst ActivateFlagID = \"ready\"\r\n"
        "ul NumConditions = 0\r\nul NumActions = 4\r\n"
        "[Team0Objective1Action0]\r\nst ActionSpeciesString = \"PlayWAV\"\r\n"
        "[Team0Objective1Action1]\r\nst ActionSpeciesString = \"SetBooleanFlag\"\r\n"
        "st FlagID = \"signal\"\r\nb Value = true\r\n"
        "[Team0Objective1Action2]\r\nst ActionSpeciesString = \"PlayWAV\"\r\n"
        "st WAVFileName = \"unavailable.wav\"\r\n"
        "[Team0Objective1Action3]\r\nst ActionSpeciesString = \"MakeNewTechnologyAvailable\"\r\n"
        "st PurchaseFileName = \"mvh_scratch_objective_bonus.fit\"\r\nFITend\r\n";
    const std::string stem = "mvh_scratch_objective_observation";
    { std::ofstream fit(stem + ".fit", std::ios::binary); fit << fitText; REQUIRE(fit.good()); }
    { std::ofstream pak(stem + ".pak", std::ios::binary); pak << "controlled-pak"; REQUIRE(pak.good()); }
    { std::ofstream bonus("mvh_scratch_objective_bonus.fit", std::ios::binary);
      bonus << "FITini\r\n[Components]\r\n[Mechs]\r\n[Pilots]\r\nFITend\r\n"; REQUIRE(bonus.good()); }
    const auto scratch = std::filesystem::absolute(stem + "_logistics");
    std::filesystem::create_directories(scratch / "data/campaign");
    { std::ofstream campaign(scratch / "data/campaign/campaign.fit", std::ios::binary);
      campaign << "FITini\r\n[Campaign]\r\nst CampaignName = \"fixture\"\r\nl GroupCount = 1\r\nl CBills = 100\r\n"
                  "[Group0]\r\nl MissionCount = 1\r\nst Video = \"\"\r\n"
                  "[Group0Mission0]\r\nst FileName = \"mission\"\r\nst PurchaseFile = \"purchase\"\r\nFITend\r\n";
      REQUIRE(campaign.good()); }
    { std::ofstream missionFile(scratch / "mission.fit", std::ios::binary);
      missionFile << "FITini\r\n[MissionSettings]\r\nst MissionName = \"fixture\"\r\nst Blurb2 = \"fixture\"\r\nFITend\r\n";
      REQUIRE(missionFile.good()); }
    { std::ofstream purchase(scratch / "purchase.fit", std::ios::binary);
      purchase << "FITini\r\n[Components]\r\nb AllComponents = false\r\n[Mechs]\r\n[Pilots]\r\nFITend\r\n";
      REQUIRE(purchase.good()); }
    mc2::gate::ScopedOverride<"MC2_DIAG_TAGS"> tags("OBJECTIVES");
    const auto diagnostic = "debug_state/objective-observation-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".jsonl";
    mc2::gate::ScopedOverride<"MC2_DIAGNOSTIC_TRACE_FILE"> trace(diagnostic.c_str());
    mc2_diag::init("objective-observation", 0);
    REQUIRE(mc2_diag::tagEnabled("OBJECTIVES"));

    auto run = [&](bool enabled) {
        mc2::gate::ScopedOverride<"MC2_DIAG_TAGS"> mode(enabled ? "OBJECTIVES" : "none");
        auto* previousTeam = Team::teams[0];
        const auto previousTeamCount = Team::numTeams;
        const auto previousTurn = turn;
        turn = 40;
        struct Restore {
            Team* team;
            long count, frame;
            GameSoundSystem* sound;
            bool audio;
            LogisticsData* logistics;
            std::string path = missionPath;
            std::filesystem::path cwd = std::filesystem::current_path();
            ~Restore() {
                Team::teams[0] = team; Team::numTeams = count; turn = frame;
                soundSystem = sound; useSound = audio;
                LogisticsData::instance = logistics;
                std::strcpy(missionPath, path.c_str());
                std::filesystem::current_path(cwd);
            }
        } restore{previousTeam, previousTeamCount, previousTurn, soundSystem, useSound,
                  LogisticsData::instance};
        ObjectiveObservationFixture marker;
        GameSoundSystem controlledSound;
        soundSystem = &controlledSound;
        useSound = false;
        Team team;
        Team::teams[0] = &team;
        team.objectives.Alignment(0);
        LogisticsData::instance = nullptr;
        LogisticsData logistics;
        std::strcpy(missionPath, (scratch.generic_string() + "/").c_str());
        std::filesystem::current_path(scratch);
        logistics.initFast();
        std::filesystem::current_path(restore.cwd);
        FitIniFile file;
        REQUIRE(file.open((stem + ".fit").c_str()) == NO_ERR);
        REQUIRE(team.objectives.Read(&file));
        REQUIRE(team.objectives.Count() == 1);
        auto* objective = *team.objectives.begin();
        REQUIRE(objective->m_actionList.Count() == 3);
        CHECK((*objective->m_actionList.begin())->observationOrdinal == 1);
        objective->m_actionList.Append(new SinkAction(false));
        auto* movie = new SinkAction(true);
        objective->m_actionList.Append(movie);
        int evaluations = 0;
        objective->m_conditionList.Append(new CountedCondition(evaluations));
        CHECK_FALSE(objective->IsActive());
        objective->IsActive(true);
        CHECK(evaluations == 0);
        CHECK(team.objectives.Status() == OS_SUCCESSFUL);
        CHECK(team.objectives.Status() == OS_SUCCESSFUL);
        CHECK(evaluations == 1);
        CHECK(team.objectives.boolFlags.getElementValue("signal"));
        objective->Status(OS_UNDETERMINED);
        CHECK(objective->Status(team.objectives) == OS_SUCCESSFUL);
        CHECK(evaluations == 2);
        auto& observation = team.objectives.observation;
        CHECK(observation.sequence == (enabled ? 24 : 0));
        objective->ObserveCBills(100, 350);
        CHECK(movie->receipt.cause == (enabled ? 23 : 0));
        CHECK(movie->receipt.path.empty() == !enabled);
        VideoObjectiveReceipt unrelated;
        unrelated.capture("./unrelated.mp4");
        unrelated.decoded();
        auto stale = movie->receipt;
        stale.missionHash = "different-mission";
        stale.decoded();
        CHECK(observation.sequence == (enabled ? 25 : 0));
        movie->receipt.decoded();
        movie->receipt.decoded();
        CHECK(observation.sequence == (enabled ? 26 : 0));
        if (enabled) {
            observation.finalResult = 5;
            observation.Finish();
            CHECK(observation.sequence == 26);
            CHECK_FALSE(observation.completed);
            observation.settled = true;
            CHECK(logistics.updateAvailability() == 0);
            CHECK(observation.sequence == 29);
            CHECK(observation.completed);
            observation.Finish();
            CHECK(logistics.updateAvailability() == 0);
            CHECK(observation.sequence == 29);
        }
        file.close();
        return evaluations;
    };
    const int disabledEvaluations = run(false);
    CHECK(disabledEvaluations == run(true));
    mc2_diag::flush();
    std::ifstream input(diagnostic);
    std::vector<std::string> records;
    for (std::string line; std::getline(input, line);) {
        if (line.find("\"tag\":\"OBJECTIVES\"") == std::string::npos) { continue; }
        auto start = line.find("\"data\":");
        REQUIRE(start != std::string::npos);
        records.push_back(line.substr(start + 7, line.size() - start - 8));
    }
    // The sink boundary is controlled; the technology queue and availability pass are production code.
    struct Expected { const char* kind; const char* cause; const char* data; };
    const std::vector<Expected> expected = {
        {"activate", "null", R"({"team":0,"objective":1,"old":0,"new":1,"traversal":0,"source":"write"})"},
        {"resolve", "\"o1\"", R"({"team":0,"objective":1,"old":0,"new":1,"traversal":1,"source":"team"})"},
        {"dispatch", "\"o2\"", R"({"team":0,"objective":1,"list":"success","entry":1,"species":"SetBooleanFlag","payload":{"FlagID":"signal","Value":true},"result":1})"},
        {"sink", "\"o3\"", R"({"sink":"flag","stage":"written","result":"true","payload":"signal"})"},
        {"dispatch", "\"o2\"", R"({"team":0,"objective":1,"list":"success","entry":2,"species":"PlayWAV","payload":"unavailable.wav","result":1})"},
        {"sink", "\"o5\"", R"({"sink":"wav","stage":"resource_channel","result":"disabled","payload":{"requested":"./unavailable.wav","resource":"","channel":18,"resourceAccepted":false,"channelAssigned":false}})"},
        {"dispatch", "\"o2\"", R"({"team":0,"objective":1,"list":"success","entry":3,"species":"MakeNewTechnologyAvailable","payload":"mvh_scratch_objective_bonus.fit","result":1})"},
        {"sink", "\"o7\"", R"({"sink":"technology","stage":"queued","result":"accepted","payload":"mvh_scratch_objective_bonus.fit"})"},
        {"dispatch", "\"o2\"", R"({"team":0,"objective":1,"list":"success","entry":4,"species":"DisplayTextMessage","payload":"ready","result":1})"},
        {"sink", "\"o9\"", R"({"sink":"chat","stage":"stored","result":"accepted","payload":"ready"})"},
        {"dispatch", "\"o2\"", R"({"team":0,"objective":1,"list":"success","entry":5,"species":"PlayBIK","payload":"briefing.bik","result":1})"},
        {"sink", "\"o11\"", R"({"sink":"movie","stage":"gui","result":"boot_to_bay","payload":"./briefing.bik"})"},
        {"reset", "\"o2\"", R"({"team":0,"objective":1,"old":1,"new":0,"traversal":0,"source":"write"})"},
        {"resolve", "\"o13\"", R"({"team":0,"objective":1,"old":0,"new":1,"traversal":3,"source":"context"})"},
        {"dispatch", "\"o14\"", R"({"team":0,"objective":1,"list":"success","entry":1,"species":"SetBooleanFlag","payload":{"FlagID":"signal","Value":true},"result":1})"},
        {"sink", "\"o15\"", R"({"sink":"flag","stage":"written","result":"true","payload":"signal"})"},
        {"dispatch", "\"o14\"", R"({"team":0,"objective":1,"list":"success","entry":2,"species":"PlayWAV","payload":"unavailable.wav","result":1})"},
        {"sink", "\"o17\"", R"({"sink":"wav","stage":"resource_channel","result":"disabled","payload":{"requested":"./unavailable.wav","resource":"","channel":18,"resourceAccepted":false,"channelAssigned":false}})"},
        {"dispatch", "\"o14\"", R"({"team":0,"objective":1,"list":"success","entry":3,"species":"MakeNewTechnologyAvailable","payload":"mvh_scratch_objective_bonus.fit","result":1})"},
        {"sink", "\"o19\"", R"({"sink":"technology","stage":"queued","result":"accepted","payload":"mvh_scratch_objective_bonus.fit"})"},
        {"dispatch", "\"o14\"", R"({"team":0,"objective":1,"list":"success","entry":4,"species":"DisplayTextMessage","payload":"ready","result":1})"},
        {"sink", "\"o21\"", R"({"sink":"chat","stage":"stored","result":"accepted","payload":"ready"})"},
        {"dispatch", "\"o14\"", R"({"team":0,"objective":1,"list":"success","entry":5,"species":"PlayBIK","payload":"briefing.bik","result":1})"},
        {"sink", "\"o23\"", R"({"sink":"movie","stage":"gui","result":"boot_to_bay","payload":"./briefing.bik"})"},
        {"resolve", "\"o14\"", R"({"scope":"cbills","team":0,"objective":1,"old":100,"new":350})"},
        {"sink", "\"o23\"", R"({"sink":"movie","stage":"decoded_frame","result":"accepted","payload":"./briefing.mp4"})"},
        {"sink", "\"o7\"", R"({"sink":"technology","stage":"purchase_file","result":"opened","payload":"mvh_scratch_objective_bonus.fit"})"},
        {"sink", "\"o19\"", R"({"sink":"technology","stage":"purchase_file","result":"opened","payload":"mvh_scratch_objective_bonus.fit"})"},
        {"complete", "null", R"({"count":28,"result":5})"},
    };
    REQUIRE(records.size() == expected.size());
    std::ofstream extracted(stem + ".trace", std::ios::binary);
    for (size_t i = 0; i < records.size(); ++i) {
        const auto number = std::to_string(i + 1);
        const std::string record = "{\"mission\":\"67b93cd668baabf9c46c9eca87f8acc510346e5910b7361b1e08f2237f32346d\",\"seq\":" + number +
            ",\"id\":\"o" + number + "\",\"frame\":40,\"time\":0,\"kind\":\"" + expected[i].kind +
            "\",\"cause\":" + expected[i].cause + ",\"data\":" + expected[i].data + "}";
        CHECK(records[i] == record);
        extracted << "OBJECTIVES/1 " << records[i] << '\n';
    }
    CHECK(extracted.good());
}


namespace {
struct SavedGlobals {
    std::vector<std::function<void()>> restorers;
    template<class T> void keep(T& value) {
        if constexpr (std::is_array_v<T>) {
            static_assert(std::is_trivially_copyable_v<T>);
            std::array<std::byte, sizeof(T)> bytes;
            std::memcpy(bytes.data(), &value, sizeof(T));
            restorers.emplace_back([slot = &value, bytes] { std::memcpy(slot, bytes.data(), bytes.size()); });
        } else {
            restorers.emplace_back([slot = &value, copy = value] { *slot = copy; });
        }
    }
    ~SavedGlobals() {
        for (auto it = restorers.rbegin(); it != restorers.rend(); ++it) (*it)();
    }
};

// CPU height samples suffice for pathing; texture transitions require a renderer.
struct FlatMesh : MapData {
    std::vector<PostcompVertex> vertices{60 * 60};
    FlatMesh() { blocks = vertices.data(); }
    ~FlatMesh() {
        if (Terrain::mapData == this) Terrain::mapData = nullptr;
        blocks = nullptr;
    }
};
struct Watches : GameObjectManager {
    GameObject* entries[2]{};
    Watches() { watchList = entries; nextWatchID = 2; collisionSystem = nullptr; moverLineOfSightTable = nullptr; }
};
struct MovingMech : BattleMech {
    void attach(MechWarrior& warrior) {
        pilot = &warrior;
        watchID = 1;
        setExists(true);
        maxMoveSpeed = 10.0f;
        moveLevel = 0;
        pathLocks = false;
        commanderId = 7;
        teamId = 0;
    }
    void nextSegment() { char throttle = -1; long gesture = -1; setNextMovePath(throttle, gesture); }
};
struct MovingPilot : MechWarrior {
    void deferTactics() { movementUpdate = scenarioTime + 1000.0f; }
    void activate() {
        teamId = 0;
        brainUpdate = combatUpdate = movementUpdate = scenarioTime + 1000.0f;
        useGoalPlan = false;
        status = WARRIOR_STATUS_NORMAL;
    }
    void finishGlobalPath() { moveOrders.curGlobalStep = moveOrders.numGlobalSteps - 1; }
    bool generalReceived() const { return newTacOrderReceived[ORDERSTATE_GENERAL]; }
};

struct MovementWorld {
    SavedGlobals saved;
    std::unique_ptr<Terrain> terrain;
    std::unique_ptr<FlatMesh> mesh;
    std::unique_ptr<MissionMap> cells;
    std::unique_ptr<GlobalMap> areas;
    std::unique_ptr<TriggerAreaManager> triggers;
    std::unique_ptr<Watches> objects;
    std::unique_ptr<MovingPilot> pilot;
    std::unique_ptr<MovingMech> mech;
    std::unique_ptr<MovePathManager> queue;

    MovementWorld() {
        REQUIRE(Terrain::terrainTextures == nullptr);
        REQUIRE(Terrain::terrainTextures2 == nullptr);
        REQUIRE(Terrain::objBlockInfo == nullptr);
        saved.keep(land);
        saved.keep(GameMap); saved.keep(GlobalMoveMap); saved.keep(PathFindMap);
        saved.keep(PathManager); saved.keep(ObjectManager);
        saved.keep(Mover::triggerAreaMgr);
        saved.keep(GameObject::initialize); saved.keep(Mover::holdFireIconHandle);
        saved.keep(Mover::numMovers); saved.keep(MechWarrior::numWarriors);
        saved.keep(MechWarrior::warriorList);
        saved.keep(Team::numTeams); saved.keep(Commander::numCommanders);
        saved.keep(scenarioTime); saved.keep(SimpleMovePathRange);
        saved.keep(MovePathManager::numPaths); saved.keep(MovePathManager::peakPaths);
        saved.keep(MovePathManager::sourceTally);
        saved.keep(GlobalMap::log); saved.keep(GlobalMap::logEnabled);
        saved.keep(GlobalMap::minRow); saved.keep(GlobalMap::maxRow);
        saved.keep(GlobalMap::minCol); saved.keep(GlobalMap::maxCol);
        saved.keep(Terrain::mapData);
        saved.keep(Terrain::realVerticesMapSide); saved.keep(Terrain::halfVerticesMapSide);
        saved.keep(Terrain::blocksMapSide); saved.keep(Terrain::visibleVerticesPerSide);
        saved.keep(Terrain::worldUnitsMapSide); saved.keep(Terrain::mapTopLeft3d);
        saved.keep(Terrain::recalcLight); saved.keep(Terrain::recalcShadows);
        saved.keep(Terrain::numObjBlocks);
        saved.keep(Terrain::objBlockInfo);
        saved.keep(TerrainQuad::rainLightLevel); saved.keep(TerrainQuad::lighteningLevel);
        saved.keep(TerrainQuad::mineTextureHandle); saved.keep(TerrainQuad::blownTextureHandle);
        saved.keep(Terrain::tileRowToWorldCoord); saved.keep(Terrain::tileColToWorldCoord);
        saved.keep(Terrain::cellRowToWorldCoord); saved.keep(Terrain::cellColToWorldCoord);
        saved.keep(Terrain::cellToWorldCoord);
        saved.keep(MapData::shallowDepth); saved.keep(MapData::waterDepth);
        saved.keep(MapData::alphaDepth); saved.keep(MapData::WaterTXMData);
        saved.keep(cellShiftDistance); saved.keep(tileMulMAPCELL_DIM);
        saved.keep(VerticesMapSideDivTwo); saved.keep(MetersMapSideDivTwo);
        saved.keep(MapCellDiagonal); saved.keep(HalfMapCell);
        saved.keep(ClearBridgeTiles);
        saved.keep(DebugMovePathType); saved.keep(RamObjectWID); saved.keep(LastMoveCalcErr);
        saved.keep(goalMap); saved.keep(goalMapRowStart); saved.keep(goalMapRowCol);
        GameObject::initialize = true;
        Mover::holdFireIconHandle = 1;
        scenarioTime = 1.0f;
        terrain = std::make_unique<Terrain>();
        land = terrain.get();
        Terrain::realVerticesMapSide = 60;
        Terrain::halfVerticesMapSide = 30;
        Terrain::blocksMapSide = 3;
        Terrain::worldUnitsMapSide = 60 * Terrain::worldUnitsPerVertex;
        Terrain::mapTopLeft3d = glm::vec3(-Terrain::worldUnitsMapSide / 2, Terrain::worldUnitsMapSide / 2, 0);
        Terrain::tileRowToWorldCoord = Terrain::tileColToWorldCoord = nullptr;
        Terrain::cellRowToWorldCoord = Terrain::cellColToWorldCoord = Terrain::cellToWorldCoord = nullptr;
        terrain->initMapCellArrays();
        mesh = std::make_unique<FlatMesh>();
        Terrain::mapData = mesh.get();
        cells = std::make_unique<MissionMap>();
        GameMap = cells.get();
        // One-cell doors make every calculated destination deterministic across four sectors.
        cells->init(180, 180);
        for (int col = 35; col <= 145; ++col) cells->setPassable(90, col, true);
        PathFindMap[0] = PathFindMap[1] = nullptr;
        GlobalMoveMap[0] = GlobalMoveMap[1] = GlobalMoveMap[2] = nullptr;
        Terrain::objBlockInfo = static_cast<ObjBlockInfo*>(std::malloc(9 * sizeof(ObjBlockInfo)));
        objects = std::make_unique<Watches>();
        ObjectManager = objects.get();
        pilot = std::make_unique<MovingPilot>();
        mech = std::make_unique<MovingMech>();
        mech->attach(*pilot);
        objects->entries[1] = mech.get();
        pilot->setVehicle(mech.get());
        pilot->deferTactics();
        queue = std::make_unique<MovePathManager>();
        PathManager = queue.get();
        triggers = std::make_unique<TriggerAreaManager>();
        Mover::triggerAreaMgr = triggers.get();
    }
    ~MovementWorld() {
        if (queue && pilot) queue->remove(pilot.get());
    }
    glm::vec3 point(int col) const {
        glm::vec3 result;
        land->cellToWorld(90, col, result);
        return result;
    }
    bool ready() const {
        return GameMap && !GameMap->map.empty() && land && Terrain::mapData && Terrain::mapData->getBlocks() &&
            ObjectManager && pilot->getVehicle() == mech.get() &&
            PathManager && PathFindMap[0] && PathFindMap[1] &&
            GlobalMoveMap[0] && GlobalMoveMap[0]->areas && GlobalMoveMap[0]->doors;
    }
    void provisionPaths() {
        MOVE_init(21);
        // PathFindMap[] is owned by move.cpp (MOVE_acquirePathFindMap); the fixture only observes it.
        areas = std::make_unique<GlobalMap>();
        GlobalMoveMap[0] = areas.get();
        areas->isGateDisabledCallback = [](int) { return false; };
        areas->isGateOpenCallback = [](int) { return true; };
        REQUIRE(areas->build(nullptr) == NO_ERR);
        for (int row = 0; row < 180; ++row)
            for (int col = 0; col < 180; ++col)
                for (int level = 0; level < NUM_MOVE_LEVELS; ++level) cells->setPathlock(level, row, col, false);
        REQUIRE(ready());
        auto start = point(40);
        mech->setPosition(start);
    }
};

// Only active steps have defined contents in a legacy path buffer.
struct BufferSnapshot {
    MovePath* identity;
    long steps, pausedSteps, cursor, globalStep, cost;
    bool marked;
    glm::vec3 goal;
    std::vector<std::array<int, 2>> cells;
    std::vector<std::array<float, 3>> destinations;
    std::vector<std::array<int, 2>> directionsAndAreas;
    std::vector<float> distances;
    explicit BufferSnapshot(MovePath* path) : identity(path), steps(path->numSteps),
        pausedSteps(path->numStepsWhenNotPaused), cursor(path->curStep), globalStep(path->globalStep),
        cost(path->cost), marked(path->marked), goal(path->goal) {
        for (long i = 0; i < pausedSteps; ++i) {
            const auto& step = path->stepList[i];
            cells.push_back({step.cell[0], step.cell[1]});
            destinations.push_back({step.destination.x, step.destination.y, step.destination.z});
            directionsAndAreas.push_back({step.direction, step.area});
            distances.push_back(step.distanceToGoal);
        }
    }
    bool operator==(const BufferSnapshot& rhs) const {
        return identity == rhs.identity && steps == rhs.steps && pausedSteps == rhs.pausedSteps &&
            cursor == rhs.cursor && globalStep == rhs.globalStep && cost == rhs.cost && marked == rhs.marked &&
            goal.x == rhs.goal.x && goal.y == rhs.goal.y && goal.z == rhs.goal.z && cells == rhs.cells &&
            destinations == rhs.destinations && directionsAndAreas == rhs.directionsAndAreas && distances == rhs.distances;
    }
};
struct Calculation {
    std::array<BufferSnapshot, 2> before, after;
    long previousGlobalCursor, globalCursor, globalSteps, pathType, source;
    unsigned long flags;
    bool drained;
};
std::array<BufferSnapshot, 2> buffers(MechWarrior& pilot) {
    return {BufferSnapshot(pilot.getMovePath(0)), BufferSnapshot(pilot.getMovePath(1))};
}

bool calculated(const Calculation& evidence, int buffer, int startCol, int endCol, int globalStep, const MovementWorld& world) {
    const auto& path = evidence.after[buffer];
    const auto cursor = globalStep == 3 ? 2 : globalStep;
    const auto goal = world.point(globalStep == 3 ? endCol : endCol + 30);
    if (evidence.pathType != MOVEPATH_COMPLEX || evidence.globalSteps != 4 || evidence.globalCursor != cursor ||
        evidence.previousGlobalCursor != (cursor == 0 ? 0 : cursor - 1) || !evidence.drained ||
        evidence.source != (cursor == 0 ? 21 : 5) ||
        evidence.flags != (MOVEPARAM_FACE_TARGET | (cursor == 0 ? MOVEPARAM_INIT : 0)) ||
        std::abs(path.goal.x - goal.x) > 0.001f || std::abs(path.goal.y - goal.y) > 0.001f || path.goal.z != goal.z) return false;
    if (evidence.before[buffer].pausedSteps != 0 || path.identity != evidence.before[buffer].identity || path.steps != endCol - startCol ||
        path.pausedSteps != path.steps || path.cursor != 0 || path.globalStep != globalStep ||
        path.cells.size() != static_cast<size_t>(path.steps) || evidence.after[1-buffer] != evidence.before[1-buffer]) return false;
    for (int i = 0; i < path.steps; ++i) {
        const auto point = world.point(startCol + i + 1);
        if (path.cells[i] != std::array<int, 2>{90, startCol + i + 1} ||
            std::abs(path.destinations[i][0] - point.x) > 0.001f ||
            std::abs(path.destinations[i][1] - point.y) > 0.001f || path.destinations[i][2] != point.z) return false;
    }
    return true;
}
Calculation service(MovementWorld& world, bool runQueue) {
    REQUIRE(world.ready());
    REQUIRE(world.pilot->getMovePathRequest() == world.queue->queueFront);
    REQUIRE(world.queue->queueFront != nullptr);
    auto before = buffers(*world.pilot);
    const auto request = *world.queue->queueFront;
    const auto previousCursor = world.pilot->getMoveCurGlobalStep();
    if (runQueue) world.queue->calcPath();
    return {before, buffers(*world.pilot), previousCursor, world.pilot->getMoveCurGlobalStep(),
        world.pilot->getMoveNumGlobalSteps(), world.pilot->getMovePathType(), request.num, request.moveParams,
        !world.pilot->getMovePathRequest() && !world.queue->queueFront};
}
void pointOrder(MovementWorld& world) {
    REQUIRE(world.ready());
    auto& order = *world.pilot->getCurTacOrder();
    order.init(ORDER_ORIGIN_SELF, TACTICAL_ORDER_MOVETO_POINT, false);
    order.setWayPoint(0, world.point(140));
    long message;
    CHECK(order.execute(world.pilot.get(), message) == TACORDER_FAILURE);
    REQUIRE(world.queue->queueFront);
    CHECK(world.queue->queueFront->num == 21);
    CHECK((world.queue->queueFront->moveParams & MOVEPARAM_INIT) != 0);
}
void consumeActive(MovementWorld& world, std::vector<int>& visited) {
    auto* path = world.pilot->getMovePath(0);
    REQUIRE(path->numStepsWhenNotPaused > 0);
    for (; path->curStep < path->numStepsWhenNotPaused; ++path->curStep) {
        auto destination = path->stepList[path->curStep].destination;
        world.mech->setPosition(destination);
        int row, col;
        world.mech->getCellPosition(row, col);
        CHECK(row == 90);
        CHECK(col == path->stepList[path->curStep].cell[1]);
        visited.push_back(col);
    }
    world.pilot->clearMovePath(0);
    world.pilot->getMovePath();
}
std::array<const void*, 16> bindings() {
    return {land, GameMap, GlobalMoveMap[0], GlobalMoveMap[1], GlobalMoveMap[2],
        PathFindMap[0], PathFindMap[1], ObjectManager, PathManager,
        Terrain::mapData, Terrain::tileRowToWorldCoord, Terrain::tileColToWorldCoord,
        Terrain::cellRowToWorldCoord, Terrain::cellColToWorldCoord, Terrain::objBlockInfo, Mover::triggerAreaMgr};
}

struct RouteMission : Mission {
    Part roster[2]{};
    RouteMission() { parts = roster; numParts = 1; roster[1].objectWID = 1; }
    using Mission::authoredRoutes;
    using Mission::routeExecutions;
    void release() {
        std::destroy_at(&orderFixtureReceipt); std::destroy_at(&authoredDiagnostics);
        std::destroy_at(&authoredRoutes); std::destroy_at(&routeExecutions);
        std::destroy_at(&rejectedRouteSignals); std::destroy_at(&authoredOpordPilots);
        std::destroy_at(&authoredMissionText); std::destroy_at(&orderRecipients);
        std::destroy_at(&orderIngress);
    }
};
// Mission's production destructor also shuts down the renderer and ABL globals.
union RouteMissionStorage {
    RouteMission owner;
    RouteMissionStorage() : owner() {}
    ~RouteMissionStorage() { owner.release(); }
};
struct RouteTeam : Team { RouteTeam() { id = 0; } };
struct StandingRouteWorld : MovementWorld {
    RouteTeam team;
    Commander commander;
    RouteMissionStorage storage;
    mc2::sim::RouteExecution snapshot;
    bool disconnected = false;
    StandingRouteWorld() {
        saved.keep(mission); saved.keep(Team::home); saved.keep(Team::teams);
        saved.keep(Commander::home); saved.keep(Commander::commanders);
        saved.keep(MechWarrior::brainsEnabled);
        Team::home = Team::teams[0] = &team;
        commander.setId(0); Commander::home = &commander;
        MechWarrior::warriorList[7] = pilot.get();
        MechWarrior::brainsEnabled[0] = true;
        pilot->activate();
        mission = &storage.owner;
        storage.owner.resetOrderIngress();
        storage.owner.bindOrderRecipients();
    }
    void install(mc2::sim::OrderKind kind = mc2::sim::OrderKind::PatrolPath, int firstCol = 70) {
        mc2::sim::AuthoredRoute route;
        route.originalPart = route.selectedPart = 1; route.pilot = 7; route.kind = kind;
        for (int col : {firstCol, 100, 130}) {
            auto p = point(col);
            route.path.points.push_back({p.x, p.y, p.z});
            route.path.mode.push_back(col == 100 ? mc2::sim::TravelMode::Fast : mc2::sim::TravelMode::Slow);
        }
        storage.owner.authoredRoutes.push_back(route);
        storage.owner.admitAuthoredRoutes();
        storage.owner.beginOrderSimulationTick(); storage.owner.completeOrderSimulationTick();
        REQUIRE(storage.owner.authoredExecutions().size() == 1);
        snapshot = storage.owner.authoredExecutions().front();
        disconnected = std::getenv("AUTHORED_SEAM_DISCONNECT_EXECUTION") != nullptr;
        if (disconnected) storage.owner.routeExecutions.clear();
        pilot->mainDecisionTree();
    }
    const mc2::sim::RouteExecution& execution() const {
        return disconnected ? snapshot : storage.owner.authoredExecutions().front();
    }
    void poll() { storage.owner.beginOrderSimulationTick(); storage.owner.completeOrderSimulationTick(); }
    bool segment() {
        const auto cursor = execution().path.curPoint;
        if (!queue->queueFront) return false;
        CHECK(pilot->getMoveRun() == (execution().path.mode[cursor] == mc2::sim::TravelMode::Fast));
        const auto sequence = execution().eventSequence;
        glm::vec3 query;
        CHECK(pilot->getNextWayPoint(query, false));
        CHECK(query.x == execution().path.points[cursor].x);
        CHECK(execution().eventSequence == sequence);
        CHECK(execution().path.curPoint == cursor);
        const bool reinit = execution().pendingSegment && execution().pendingSegment->reinit;
        const auto flags = queue->queueFront->moveParams;
        CHECK(((flags & MOVEPARAM_INIT) != 0 || reinit));
        queue->calcPath();
        CHECK(pilot->getMoveCurGlobalStep() == 0);
        CHECK(pilot->getMovePath(0)->numStepsWhenNotPaused > 0);
        CHECK_FALSE(execution().pendingSegment);
        std::vector<int> visited;
        for (int i = 0; i < 12; ++i) {
            pilot->movementDecisionTree();
            if (queue->queueFront) queue->calcPath();
            if (pilot->getMovePath(0)->numStepsWhenNotPaused) consumeActive(*this, visited);
            else break;
        }
        const auto p = execution().path.points[cursor];
        const auto pos = mech->getPosition();
        CHECK(pos.x == doctest::Approx(p.x)); CHECK(pos.y == doctest::Approx(p.y));
        CHECK_FALSE(visited.empty());
        mech->nextSegment();
        return std::abs(pos.x - p.x) < 0.001f && std::abs(pos.y - p.y) < 0.001f;
    }
    void alarm() {
        TacticalOrder order;
        order.init(ORDER_ORIGIN_SELF, TACTICAL_ORDER_WAIT, false);
        order.delayedTime = scenarioTime + 1000;
        pilot->setAlarmTacOrder(order, 100);
        pilot->mainDecisionTree();
    }
};
struct RouteLog {
    FILE* file = nullptr;
    int previous = -1;
    RouteLog() {
        std::filesystem::create_directories(".scratch");
        file = std::fopen(".scratch/s4b2b1w-events.log", "w+");
        REQUIRE(file);
        std::fflush(stderr); previous = _dup(_fileno(stderr)); _dup2(_fileno(file), _fileno(stderr));
    }
    std::string finish() {
        std::fflush(stderr); _dup2(previous, _fileno(stderr)); _close(previous); previous = -1;
        std::rewind(file);
        std::string result;
        char buffer[1024];
        while (auto count = std::fread(buffer, 1, sizeof(buffer), file)) result.append(buffer, count);
        return result;
    }
    ~RouteLog() {
        if (previous != -1) { std::fflush(stderr); _dup2(previous, _fileno(stderr)); _close(previous); }
        if (file) std::fclose(file);
    }
};
size_t occurrences(const std::string& text, const std::string& token) {
    size_t count = 0, offset = 0;
    while ((offset = text.find(token, offset)) != text.npos) { ++count; offset += token.size(); }
    return count;
}
}

TEST_CASE("AuthoredOrderProduction/BattleMech queued point move calculates complex buffers") {
    const auto previous = bindings();
    {
        MovementWorld world;
        // Missing PathFindMaps are a fixture precondition, not legacy deferral/recovery.
        CHECK_FALSE(world.ready());
        CHECK(world.cells->getPassable(90, 40));
        CHECK(world.pilot->getVehicle() == world.mech.get());
        world.provisionPaths();
        REQUIRE(world.ready());
        REQUIRE(world.areas->numAreas == 4);
        REQUIRE(world.areas->numDoors == 3);
        pointOrder(world);
        const auto first = service(world, true);
        INFO("first steps=" << first.after[0].steps << " global=" << first.after[0].globalStep << " goal=" << first.after[0].goal.x);
        CHECK(world.pilot->getMovePathType() == MOVEPATH_COMPLEX);
        CHECK(world.pilot->getMoveNumGlobalSteps() == 4);
        CHECK(first.globalCursor == 0);
        CHECK(calculated(first, 0, 40, 60, 0, world));
        CHECK(world.pilot->movementDecisionTree());
        REQUIRE(world.queue->queueFront);
        CHECK(world.queue->queueFront->num == 5);
        const auto second = service(world, true);
        INFO("second steps=" << second.after[1].steps << " global=" << second.after[1].globalStep << " goal=" << second.after[1].goal.x);
        CHECK(second.globalCursor == 1);
        CHECK(calculated(second, 1, 60, 90, 1, world));
        std::vector<int> visited;
        consumeActive(world, visited);
        CHECK(world.pilot->movementDecisionTree());
        const auto third = service(world, true);
        INFO("third steps=" << third.after[1].steps << " global=" << third.after[1].globalStep << " goal=" << third.after[1].goal.x);
        CHECK(calculated(third, 1, 90, 140, 3, world));
        CHECK(third.after[0].identity == second.after[1].identity);
        CHECK(third.after[1].identity == first.after[0].identity);
        consumeActive(world, visited);
        consumeActive(world, visited);
        REQUIRE(visited.size() == 100);
        for (int i = 0; i < 100; ++i) CHECK(visited[i] == 41 + i);
        CHECK(world.pilot->getCurTacOrder()->status(world.pilot.get()) == TACORDER_SUCCESS);
    }
    CHECK(bindings() == previous);
}

TEST_CASE("AuthoredOrderProduction/omitting queue service fails the same complex buffer proof") {
    const auto previous = bindings();
    {
        MovementWorld world;
        world.provisionPaths();
        pointOrder(world);
        const auto omitted = service(world, false);
        CHECK_FALSE(calculated(omitted, 0, 40, 60, 0, world));
        CHECK(omitted.after == omitted.before);
        CHECK(world.pilot->getMovePathType() == MOVEPATH_UNDEFINED);
        CHECK(world.pilot->getMovePathRequest() != nullptr);
    }
    CHECK(bindings() == previous);
}

TEST_CASE("AuthoredOrderProduction/standing route moves and survives general writes") {
    using namespace mc2::sim;
    StandingRouteWorld world;
    world.provisionPaths();
    RouteLog log;
    world.install();
    std::vector<unsigned> cursors;
    bool moved = true;
    for (int i = 0; i < 6; ++i) {
        cursors.push_back(world.execution().path.curPoint);
        moved = world.segment() && moved;
    }
    const bool cursor_sequence = moved && cursors == std::vector<unsigned>{0, 1, 2, 0, 1, 2};
    CHECK(cursor_sequence);
    CHECK(world.execution().path.curPoint == 0);
    CHECK(world.segment());
    const auto sequence = world.execution().eventSequence;
    ::TacticalOrder replacement;
    replacement.init(ORDER_ORIGIN_SELF, TACTICAL_ORDER_STOP, false);
    for (int i = 0; i < 25; ++i) world.pilot->setGeneralTacOrder(replacement);
    CHECK(world.execution().eventSequence == sequence);
    CHECK_FALSE(world.pilot->generalReceived());
    world.alarm();
    const auto suspended = world.execution().eventSequence;
    for (int i = 0; i < 25; ++i) world.pilot->setGeneralTacOrder(replacement);
    const bool general_slot_authored = world.pilot->getTacOrder(ORDERSTATE_GENERAL)->code == TACTICAL_ORDER_PATROL_PATH;
    CHECK(general_slot_authored);
    CHECK(world.execution().eventSequence == suspended);
    CHECK_FALSE(world.pilot->generalReceived());
    CHECK(world.execution().state == RouteState::Suspended);
    world.pilot->clearCurTacOrder();
    CHECK(world.execution().state == RouteState::Active);
    CHECK(world.execution().path.curPoint == 1);
    CHECK(world.pilot->getCurTacOrder()->moveParams.wayPath.curPoint == 1);
    CHECK(world.segment());
    const auto output = log.finish();
    CHECK(occurrences(output, "general_write_absorbed") == 1);
    CHECK(occurrences(output, "event=0 reason=0") == 1);
    CHECK(output.find("rejected") == output.npos);
}

TEST_CASE("AuthoredOrderProduction/traverse completes only after real final arrival") {
    StandingRouteWorld world;
    world.provisionPaths();
    world.install(mc2::sim::OrderKind::TraversePath);
    for (int i = 0; i < 3; ++i) CHECK(world.segment());
    CHECK(world.execution().state == mc2::sim::RouteState::Completed);
    CHECK(world.pilot->getCurTacOrder()->status(world.pilot.get()) == TACORDER_SUCCESS);
    CHECK(world.queue->queueFront == nullptr);
}

TEST_CASE("AuthoredOrderProduction/ABL movement reissues cannot steer active or suspended routes") {
    StandingRouteWorld world;
    world.provisionPaths(); world.install();
    auto path = world.pilot->getTacOrder(ORDERSTATE_GENERAL)->moveParams.wayPath;
    for (bool suspended : {false, true}) {
        if (suspended) world.alarm();
        std::array<std::byte, sizeof(*world.pilot->getMoveOrders())> before;
        std::memcpy(before.data(), world.pilot->getMoveOrders(), before.size());
        const auto position = world.mech->getPosition();
        const auto first = BufferSnapshot(world.pilot->getMovePath(0));
        const auto second = BufferSnapshot(world.pilot->getMovePath(1));
        const auto* request = world.queue->queueFront;
        const auto paths = MovePathManager::numPaths;
        for (int tick = 0; tick < 5; ++tick) {
            scenarioTime += 1.0f;
            world.pilot->orderMoveToPoint(false, true, ORDER_ORIGIN_COMMANDER, world.point(140), -1, TACORDER_PARAM_RUN);
            world.pilot->orderMoveToObject(false, true, ORDER_ORIGIN_COMMANDER, world.mech.get(), -1, -1, 0);
            world.pilot->orderTraversePath(false, true, ORDER_ORIGIN_COMMANDER, &path, 0);
            world.pilot->orderPatrolPath(false, true, ORDER_ORIGIN_COMMANDER, &path);
            CHECK(std::memcmp(before.data(), world.pilot->getMoveOrders(), before.size()) == 0);
            CHECK(world.queue->queueFront == request);
            CHECK(MovePathManager::numPaths == paths);
            CHECK(BufferSnapshot(world.pilot->getMovePath(0)) == first);
            CHECK(BufferSnapshot(world.pilot->getMovePath(1)) == second);
            CHECK(world.mech->getPosition() == position);
        }
    }
}

TEST_CASE("AuthoredOrderProduction/terminal routes cancel movement and cannot fall back to legacy paths") {
    using namespace mc2::sim;
    for (auto action : {RouteAction::Removed, RouteAction::PlayerOrder, RouteAction::Arrived}) {
        StandingRouteWorld world;
        world.provisionPaths(); world.install(OrderKind::TraversePath);
        world.queue->calcPath();
        REQUIRE(world.pilot->getMovePath(0)->numStepsWhenNotPaused > 0);
        world.pilot->requestMovePath(-1, MOVEPARAM_INIT, 17);
        REQUIRE(world.queue->queueFront);
        auto& route = world.storage.owner.routeExecutions.front();
        if (action == RouteAction::Arrived) route.path.curPoint = 2;
        world.storage.owner.signalRoute(route, action);
        REQUIRE(route.terminal());
        CHECK_FALSE(route.pendingSegment);
        CHECK(world.queue->queueFront == nullptr);
        CHECK(world.pilot->getMovePathRequest() == nullptr);
        CHECK(world.pilot->getMoveOrders()->numWayPts == 0);
        const auto position = world.mech->getPosition();
        for (int tick = 0; tick < 3; ++tick) {
            glm::vec3 next;
            CHECK_FALSE(world.pilot->getNextWayPoint(next, true));
            LastMoveCalcErr = 12345;
            CHECK(world.pilot->calcMovePath(-1, MOVEPARAM_INIT) == NO_ERR);
            CHECK(LastMoveCalcErr == 12345);
            CHECK(world.pilot->getMovePath(0)->numStepsWhenNotPaused == 0);
            CHECK(world.pilot->getMovePath(1)->numStepsWhenNotPaused == 0);
            CHECK(world.mech->getPosition() == position);
        }
    }
}

TEST_CASE("AuthoredOrderProduction/unready maps recover without rejected signals or duplicate start") {
    for (bool queued : {false, true}) {
        StandingRouteWorld world;
        if (queued) world.provisionPaths();
        RouteLog log;
        world.install();
        if (queued) {
            auto* map = PathFindMap[SECTOR_PATHMAP];
            PathFindMap[SECTOR_PATHMAP] = nullptr;
            world.queue->calcPath();
            CHECK(world.execution().pendingSegment.has_value());
            CHECK(world.execution().eventSequence == 1);
            PathFindMap[SECTOR_PATHMAP] = map;
        } else {
            for (int i = 0; i < 3; ++i) world.poll();
            CHECK(world.execution().state == mc2::sim::RouteState::Pending);
            CHECK(world.execution().eventSequence == 0);
            CHECK(world.queue->queueFront == nullptr);
            world.provisionPaths();
        }
        world.poll();
        CHECK(world.segment());
        const auto output = log.finish();
        CHECK(occurrences(output, "event=0 reason=0") == 1);
        CHECK(output.find("rejected") == output.npos);
    }
}

TEST_CASE("AuthoredOrderProduction/player cancellation and dead pilots cannot emit later progress") {
    using namespace mc2::sim;
    for (int ending : {0, 1, 2}) {
        StandingRouteWorld world;
        world.provisionPaths(); world.install();
        RouteLog log;
        if (ending == 2) world.alarm();
        if (ending == 0) {
            ::TacticalOrder player;
            player.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_WAIT, false);
            world.pilot->setPlayerTacOrder(player, false);
        } else world.pilot->setStatus(WARRIOR_STATUS_DEAD);
        if (world.queue->queueFront) world.queue->calcPath();
        if (ending == 0) world.pilot->mainDecisionTree();
        world.poll();
        const auto sequence = world.execution().eventSequence;
        CHECK(world.execution().state == RouteState::Interrupted);
        if (ending == 2) world.pilot->clearCurTacOrder();
        glm::vec3 point;
        world.pilot->getNextWayPoint(point, true);
        world.poll();
        CHECK(world.execution().eventSequence == sequence);
        CHECK(world.execution().path.curPoint == 0);
        const auto output = log.finish();
        CHECK(occurrences(output, ending == 0 ? "event=6 reason=1" : "event=6 reason=2") == 1);
        CHECK(output.find("event=8") == output.npos);
        CHECK(output.find("event=1") == output.npos);
    }
}

TEST_CASE("AuthoredOrderProduction/ABL temporaries and alarm PATH remain stock with a standing route") {
    StandingRouteWorld world;
    world.provisionPaths(); world.install();
    auto path = world.pilot->getTacOrder(ORDERSTATE_GENERAL)->moveParams.wayPath;
    auto* request = world.queue->queueFront;
    CHECK(world.pilot->orderPatrolPath(false, true, ORDER_ORIGIN_SELF, &path) == TACORDER_SUCCESS);
    CHECK(world.pilot->orderTraversePath(false, true, ORDER_ORIGIN_SELF, &path, 0) == TACORDER_SUCCESS);
    CHECK(world.queue->queueFront == request);
    TacticalOrder alarm;
    alarm.init(ORDER_ORIGIN_SELF, TACTICAL_ORDER_PATROL_PATH, false);
    alarm.moveParams.wayPath = path;
    world.pilot->setAlarmTacOrder(alarm, 100); world.pilot->mainDecisionTree();
    CHECK(world.execution().state == mc2::sim::RouteState::Suspended);
    CHECK(world.queue->queueFront == nullptr);
    CHECK(world.pilot->getCurTacOrder()->status(world.pilot.get()) == TACORDER_SUCCESS);
}

TEST_CASE("AuthoredOrderProduction/unserviced route queue cannot claim path acceptance") {
    StandingRouteWorld world;
    world.provisionPaths(); world.install();
    const auto sequence = world.execution().eventSequence;
    world.poll();
    CHECK(world.queue->queueFront != nullptr);
    CHECK(world.pilot->getMovePath(0)->numStepsWhenNotPaused == 0);
    CHECK(world.execution().eventSequence == sequence);
    CHECK(world.execution().pendingSegment.has_value());
}

TEST_CASE("AuthoredOrderProduction/brain and general-slot purges release ownership at the next poll") {
    for (bool disabled : {false, true}) {
        StandingRouteWorld world;
        world.provisionPaths(); world.install();
        if (disabled) {
            MechWarrior::brainsEnabled[0] = false;
            world.pilot->mainDecisionTree();
        } else world.pilot->getTacOrder(ORDERSTATE_GENERAL)->init();
        CHECK(world.execution().state == mc2::sim::RouteState::Active);
        world.poll();
        CHECK(world.execution().state == mc2::sim::RouteState::Interrupted);
        const auto sequence = world.execution().eventSequence;
        world.poll();
        CHECK(world.execution().eventSequence == sequence);
    }
}

TEST_CASE("AuthoredOrderProduction/blank MOVE initializes passability before building areas") {
    MovementWorld world;
    MOVE_init(21);
    MOVE_buildData(180, 180, nullptr, 0, nullptr);
    REQUIRE(GameMap != nullptr);
    CHECK(GameMap->getPassable(90, 90));
    for (int level : {0, 1}) {
        REQUIRE(GlobalMoveMap[level] != nullptr);
        GlobalMoveMap[level]->isGateDisabledCallback = [](int) { return false; };
        GlobalMoveMap[level]->isGateOpenCallback = [](int) { return true; };
        const long start = GlobalMoveMap[level]->calcArea(90, 40);
        const long goal = GlobalMoveMap[level]->calcArea(90, 140);
        INFO("level=" << level << " areas=" << GlobalMoveMap[level]->numAreas);
        CHECK(start >= 0);
        CHECK(goal >= 0);
        if (start < 0 || goal < 0) continue;
        GlobalPathStep path[MAX_GLOBAL_PATH];
        CHECK(GlobalMoveMap[level]->calcPath(start, goal, path, 90, 40, 90, 140) > 0);
    }
}

TEST_CASE("AuthoredOrderProduction/MOVE layer revisions move only with their own layer") {
    MovementWorld world;
    MOVE_init(21);
    MOVE_buildData(180, 180, nullptr, 0, nullptr);
    REQUIRE(GameMap != nullptr);
    const MoveLayerRevisions r0 = GameMap->rev;
    GameMap->setMover(90, 90, true);
    CHECK(GameMap->rev.mover == r0.mover + 1);
    GameMap->setPathlock(0, 90, 90, true);
    CHECK(GameMap->rev.pathlock == r0.pathlock + 1);
    GameMap->setGate(90, 91, 1);
    CHECK(GameMap->rev.gate == r0.gate + 1);
    GameMap->setMine(90, 92, 1);
    CHECK(GameMap->rev.mine == r0.mine + 1);
    GameMap->setPreserved(90, 93, true);
    CHECK(GameMap->rev.preserved == r0.preserved + 1);
    CHECK(GameMap->rev.statics == r0.statics);
    GameMap->setPassable(90, 94, false);
    CHECK(GameMap->rev.statics == r0.statics + 1);
    GameMap->setMover(-5, -5, true);  // off-map is a no-op
    CHECK(GameMap->rev.mover == r0.mover + 1);
}

TEST_CASE("AuthoredOrderProduction/MOVE revisions see runtime footprint and spread writes") {
    MovementWorld world;
    MOVE_init(21);
    MOVE_buildData(180, 180, nullptr, 0, nullptr);
    REQUIRE(GameMap != nullptr);
    MoveLayerRevisions r = GameMap->rev;
    char footPrint[] = {0, 0, 2, -1, -1, -1};   // one run of two cells (terrain-object destroy)
    GameMap->setPassable(90, 90, footPrint, true);
    CHECK(GameMap->rev.statics == r.statics + 1);
    r = GameMap->rev;
    GameMap->spreadState(100, 100, 1);          // per-frame mover placement
    const uint32_t first = GameMap->rev.preserved;
    CHECK(first > r.preserved);
    GameMap->spreadState(100, 100, 1);          // tile already preserved: the passable write must still count
    CHECK(GameMap->rev.preserved > first);
}

TEST_CASE("AuthoredOrderProduction/path scratch is private to its worker and retained") {
    MovementWorld world;
    MOVE_init(21);
    auto* mainScratch = PathFindMap[SIMPLE_PATHMAP];
    mainScratch->map[0].parent = 11;
    mainScratch->placeStationaryMoversCallback = [](MoveMapPtr) {};
    MOVE_configurePathFindMaps();
    MoveMapPtr workerScratch = nullptr;
    bool initiallyEmpty = false, retained = false, prepared = false, configured = false;
    std::thread worker([&] {
        initiallyEmpty = PathFindMap[SIMPLE_PATHMAP] == nullptr;
        MOVE_bindPathFindMaps(1);
        prepared = PathFindMap[SIMPLE_PATHMAP] && PathFindMap[SIMPLE_PATHMAP]->map.size() == 43 * 43;
        workerScratch = MOVE_acquirePathFindMap(SIMPLE_PATHMAP);
        const auto allocations = workerScratch->solverScratch.openHeap.allocations();
        configured = workerScratch->placeStationaryMoversCallback == mainScratch->placeStationaryMoversCallback;
        workerScratch->map[0].parent = 17;
        MOVE_bindPathFindMaps(1);
        retained = MOVE_acquirePathFindMap(SIMPLE_PATHMAP) == workerScratch &&
                   workerScratch->solverScratch.openHeap.allocations() == allocations;
    });
    worker.join();
    CHECK(initiallyEmpty);
    CHECK(prepared);
    CHECK(configured);
    CHECK(retained);
    CHECK(workerScratch != mainScratch);
    CHECK(PathFindMap[SIMPLE_PATHMAP] == mainScratch);
    CHECK(mainScratch->map[0].parent == 11);
}

TEST_CASE("AuthoredOrderProduction/path scratch rejects changed inputs and rebakes without allocations") {
    MovementWorld world;
    MOVE_init(21);
    MOVE_buildData(180, 180, nullptr, 0, nullptr);
    auto* scratch = PathFindMap[SIMPLE_PATHMAP];
    auto prepare = [&] {
        auto start = world.point(90);
        REQUIRE(scratch->setUp(70, 70, 43, 43, 0, &start, 90, 90,
                              world.point(95), 20, 25, 50, 0, 8) == NO_ERR);
        REQUIRE(scratch->inputRevisionsValid());
    };
    prepare();
    const auto allocations = scratch->solverScratch.openHeap.allocations();
    MovePath path;
    int goalCell[2]{};
    const mc2_move::PathQuery query;
    REQUIRE(scratch->calcPath(query, scratch->solverScratch, &path, nullptr, goalCell) > 0);
    const auto expectedPath = mc2_path_trace::hashMovePath(&path);
    {
        // Reentrancy: the same query on a freshly prepared grid gives the identical path whichever
        // scratch the solver is handed, and a scratch left dirty by another solve changes nothing.
        mc2_move::SolverScratch otherScratch;
        for (auto* use : {&otherScratch, &scratch->solverScratch, &otherScratch}) {
            prepare();
            MovePath again;
            int againCell[2]{};
            REQUIRE(scratch->calcPath(query, *use, &again, nullptr, againCell) > 0);
            CHECK(mc2_path_trace::hashMovePath(&again) == expectedPath);
            CHECK(againCell[0] == goalCell[0]);
            CHECK(againCell[1] == goalCell[1]);
        }
        prepare();
    }
    using Mutation = void (*)(MissionMap&);
    const Mutation mutations[] = {
        [](MissionMap& m) { m.setTerrain(1, 1, 1); },
        [](MissionMap& m) { m.setOverlay(1, 1, 1); },
        [](MissionMap& m) { m.setLocalHeight(1, 1, 1); },
        [](MissionMap& m) { m.setPassable(1, 1, false); },
        [](MissionMap& m) { m.setRoad(1, 1, true); },
        [](MissionMap& m) { m.setWall(1, 1, true); },
        [](MissionMap& m) { m.setShallowWater(1, 1, true); },
        [](MissionMap& m) { m.setDeepWater(1, 1, true); },
        [](MissionMap& m) { m.setBuildGate(1, 1, true); },
        [](MissionMap& m) { m.setBuildWall(1, 1, true); },
        [](MissionMap& m) { m.setBuildLandBridge(1, 1, true); },
        [](MissionMap& m) { m.setBuildNotSet(1, 1, true); },
        [](MissionMap& m) { m.setForest(1, 1, true); },
        [](MissionMap& m) { m.setOffMap(1, 1, true); },
        [](MissionMap& m) { m.setMover(1, 1, true); },
        [](MissionMap& m) { m.setGate(1, 1, true); },
        [](MissionMap& m) { m.setMine(1, 1, 1); },
        [](MissionMap& m) { m.setPathlock(0, 1, 1, true); },
        [](MissionMap& m) { m.setPreserved(1, 1, true); },
        [](MissionMap& m) { m.preserveCell(1, 2); },
    };
    for (auto mutate : mutations) {
        mutate(*GameMap);
        CHECK_FALSE(scratch->inputRevisionsValid());
        CHECK(scratch->calcPath(query, scratch->solverScratch, &path, nullptr, nullptr) == 0);
        CHECK(scratch->calcPathJUMP(query, scratch->solverScratch, &path, nullptr, nullptr) == 0);
        CHECK(scratch->calcEscapePath(query, scratch->solverScratch, &path, nullptr, nullptr) == 0);
        CHECK(path.numSteps == 0);
        CHECK(path.numStepsWhenNotPaused == 0);
        prepare();
        REQUIRE(scratch->calcPath(query, scratch->solverScratch, &path, nullptr, goalCell) > 0);
        CHECK(mc2_path_trace::hashMovePath(&path) == expectedPath);
        CHECK(scratch->solverScratch.openHeap.allocations() == allocations);
    }
    const auto revision = GameMap->rev.statics;
    GameMap->init(180, 180);
    CHECK(GameMap->rev.statics == revision + 1);
    CHECK_FALSE(scratch->inputRevisionsValid());
    CHECK(scratch->calcPath(query, scratch->solverScratch, &path, nullptr, nullptr) == 0);
}

TEST_CASE("AuthoredOrderProduction/classification uses the requested buffer and retries a ready zero-step path") {
    using namespace mc2::sim;
    for (bool zeroSteps : {false, true}) {
        StandingRouteWorld world;
        world.provisionPaths(); world.install(OrderKind::PatrolPath, 140);
        world.queue->calcPath();
        CHECK(world.pilot->getMovePath(0)->numStepsWhenNotPaused > 0);
        world.pilot->movementDecisionTree();
        REQUIRE(world.queue->queueFront);
        auto& route = world.storage.owner.routeExecutions.front();
        // A correlated continuation must classify its own buffer despite an older active path.
        route.pendingSegment = {route.path.curPoint, route.revision, false};
        auto goal = world.point(140);
        world.pilot->setMoveGoal(MOVEGOAL_LOCATION, &goal);
        if (zeroSteps) world.pilot->finishGlobalPath();
        const auto sequence = route.eventSequence;
        const auto evidence = service(world, true);
        CHECK(evidence.before[0].pausedSteps > 0);
        CHECK(evidence.after[0] == evidence.before[0]);
        CHECK((evidence.after[1].pausedSteps == 0) == zeroSteps);
        CHECK(route.eventSequence == sequence + 1);
        CHECK_FALSE(route.pendingSegment);
        CHECK(route.retryPending == zeroSteps);
        CHECK(route.retriesRemaining == routeRetryBudget - (zeroSteps ? 1 : 0));
        if (zeroSteps) {
            world.poll();
            CHECK(world.queue->queueFront != nullptr);
            CHECK(world.segment());
        }
    }
}

TEST_CASE("MovementCollision/non-ground blockage defines the path end output") {
    MovementWorld world;
    for (long level : {1, 2}) {
        world.mech->setMoveLevel(level);
        for (bool initial : {false, true}) {
            bool reachedEnd = initial;
            CHECK_FALSE(world.mech->getPathRangeBlocked(1, &reachedEnd));
            CHECK_FALSE(reachedEnd);
        }
        CHECK_FALSE(world.mech->getPathRangeBlocked(1, nullptr));
    }
}

TEST_CASE("JumpBackoff/production admission and active FIX1 restore use simulation ticks") {
    Team home;
    MovementWorld world;
    world.saved.keep(Team::home);
    world.saved.keep(MPlayer);
    Team::home = &home;
    home.id = 1; // Fixture mover is team zero: single-player AI.
    MPlayer = nullptr;
    world.pilot->getCurTacOrder()->init(ORDER_ORIGIN_SELF, TACTICAL_ORDER_MOVETO_POINT, false);
    auto goal = world.point(140);
    world.pilot->setMoveGoal(MOVEGOAL_LOCATION, &goal);
    auto admitted = [&] {
        REQUIRE(world.queue->queueFront == nullptr);
        world.pilot->requestMovePath(0, MOVEPARAM_NONE, 1);
        const bool queued = world.queue->queueFront != nullptr;
        CHECK((world.pilot->getMovePathRequest() != nullptr) == queued);
        world.queue->remove(world.pilot.get());
        return queued;
    };
    CHECK_FALSE(world.pilot->getJumpBackoff());
    CHECK(admitted()); // No stamp.
    world.pilot->noteJumpPathFailed();
    REQUIRE(world.pilot->getJumpBackoff());
    CHECK(world.pilot->getJumpBackoff()->age == 0);
    CHECK_FALSE(admitted());
    auto changed = world.point(139);
    world.pilot->setMoveGoal(MOVEGOAL_LOCATION, &changed);
    CHECK(admitted());
    world.pilot->setMoveGoal(MOVEGOAL_LOCATION, &goal);
    for (int tick = 0; tick < 44; ++tick) world.queue->update();
    REQUIRE(world.pilot->getJumpBackoff());
    CHECK(world.pilot->getJumpBackoff()->age == 44);
    CHECK_FALSE(admitted());

    // Persist a LIVE production stamp through FIX1, then restore at a different
    // clock origin. This must still suppress a real request, not just decode.
    const auto active = *world.pilot->getJumpBackoff();
    mc2::save::FixSection fix;
    fix.backoffs.push_back({0, active.goalR, active.goalC, active.age});
    const auto bytes = mc2::save::encodeFixSection(fix);
    const auto decoded = mc2::save::decodeValidFixSection(mc2::save::fixSectionVersion, bytes, MAX_WARRIORS, 1);
    REQUIRE(decoded);
    REQUIRE(decoded->backoffs.size() == 1);
    world.queue->update(); // Age 45: expires exactly here.
    CHECK_FALSE(world.pilot->getJumpBackoff());
    CHECK(admitted());
    for (int tick = 0; tick < 100; ++tick) world.queue->update();
    const auto& restored = decoded->backoffs.front();
    world.pilot->setJumpBackoff(restored.goalR, restored.goalC, restored.age);
    REQUIRE(world.pilot->getJumpBackoff());
    CHECK(world.pilot->getJumpBackoff()->age == 44);
    CHECK_FALSE(admitted());
    home.id = 0; // Ownership change after stamp/restore: player bypasses.
    CHECK(admitted());
    home.id = 1;
    world.pilot->getCurTacOrder()->origin = ORDER_ORIGIN_PLAYER;
    CHECK(admitted());
    world.pilot->getCurTacOrder()->origin = ORDER_ORIGIN_SELF;
    // Only pointer presence is consulted: no multiplayer service may be used.
    MPlayer = reinterpret_cast<MultiPlayer*>(world.mech.get());
    CHECK(admitted());
    MPlayer = nullptr;
    CHECK_FALSE(admitted());
    world.queue->update();
    CHECK_FALSE(world.pilot->getJumpBackoff());
    CHECK(admitted());

    // Protected owners never create a new stamp either.
    home.id = 0;
    world.pilot->noteJumpPathFailed();
    CHECK_FALSE(world.pilot->getJumpBackoff());
    home.id = 1;
    MPlayer = reinterpret_cast<MultiPlayer*>(world.mech.get());
    world.pilot->noteJumpPathFailed();
    MPlayer = nullptr;
    CHECK_FALSE(world.pilot->getJumpBackoff());
    world.pilot->getCurTacOrder()->origin = ORDER_ORIGIN_PLAYER;
    world.pilot->noteJumpPathFailed();
    CHECK_FALSE(world.pilot->getJumpBackoff());
}

TEST_CASE("PathWorkProduction/all four solver loops count successful and exhausted searches") {
    MovementWorld world;
    MOVE_init(21);
    MOVE_buildData(180, 180, nullptr, 0, nullptr);
    auto* map = PathFindMap[SIMPLE_PATHMAP];
    const mc2_move::PathQuery query;
    for (int solver = 0; solver < 3; ++solver) {
        for (bool exhausted : {false, true}) {
            INFO("solver=" << solver << " exhausted=" << exhausted);
            auto start = world.point(90);
            REQUIRE(map->setUp(70, 70, 43, 43, 0, &start, 90, 90,
                                world.point(95), 20, 25, 50, 0, 8) == NO_ERR);
            if (exhausted) {
                // Leave the start insert intact but no legal successors/goals.
                for (auto& cell : map->map) {
                    cell.cost = COST_BLOCKED;
                    cell.clearFlag(MOVEFLAG_GOAL);
                }
            }
            MovePath path;
            int goal[2]{};
            long escapeGoal[2]{};
            const auto before = mc2::path_work::expansions;
            long result = 0;
            if (solver == 0)
                result = map->calcPath(query, map->solverScratch, &path, nullptr, goal);
            else if (solver == 1)
                result = map->calcPathJUMP(query, map->solverScratch, &path, nullptr, goal);
            else
                result = map->calcEscapePath(query, map->solverScratch, &path, nullptr, escapeGoal);
            CHECK(mc2::path_work::expansions > before);
            if (exhausted) CHECK(result == 0);
            else if (solver == 2) {
                // Release escape search reaches the goal but initializes the
                // path length only inside _DEBUG (legacy behavior). Pin the
                // goal-reaching branch without fixing that unrelated defect.
                CHECK(escapeGoal[0] == 20);
                CHECK(escapeGoal[1] == 25);
                CHECK(path.cost > 0);
            }
            else CHECK(result > 0);
        }
    }
    auto* global = GlobalMoveMap[0];
    global->isGateDisabledCallback = [](int) { return false; };
    global->isGateOpenCallback = [](int) { return true; };
    const auto start = global->calcArea(90, 40);
    const auto goal = global->calcArea(90, 140);
    REQUIRE(start >= 0);
    REQUIRE(goal >= 0);
    REQUIRE(start != goal);
    GlobalPathStep path[MAX_GLOBAL_PATH];
    auto before = mc2::path_work::expansions;
    REQUIRE(global->calcPath(start, goal, path, 90, 40, 90, 140) > 0);
    CHECK(mc2::path_work::expansions > before);
    global->useClosedAreas = false;
    for (long door = 0; door < global->numDoors; ++door)
        global->doors[door].open = false;
    before = mc2::path_work::expansions;
    CHECK(global->calcPath(start, goal, path, 90, 40, 90, 140) == 0);
    CHECK(mc2::path_work::expansions > before);
}

TEST_CASE("PathWorkProduction/replacement retains age position and source accounting") {
    MovementWorld world;
    auto& queue = *world.queue;
    queue.request(world.pilot.get(), 1, MOVEPARAM_INIT, 16);
    auto* original = queue.queueFront;
    original->enqueueFrame = MovePathManager_getFrameCounter() - 2;
    MovingPilot second;
    queue.request(&second, 2, MOVEPARAM_INIT, 17);
    queue.request(world.pilot.get(), 3, MOVEPARAM_JUMP, 18);
    CHECK(queue.queueFront == original);
    CHECK(queue.queueEnd == second.getMovePathRequest());
    CHECK(original->enqueueFrame == MovePathManager_getFrameCounter() - 2);
    CHECK(original->selectionIndex == 3);
    CHECK(original->moveParams == MOVEPARAM_JUMP);
    CHECK(MovePathManager::numPaths == 2);
    CHECK(MovePathManager::sourceTally[16] == 0);
    CHECK(MovePathManager::sourceTally[17] == 1);
    CHECK(MovePathManager::sourceTally[18] == 1);
    queue.remove(&second);
}

TEST_CASE("PathWorkProduction/empty and zero expansion requests reset each update") {
    MovementWorld world;
    auto& queue = *world.queue;
    const auto frame = MovePathManager_getFrameCounter();
    const auto before = mc2::path_work::expansions;
    queue.update();
    CHECK(MovePathManager_getFrameCounter() == frame + 1);
    std::array<MovingPilot, 9> pilots;
    for (auto& pilot : pilots) queue.request(&pilot, 0, MOVEPARAM_INIT, 16);
    CHECK(MovePathManager::numPaths == 9);
    queue.update(); // null vehicles cost zero expansions; all nine can drain
    CHECK(MovePathManager::numPaths == 0);
    CHECK(queue.queueFront == nullptr);
    CHECK(queue.queueEnd == nullptr);
    CHECK(mc2::path_work::expansions == before);
    queue.request(&pilots[0], 0, MOVEPARAM_INIT, 16);
    queue.update();
    CHECK(MovePathManager::numPaths == 0);
    CHECK(MovePathManager_getFrameCounter() == frame + 3);
}
