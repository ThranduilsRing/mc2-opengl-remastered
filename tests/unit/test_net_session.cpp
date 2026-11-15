// Trigger paths: code/net/, code/tactical_order_queue.*, code/mission_update.cpp.
#include "doctest.h"
#include "net/session.h"
using namespace mc2;

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
    auto wrong = packet; wrong[4] = 2;
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
