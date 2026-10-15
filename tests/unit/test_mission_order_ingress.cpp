#include "doctest.h"
#include "mission_init_frame.h"
#include "tactical_order_queue.h"

#include <algorithm>

namespace {
using namespace mc2::sim;
// Only the actor endpoint is a fixture; queue, permissions, conversion and tick lifecycle are production.
struct PilotEndpoint {
    ::TacticalOrder orders[2];
    PilotEndpoint() {
        for (auto& order : orders) { order.code = TACTICAL_ORDER_NONE; order.origin = ORDER_ORIGIN_SELF; }
    }
    ::TacticalOrder* getTacOrder(long state) { return &orders[state]; }
};
struct ActorEndpoint {
    PilotEndpoint pilot;
    bool exists = true, disabled = false, mobile = true, suppress = false, corruptMode = false;
    int calls = 0;
    std::vector<float> points;
    std::uint32_t watchId = 41;
    long partId = 1;
    bool getExists() { return exists; }
    bool isDestroyed() { return false; }
    bool isDisabled() { return disabled; }
    bool canMove() { return mobile; }
    PilotEndpoint* getPilot() { return &pilot; }
    long getCommanderId() { return 7; }
    std::uint32_t getWatchID(bool assign) { CHECK_FALSE(assign); return watchId; }
    long getPartId() { return partId; }
    long handleTacticalOrder(const ::TacticalOrder& order, long priority, bool queued) {
        CHECK(priority == 0);
        CHECK_FALSE(queued);
        ++calls;
        if (!suppress) {
            auto& stored = pilot.orders[order.origin == ORDER_ORIGIN_PLAYER ? 1 : 0];
            stored = order;
            if (corruptMode) stored.moveParams.wayPath.mode[0] = TRAVEL_MODE_SLOW;
            if (order.code == TACTICAL_ORDER_MOVETO_POINT) points.push_back(order.moveParams.wayPath.points[0]);
        }
        return NO_ERR;
    }
};
struct ActorAccess {
    ActorEndpoint& actor;
    MissionRecipientBinding<ActorEndpoint, PilotEndpoint>& binding;
    ActorEndpoint* resolve(ObjectRef ref) const {
        return binding.resolve(ref, [&](std::uint32_t) { return &actor; }, actor.watchId);
    }
    float simulationTime() const { return 12.5f; }
};
using ProductionAdapter = MoverOrderApplication<::TacticalOrder, ActorAccess>;
struct LiveIngress {
    MissionOrderIngress ingress;
    ActorEndpoint actor;
    MissionRecipientBinding<ActorEndpoint, PilotEndpoint> binding{{1,1}, 1, 41, &actor, &actor.pilot};
    ProductionAdapter adapter{ActorAccess{actor, binding}};
    void completeOrderSimulationTick() { REQUIRE(ingress.completeSimulationTick()); }
    void frame(bool active = true, bool frozen = false, bool multiplayer = false, bool earlyReturn = false) {
        SimulationOrderCompletion<LiveIngress> completion{*this};
        REQUIRE(ingress.beginSimulationTick(active, frozen, multiplayer, adapter));
        if (earlyReturn) return;
    }
    OrderEnvelope order(std::uint64_t sequence = 0) {
        OrderEnvelope e;
        e.sourceId = 1; e.sequence = sequence;
        e.issuer = CommanderId{7}; e.recipients = {{1,1}};
        e.payload.origin = OrderOrigin::Commander;
        e.payload.kind = OrderKind::MoveToPoint;
        e.payload.move.emplace();
        e.payload.move->wayPath.points = {{static_cast<float>(sequence + 1),2,3}};
        e.payload.move->wayPath.mode = {TravelMode::Fast};
        return e;
    }
    MissionReceipt submit(OrderEnvelope e) {
        auto receipt = ingress.submit(ingress.session(), std::move(e), adapter);
        REQUIRE(receipt);
        return *receipt;
    }
};
}

TEST_SUITE("MissionOrderIngress") {
TEST_CASE("production admission permission malformed payload and typed legacy conversion") {
    LiveIngress live;
    auto wrong = live.order(); wrong.issuer = CommanderId{8};
    CHECK(live.ingress.submit(live.ingress.session(), wrong, live.adapter).error() == OrderError::Permission);
    auto malformed = live.order(); malformed.payload.move->wayPath.mode.clear();
    CHECK(live.ingress.submit(live.ingress.session(), malformed, live.adapter).error() == OrderError::Shape);
    const auto receipt = live.submit(live.order());
    CHECK(live.actor.calls == 0);
    live.frame();
    REQUIRE(live.actor.calls == 1);
    const auto* stored = live.actor.pilot.getTacOrder(ORDERSTATE_GENERAL);
    CHECK(stored->code == TACTICAL_ORDER_MOVETO_POINT);
    CHECK(stored->origin == ORDER_ORIGIN_COMMANDER);
    CHECK(stored->time == 12.5f);
    CHECK(stored->moveParams.wayPath.numPoints == 1);
    CHECK(stored->moveParams.wayPath.points[2] == 3);
    CHECK(stored->moveParams.wayPath.mode[0] == TRAVEL_MODE_FAST);
    CHECK(live.ingress.queue().receipts().front().applicationStatus == ApplicationStatus::Applied);
    CHECK(live.ingress.receipt(receipt)->applicationStatus == ApplicationStatus::Applied);
    REQUIRE(live.ingress.acknowledge(receipt));
    live.ingress.queue().retireAcknowledged(receipt.receiptId);
    CHECK(live.ingress.queue().receipts().empty());
}
TEST_CASE("inactive pause freeze multiplayer contiguous simulation ticks and early return") {
    LiveIngress live;
    live.submit(live.order());
    live.frame(false); live.frame(true, true); live.frame(true, true);
    CHECK(live.ingress.queue().completedTick() == 0);
    CHECK(live.actor.calls == 0);
    live.frame(true, true, true, true);
    CHECK(live.ingress.queue().completedTick() == 1);
    CHECK_FALSE(live.ingress.queue().executingTick());
    CHECK(live.actor.calls == 1);
    live.frame(true, true);
    CHECK(live.ingress.queue().completedTick() == 1);
    live.frame();
    CHECK(live.ingress.queue().completedTick() == 2);
}
TEST_CASE("production adapter installs routes and rejects unsupported modes and silent handler suppression") {
    for (const auto mode : {QueueMode::Append, QueueMode::Prepend}) {
        LiveIngress live;
        auto e = live.order(); e.queueMode = mode;
        live.submit(e); live.frame();
        CHECK(live.actor.calls == 0);
        CHECK(live.ingress.queue().receipts().front().results[0].reasonCode ==
            static_cast<std::uint32_t>(MoverOrderRejection::UnsupportedMode));
    }
    for (const auto kind : {OrderKind::PatrolPath, OrderKind::TraversePath}) {
        LiveIngress live;
        auto e = live.order(); e.payload.kind = kind;
        live.submit(e); live.frame();
        CHECK(live.actor.calls == 1);
        CHECK(live.actor.pilot.getTacOrder(ORDERSTATE_GENERAL)->code == static_cast<int>(kind));
        CHECK(live.ingress.queue().receipts().front().applicationStatus == ApplicationStatus::Installed);
    }
    LiveIngress wrongMode;
    wrongMode.actor.corruptMode = true;
    wrongMode.submit(wrongMode.order()); wrongMode.frame();
    CHECK(wrongMode.actor.calls == 1);
    CHECK(wrongMode.ingress.queue().receipts().front().results[0].reasonCode ==
        static_cast<std::uint32_t>(MoverOrderRejection::HandlerFailure));
    for (const auto origin : {OrderOrigin::Commander, OrderOrigin::Player}) {
        for (const auto kind : {OrderKind::MoveToPoint, OrderKind::Stop}) {
            for (const bool matchingMode : {false, true}) {
                LiveIngress suppressed;
                auto e = suppressed.order(); e.payload.origin = origin; e.payload.kind = kind;
                if (kind == OrderKind::Stop) e.payload.move.reset();
                if (origin == OrderOrigin::Player) {
                    REQUIRE(suppressed.ingress.queue().registerSource({2, SourceKind::UI}));
                    e.sourceId = 2;
                }
                REQUIRE(suppressed.adapter.applyRecipient(e, {1,1}, 0).result == RecipientStatus::Applied);
                auto* stored = suppressed.actor.pilot.getTacOrder(origin == OrderOrigin::Player ? 1 : 0);
                if (!matchingMode) stored->moveParams.wayPath.mode[0] = TRAVEL_MODE_SLOW;
                const auto previousTime = stored->time;
                const auto previousMode = stored->moveParams.wayPath.mode[0];
                suppressed.actor.suppress = true;
                suppressed.submit(e); suppressed.frame();
                CHECK(suppressed.actor.calls == 2);
                const auto& receipt = suppressed.ingress.queue().receipts().front();
                CHECK(receipt.applicationStatus == ApplicationStatus::Rejected);
                CHECK(receipt.results[0].reasonCode == static_cast<std::uint32_t>(MoverOrderRejection::HandlerFailure));
                CHECK(stored->code == (kind == OrderKind::Stop ? TACTICAL_ORDER_STOP : TACTICAL_ORDER_MOVETO_POINT));
                CHECK(stored->time == previousTime);
                CHECK(stored->moveParams.wayPath.mode[0] == previousMode);
            }
        }
    }
}
TEST_CASE("disappeared recipient disabled mover and restarted mission cannot inherit tokens") {
    LiveIngress live;
    const auto oldSession = live.ingress.session();
    const auto receipt = live.submit(live.order());
    live.actor.exists = false; live.frame();
    CHECK(live.actor.calls == 0);
    CHECK(live.ingress.queue().receipts().front().results[0].reasonCode ==
        static_cast<std::uint32_t>(MoverOrderRejection::MissingRecipient));
    live.ingress.reset(); live.actor.exists = true;
    live.binding = {{1,1}, 1, 41, &live.actor, &live.actor.pilot};
    CHECK(live.ingress.session() != oldSession);
    CHECK(live.ingress.acknowledge(receipt).error() == OrderError::UnknownReceipt);
    CHECK(live.ingress.receipt(receipt).error() == OrderError::UnknownReceipt);
    CHECK(live.ingress.submit(oldSession, live.order(), live.adapter).error() == OrderError::Permission);
    live.submit(live.order()); live.actor.mobile = false; live.frame();
    CHECK(live.actor.calls == 0);
    CHECK(live.ingress.queue().receipts().front().results[0].reasonCode ==
        static_cast<std::uint32_t>(MoverOrderRejection::CannotMove));
    live.ingress.close();
    CHECK(live.ingress.session() == 0);
    CHECK(live.ingress.submit(0, live.order(), live.adapter).error() == OrderError::Permission);
    CHECK(live.ingress.queue().sources().empty());
}
TEST_CASE("canonical source order reaches the production conversion") {
    LiveIngress live;
    REQUIRE(live.ingress.queue().registerSource({8, SourceKind::MissionContent}));
    auto first = live.order(); first.sourceId = 8; first.payload.move->wayPath.points[0].x = 8;
    live.submit(first); live.submit(live.order()); live.frame();
    CHECK(live.actor.points == std::vector<float>{1,8});
    CHECK(live.ingress.queue().receipts().size() == 2);
}
TEST_CASE("production binding retires stale mappings even when an actor address is reused") {
    LiveIngress live;
    const auto ref = live.binding.ref;
    REQUIRE(live.adapter.validateRecipient(live.order(), ref, 0) == 0);
    live.actor.watchId = 42;
    CHECK(live.adapter.validateRecipient(live.order(), ref, 0) != 0);
    CHECK(live.binding.retired);
    live.actor.watchId = 41;
    CHECK(live.adapter.validateRecipient(live.order(), ref, 0) != 0);
    LiveIngress differentPart;
    differentPart.actor.partId = 2;
    CHECK(differentPart.adapter.validateRecipient(differentPart.order(), ref, 0) != 0);
    CHECK(differentPart.binding.retired);
}
}
