#include "session.h"
#include <enet/enet.h>
#include <charconv>
#include <stdexcept>

namespace mc2::net {
namespace {
constexpr std::uint32_t magic = 0x314e324d; // M2N1, little endian fields
void fields(save::Archive& a, std::uint32_t& tag, std::uint32_t& version, Identity& id) {
    a(tag, version, id.executable, id.content);
    std::vector<std::uint8_t> name(id.mission.begin(), id.mission.end());
    a.sequence(name, 128);
    if (a.reading()) id.mission.assign(name.begin(), name.end());
    a(id.seed);
}
}
save::Bytes hello(const Identity& identity) {
    save::Archive a;
    auto id = identity;
    auto tag = magic, version = protocolVersion;
    fields(a, tag, version, id);
    if (!a.good() || id.mission.empty()) throw std::runtime_error("invalid MP identity");
    return a.bytes();
}
std::string checkHello(std::span<const std::uint8_t> packet, const Identity& local) {
    if (packet.size() > 208) return "handshake size";
    save::Archive a(packet);
    Identity remote;
    std::uint32_t tag = 0, version = 0;
    fields(a, tag, version, remote);
    if (!a.finish() || tag != magic || remote.mission.empty()) return "malformed handshake";
    if (version != protocolVersion) return "protocol version mismatch";
    if (remote.executable != local.executable) return "executable SHA-256 mismatch";
    if (remote.content != local.content) return "content SHA-256 mismatch";
    if (remote.mission != local.mission) return "mission mismatch";
    if (remote.seed != local.seed) return "seed mismatch";
    return {};
}
struct Session::Impl {
    ENetHost* host = nullptr;
    ENetPeer* peer = nullptr;
    Identity identity;
    std::string error;
    bool initialized = false, ready = false;
    ~Impl() {
        if (host) enet_host_destroy(host);
        if (initialized) enet_deinitialize();
    }
    void fail(std::string message) {
        error = std::move(message);
        ready = false;
        if (peer) enet_peer_disconnect_later(peer, 1);
    }
};
Session::Session() : impl_(std::make_unique<Impl>()) {}
Session::~Session() = default;
void Session::open(bool hosting, std::string_view endpoint, Identity identity) {
    if (impl_->host) throw std::runtime_error("MP session already open");
    if (enet_initialize() != 0) throw std::runtime_error("ENet initialization failed");
    impl_->initialized = true;
    impl_->identity = std::move(identity);
    if (endpoint.empty()) endpoint = "127.0.0.1:23460";
    const auto colon = endpoint.find(':');
    const std::string ip(endpoint.substr(0, colon));
    unsigned port = 23460;
    if (colon != std::string_view::npos) {
        auto value = endpoint.substr(colon + 1);
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), port);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) port = 0;
    }
    ENetAddress address{};
    if (!port || port > 65535 || enet_address_set_host_ip(&address, ip.c_str()) != 0 ||
        address.host == ENET_HOST_ANY) throw std::runtime_error("invalid MP endpoint (numeric IP:port required)");
    address.port = static_cast<enet_uint16>(port);
    impl_->host = enet_host_create(hosting ? &address : nullptr, 1, 1, 0, 0);
    if (!impl_->host) throw std::runtime_error("ENet host creation failed");
    impl_->host->maximumPacketSize = maxPacketBytes;
    impl_->host->maximumWaitingData = 2 * maxPacketBytes;
    if (!hosting) {
        impl_->peer = enet_host_connect(impl_->host, &address, 1, protocolVersion);
        if (!impl_->peer) throw std::runtime_error("ENet connect failed");
    }
}
std::vector<save::Bytes> Session::pump() {
    auto& s = *impl_;
    std::vector<save::Bytes> received;
    if (!s.host || !s.error.empty()) return received;
    ENetEvent event{};
    // Nonblocking, bounded work; frames and the owner desktop keep running during a stall.
    for (unsigned count = 0; count < 128; ++count) {
        const auto result = enet_host_service(s.host, &event, 0);
        if (result < 0) { s.fail("ENet service failed"); break; }
        if (!result) break;
        if (event.type == ENET_EVENT_TYPE_CONNECT) {
            s.peer = event.peer;
            enet_peer_timeout(s.peer, 8, 5000, 15000);
            const auto packet = hello(s.identity);
            auto* outgoing = enet_packet_create(packet.data(), packet.size(), ENET_PACKET_FLAG_RELIABLE);
            if (!outgoing) { s.fail("ENet packet allocation failed"); break; }
            if (enet_peer_send(s.peer, 0, outgoing) < 0) {
                enet_packet_destroy(outgoing); s.fail("ENet handshake send failed"); break;
            }
        } else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
            const std::span<const std::uint8_t> packet(event.packet->data, event.packet->dataLength);
            if (!s.ready) {
                auto mismatch = checkHello(packet, s.identity);
                if (!mismatch.empty()) s.fail(std::move(mismatch));
                else s.ready = true;
            } else if (packet.size() > maxPacketBytes) s.fail("MP packet exceeds limit");
            else received.emplace_back(packet.begin(), packet.end());
            enet_packet_destroy(event.packet);
            if (!s.error.empty()) break;
        } else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
            s.fail("MP peer disconnected"); break;
        }
    }
    enet_host_flush(s.host);
    return received;
}
void Session::send(std::span<const std::uint8_t> packet) {
    auto& s = *impl_;
    if (!s.ready || !s.error.empty()) throw std::runtime_error("MP session is not connected");
    if (packet.empty() || packet.size() > maxPacketBytes) throw std::runtime_error("invalid MP packet size");
    auto* outgoing = enet_packet_create(packet.data(), packet.size(), ENET_PACKET_FLAG_RELIABLE);
    if (!outgoing) { s.fail("ENet packet allocation failed"); return; }
    if (enet_peer_send(s.peer, 0, outgoing) < 0) {
        enet_packet_destroy(outgoing); s.fail("ENet send failed");
    }
    enet_host_flush(s.host);
}
bool Session::connected() const { return impl_->ready; }
const std::string& Session::error() const { return impl_->error; }
}
