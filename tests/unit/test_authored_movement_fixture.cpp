#include "doctest.h"
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
#include <chrono>
#include <fstream>

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
    aFont font;
    aFont* previous = CObjective::s_markerFont;
    ObjectiveObservationFixture() { CObjective::s_markerFont = &font; }
    ~ObjectiveObservationFixture() { CObjective::s_markerFont = previous; }
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
        UserHeap heap;
        heap.init(1024 * 1024, "objective-observation");
        auto* previousHeap = systemHeap;
        systemHeap = &heap;
        auto* previousTeam = Team::teams[0];
        const auto previousTeamCount = Team::numTeams;
        const auto previousTurn = turn;
        turn = 40;
        struct Restore {
            UserHeap* heap;
            Team* team;
            long count, frame;
            GameSoundSystem* sound;
            bool audio;
            LogisticsData* logistics;
            std::string path = missionPath;
            std::filesystem::path cwd = std::filesystem::current_path();
            ~Restore() {
                systemHeap = heap; Team::teams[0] = team; Team::numTeams = count; turn = frame;
                soundSystem = sound; useSound = audio;
                LogisticsData::instance = logistics;
                std::strcpy(missionPath, path.c_str());
                std::filesystem::current_path(cwd);
            }
        } restore{previousHeap, previousTeam, previousTeamCount, previousTurn, soundSystem, useSound,
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
    UserHeap heap;
    SavedGlobals saved;
    std::unique_ptr<Terrain> terrain;
    std::unique_ptr<FlatMesh> mesh;
    std::unique_ptr<MissionMap> cells;
    std::array<std::unique_ptr<MoveMap>, 2> localMaps;
    std::unique_ptr<GlobalMap> areas;
    std::unique_ptr<TriggerAreaManager> triggers;
    std::unique_ptr<Watches> objects;
    std::unique_ptr<MovingPilot> pilot;
    std::unique_ptr<MovingMech> mech;
    std::unique_ptr<MovePathManager> queue;

    MovementWorld() {
        REQUIRE(Terrain::terrainHeap == nullptr);
        REQUIRE(Terrain::terrainTextures == nullptr);
        REQUIRE(Terrain::terrainTextures2 == nullptr);
        REQUIRE(Terrain::objBlockInfo == nullptr);
        saved.keep(systemHeap); saved.keep(missionHeap); saved.keep(land);
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
        saved.keep(Terrain::terrainHeap); saved.keep(Terrain::mapData);
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
        saved.keep(ClearBridgeTiles); saved.keep(GoalIsDoor); saved.keep(JumpOnBlocked);
        saved.keep(DebugMovePathType); saved.keep(RamObjectWID); saved.keep(LastMoveCalcErr);
        saved.keep(numNodesVisited); saved.keep(topOpenNodes);
        saved.keep(goalMap); saved.keep(goalMapRowStart); saved.keep(goalMapRowCol);
        heap.init(16 * 1024 * 1024, "offline-movement");
        systemHeap = missionHeap = &heap;
        GameObject::initialize = true;
        Mover::holdFireIconHandle = 1;
        scenarioTime = 1.0f;
        terrain = std::make_unique<Terrain>();
        land = terrain.get();
        Terrain::realVerticesMapSide = 60;
        Terrain::halfVerticesMapSide = 30;
        Terrain::blocksMapSide = 3;
        Terrain::worldUnitsMapSide = 60 * Terrain::worldUnitsPerVertex;
        Terrain::mapTopLeft3d = Stuff::Vector3D(-Terrain::worldUnitsMapSide / 2, Terrain::worldUnitsMapSide / 2, 0);
        Terrain::terrainHeap = new UserHeap;
        Terrain::terrainHeap->init(1024 * 1024, "offline-terrain");
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
        Terrain::objBlockInfo = static_cast<ObjBlockInfo*>(Terrain::terrainHeap->Malloc(9 * sizeof(ObjBlockInfo)));
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
    Stuff::Vector3D point(int col) const {
        Stuff::Vector3D result;
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
        for (int i = 0; i < 2; ++i) localMaps[i].reset(PathFindMap[i]);
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
    Stuff::Vector3D goal;
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
std::array<const void*, 19> bindings() {
    return {systemHeap, missionHeap, land, GameMap, GlobalMoveMap[0], GlobalMoveMap[1], GlobalMoveMap[2],
        PathFindMap[0], PathFindMap[1], ObjectManager, PathManager,
        Terrain::mapData, Terrain::terrainHeap, Terrain::tileRowToWorldCoord, Terrain::tileColToWorldCoord,
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
        Stuff::Vector3D query;
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
            Stuff::Vector3D next;
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
        Stuff::Vector3D point;
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
