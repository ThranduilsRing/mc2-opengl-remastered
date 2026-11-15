// Trigger paths: code/net/, code/tactical_order_queue.*, code/mission_update.cpp.
#include "doctest.h"
#include "net/session.h"
using namespace mc2;
auto settled(net::LinkSetup& host, net::LinkSetup& client, std::uint32_t now) {
    CHECK_FALSE(host.begin(now)); CHECK_FALSE(client.begin(now));
    for (unsigned i = 1; i <= 30; ++i) {
        auto h = host.frame(now + i), c = client.frame(now + i);
        if (h) client.receive(*h, now + i);
        if (c) host.receive(*c, now + i);
    }
    client.frame(now + 31);
    return host.frame(now + 31);
}

TEST_CASE("net session handshake accepts only identical protocol/executable/content/mission/seed") {
    net::Identity id;
    id.mission = "mc2_01";
    id.seed = 12345;
    id.executable.fill(0x12); id.content.fill(0x34);
    const auto packet = net::hello(id);
    CHECK(net::checkHello(packet, id).empty());
    auto remote = id;
    remote.executable[0] ^= 1;
    CHECK(net::checkHello(net::hello(remote), id) == "executable SHA-256 mismatch");
    remote = id; remote.content[0] ^= 1;
    CHECK(net::checkHello(net::hello(remote), id) == "content SHA-256 mismatch");
    remote = id; remote.mission = "mc2_10";
    CHECK(net::checkHello(net::hello(remote), id) == "mission mismatch");
    remote = id; ++remote.seed;
    CHECK(net::checkHello(net::hello(remote), id) == "seed mismatch");
    auto wrong = packet; wrong[4] = net::protocolVersion + 1;
    CHECK(net::checkHello(wrong, id) == "protocol version mismatch");
    CHECK(packet[0] == 'M'); CHECK(packet[1] == '2');
    CHECK(packet[packet.size() - 4] == 0x39); // fixed-width LE uint32 seed
}
TEST_CASE("net session handshake rejects truncation, excessive lengths and trailing bytes") {
    const net::Identity id{{}, {}, "mc2_01", 0};
    auto packet = net::hello(id);
    for (std::size_t n = 0; n < packet.size(); ++n)
        CHECK_FALSE(net::checkHello(std::span(packet).first(n), id).empty());
    packet.push_back(0);
    CHECK_FALSE(net::checkHello(packet, id).empty());
    packet = net::hello(id); packet[72] = 255;
    CHECK_FALSE(net::checkHello(packet, id).empty());
    auto invalid = id; invalid.mission = std::string(129, 'x');
    CHECK_THROWS(net::hello(invalid));
}

TEST_CASE("net setup derives bounded delay from reliable RTT and jitter before releasing either peer") {
    for (const auto latency : {0u, 1u, 20u, 80u, 300u}) {
        net::LinkSetup host, client;
        host.hosting = true;
        auto now = 0xfffffff0u; // ENet's millisecond clock wraps during a live session.
        auto message = settled(host, client, now);
        now += 31;
        for (unsigned sample = 0; sample < 4; ++sample) {
            REQUIRE(message);
            CHECK(message->kind == 3);
            auto reply = client.receive(*message, now + latency);
            REQUIRE(reply);
            CHECK(host.phase != net::LinkSetup::Phase::Ready);
            now += latency * 2 + sample;
            message = host.receive(*reply, now);
        }
        REQUIRE(message);
        CHECK(message->kind == 5);
        CHECK(host.phase == net::LinkSetup::Phase::Measuring);
        CHECK(client.phase == net::LinkSetup::Phase::Measuring);
        CHECK(host.rtt == latency * 2);
        CHECK(host.jitter == 1);
        CHECK(host.delay == net::delayForRtt(host.rtt, host.jitter));
        auto ack = client.receive(*message, now);
        REQUIRE(ack);
        CHECK(client.delay == host.delay);
        CHECK(client.phase == net::LinkSetup::Phase::Ready);
        CHECK_FALSE(host.receive(*ack, now));
        CHECK(host.phase == net::LinkSetup::Phase::Ready);
    }
    CHECK(net::delayForRtt(0, 0) == 3);
    CHECK(net::delayForRtt(100, 0) == 5);
    CHECK(net::delayForRtt(0xffffffffu, 0xffffffffu) == 12);
}
TEST_CASE("net setup refuses premature or malformed timing agreement and retains first stop reason") {
    net::LinkSetup client;
    client.begin(0);
    CHECK_FALSE(client.receive({5, 3}, 1));
    CHECK(client.phase == net::LinkSetup::Phase::Stopped);
    CHECK(client.error == "MP invalid timing negotiation");
    client.stop("later disconnect");
    CHECK(client.error == "MP invalid timing negotiation");
    net::LinkSetup host;
    host.hosting = true;
    host.begin(10);
    CHECK_FALSE(host.receive({4, 9}, 11));
    CHECK(host.phase == net::LinkSetup::Phase::Stopped);
}
TEST_CASE("net timeout bounds permit transient packet loss but end an unresponsive session") {
    CHECK(net::progressTimeout(false, 59999) == nullptr);
    REQUIRE(net::progressTimeout(false, 60000));
    CHECK(std::string_view(net::progressTimeout(false, 60000)) == "MP handshake timeout");
    CHECK(net::progressTimeout(true, 14999) == nullptr);
    REQUIRE(net::progressTimeout(true, 15000));
    net::Session unopened;
    const save::Bytes packet{1};
    CHECK_NOTHROW(unopened.send(packet));
    CHECK(unopened.error() == "MP session is not connected");
    CHECK_FALSE(unopened.connected());
}

TEST_CASE("net content identity includes effective mounted art options") {
    const save::Digest files{};
    const auto high = net::contentIdentity(files, false, true);
    CHECK(high == net::contentIdentity(files, false, true));
    CHECK(high != net::contentIdentity(files, true, true));
    CHECK(high != net::contentIdentity(files, false, false));
}

TEST_CASE("net timing ignores a transient post-load frame stall") {
    net::LinkSetup host;
    host.hosting = true;
    net::LinkSetup client;
    auto packet = settled(host, client, 0);
    unsigned now = 31;
    for (const auto duration : {860u, 12u, 11u, 12u}) {
        REQUIRE(packet);
        now += duration;
        packet = host.receive({4, packet->value}, now);
    }
    CHECK(host.rtt == 11);
    CHECK(host.jitter == 1);
    CHECK(host.delay == 3);
}

TEST_CASE("net timing waits for thirty consecutive sub-50ms frames on both peers") {
    net::LinkSetup host;
    host.hosting = true;
    CHECK_FALSE(host.begin(0));
    for (unsigned i = 1; i <= 40; ++i) CHECK_FALSE(host.frame(i * 100));
    for (unsigned i = 1; i < 30; ++i) CHECK_FALSE(host.frame(4000 + i));
    auto ready = host.frame(4030);
    REQUIRE(ready); CHECK(ready->kind == 7);
    CHECK_FALSE(host.frame(4031));
    CHECK_FALSE(host.receive({7, 0}, 9000));
    auto probe = host.frame(9001);
    REQUIRE(probe); CHECK(probe->kind == 3); CHECK(probe->value == 9001);
}
