#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "mission.h"
#include "mover.h"
#include "heap.h"

#include <cstdlib>

namespace {
// Only world resources are absent. Constructors, conversion, handler and setter are production.
struct Actor : Mover {
    void attach(MechWarrior& warrior) { pilot = &warrior; commanderId = 7; setExists(true); }
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
    }
};
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
