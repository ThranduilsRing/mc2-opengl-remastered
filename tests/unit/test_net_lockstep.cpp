// Trigger paths: code/net/, code/tactical_order_queue.*, code/mission_update.cpp.
#include "doctest.h"
#include "net/lockstep.h"
using namespace mc2;
namespace {
void deliver(net::Lockstep& from, net::Lockstep& to) {
    for (auto& packet : from.takeOutgoing()) to.receive(packet);
}
sim::OrderEnvelope command(std::uint32_t unit) {
    sim::OrderEnvelope e;
    e.issuer = sim::CommanderId{0};
    e.recipients = {{unit, 1}};
    e.payload.origin = sim::OrderOrigin::Player;
    e.payload.kind = sim::OrderKind::Stop;
    return e;
}
}
TEST_CASE("net lockstep needs both markers and previous matching hashes, delay is three") {
    net::Lockstep host(0), client(1);
    CHECK(host.queue(command(1))); CHECK(client.queue(command(2)));
    host.seal(); client.seal();
    CHECK_FALSE(host.queue(command(1))); // a sealed batch cannot acquire late inputs
    CHECK_FALSE(host.ready());
    deliver(host, client); deliver(client, host);
    for (sim::Tick tick = 1; tick <= 4; ++tick) {
        REQUIRE(host.ready()); REQUIRE(client.ready());
        const auto a = host.beginTick(), b = client.beginTick();
        CHECK(a == b);
        if (tick < 4) CHECK(a.empty());
        else {
            REQUIRE(a.size() == 2);
            CHECK(a[0].applyTick == 4); CHECK(a[0].sourceId == 100); CHECK(a[1].sourceId == 101);
        }
        host.finishTick({tick, 2, 3, 4, 5});
        CHECK_FALSE(host.ready());
        client.finishTick({tick, 2, 3, 4, 5});
        deliver(host, client); deliver(client, host);
        host.seal(); client.seal(); deliver(host, client); deliver(client, host);
    }
}
TEST_CASE("net lockstep delayed peer remains identical for 18000 ticks and catches first divergent tick") {
    net::Lockstep host(0), client(1);
    deliver(host, client); deliver(client, host);
    for (sim::Tick tick = 1; tick <= 18000; ++tick) {
        if (tick % 60 == 1) { REQUIRE(host.queue(command(1))); REQUIRE(client.queue(command(2))); }
        host.seal(); client.seal();
        // A transport shim holds one side's markers until after the other is serviced.
        auto delayed = client.takeOutgoing();
        deliver(host, client);
        for (auto& packet : delayed) host.receive(packet);
        REQUIRE(host.ready()); REQUIRE(client.ready());
        CHECK(host.beginTick() == client.beginTick());
        host.finishTick({tick, tick * 2, 3, 4, 5});
        deliver(host, client); // remote hash can arrive before local completion
        client.finishTick({tick, tick * 2, 3, 4, tick == 18000 ? 6u : 5u});
        CHECK_FALSE(host.ready());
        deliver(client, host);
        if (tick < 18000) { REQUIRE(host.error().empty()); REQUIRE(client.error().empty()); }
    }
    REQUIRE(host.divergence()); REQUIRE(client.divergence());
    CHECK(host.divergence()->tick == 18000); CHECK(client.divergence()->tick == 18000);
    CHECK(host.divergence()->local == client.divergence()->remote);
    CHECK_FALSE(host.ready()); CHECK_FALSE(client.ready());
}
TEST_CASE("net lockstep rejects forged source, duplicate batches, future markers and truncated packets") {
    net::Lockstep host(0), client(1);
    auto packets = host.takeOutgoing();
    client.receive(packets[0]); client.receive(packets[0]);
    CHECK_FALSE(client.error().empty());
    net::Lockstep reordered(1);
    reordered.receive(packets[1]); CHECK_FALSE(reordered.error().empty());
    net::Lockstep forged(1);
    auto packet = packets[0]; packet[4] = 6;
    forged.receive(packet); CHECK_FALSE(forged.error().empty());
    for (std::size_t n = 0; n < packets[0].size(); ++n) {
        net::Lockstep truncated(1);
        truncated.receive(std::span(packets[0]).first(n));
        CHECK_FALSE(truncated.error().empty());
    }
    net::Lockstep sender(0), receiver(1);
    deliver(sender, receiver); REQUIRE(sender.queue(command(1))); sender.seal();
    packet = sender.takeOutgoing().front();
    packet[24] = 101; // OrderEnvelope.sourceId, after kind/tick/count/applyTick
    receiver.receive(packet); CHECK(receiver.error() == "MP forged order source, tick or sequence");
}
TEST_CASE("net lockstep partitions stock and larger rosters, additions stay with host") {
    CHECK(net::moverOwner(0, 3) == 0); CHECK(net::moverOwner(1, 3) == 1); CHECK(net::moverOwner(2, 3) == 1);
    CHECK(net::moverOwner(1, 4) == 1); CHECK(net::moverOwner(2, 4) == 1);
    CHECK(net::moverOwner(9, 3, false) == 0);
}
TEST_CASE("net lockstep UI during a running step enters the following delayed batch") {
    net::Lockstep host(0), client(1);
    host.seal(); client.seal(); deliver(host, client); deliver(client, host);
    REQUIRE(host.ready()); host.beginTick(); client.beginTick();
    REQUIRE(host.queue(command(1)));
    host.finishTick({}); client.finishTick({});
    deliver(host, client); deliver(client, host);
    host.seal(); client.seal(); deliver(host, client); deliver(client, host);
    for (sim::Tick tick = 2; tick <= 5; ++tick) {
        REQUIRE(host.ready()); REQUIRE(client.ready());
        auto a = host.beginTick(); CHECK(a == client.beginTick());
        if (tick < 5) CHECK(a.empty());
        else { REQUIRE(a.size() == 1); CHECK(a[0].applyTick == 5); }
        host.finishTick({}); client.finishTick({});
        deliver(host, client); deliver(client, host);
        host.seal(); client.seal(); deliver(host, client); deliver(client, host);
    }
}
TEST_CASE("net lockstep timeout or disconnect stops without releasing an unagreed tick") {
    for (const auto reason : {"MP peer disconnected", "MP peer timed out"}) {
        net::Lockstep host(0), client(1);
        deliver(host, client); deliver(client, host);
        REQUIRE(host.ready());
        host.stop(reason);
        CHECK_FALSE(host.ready()); CHECK(host.completed() == 0); CHECK(host.error() == reason);
    }
}
