#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "mission.h"
#include "mover.h"
#include "heap.h"
#include "mission_init_frame.h"
#include "mech.h"
#include "movemgr.h"
#include "team.h"

#include <cstdlib>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include "abl.h"
#include "ablscan.h"
#include <io.h>
#include <stdexcept>
#include <cstring>

bool missionFitOpordApplies(const MissionFitBrain&, unsigned long, bool);
extern bool InitWayPath;

namespace {
struct CompileFixture {
    std::map<std::string, std::string> sources;
    int openFiles = 0;
    long (*previousRemaining)(void*) = ABLFile::remainingCB;
    struct Reader { std::string text; size_t offset = 0; };
    static CompileFixture* current;
    static long open(void** file, const char* name) {
        auto found = current->sources.find(name);
        if (found == current->sources.end()) return -1;
        *file = new Reader{found->second};
        ++current->openFiles;
        return 0;
    }
    static long close(void** file) {
        delete static_cast<Reader*>(*file);
        *file = nullptr;
        --current->openFiles;
        return 0;
    }
    static bool eof(void* file) {
        const auto& reader = *static_cast<Reader*>(file);
        return reader.offset == reader.text.size();
    }
    static long read(void* file, unsigned char* buffer, long length) {
        auto& reader = *static_cast<Reader*>(file);
        long count = 0;
        while (count < length && reader.offset < reader.text.size())
            buffer[count++] = reader.text[reader.offset++];
        return count;
    }
    static long remaining(void* file) {
        const auto& reader = *static_cast<Reader*>(file);
        return static_cast<long>(reader.text.size() - reader.offset);
    }
    CompileFixture() {
        current = this;
        ABLi_init(65536, 20480, 20, 100, nullptr, open, close, eof,
            read, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            nullptr, [](long, const char* message) { throw std::runtime_error(message); });
        ABLFile::remainingCB = remaining;
        extern char* codeBuffer;
        std::memset(codeBuffer, 0xa5, 20480);
    }
    ~CompileFixture() {
        ABLi_close();
        ABLFile::remainingCB = previousRemaining;
        current = nullptr;
    }
};
CompileFixture* CompileFixture::current = nullptr;

std::string compileBytes(const CompileFixture& fixture, const Routine& routine) {
    // Symbol addresses vary between processes; retain their names in the byte control.
    std::string result;
    constexpr char hex[] = "0123456789abcdef";
    for (long i = 0; i < routine.codeSegmentSize;) {
        SymTableNodePtr symbol = nullptr;
        if (i + sizeof(symbol) <= routine.codeSegmentSize) {
            std::memcpy(&symbol, routine.codeSegment + i, sizeof(symbol));
            if (!ABL_ownsBlock(symbol)) symbol = nullptr;
        }
        if (symbol) {
            result += "<" + std::string(symbol->name) + ">";
            i += sizeof(symbol);
        } else {
            const auto byte = static_cast<unsigned char>(routine.codeSegment[i++]);
            result += hex[byte >> 4]; result += hex[byte & 15];
        }
    }
    return result;
}

struct CompileLog {
    FILE* file = nullptr;
    int saved = -1;
    CompileLog() {
        const auto path = std::filesystem::temp_directory_path() / "abl-r1-compile-stderr.txt";
        file = std::fopen(path.string().c_str(), "w+b");
        if (!file) throw std::runtime_error("cannot capture compiler stderr");
        std::fflush(stderr);
        saved = _dup(_fileno(stderr));
        _dup2(_fileno(file), _fileno(stderr));
    }
    ~CompileLog() {
        std::fflush(stderr);
        _dup2(saved, _fileno(stderr));
        _close(saved);
        std::fclose(file);
    }
    std::string text() {
        std::fflush(stderr);
        std::rewind(file);
        std::string result;
        for (int ch; (ch = std::fgetc(file)) != EOF;) result += static_cast<char>(ch);
        return result;
    }
};
}

TEST_CASE("ABLCompile/syntax rejection rolls back before the next compile") {
    extern int32_t NumModulesRegistered;
    extern ModuleEntryPtr ModuleRegistry;
    extern long eternalOffset;
    extern TypePtr IntegerTypePtr;
    extern SymTableNodePtr SymTableDisplay[MAX_NESTING_LEVEL];
    extern bool EnterStateSymbol;
    extern bool Crunch;
    const std::string valid = "module retry;\nvar\neternal integer retained;\ncode\nendmodule.\n";
    struct Failure { const char* name; std::string source; const char* error; bool library = false; };
    const Failure failures[] = {
        {"undefined expression", valid.substr(0, valid.find("code")) + "code\nundefined = ;\nendmodule\n", "Undefined identifier"},
        {"failed state handle argument", "module retry;\ncode\ngetstatehandle(undefined);\nendmodule.\n", "Undefined identifier"},
        {"failed suppressed print argument", "module retry;\ncode\nprint(undefined);\nendmodule.\n", "Undefined identifier"},
        {"failed eternal array allocation", "module retry;\nvar\neternal integer[3] retained;\ncode\nundefined = ;\nendmodule.\n", "Undefined identifier"},
        {"missing period after bytecode allocation", valid.substr(0, valid.size() - 2), "Missing period"},
        {"unterminated comment", "/* unterminated", "Unexpected end-of-file"},
        {"unterminated disabled debug block", "#debug_start\nunterminated\n", "Unexpected end-of-file"},
        {"missing header", "code\nendmodule.\n", "Missing module"},
        {"failed first library", "library retry;\ncode\nundefined = ;\nendlibrary.\n", "Undefined identifier", true},
        {"failed include", "#include \"include.abl\"\n", "Missing module"},
    };
    for (const auto& failure : failures) SUBCASE(failure.name) {
    CompileFixture fixture;
    fixture.sources["bad.abl"] = failure.source;
    fixture.sources["include.abl"] = "code\nendmodule.\n";
    fixture.sources["valid.abl"] = valid;
    const auto blockCount = ABL_liveBlocks();
    const auto typeCount = IntegerTypePtr->numInstances;
    const auto root = SymTableDisplay[0];
    CompileLog log;
    long errors = 0;
    if (failure.library)
        CHECK(ABLi_loadLibrary("bad.abl", &errors) == nullptr);
    else
        CHECK(ABLi_preProcess("bad.abl", &errors) < 0);
    CHECK(errors == 1);
    CHECK(fixture.openFiles == 0);
    CHECK(ABL_liveBlocks() == blockCount);
    CHECK(NumModulesRegistered == 0);
    CHECK(eternalOffset == 0);
    CHECK(IntegerTypePtr->numInstances == typeCount);
    CHECK(SymTableDisplay[0] == root);
    CHECK_FALSE(EnterStateSymbol);
    CHECK(Crunch);
    const auto message = log.text();
    CHECK(std::count(message.begin(), message.end(), '\n') == 1);
    CHECK(message.find(failure.name == std::string("failed include") ? "include.abl [line " : "bad.abl [line ") != std::string::npos);
    CHECK(message.find(failure.error) != std::string::npos);
    REQUIRE(ABLi_preProcess("valid.abl", &errors) == 0);
    CHECK(errors == 0);
    CHECK(IntegerTypePtr->numInstances == typeCount + 1);
    CHECK(searchSymTable("retry", SymTableDisplay[0]) == ModuleRegistry[0].moduleIdPtr);
    CHECK(log.text() == message);
    }
}

TEST_CASE("ABLArena/the environment owns its blocks: zero-filled, freed one by one or all at close") {
    {
        CompileFixture fixture;
        const auto atInit = ABL_liveBlocks();
        CHECK(atInit > 0); // code buffer, size lists, registries
        auto* stackBlock = static_cast<unsigned char*>(ABL_alloc(ABLPool::Stack, 64));
        auto* symbolBlock = static_cast<unsigned char*>(ABL_alloc(ABLPool::Symbol, 32));
        CHECK(std::all_of(stackBlock, stackBlock + 64, [](unsigned char b) { return b == 0; }));
        CHECK(ABL_ownsBlock(stackBlock));
        CHECK(ABL_ownsBlock(symbolBlock));
        CHECK(ABL_liveBlocks() == atInit + 2);
        std::memset(stackBlock, 0xcd, 64);
        ABL_free(ABLPool::Stack, stackBlock);
        CHECK_FALSE(ABL_ownsBlock(stackBlock));
        auto* again = static_cast<unsigned char*>(ABL_alloc(ABLPool::Stack, 64));
        CHECK(std::all_of(again, again + 64, [](unsigned char b) { return b == 0; }));
        ABL_free(ABLPool::Stack, nullptr);
        // symbolBlock and again are never freed one by one: closing the environment hands them back.
    }
    CHECK(ABL_liveBlocks() == 0);
    CHECK_FALSE(ABL_ownsBlock(nullptr));
}

TEST_CASE("ABLCompile/statements includes and library calls preserve parent bytecode after rejection") {
    extern ModuleEntryPtr ModuleRegistry;
    CompileFixture fixture;
    fixture.sources["helpers.abx"] = "library helpers;\nfunction bump: integer;\ncode\nreturn(7);\nendfunction;\ncode\nendlibrary.\n";
    fixture.sources["vars.abi"] = "var\neternal integer retained;\n";
    fixture.sources["valid.abl"] = "module retry;\n#include \"vars.abi\"\ncode\nretained = helpers.bump;\nretained = retained + 1;\nprint(\"ordinary text\");\nendmodule.\n";
    REQUIRE(ABLi_loadLibrary("helpers.abx") != nullptr);
    SUBCASE("clean compile") {}
    SUBCASE("undefined state handle does not turn a later string into a state") {
        fixture.sources["bad.abl"] = "module broken;\ncode\ngetstatehandle(undefined);\nendmodule.\n";
        CompileLog log;
        CHECK(ABLi_preProcess("bad.abl") < 0);
    }
    SUBCASE("malformed suppressed print does not suppress the next module") {
        fixture.sources["bad.abl"] = "module broken;\ncode\nprint(undefined);\nendmodule.\n";
        CompileLog log;
        CHECK(ABLi_preProcess("bad.abl") < 0);
    }
    REQUIRE(ABLi_preProcess("valid.abl") == 1);
    const auto& routine = ModuleRegistry[1].moduleIdPtr->defn.info.routine;
    CHECK(searchSymTableForState("ordinary text", routine.localSymTable) == nullptr);
    constexpr auto expectedBytesFromD419c25bb =
        "1a4a01<retained>0a01<bump>0e4a01<retained>0a01<retained>0902<1>0e2b12a5";
    CHECK(compileBytes(fixture, routine) == expectedBytesFromD419c25bb);
}

namespace {
// Supply world resources without replacing production constructors or order handling.
struct Actor : Mover {
    void attach(MechWarrior& warrior) { pilot = &warrior; commanderId = 7; partId = 2; watchID = 1; setExists(true); }
};
struct Pilot : MechWarrior {
    bool received() const { return newTacOrderReceived[ORDERSTATE_GENERAL]; }
};
struct RouteMech : BattleMech {
    void attach(MechWarrior& warrior) { pilot = &warrior; watchID = 1; setExists(true); }
};
struct RouteActors {
    struct Watches : GameObjectManager {
        GameObject* entries[2]{};
        Watches() { watchList = entries; nextWatchID = 2; collisionSystem = nullptr; moverLineOfSightTable = nullptr; }
    } objects;
    Pilot pilot;
    RouteMech mech;
    bool previousInit = InitWayPath;
    RouteActors() {
        ObjectManager = &objects;
        objects.entries[1] = &mech;
        mech.attach(pilot);
        pilot.setVehicle(&mech);
        InitWayPath = true;
    }
    ~RouteActors() { InitWayPath = previousInit; }
    void path(TacticalOrderCode code) {
        auto& order = *pilot.getCurTacOrder();
        order.init(ORDER_ORIGIN_SELF, code, false);
        auto& path = order.moveParams.wayPath;
        path.numPoints = 3;
        for (int i = 0; i < path.numPoints; ++i) {
            path.points[3*i] = 100.0f * (i + 1);
            path.points[3*i+1] = 200.0f;
            path.points[3*i+2] = 0.0f;
            path.mode[i] = TRAVEL_MODE_SLOW;
        }
        pilot.setMoveWayPath(&path, code == TACTICAL_ORDER_PATROL_PATH);
    }
};
struct Access {
    Actor& actor;
    bool& evidence;
    Actor* resolve(mc2::sim::ObjectRef ref) const { return ref == mc2::sim::ObjectRef{1,1} ? &actor : nullptr; }
    float simulationTime() const { return 12.5f; }
    void installed(const mc2::sim::OrderEnvelope&, mc2::sim::ObjectRef) const { evidence = true; }
};
using ProductionAdapter = mc2::sim::MoverOrderApplication<::TacticalOrder, Access>;
// Successful receipts alone cannot prove installation.
struct DisconnectedApplication : mc2::sim::OrderApplication {
    ProductionAdapter& adapter;
    explicit DisconnectedApplication(ProductionAdapter& connected) : adapter(connected) {}
    std::uint32_t validateCommand(const mc2::sim::OrderEnvelope& e) const override { return adapter.validateCommand(e); }
    std::uint32_t validateRecipient(const mc2::sim::OrderEnvelope& e, mc2::sim::ObjectRef ref,
                                   std::uint32_t ordinal) const override { return adapter.validateRecipient(e, ref, ordinal); }
    mc2::sim::ApplicationOutcome applyRecipient(const mc2::sim::OrderEnvelope&, mc2::sim::ObjectRef,
                                               std::uint32_t) override { return {}; }
    mc2::sim::ApplicationOutcome applySupport(const mc2::sim::OrderEnvelope& e) override { return adapter.applySupport(e); }
};
struct SeamWorldResources {
    UserHeap heap;
    UserHeap* previousHeap = systemHeap;
    UserHeap* previousMissionHeap = missionHeap;
    bool previousInitialize = GameObject::initialize;
    unsigned long previousIcon = Mover::holdFireIconHandle;
    GameObjectManager* previousObjects = ObjectManager;
    MechWarrior* previousPilot = MechWarrior::warriorList[7];
    Team* previousTeam = Team::teams[0];
    bool previousBrain = MechWarrior::brainsEnabled[0];
    long previousTeamCount = Team::numTeams;
    SeamWorldResources() {
        heap.init(1024 * 1024, "authored-order-seam");
        systemHeap = &heap;
        missionHeap = &heap;
        GameObject::initialize = true;
        Mover::holdFireIconHandle = 1;
    }
    ~SeamWorldResources() {
        Mover::holdFireIconHandle = previousIcon;
        GameObject::initialize = previousInitialize;
        systemHeap = previousHeap;
        missionHeap = previousMissionHeap;
        ObjectManager = previousObjects;
        MechWarrior::warriorList[7] = previousPilot;
        Team::teams[0] = previousTeam;
        MechWarrior::brainsEnabled[0] = previousBrain;
        Team::numTeams = previousTeamCount;
    }
};
}

TEST_CASE("AuthoredOrderProduction/BattleMech patrol reverses at endpoints (warrior_move.cpp:743-770)") {
    SeamWorldResources resources;
    RouteActors actors;
    actors.path(TACTICAL_ORDER_PATROL_PATH);
    const int expected[] = {1, 2, 1, 0, 1};
    for (const auto index : expected) {
        Stuff::Vector3D next;
        REQUIRE(actors.pilot.getNextWayPoint(next, false));
        const auto cursor = actors.pilot.getMoveOrders()->curWayPt;
        REQUIRE(actors.pilot.getNextWayPoint(next, false));
        CHECK(actors.pilot.getMoveOrders()->curWayPt == cursor);
        REQUIRE(actors.pilot.getNextWayPoint(next, true));
        CHECK(next.x == 100.0f * (index + 1));
        CHECK(next.y == 200.0f);
        CHECK(actors.pilot.getMoveOrders()->curWayPt == index);
        CHECK(actors.pilot.getCurTacOrder()->moveParams.wayPath.curPoint == index);
    }
}

TEST_CASE("AuthoredOrderProduction/BattleMech traverse consumes extra mode sentinel (warrior_move.cpp:732)") {
    SeamWorldResources resources;
    RouteActors actors;
    actors.path(TACTICAL_ORDER_TRAVERSE_PATH);
    auto& path = actors.pilot.getCurTacOrder()->moveParams.wayPath;
    REQUIRE(path.numPoints < MAX_WAYPTS);
    path.mode[path.numPoints] = TRAVEL_MODE_FAST;
    Stuff::Vector3D next;
    for (int index = 1; index < path.numPoints; ++index) {
        REQUIRE(actors.pilot.getNextWayPoint(next, true));
        CHECK(next.x == 100.0f * (index + 1));
        CHECK_FALSE(actors.pilot.getMoveRun());
    }
    const auto last = next;
    CHECK_FALSE(actors.pilot.getNextWayPoint(next, true));
    CHECK(next.x == last.x);
    CHECK(actors.pilot.getMoveRun());
    CHECK(actors.pilot.getMoveOrders()->curWayPt == path.numPoints);
    CHECK(path.curPoint == path.numPoints);
    CHECK(actors.pilot.getCurTacOrder()->stage == 2);
}

TEST_CASE("AuthoredOrderProduction/BattleMech waypoint reset depends on InitWayPath (warrior_move.cpp:186)") {
    SeamWorldResources resources;
    RouteActors actors;
    actors.path(TACTICAL_ORDER_PATROL_PATH);
    Stuff::Vector3D next;
    REQUIRE(actors.pilot.getNextWayPoint(next, true));
    auto& path = actors.pilot.getCurTacOrder()->moveParams.wayPath;
    InitWayPath = false;
    actors.pilot.setMoveWayPath(&path, true);
    CHECK(actors.pilot.getMoveOrders()->curWayPt == 1);
    InitWayPath = true;
    actors.pilot.setMoveWayPath(&path, true);
    CHECK(actors.pilot.getMoveOrders()->curWayPt == 0);
}

TEST_CASE("AuthoredOrderProduction/BattleMech path orders cannot reach the queue (tacordr.cpp:868-884,1263)") {
    SeamWorldResources resources;
    RouteActors actors;
    struct QueueScope {
        MovePathManager* previous = PathManager;
        long paths = MovePathManager::numPaths, peak = MovePathManager::peakPaths;
        std::array<long, 50> tally;
        std::unique_ptr<MovePathManager> manager;
        QueueScope() {
            std::copy_n(MovePathManager::sourceTally, tally.size(), tally.begin());
            manager = std::make_unique<MovePathManager>();
            PathManager = manager.get();
        }
        ~QueueScope() {
            PathManager = previous;
            MovePathManager::numPaths = paths;
            MovePathManager::peakPaths = peak;
            std::copy(tally.begin(), tally.end(), MovePathManager::sourceTally);
        }
    } queue;
    for (const auto code : {TACTICAL_ORDER_TRAVERSE_PATH, TACTICAL_ORDER_PATROL_PATH}) {
        auto& order = *actors.pilot.getCurTacOrder();
        order.init(ORDER_ORIGIN_SELF, code, false);
        order.moveParams.wayPath.numPoints = 2;
        order.setWayPoint(0, Stuff::Vector3D(100.0f, 200.0f, 0.0f));
        order.setWayPoint(1, Stuff::Vector3D(300.0f, 400.0f, 0.0f));
        long message = 0;
        CHECK(order.execute(&actors.pilot, message) == TACORDER_FAILURE);
        CHECK(message == -1);
        CHECK(order.status(&actors.pilot) == TACORDER_SUCCESS);
        auto path = order.moveParams.wayPath;
        CHECK(actors.pilot.orderTraversePath(false, true, ORDER_ORIGIN_SELF, &path, 0) == TACORDER_SUCCESS);
        CHECK(actors.pilot.orderPatrolPath(false, true, ORDER_ORIGIN_SELF, &path) == TACORDER_SUCCESS);
        CHECK(queue.manager->queueFront == nullptr);
        CHECK(actors.pilot.getMovePathRequest() == nullptr);
        CHECK(actors.pilot.getMoveOrders()->numWayPts == 0);
        CHECK(actors.pilot.getMovePath(0)->numStepsWhenNotPaused == 0);
        CHECK(actors.pilot.getMovePath(1)->numStepsWhenNotPaused == 0);
    }
}

TEST_CASE("AuthoredOrderProduction/mission admission resolves recipient and records installation") {
    SeamWorldResources resources;
    Pilot pilot;
    Team team;
    team.id = 0;
    Team::teams[0] = &team;
    MechWarrior::brainsEnabled[0] = true;
    pilot.setTeam(&team);
    Actor actor;
    actor.attach(pilot);
    struct WatchManager : GameObjectManager {
        GameObject* watches[2]{};
        WatchManager() { watchList = watches; nextWatchID = 2; collisionSystem = nullptr; moverLineOfSightTable = nullptr; }
    } objects;
    ObjectManager = &objects;
    objects.watches[1] = &actor;
    pilot.setVehicle(&actor);
    MechWarrior::warriorList[7] = &pilot;
    struct AuthoredMission : Mission {
        using Mission::authoredRoutes;
        using Mission::orderIngress;
        using Mission::rejectedRouteSignals;
        using Mission::nextPartIncarnation;
        using Mission::nextPilotIncarnation;
        using Mission::nextRouteExecution;
        using Mission::nextOrderRevision;
        using Mission::authoredDiagnostics;
        void roster(Part* entries) { parts = entries; numParts = 2; }
        void releaseOrderState() {
            std::destroy_at(&orderFixtureReceipt);
            std::destroy_at(&authoredDiagnostics);
            std::destroy_at(&authoredRoutes);
            std::destroy_at(&routeExecutions);
            std::destroy_at(&rejectedRouteSignals);
            std::destroy_at(&authoredOpordPilots);
            std::destroy_at(&authoredMissionText);
            std::destroy_at(&orderRecipients);
            std::destroy_at(&orderIngress);
        }
    };
    // Mission's destructor owns global renderer/ABL shutdown; this fixture owns only its order members.
    union MissionStorage {
        AuthoredMission owner;
        MissionStorage() : owner() {}
        ~MissionStorage() { owner.releaseOrderState(); }
    } storage;
    auto& owner = storage.owner;
    Part roster[3]{};
    roster[2].objectWID = 1;
    owner.roster(roster);
    owner.resetOrderIngress();
    owner.bindOrderRecipients();
    REQUIRE(owner.orderRecipient(2));
    const auto parsed = mc2::sim::readAuthoredRoute(
        "[Part9]\nl OrderType=2\nl OrderStance=4\nul WaypointCount=1\n"
        "f Waypoint0X=1\nf Waypoint0Y=2\nf Waypoint0Z=3\n", 2, 9, 7, 1000);
    REQUIRE(parsed); REQUIRE(*parsed);
    owner.authoredRoutes.push_back(**parsed);
    owner.admitAuthoredRoutes();
    REQUIRE(owner.authoredRoutes[0].receiptId);
    CHECK(owner.orderIngress.queue().sources().front().sourceId == 1);
    CHECK(owner.orderIngress.queue().sources().front().nextSequence == 1);
    CHECK_FALSE(owner.authoredRoutes[0].installed);
    owner.beginOrderSimulationTick();
    CHECK(owner.authoredRoutes[0].installed);
    CHECK(pilot.received());
    CHECK(pilot.getTacOrder(ORDERSTATE_GENERAL)->code == TACTICAL_ORDER_PATROL_PATH);
    CHECK(owner.authoredRouteEvidence().find("\"original\":2,\"selected\":9,\"pilot\":7,\"stance\":4") != std::string::npos);
    owner.completeOrderSimulationTick();
    CHECK(owner.orderIngress.queue().receipts().empty());
    CHECK(owner.authoredRoutes[0].installed);
    using namespace mc2::sim;
    REQUIRE(owner.authoredExecutions().size() == 1);
    auto previous = owner.authoredExecutions().front();
    CHECK(previous.originalPart == 2); CHECK(previous.selectedPart == 9); CHECK(previous.pilot == 7);
    CHECK(previous.path == (**parsed).path); CHECK(previous.stance == 4);
    CHECK(previous.state == RouteState::Pending); CHECK(previous.eventSequence == 0);
    auto input = RouteSignal{previous.key, previous.revision, 1, 1, RouteAction::Ready, true, true, true, previous.pilotIncarnation};
    SUBCASE("pilot replacement interrupts the old execution on its next signal") {
        Pilot replacement;
        actor.attach(replacement);
        const auto event = owner.onRouteEvent(input);
        REQUIRE(event); CHECK(event->event == RouteEventKind::Interrupted);
        CHECK(event->reason == RouteReason::Removed);
        CHECK(owner.routeExecution(previous.key)->state == RouteState::Interrupted);
    }
    SUBCASE("resetting mission records reproduces execution identities") {
        const auto oldSession = owner.orderSession();
        owner.resetOrderIngress(); owner.bindOrderRecipients();
        owner.authoredRoutes.push_back(**parsed); owner.admitAuthoredRoutes();
        owner.beginOrderSimulationTick(); owner.completeOrderSimulationTick();
        REQUIRE(owner.authoredExecutions().size() == 1);
        const auto& current = owner.authoredExecutions().front();
        CHECK(current.key == previous.key); CHECK(current.revision == previous.revision);
        CHECK(current.pilotIncarnation == previous.pilotIncarnation);
        CHECK(owner.orderSession() != oldSession);
        CHECK(owner.orderRecipient(2)->generation == 1);
    }
    SUBCASE("rejections retain only the last signal for an endless patrol") {
        REQUIRE(owner.onRouteEvent(input));
        for (int i = 0; i < 100; ++i) {
            input.action = RouteAction::Ready; ++input.sequence;
            CHECK_FALSE(owner.onRouteEvent(input));
            CHECK(owner.rejectedRouteSignals.size() == 1);
            CHECK(owner.rejectedRouteSignals.front().sequence == input.sequence);
            input.action = RouteAction::Arrived;
            REQUIRE(owner.onRouteEvent(input));
        }
    }
    SUBCASE("destroy discards live routes and rejections before a mission reload") {
        REQUIRE(owner.onRouteEvent(input));
        CHECK_FALSE(owner.onRouteEvent(input));
        ObjectManager = nullptr; MechWarrior::warriorList[7] = nullptr; missionHeap = nullptr;
        Team::teams[0] = nullptr;
        owner.destroy(false);
        Team::teams[0] = &team;
        ObjectManager = &objects; MechWarrior::warriorList[7] = &pilot; missionHeap = &resources.heap;
        CHECK(owner.authoredExecutions().empty()); CHECK(owner.rejectedRouteSignals.empty());
        CHECK(owner.authoredRoutes.empty()); CHECK_FALSE(owner.orderRecipient(2));
        CHECK_FALSE(owner.onRouteEvent(input));
        owner.bindOrderRecipients();
        CHECK(owner.authoredExecutions().empty());
        CHECK(owner.nextRouteExecution == 1); CHECK(owner.nextOrderRevision == 1);
        CHECK(owner.nextPartIncarnation == 2); CHECK(owner.nextPilotIncarnation == 2);
    }
    SUBCASE("revision and terminal rejection preserve execution ownership") {
        REQUIRE(owner.onRouteEvent(input));
        CHECK_FALSE(owner.onRouteEvent(input)); CHECK_FALSE(owner.onRouteEvent(input));
        CHECK(owner.rejectedRouteSignals.size() == 1); CHECK(owner.routeExecution(previous.key)->eventSequence == 1);
        input.sequence = 2; input.action = RouteAction::Alarm;
        REQUIRE(owner.onRouteEvent(input));
        input.sequence = 3; input.action = RouteAction::AlarmCleared; --input.revision;
        CHECK_FALSE(owner.onRouteEvent(input));
        CHECK_FALSE(owner.onRouteEvent(input)); CHECK(owner.rejectedRouteSignals.size() == 1);
        ++input.revision; input.segmentReachable = false;
        REQUIRE(owner.onRouteEvent(input));
        CHECK(owner.routeExecution(previous.key)->state == RouteState::Active);
        input.sequence = 4; input.action = RouteAction::Alarm;
        auto event = owner.onRouteEvent(input);
        REQUIRE(event); CHECK(event->event == RouteEventKind::Suspended);
        input.sequence = 5; input.action = RouteAction::AlarmCleared; input.segmentReachable = true;
        REQUIRE(owner.onRouteEvent(input));
        const auto sequence = owner.routeExecution(previous.key)->eventSequence;
        ::TacticalOrder replacement;
        replacement.init(ORDER_ORIGIN_SELF, TACTICAL_ORDER_STOP, false);
        auto* savedMission = mission;
        mission = &owner;
        pilot.setGeneralTacOrder(replacement);
        mission = savedMission;
        CHECK(pilot.getTacOrder(ORDERSTATE_GENERAL)->code == TACTICAL_ORDER_PATROL_PATH);
        CHECK(owner.routeExecution(previous.key)->eventSequence == sequence);
        input.sequence = 6; input.action = RouteAction::AlarmCleared;
        CHECK_FALSE(owner.onRouteEvent(input)); CHECK_FALSE(owner.onRouteEvent(input));
        CHECK(owner.rejectedRouteSignals.size() == 1);
        CHECK(owner.routeExecution(previous.key)->eventSequence == 5);
    }
    SUBCASE("identity exhaustion rejects once without unwinding queue application") {
        for (const auto counter : {&owner.nextPartIncarnation, &owner.nextPilotIncarnation,
                                  &owner.nextRouteExecution, &owner.nextOrderRevision}) {
            owner.resetOrderIngress();
            owner.authoredRoutes.push_back(**parsed);
            *counter = std::numeric_limits<std::uint64_t>::max();
            CHECK_NOTHROW(owner.bindOrderRecipients());
            CHECK_NOTHROW(owner.admitAuthoredRoutes());
            CHECK_NOTHROW(owner.beginOrderSimulationTick());
            CHECK_NOTHROW(owner.completeOrderSimulationTick());
            CHECK(owner.authoredExecutions().empty());
            CHECK(owner.authoredRoutes[0].diagnosed);
            REQUIRE(owner.authoredDiagnostics.size() == 1);
            CHECK(owner.authoredDiagnostics[0].find("identity") != std::string::npos);
            CHECK(owner.orderIngress.queue().receipts().empty());
        }
    }

}

TEST_CASE("AuthoredOrderProduction/OPORD eligibility matches the consumer skip rules") {
    SeamWorldResources resources;
    Pilot pilot;
    MechWarrior::warriorList[7] = &pilot;
    MissionFitBrain brain{};
    brain.warriorIndex = 7;
    strcpy_s(brain.compatibilityMode, "Enhanced");
    strcpy_s(brain.secondary.type, "Guard");
    strcpy_s(brain.primary.type, "Guard");
    CHECK(missionFitOpordApplies(brain, 7, true));
    CHECK_FALSE(missionFitOpordApplies(brain, 7, false));
    CHECK_FALSE(missionFitOpordApplies(brain, 6, true));
    for (const auto index : {0, -1, MAX_WARRIORS}) {
        brain.warriorIndex = index;
        CHECK_FALSE(missionFitOpordApplies(brain, MAX_WARRIORS, true));
    }
    brain.warriorIndex = 7;
    for (const auto type : {"", "Unknown", "PlayerControlled"}) {
        strcpy_s(brain.primary.type, type);
        CHECK_FALSE(missionFitOpordApplies(brain, 7, true));
    }
    strcpy_s(brain.primary.type, "Guard");
    strcpy_s(brain.archetype, "Archetype.PlayerControlled");
    CHECK_FALSE(missionFitOpordApplies(brain, 7, true));
    brain.archetype[0] = 0;
    MechWarrior::warriorList[7] = nullptr;
    CHECK_FALSE(missionFitOpordApplies(brain, 7, true));
}

TEST_CASE("AuthoredOrderProduction/semicolon scalar comments agree with FitIniFile") {
    SeamWorldResources resources;
    std::string text = "FITini\n[Part9]\n; l OrderType=1\nl OrderType=2; patrol\n"
        "l OrderStance=4 ; stance\nul WaypointCount=1; count\n"
        "f Waypoint0X=1.25; x\nf Waypoint0Y=-2.5 ; y\nf Waypoint0Z=3; z\nFITend\n";
    FitIniFile fit;
    REQUIRE(fit.open(text.data(), static_cast<int>(text.size())) == NO_ERR);
    REQUIRE(fit.seekBlock("Part9") == NO_ERR);
    long type = 0, stance = 0; unsigned long count = 0;
    float x = 0, y = 0, z = 0;
    REQUIRE(fit.readIdLong("OrderType", type) == NO_ERR);
    REQUIRE(fit.readIdLong("OrderStance", stance) == NO_ERR);
    REQUIRE(fit.readIdULong("WaypointCount", count) == NO_ERR);
    REQUIRE(fit.readIdFloat("Waypoint0X", x) == NO_ERR);
    REQUIRE(fit.readIdFloat("Waypoint0Y", y) == NO_ERR);
    REQUIRE(fit.readIdFloat("Waypoint0Z", z) == NO_ERR);
    const auto parsed = mc2::sim::readAuthoredRoute(text, 2, 9, 7, 1000);
    REQUIRE(parsed); REQUIRE(*parsed);
    CHECK(type == 2); CHECK((**parsed).kind == mc2::sim::OrderKind::PatrolPath);
    CHECK((**parsed).stance == stance); CHECK((**parsed).path.points.size() == count);
    CHECK((**parsed).path.points[0] == mc2::save::Vec3{x,y,z});
}

TEST_CASE("AuthoredOrderProduction/read-only stock and mod mission Part census") {
    SeamWorldResources resources;
    namespace fs = std::filesystem;
    const fs::path root = "A:/Games/mc2-opengl";
    REQUIRE(fs::is_directory(root));
    struct Counts { unsigned files = 0, parts = 0, routes = 0, diagnostics = 0, unreadable = 0; };
    std::map<std::string, Counts> counts;
    std::set<std::string> stockMissions;
    const std::regex part(R"((?:^|\n)[ \t]*\[Part([0-9]+)\])", std::regex::icase);
    const std::regex stockName(R"(mc2_(0[1-9]|1[0-9]|2[0-4])\.fit)", std::regex::icase);
    const auto normalize = [](std::string name) {
        std::replace(name.begin(), name.end(), '\\', '/');
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return name;
    };
    const auto inspect = [&](const std::string& group, const std::string& name, const std::string& text) {
        auto& total = counts[group];
        ++total.files;
        for (auto it = std::sregex_iterator(text.begin(), text.end(), part); it != std::sregex_iterator(); ++it) {
            const auto id = static_cast<std::uint32_t>(std::stoul((*it)[1].str()));
            const auto start = static_cast<std::size_t>(it->position());
            const auto end = text.find("\n[", start + 1);
            const auto parsed = mc2::sim::readAuthoredRoute(std::string_view(text).substr(start, end == text.npos ? end : end - start),
                id, id, 0, std::numeric_limits<float>::max());
            ++total.parts;
            if (!parsed) ++total.diagnostics;
            else if (*parsed && (**parsed).kind != mc2::sim::OrderKind::None) ++total.routes;
        }
        if (group == "stock") stockMissions.insert(fs::path(name).filename().string());
    };
    for (const auto& entry : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied)) {
        if (!entry.is_regular_file()) continue;
        const auto relative = normalize(entry.path().lexically_relative(root).generic_string());
        const auto installation = *fs::path(relative).begin();
        const auto mod = relative.find("/mods/");
        const auto group = mod == std::string::npos ? installation.string() : relative.substr(0, relative.find('/', mod + 6));
        const auto groupFor = [&](const std::string& name) {
            return installation.string().starts_with("mc2-win64-") && mod == std::string::npos &&
                std::regex_match(fs::path(name).filename().string(), stockName) ? std::string("stock") : group;
        };
        const auto extension = normalize(entry.path().extension().string());
        if (extension == ".fit" && relative.find("/missions/") != std::string::npos) {
            std::ifstream file(entry.path(), std::ios::binary);
            if (!file) { ++counts[groupFor(relative)].unreadable; continue; }
            inspect(groupFor(relative), relative, {std::istreambuf_iterator<char>(file), {}});
        } else if (extension == ".fst") {
            FastFile archive;
            if (archive.open(entry.path().string().c_str()) != NO_ERR) { ++counts[group].unreadable; continue; }
            for (long i = 0; i < archive.getNumFiles(); ++i) {
                const auto& info = *archive.getFilesInfo()[i].pfe;
                const auto name = normalize(info.name);
                if (!name.ends_with(".fit") || name.find("missions/") == std::string::npos) continue;
                std::string text(info.realSize, '\0');
                const auto handle = archive.openFast(info.hash, info.name);
                if (handle < 0 || archive.readFast(handle, text.data(), info.realSize) != static_cast<long>(info.realSize))
                    ++counts[groupFor(name)].unreadable;
                else inspect(groupFor(name), name, text);
                if (handle >= 0) archive.closeFast(handle);
            }
        }
    }
    for (const auto& [group, total] : counts)
        std::cout << "authored census " << group << " files=" << total.files << " parts=" << total.parts
                  << " routes=" << total.routes << " diagnostics=" << total.diagnostics << " unreadable=" << total.unreadable << '\n';
    CHECK(stockMissions.size() == 24);
    CHECK(counts["stock"].parts > 0);
    CHECK(counts["stock"].routes == 0);
    CHECK(counts["stock"].diagnostics == 0);
    CHECK(counts["stock"].unreadable == 0);
}

TEST_CASE("AuthoredOrderProduction/parsed patrol and traverse reach real general setter") {
    using namespace mc2::sim;
    SeamWorldResources resources;
    for (const auto editorId : {1, 2}) {
        for (const auto count : {1, 8}) {
            Pilot pilot;
            Actor actor;
            actor.attach(pilot);
            bool installed = false;
            ProductionAdapter adapter{Access{actor, installed}};
            DisconnectedApplication disconnected{adapter};
            MissionOrderIngress ingress;
            std::string text = "[Part9]\nl OrderType=" + std::to_string(editorId) +
                "\nl OrderStance=4\nul WaypointCount=" + std::to_string(count) + "\n";
            for (int i = 0; i < count; ++i) {
                const auto key = "f Waypoint" + std::to_string(i);
                text += key + "X=" + std::to_string(i + 1) + "\n" + key + "Y=-2\n" + key + "Z=3\n";
            }
            const auto parsed = readAuthoredRoute(text, 2, 9, 7, 1000);
            REQUIRE(parsed);
            REQUIRE(*parsed);
            const auto& route = **parsed;
            OrderEnvelope e;
            e.sourceId = 1; e.issuer = CommanderId{7}; e.recipients = {{1,1}};
            e.payload.kind = route.kind; e.payload.origin = OrderOrigin::Commander;
            e.payload.move.emplace(); e.payload.move->wayPath = route.path;
            // Distinct values expose incomplete conversion/copying.
            for (int i = 0; i < count; ++i)
                e.payload.move->wayPath.mode[i] = i % 2 ? TravelMode::Fast : TravelMode::Slow;
            REQUIRE(ingress.submit(ingress.session(), e, adapter));
            auto& application = std::getenv("MC2_SEAM_DISCONNECT_ADAPTER")
                ? static_cast<OrderApplication&>(disconnected) : static_cast<OrderApplication&>(adapter);
            REQUIRE(ingress.beginSimulationTick(true, false, false, application));
            REQUIRE(ingress.completeSimulationTick());
            REQUIRE(ingress.queue().receipts().size() == 1);
            CHECK(ingress.queue().receipts().front().applicationStatus == ApplicationStatus::Installed);
            CHECK(installed);
            CHECK(pilot.received());
            const auto* stored = pilot.getTacOrder(ORDERSTATE_GENERAL);
            CHECK(stored->code == (editorId == 1 ? TACTICAL_ORDER_TRAVERSE_PATH : TACTICAL_ORDER_PATROL_PATH));
            CHECK(stored->origin == ORDER_ORIGIN_COMMANDER);
            CHECK(stored->time == 12.5f);
            CHECK(stored->moveParams.faceObject == false);
            CHECK(stored->moveParams.keepMoving == false);
            REQUIRE(stored->moveParams.wayPath.numPoints == count);
            CHECK(stored->moveParams.wayPath.curPoint == 0);
            for (int i = 0; i < count; ++i) {
                CHECK(stored->moveParams.wayPath.points[3*i] == i + 1);
                CHECK(stored->moveParams.wayPath.points[3*i+1] == -2);
                CHECK(stored->moveParams.wayPath.points[3*i+2] == 3);
                CHECK(stored->moveParams.wayPath.mode[i] == (i % 2 ? TRAVEL_MODE_FAST : TRAVEL_MODE_SLOW));
            }
        }
    }
}
