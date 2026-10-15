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

#include <array>
#include <cstring>
#include <cmath>
#include <functional>
#include <memory>
#include <type_traits>
#include <vector>

extern long LastMoveCalcErr;

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
    }
};
struct MovingPilot : MechWarrior {
    void deferTactics() { movementUpdate = scenarioTime + 1000.0f; }
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
        saved.keep(ObjectTypeManager::objectCache);
        saved.keep(GameObject::initialize); saved.keep(Mover::holdFireIconHandle);
        saved.keep(Mover::numMovers); saved.keep(MechWarrior::numWarriors);
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
        ObjectTypeManager::objectCache = &heap;
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
        return GameMap && GameMap->map && land && Terrain::mapData && Terrain::mapData->getBlocks() &&
            ObjectManager && ObjectTypeManager::objectCache && pilot->getVehicle() == mech.get() &&
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
std::array<const void*, 20> bindings() {
    return {systemHeap, missionHeap, land, GameMap, GlobalMoveMap[0], GlobalMoveMap[1], GlobalMoveMap[2],
        PathFindMap[0], PathFindMap[1], ObjectManager, PathManager, ObjectTypeManager::objectCache,
        Terrain::mapData, Terrain::terrainHeap, Terrain::tileRowToWorldCoord, Terrain::tileColToWorldCoord,
        Terrain::cellRowToWorldCoord, Terrain::cellColToWorldCoord, Terrain::objBlockInfo, Mover::triggerAreaMgr};
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
