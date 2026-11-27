// Trigger paths: code/net/, code/tactical_order_queue.*, code/mission_update.cpp.
#include "doctest.h"
#include "net/session.h"
#include "net/runtime.h"
#include "../../mclib/vfs.h"
#include <enet/enet.h>
#include "../../mclib/file.h"
#include <chrono>
#include <thread>
using namespace mc2;
namespace mc2::net {
void testLobbyPeerLoss();
void testMissionLifecycle();
void testUserDirectoryLifecycle();
}
TEST_CASE("NetLobby/front-end eligibility rejects campaign logistics") {
    CHECK(net::lobbyFrontEndEligible(true));
    CHECK_FALSE(net::lobbyFrontEndEligible(false));
}
TEST_CASE("NetRuntime/automatic user directory lifecycle") { net::testUserDirectoryLifecycle(); }
TEST_CASE("NetLobby/peer loss after offer restores Host-Join controls") { net::testLobbyPeerLoss(); }
TEST_CASE("NetRuntime/lobby MP teardown SP teardown second MP has no retained mission state") { net::testMissionLifecycle(); }
namespace {
struct RawPeer {
    ENetHost* host = enet_host_create(nullptr, 1, 1, 0, 0);
    ENetPeer* peer = nullptr;
    explicit RawPeer(unsigned port) {
        ENetAddress address{};
        enet_address_set_host_ip(&address, "127.0.0.1");
        address.port = static_cast<enet_uint16>(port);
        if (host) peer = enet_host_connect(host, &address, 1, net::protocolVersion);
    }
    ~RawPeer() { if (host) enet_host_destroy(host); }
    void send(const save::Bytes& bytes) {
        auto* packet = enet_packet_create(bytes.data(), bytes.size(), ENET_PACKET_FLAG_RELIABLE);
        REQUIRE(packet);
        REQUIRE(enet_peer_send(peer, 0, packet) == 0);
        enet_host_flush(host);
    }
};
template<class Predicate, class Step> bool until(Predicate done, Step step, unsigned milliseconds = 1500) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        step();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return done();
}
}

TEST_CASE("net unilateral protocol failure disconnects with reliable traffic outstanding") {
    net::Session host;
    net::Identity identity{{}, {}, "mc2_01", 7};
    host.open(true, "127.0.0.1:23471", identity);
    RawPeer remote(23471);
    REQUIRE(remote.peer);
    bool disconnected = false, sent = false;
    REQUIRE(until([&] { return disconnected; }, [&] {
        ENetEvent event{};
        while (enet_host_service(remote.host, &event, 0) > 0) {
            if (event.type == ENET_EVENT_TYPE_CONNECT && !sent) {
                // The host's reliable hello is outstanding when this malformed hello arrives.
                remote.send({0});
                remote.send(save::Bytes(4096, 1));
                sent = true;
            }
            if (event.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(event.packet);
            if (event.type == ENET_EVENT_TYPE_DISCONNECT) disconnected = true;
        }
    }, 3000));
    CHECK(sent);
    CHECK_FALSE(host.error().empty());
    CHECK(until([&] { return host.transportClosed(); }, [] {}));
}

TEST_CASE("net disconnect deadline resets a peer that never services teardown") {
    net::Session host({50, 200, 80});
    host.open(true, "127.0.0.1:23472", {{}, {}, "mc2_01", 7});
    RawPeer remote(23472);
    REQUIRE(remote.peer);
    bool receivedHello = false;
    REQUIRE(until([&] { return receivedHello; }, [&] {
        ENetEvent event{};
        while (enet_host_service(remote.host, &event, 0) > 0) {
            if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                receivedHello = true;
                enet_packet_destroy(event.packet);
            }
        }
    }));
    host.send({}); // A unilateral local protocol error; remote stops servicing entirely.
    CHECK(host.error() == "MP session is not connected");
    CHECK_FALSE(host.transportClosed());
    CHECK(until([&] { return host.transportClosed(); }, [] {}, 500));
}

TEST_CASE("net transport silence expires independently of live slow mission progress") {
    SUBCASE("silent peer") {
        net::Session host({50, 200, 80});
        host.open(true, "127.0.0.1:23473", {{}, {}, "mc2_01", 7});
        RawPeer remote(23473);
        REQUIRE(remote.peer);
        bool connected = false;
        REQUIRE(until([&] { return connected; }, [&] {
            ENetEvent event{};
            while (enet_host_service(remote.host, &event, 0) > 0) {
                if (event.type == ENET_EVENT_TYPE_CONNECT) connected = true;
                if (event.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(event.packet);
            }
        }));
        REQUIRE(until([&] { return !host.error().empty(); }, [] {}, 1000));
        CHECK(host.error() == "MP peer disconnected");
    }
    SUBCASE("live workers with stalled mission threads") {
        net::Session host({50, 200, 80}), client({50, 200, 80});
        host.open(true, "127.0.0.1:23474", {{}, {}, "mc2_01", 7});
        client.open(false, "127.0.0.1:23474", {{}, {}, "mc2_01", 7});
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(600);
        // No mission progress or pump calls: the workers must still exchange pings.
        REQUIRE(until([&] { return std::chrono::steady_clock::now() >= end; }, [] {}, 1000));
        CHECK(host.error().empty()); CHECK(client.error().empty());
        CHECK(net::progressTimeout(true, 119999) == nullptr);
        CHECK(net::progressTimeout(true, 120000) != nullptr);
    }
}
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
    CHECK(net::checkHello(net::hello(remote), id).find("simulation code version mismatch") != std::string::npos);
    remote = id; remote.content[0] ^= 1;
    CHECK(net::checkHello(net::hello(remote), id) == "content SHA-256 mismatch");
    remote = id; remote.mission = "mc2_10";
    CHECK(net::checkHello(net::hello(remote), id) == "mission mismatch");
    remote = id; ++remote.seed;
    CHECK(net::checkHello(net::hello(remote), id) == "seed mismatch");
    auto wrong = packet; wrong[4] = net::protocolVersion + 1;
    CHECK(net::checkHello(wrong, id) == "protocol version mismatch");
    CHECK(packet[0] == 'M'); CHECK(packet[1] == '2');
    CHECK(packet[packet.size() - 9] == 0x39); // seed precedes sync capability and component count
}

TEST_CASE("net session rejects changed heat profile content without data sync") {
    CHECK(net::protocolVersion == 7);
    net::Identity local, remote;
    local.mission = "mc2_01";
    const save::Bytes heatFit{1, 20, 60, 0};
    auto modifiedHeatFit = heatFit; modifiedHeatFit[1] = 21;
    local.content = save::sha256(heatFit);
    remote = local;
    remote.content = save::sha256(modifiedHeatFit);
    CHECK(remote.content != local.content);
    CHECK(net::checkHello(net::hello(remote), local) == "content SHA-256 mismatch");
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
    // RCA: host finished at 82.6 s; the cold client finished at 208.8 s.
    CHECK(net::progressTimeout(false, 208800 - 82600) == nullptr);
    CHECK(net::progressTimeout(false, 599999) == nullptr);
    REQUIRE(net::progressTimeout(false, 600000));
    CHECK(net::progressTimeout(true, 27000) == nullptr);
    CHECK(net::progressTimeout(true, 119999) == nullptr);
    REQUIRE(net::progressTimeout(true, 120000));
    net::Session unopened;
    const save::Bytes packet{1};
    CHECK_NOTHROW(unopened.send(packet));
    CHECK(unopened.error() == "MP session is not connected");
    CHECK_FALSE(unopened.connected());
}

TEST_CASE("net transport services identity while both mission threads are loading") {
    net::Session host, client;
    net::Identity identity{{}, {}, "mc2_01", 12345};
    host.open(true, "127.0.0.1:23469", identity);
    ++identity.seed;
    client.open(false, "127.0.0.1:23469", identity);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    // Neither peer calls pump: the main threads are occupied by cold loading.
    while (host.error().empty() && client.error().empty() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK((host.error() == "seed mismatch" || client.error() == "seed mismatch"));
}

TEST_CASE("net LOADED barrier holds the fast peer until the cold peer finishes") {
    net::Session host, client;
    const net::Identity identity{{}, {}, "mc2_01", 12345};
    host.open(true, "127.0.0.1:23470", identity);
    client.open(false, "127.0.0.1:23470", identity);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    auto settle = [&] {
        while ((!host.connected() || !client.connected()) && std::chrono::steady_clock::now() < deadline) {
            host.pump(); client.pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    };
    settle();
    REQUIRE(host.connected()); REQUIRE(client.connected());
    host.missionLoading(true); client.missionLoading(true);
    host.missionLoading(false);
    // The host is loaded; the client main thread is still inside its loader.
    for (unsigned i = 0; i < 20; ++i) {
        host.pump();
        CHECK_FALSE(host.connected()); CHECK_FALSE(client.connected());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    client.missionLoading(false);
    settle();
    CHECK(host.connected()); CHECK(client.connected());
    CHECK(host.error().empty()); CHECK(client.error().empty());
}

TEST_CASE("net LOADED marker precedes immediate mission input") {
    net::Session host, client;
    const net::Identity identity{{}, {}, "mc2_01", 12345};
    host.open(true, "127.0.0.1:23475", identity);
    client.open(false, "127.0.0.1:23475", identity);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while ((!host.connected() || !client.connected()) && std::chrono::steady_clock::now() < deadline) {
        host.pump(); client.pump();
    }
    REQUIRE(host.connected()); REQUIRE(client.connected());
    for (unsigned round = 0; round < 25; ++round) {
        auto& last = round % 2 ? client : host;
        auto& first = round % 2 ? host : client;
        host.missionLoading(true); client.missionLoading(true);
        first.missionLoading(false);
        deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
        while (std::chrono::steady_clock::now() < deadline) {
            host.pump(); client.pump();
        }
        last.missionLoading(false);
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!last.connected() && std::chrono::steady_clock::now() < deadline) last.pump();
        REQUIRE(last.connected());
        const save::Bytes payload{0x52, 0x4e, 0x44, static_cast<std::uint8_t>(round)};
        last.send(payload);
        bool received = false;
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!received && std::chrono::steady_clock::now() < deadline) {
            for (const auto& packet : first.pump()) {
                REQUIRE(packet == payload);
                CHECK(first.connected());
                received = true;
            }
        }
        REQUIRE(received);
        CHECK(host.error().empty()); CHECK(client.error().empty());
    }
}

TEST_CASE("net content identity changes with effective simulation bytes") {
    const net::SimFiles files{{"data/objects/a.fit", {1}}};
    auto changed = files;
    changed.begin()->second[0] = 2;
    CHECK(net::simDataHash(files) != net::simDataHash(changed));
}

TEST_CASE("net simulation manifest excludes cosmetics and rejects unsafe names") {
    for (const auto* path : {"data/missions/mc2_01.fit", "data/missions/warriors/a.abl",
            "data/objects/compbas.csv", "data/defs/rules/heat.fit", "data/defs/units/core/a/chassisdef.fit",
            "data/defs/chassis/pack/a/chassisdef.fit", "data/tgl/madcat.ini", "data/tgl/madcat.tgl",
            "data/tgl/madcat.agl", "data/tgl/madcat.ase", "data/tgl/madcat.glb", "data/tgl/madcat.fbx",
            "data/tgl/madcat.bin", "data/tgl/madcat.package.json", "data/anim_overrides/anims.json"})
        CHECK(net::simDataPath(path));
    for (const auto* path : {"data/textures/hd.fit", "data/tgl/tree.fit", "data/tgl/madcat.tga",
            "data/tgl/madcat.ktx2", "data/tgl/madcat.png", "data/defs/ui/theme.json",
            "mods.cfg", "options.cfg", "data/objects/../../x.csv", "data/objects/C:x.csv",
            "data/objects/x.csv\n", "Data/objects/a.csv"}) CHECK_FALSE(net::simDataPath(path));
    CHECK(net::simCodeHash() != save::Digest{});
}

TEST_CASE("net simulation transfer syncs changed missing and empty data and hides local extras") {
    const net::SimFiles source{{"data/objects/compbas.csv", save::Bytes(150000, 7)},
        {"data/missions/empty.abl", {}}, {"data/missions/mc2_01.fit", {1, 2}},
        {"data/defs/rules/heat.fit", {3}}, {"data/tgl/madcat.ini", {4}},
        {"data/tgl/madcat.glb", {5}}, {"data/tgl/madcat.package.json", {6}},
        {"data/tgl/madcat.agl", {7}}};
    net::SimFiles installed{{"data/objects/compbas.csv", {9}},
        {"data/missions/mc2_01.fit", {1, 2}}, {"data/missions/local.abl", {99}},
        {"data/tgl/madcat.glb", {55}}, {"data/tgl/client-only.glb", {99}}};
    net::SimTransfer host(true, source), client(false, installed);
    auto packet = host.begin();
    unsigned exchanges = 0;
    while (!host.ready && exchanges++ < 100) {
        REQUIRE_FALSE(packet.empty());
        REQUIRE(packet.size() <= net::maxPacketBytes);
        auto reply = client.receive(packet);
        REQUIRE(client.error.empty());
        packet = host.receive(reply);
        REQUIRE(host.error.empty());
    }
    CHECK(host.ready); CHECK(client.ready);
    CHECK(client.files == source); CHECK(installed.at("data/objects/compbas.csv") == save::Bytes{9});
    CHECK(net::simDataHash(client.files) == net::simDataHash(source));
    CHECK(net::simDataHash(installed) != net::simDataHash(source));
}

TEST_CASE("net simulation transfer rejects corrupt chunks and malformed packets") {
    net::SimTransfer host(true, {{"data/objects/compbas.csv", {1, 2, 3}}}), client(false, {});
    auto request = client.receive(host.begin());
    auto chunk = host.receive(request);
    REQUIRE(chunk.size() > 8);
    chunk.back() ^= 1;
    CHECK(client.receive(chunk).empty());
    CHECK(client.error == "simulation data SHA-256 mismatch");
    net::SimTransfer bad(false, {});
    CHECK(bad.receive({}).empty()); CHECK_FALSE(bad.error.empty());
    CHECK_THROWS(net::SimTransfer(true, {{"data/objects/../escape.csv", {}}}));
}

TEST_CASE("net simulation identical manifests exchange have need in bounded batches") {
    net::SimFiles files;
    for (unsigned i = 0; i < 300; ++i) files.emplace("data/objects/" + std::to_string(i) + ".fit", save::Bytes{1});
    net::SimTransfer host(true, files), client(false, files);
    auto packet = host.begin();
    unsigned exchanges = 0;
    while (!host.ready && exchanges < 10) {
        ++exchanges;
        REQUIRE(packet.size() <= net::maxPacketBytes);
        auto reply = client.receive(packet);
        REQUIRE(client.error.empty());
        packet = host.receive(reply);
        REQUIRE(host.error.empty());
    }
    CHECK(host.ready); CHECK(client.ready);
    CHECK(exchanges == 4); // three batches and the final digest, rather than 301 round trips
    CHECK(client.files == files);
}

TEST_CASE("net simulation match cache takes precedence and is released") {
    struct Cleanup { ~Cleanup() { vfs::clearMatchData(); } } cleanup;
    vfs::setMatchData({{"data/objects/compbas.csv", {42}},
        {"data/defs/chassis/host/a/chassisdef.fit", {43}},
        {"data/tgl/madcat.glb", {44}}, {"data/tgl/madcat.agl", {45}},
        {"data/tgl/madcat.package.json", {46}}}, net::simDataPath);
    CHECK(vfs::matchDataActive());
    for (const auto* path : {"data/tgl/madcat.glb", "data/tgl/madcat.agl", "data/tgl/madcat.package.json"}) {
        File appearance;
        REQUIRE(appearance.open(path) == 0);
        unsigned char value = 0;
        REQUIRE(appearance.read(&value, 1) == 1);
        CHECK(value >= 44); CHECK(value <= 46);
    }
    CHECK_FALSE(fileExists("data/tgl/client-only.glb"));
    const auto chassis = vfs::enumerate("data/defs/chassis", ".fit", true);
    REQUIRE(chassis.size() == 1);
    CHECK(chassis.front().key == "data/defs/chassis/host/a/chassisdef.fit");
    CHECK(chassis.front().mountName == "match-cache");
    const auto weapon = vfs::openRead("DATA\\OBJECTS\\COMPBAS.CSV");
    REQUIRE(weapon.pakBytes);
    CHECK(*weapon.pakBytes == save::Bytes{42});
    CHECK(weapon.layer == "match-cache");
    CHECK(fileExists("data/objects/compbas.csv") != 0);
    CHECK(fileExists("data/objects/client-only.csv") == 0);
    CHECK(vfs::lookup("data/objects/compbas.csv")->mountName == "match-cache");
    CHECK_FALSE(vfs::lookup("data/objects/client-only.csv"));
    CHECK(vfs::contentDigest("data/objects/compbas.csv") != 0);
    CHECK(vfs::contentDigest("data/objects/client-only.csv") == 0);
    File file;
    REQUIRE(file.open("data/objects/compbas.csv") == 0);
    unsigned char byte = 0;
    REQUIRE(file.read(&byte, 1) == 1);
    CHECK(byte == 42);
    CHECK_FALSE(vfs::openRead("data/objects/client-only.csv").found());
    const auto listing = vfs::enumerate("data/objects", ".csv");
    REQUIRE(listing.size() == 1);
    CHECK(listing.front().key == "data/objects/compbas.csv");
    vfs::clearMatchData();
    CHECK_FALSE(vfs::matchDataActive());
    CHECK(vfs::openRead("data/objects/compbas.csv").layer != "match-cache");
}

TEST_CASE("net simulation transfer over loopback completes before transport readiness") {
    net::Session host, client;
    net::Identity a{net::simCodeHash(), {}, "mc2_01", 12345};
    a.syncData = true;
    net::SimFiles source{{"data/objects/compbas.csv", save::Bytes(150000, 42)}};
    a.content = net::simDataHash(source);
    auto b = a; b.content[0] ^= 1;
    host.open(true, "127.0.0.1:23476", a, source);
    client.open(false, "127.0.0.1:23476", b, {{"data/objects/compbas.csv", {9}}});
    REQUIRE(until([&] { return host.dataReady() && client.dataReady(); }, [&] { host.pump(); client.pump(); }));
    CHECK(host.error().empty()); CHECK(client.error().empty());
    CHECK(client.matchFiles() == source);
}

TEST_CASE("net content refusal names differing and missing components with both digests") {
    net::Identity local{{}, {}, "mc2_01", 0};
    local.components = {{"mission.fst", {}}, {"data/missions", {}}};
    auto remote = local;
    remote.content[0] = 1;
    remote.components[0].digest[0] = 0xab;
    remote.components.erase(remote.components.begin() + 1);
    remote.components.push_back({"mods/extra", {}});
    const auto reason = net::checkHello(net::hello(remote), local);
    CHECK(reason.find("mission.fst local=" + net::digestText(local.components[0].digest)) != std::string::npos);
    CHECK(reason.find("peer=ab00") != std::string::npos);
    CHECK(reason.find("data/missions local=") != std::string::npos);
    CHECK(reason.find("peer=missing") != std::string::npos);
    CHECK(reason.find("mods/extra local=missing peer=") != std::string::npos);
    CHECK(net::digestText(local.content).size() == 64);
    remote.components.resize(257);
    CHECK_THROWS(net::hello(remote));
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

TEST_CASE("net lobby releases only two ready players and validates host mission and start authority") {
    net::LobbyState host, client;
    host.hosting = true;
    host.mission = "mc2_10";
    CHECK(host.start().empty());
    CHECK(client.ready(true).empty());
    client.receive(host.offer());
    CHECK(client.mission == "mc2_10");
    client.receive(host.ready(true));
    CHECK(host.start().empty());
    host.receive(client.ready(true));
    CHECK(host.canStart());
    host.receive(client.ready(false));
    CHECK_FALSE(host.canStart());
    host.receive(client.ready(true));
    CHECK(client.start().empty());
    const auto startPacket = host.start();
    client.receive(startPacket);
    CHECK(host.started); CHECK(client.started);
    net::LobbyState premature;
    premature.receive(startPacket);
    CHECK_FALSE(premature.error.empty());
    net::LobbyState invalid;
    host.mission = "../mod";
    invalid.receive(host.offer());
    CHECK_FALSE(invalid.error.empty());
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
