#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "mission.h"
#include "mover.h"
#include "heap.h"
#include "mission_init_frame.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <set>

bool missionFitOpordApplies(const MissionFitBrain&, unsigned long, bool);

namespace {
// Only world resources are absent. Constructors, conversion, handler and setter are production.
struct Actor : Mover {
    void attach(MechWarrior& warrior) { pilot = &warrior; commanderId = 7; partId = 2; watchID = 1; setExists(true); }
};
struct Pilot : MechWarrior {
    bool received() const { return newTacOrderReceived[ORDERSTATE_GENERAL]; }
};
struct Access {
    Actor& actor;
    bool& evidence;
    Actor* resolve(mc2::sim::ObjectRef ref) const { return ref == mc2::sim::ObjectRef{1,1} ? &actor : nullptr; }
    float simulationTime() const { return 12.5f; }
    void installed(const mc2::sim::OrderEnvelope&, mc2::sim::ObjectRef) const { evidence = true; }
};
using ProductionAdapter = mc2::sim::MoverOrderApplication<::TacticalOrder, Access>;
// This control preserves real admission and tick consumption while removing the installation bridge.
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
    SeamWorldResources() {
        heap.init(1024 * 1024, "authored-order-seam");
        systemHeap = &heap;
        missionHeap = &heap;
        GameObject::initialize = true;
        Mover::holdFireIconHandle = 1;
    }
    ~SeamWorldResources() {
        delete Mover::sortList;
        Mover::sortList = nullptr;
        Mover::holdFireIconHandle = previousIcon;
        GameObject::initialize = previousInitialize;
        systemHeap = previousHeap;
        missionHeap = previousMissionHeap;
        ObjectManager = previousObjects;
        MechWarrior::warriorList[7] = previousPilot;
    }
};
}

TEST_CASE("AuthoredOrderProduction/mission admission resolves recipient and records installation") {
    SeamWorldResources resources;
    Pilot pilot;
    Actor actor;
    actor.attach(pilot);
    struct WatchManager : GameObjectManager {
        GameObject* watches[2]{};
        WatchManager() { watchList = watches; nextWatchID = 2; collisionSystem = nullptr; moverLineOfSightTable = nullptr; }
    } objects;
    ObjectManager = &objects;
    objects.watches[1] = &actor;
    MechWarrior::warriorList[7] = &pilot;
    struct AuthoredMission : Mission {
        using Mission::authoredRoutes;
        using Mission::orderIngress;
        void roster(Part* entries) { parts = entries; numParts = 2; }
        void releaseOrderState() {
            std::destroy_at(&orderFixtureReceipt);
            std::destroy_at(&authoredDiagnostics);
            std::destroy_at(&authoredRoutes);
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
            // Distinct modes and coordinates expose prefix-only conversion or setter copies.
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
