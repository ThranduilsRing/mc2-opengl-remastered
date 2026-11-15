// Trigger paths: code/net/, code/tactical_order_queue.*, code/mission_update.cpp.
#include "doctest.h"
#include "net/lockstep.h"
#include "net/runtime.h"
#include "sim_tick_clock.h"
#include <fstream>
#include <sstream>
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
    CHECK(host.queue(command(1))); // late UI input waits for the next batch
    CHECK_FALSE(host.ready());
    deliver(host, client); deliver(client, host);
    for (sim::Tick tick = 1; tick <= 5; ++tick) {
        REQUIRE(host.ready()); REQUIRE(client.ready());
        const auto a = host.beginTick(), b = client.beginTick();
        CHECK(a == b);
        if (tick < 4) CHECK(a.empty());
        else if (tick == 4) {
            REQUIRE(a.size() == 2);
            CHECK(a[0].applyTick == 4); CHECK(a[0].sourceId == 100); CHECK(a[1].sourceId == 101);
        }
        else { REQUIRE(a.size() == 1); CHECK(a[0].applyTick == 5); CHECK(a[0].sequence == 1); }
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
TEST_CASE("net lockstep negotiated delays survive loss and jitter without skipping an input") {
    for (const auto delay : {3u, 5u, 12u}) {
        net::Lockstep host(0, delay), client(1, delay);
        REQUIRE(host.queue(command(1)));
        host.seal(); client.seal();
        auto held = host.takeOutgoing();
        deliver(client, host);
        CHECK_FALSE(client.ready()); // Lost reliable data is still pending retransmission.
        CHECK(client.completed() == 0);
        for (const auto& packet : held) client.receive(packet);
        for (sim::Tick tick = 1; tick <= delay + 1; ++tick) {
            REQUIRE(host.ready()); REQUIRE(client.ready());
            const auto a = host.beginTick(); CHECK(a == client.beginTick());
            CHECK(a.size() == (tick == delay + 1 ? 1 : 0));
            host.finishTick({tick}); client.finishTick({tick});
            auto jittered = client.takeOutgoing();
            deliver(host, client);
            CHECK_FALSE(host.ready()); // No tick escapes while its remote hash is delayed.
            for (const auto& packet : jittered) host.receive(packet);
            host.seal(); client.seal(); deliver(host, client); deliver(client, host);
        }
    }
    CHECK_THROWS(net::Lockstep(0, 2)); CHECK_THROWS(net::Lockstep(1, 13));
}
TEST_CASE("net lockstep ready markers cannot create wall clock credit") {
    for (const auto fps : {30, 60, 144, 240}) {
        net::Lockstep host(0), client(1);
        sim::TickClock clock;
        deliver(host, client); deliver(client, host);
        for (int frame = 0; frame < fps * 10; ++frame) {
            host.seal(); client.seal(); deliver(host, client); deliver(client, host);
            const auto due = clock.advance(1.0 / fps, 1.0);
            for (int tick = 0; tick < due; ++tick) {
                REQUIRE(host.ready()); REQUIRE(client.ready());
                CHECK(host.beginTick() == client.beginTick());
                host.finishTick({}); client.finishTick({});
                deliver(host, client); deliver(client, host);
                host.seal(); client.seal(); deliver(host, client); deliver(client, host);
            }
        }
        CHECK(host.completed() == 300); CHECK(client.completed() == 300);
    }
}
TEST_CASE("net scripted player orders use the mission submit callback including rejection") {
    unsigned called = 0;
    auto submit = [&](sim::OrderEnvelope order) { ++called; return order.recipients == command(1).recipients; };
    CHECK(net::submitScriptOrder(command(1), submit));
    CHECK_FALSE(net::submitScriptOrder(command(2), submit));
    CHECK(called == 2);
    CHECK_FALSE(net::submitScriptOrder(command(1), {}));
}
TEST_CASE("net soak3 recorded stream isolates live Permission and rejection preserves subsequent admissions") {
    struct Admission : sim::OrderAdmissionValidator {
        bool allowed = true;
        bool permitted(const sim::OrderSource&, const sim::OrderEnvelope&) const override { return allowed; }
    } admission;
    struct Apply : sim::OrderApplication {
        std::uint32_t validateCommand(const sim::OrderEnvelope&) const override { return 0; }
        std::uint32_t validateRecipient(const sim::OrderEnvelope&, sim::ObjectRef, std::uint32_t) const override { return 0; }
        sim::ApplicationOutcome applyRecipient(const sim::OrderEnvelope&, sim::ObjectRef, std::uint32_t) override { return {}; }
        sim::ApplicationOutcome applySupport(const sim::OrderEnvelope&) override { return {}; }
    } apply;
    std::ifstream file(std::filesystem::path(__FILE__).parent_path() / "fixtures/mp_soak3_orders.txt");
    REQUIRE(file.good());
    std::vector<sim::OrderEnvelope> stream;
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream row(line);
        std::string marker, hex; sim::Tick tick;
        REQUIRE(bool(row >> marker >> tick >> hex));
        save::Bytes bytes;
        for (std::size_t i = 0; i < hex.size(); i += 2)
            bytes.push_back(static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
        sim::OrderEnvelope order;
        REQUIRE(save::decodeRecord(bytes, order)); REQUIRE(order.applyTick == tick);
        stream.push_back(order);
    }
    REQUIRE(stream.size() == 15);
    REQUIRE(stream.back().applyTick == 2104); REQUIRE(stream.back().sequence == 7);
    for (const bool reject : {false, true}) {
        sim::TacticalOrderQueue queue;
        REQUIRE(queue.registerSource({100, sim::SourceKind::Peer}));
        REQUIRE(queue.registerSource({101, sim::SourceKind::Peer}));
        for (sim::Tick tick = 1; tick <= 2104; ++tick) {
            for (const auto& order : stream) if (order.applyTick == tick) {
                admission.allowed = !(reject && tick == 2104);
                const auto result = queue.submit(order, admission);
                if (!admission.allowed) { REQUIRE_FALSE(result); CHECK(result.error() == sim::OrderError::Permission); }
                else REQUIRE(result); // Excludes Late/Source/Sequence/Shape/Capacity for the recorded stream.
            }
            REQUIRE(queue.beginTick(tick));
            for (const auto& receipt : queue.receipts()) REQUIRE(queue.apply(receipt.receiptId, apply));
            REQUIRE(queue.endTick());
            for (const auto& receipt : queue.receipts()) REQUIRE(queue.acknowledge(receipt.receiptId));
            queue.retireAcknowledged(queue.nextReceiptId() - 1);
        }
        admission.allowed = false;
        auto next = stream.back(); next.applyTick = 2105; next.sequence = 8;
        const auto denied = net::admitAgreedOrder(queue, next, admission);
        REQUIRE_FALSE(denied); CHECK(denied.error() == sim::OrderError::Permission);
        admission.allowed = true; next.applyTick = 2106; next.sequence = 9;
        REQUIRE(net::admitAgreedOrder(queue, next, admission));
        save::Archive snapshot; queue.serialize(snapshot); CHECK(snapshot.good());
    }
}
