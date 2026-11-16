#include "session.h"
#include <enet/enet.h>
#include <charconv>
#include <stdexcept>
#include <algorithm>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>

namespace mc2::net {
namespace {
constexpr std::uint32_t magic = 0x314e324d; // M2N1, little endian fields
constexpr std::uint32_t loadedMagic = 0x44414f4c; // LOAD, reserved transport control
void fields(save::Archive& a, std::uint32_t& tag, std::uint32_t& version, Identity& id) {
    a(tag, version, id.executable, id.content);
    std::vector<std::uint8_t> name(id.mission.begin(), id.mission.end());
    a.sequence(name, 128);
    if (a.reading()) id.mission.assign(name.begin(), name.end());
    a(id.seed);
    a.sequence(id.components, 256, [](save::Archive& archive, Identity::Component& component) {
        std::vector<std::uint8_t> name(component.name.begin(), component.name.end());
        archive.sequence(name, 128);
        if (archive.reading()) component.name.assign(name.begin(), name.end());
        archive(component.digest);
    });
}
}
std::string digestText(const save::Digest& digest) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : digest) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}
save::Digest contentIdentity(const save::Digest& files, bool lowCost, bool remasteredBuildings) {
    save::Archive manifest;
    auto digest = files;
    manifest(digest, lowCost, remasteredBuildings);
    return save::sha256(manifest.bytes());
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
    if (packet.size() > 48 * 1024) return "handshake size";
    save::Archive a(packet);
    Identity remote;
    std::uint32_t tag = 0, version = 0;
    fields(a, tag, version, remote);
    if (!a.finish() || tag != magic || remote.mission.empty()) return "malformed handshake";
    if (version != protocolVersion) return "protocol version mismatch";
    if (remote.executable != local.executable) return "executable SHA-256 mismatch";
    if (remote.content != local.content) {
        std::string reason = "content SHA-256 mismatch";
        for (const auto& component : local.components) {
            const auto peer = std::ranges::find(remote.components, component.name, &Identity::Component::name);
            if (peer == remote.components.end() || peer->digest != component.digest)
                reason += "\n" + component.name + " local=" + digestText(component.digest) +
                    " peer=" + (peer == remote.components.end() ? "missing" : digestText(peer->digest));
        }
        for (const auto& component : remote.components)
            if (std::ranges::find(local.components, component.name, &Identity::Component::name) == local.components.end())
                reason += "\n" + component.name + " local=missing peer=" + digestText(component.digest);
        return reason;
    }
    if (remote.mission != local.mission) return "mission mismatch";
    if (remote.seed != local.seed) return "seed mismatch";
    return {};
}
std::uint32_t delayForRtt(std::uint32_t rtt, std::uint32_t jitter) {
    const auto budget = static_cast<std::uint64_t>(rtt) + 2ull * jitter;
    return static_cast<std::uint32_t>(std::clamp<std::uint64_t>((budget * 30 + 999) / 1000 + 2, 3, 12));
}
save::Bytes LobbyState::offer() {
    if (!hosting) return {};
    save::Archive a;
    std::uint32_t kind = 2;
    std::vector<std::uint8_t> name(mission.begin(), mission.end());
    a(kind); a.sequence(name, 32);
    offered = true;
    return a.bytes();
}
save::Bytes LobbyState::ready(bool value) {
    if (!offered || started || !error.empty()) return {};
    localReady = value;
    save::Archive a;
    std::uint32_t kind = 1;
    a(kind, value);
    return a.bytes();
}
save::Bytes LobbyState::start() {
    if (!hosting || !canStart()) return {};
    started = true;
    save::Archive a;
    std::uint32_t kind = 3;
    a(kind);
    return a.bytes();
}
void LobbyState::receive(std::span<const std::uint8_t> packet) {
    if (!error.empty()) return;
    save::Archive a(packet);
    std::uint32_t kind = 0;
    a(kind);
    if (kind == 2 && !hosting && !offered) {
        std::vector<std::uint8_t> name;
        a.sequence(name, 32);
        const std::string selected(name.begin(), name.end());
        if (selected.size() != 6 || selected.substr(0, 4) != "mc2_" ||
            selected.substr(4) < "01" || selected.substr(4) > "24") a.reject("stock mission required");
        if (a.finish()) { mission = selected; offered = true; return; }
    } else if (kind == 1 && offered && !started) {
        bool value = false;
        a(value);
        if (a.finish()) { peerReady = value; return; }
    } else if (kind == 3 && !hosting && canStart()) {
        if (a.finish()) { started = true; return; }
    }
    error = "MP invalid lobby message or premature start";
}
void LinkSetup::stop(std::string_view reason) {
    if (error.empty()) error = reason;
    phase = Phase::Stopped;
}
std::optional<LinkSetup::Control> LinkSetup::begin(std::uint32_t now) {
    if (phase != Phase::Identity) { stop("MP duplicate identity"); return {}; }
    phase = Phase::Settling;
    lastFrame = now;
    return {};
}
std::optional<LinkSetup::Control> LinkSetup::frame(std::uint32_t now) {
    if (phase != Phase::Settling) return {};
    settledFrames = now - lastFrame < 50 ? settledFrames + 1 : 0;
    lastFrame = now;
    if (!localSettled && settledFrames >= 30) {
        localSettled = true;
        return Control{7, 0};
    }
    if (localSettled && peerSettled) {
        phase = Phase::Measuring;
        probe = now;
        if (hosting) return Control{3, probe};
    }
    return {};
}
std::optional<LinkSetup::Control> LinkSetup::receive(Control message, std::uint32_t now) {
    if (phase == Phase::Settling && message.kind == 7 && !peerSettled && message.value == 0) {
        peerSettled = true;
        return {};
    }
    // The host may send its first probe in the same service batch as our ready ACK.
    if (phase == Phase::Settling && !hosting && localSettled && peerSettled && message.kind == 3)
        phase = Phase::Measuring;
    if (phase == Phase::Settling) { stop("MP invalid timing negotiation"); return {}; }
    if (phase != Phase::Measuring) { stop("MP unexpected setup packet"); return {}; }
    if (!hosting && message.kind == 3 && samples < 4) { ++samples; return Control{4, message.value}; }
    if (hosting && message.kind == 4 && samples < 4 && message.value == probe) {
        const auto measured = now - probe;
        if (samples) {
            const auto variation = measured > previous ? measured - previous : previous - measured;
            jitter = samples == 1 ? variation : (std::min)(jitter, variation);
        }
        previous = measured;
        // The minimum pair excludes one-off post-load frame stalls from input latency.
        rtt = samples == 0 ? measured : (std::min)(rtt, measured);
        if (++samples < 4) { probe = now; return Control{3, probe}; }
        delay = delayForRtt(rtt, jitter);
        return Control{5, delay};
    }
    if (!hosting && message.kind == 5 && samples == 4 && message.value >= 3 && message.value <= 12) {
        delay = message.value;
        phase = Phase::Ready;
        return Control{6, delay};
    }
    if (hosting && message.kind == 6 && samples == 4 && message.value == delay) {
        phase = Phase::Ready;
        return {};
    }
    stop("MP invalid timing negotiation");
    return {};
}
struct Session::Impl {
    // open() initializes before publishing to worker; destruction joins before cleanup.
    // mutex protects every field below, all ENet calls, setup, identity and packet queues.
    // worker itself is owner-thread-only; no reference into protected state escapes.
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    bool stopping = false;
    std::vector<save::Bytes> received;
    std::size_t queuedBytes = 0;
    std::uint32_t loadEpoch = 0, peerLoadedEpoch = 0;
    bool localLoaded = false, loadedSent = false, missionTransportReady = false;
    ENetHost* host = nullptr;
    ENetPeer* peer = nullptr;
    Identity identity;
    std::string error;
    bool initialized = false;
    LinkSetup setup;
    bool loaded() const { return !loadEpoch || (localLoaded && peerLoadedEpoch == loadEpoch); }
    void service(); // Called only with mutex held.
    ~Impl() {
        { std::lock_guard lock(mutex); stopping = true; }
        wake.notify_one();
        if (worker.joinable()) worker.join();
        if (host) enet_host_destroy(host);
        if (initialized) enet_deinitialize();
    }
    void fail(std::string message) {
        error = std::move(message);
        setup.stop(error);
        if (peer) { enet_peer_disconnect_later(peer, 1); enet_host_flush(host); }
    }
    void transmit(std::span<const std::uint8_t> bytes) {
        auto* outgoing = enet_packet_create(bytes.data(), bytes.size(), ENET_PACKET_FLAG_RELIABLE);
        if (!outgoing) { fail("ENet packet allocation failed"); return; }
        if (enet_peer_send(peer, 0, outgoing) < 0) {
            enet_packet_destroy(outgoing); fail("ENet send failed");
        }
    }
    void control(std::optional<LinkSetup::Control> message) {
        if (!message) return;
        save::Archive a;
        a(message->kind, message->value);
        transmit(a.bytes());
    }
};
Session::Session() : impl_(std::make_unique<Impl>()) {}
Session::~Session() = default;
void Session::open(bool hosting, std::string_view endpoint, Identity identity) {
    if (impl_->host) throw std::runtime_error("MP session already open");
    (void)hello(identity); // Reject invalid local identities inside the startup error boundary.
    if (enet_initialize() != 0) throw std::runtime_error("ENet initialization failed");
    impl_->initialized = true;
    impl_->identity = std::move(identity);
    impl_->setup.hosting = hosting;
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
    impl_->worker = std::thread([s = impl_.get()] {
        std::unique_lock lock(s->mutex);
        while (!s->stopping) {
            try { s->service(); }
            catch (const std::exception& error) { s->fail(error.what()); }
            s->wake.wait_for(lock, std::chrono::milliseconds(5), [s] { return s->stopping; });
        }
    });
}
void Session::Impl::service() {
    auto& s = *this;
    if (!s.host || !s.error.empty()) return;
    ENetEvent event{};
    // Nonblocking, bounded work; frames and the owner desktop keep running during a stall.
    for (unsigned count = 0; count < 128; ++count) {
        const auto result = enet_host_service(s.host, &event, 0);
        if (result < 0) { s.fail("ENet service failed"); break; }
        if (!result) break;
        if (event.type == ENET_EVENT_TYPE_CONNECT) {
            s.peer = event.peer;
            enet_peer_timeout(s.peer, 8, 120000, 600000);
            const auto packet = hello(s.identity);
            s.transmit(packet);
        } else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
            const std::span<const std::uint8_t> packet(event.packet->data, event.packet->dataLength);
            if (s.setup.phase == LinkSetup::Phase::Identity) {
                auto mismatch = checkHello(packet, s.identity);
                if (!mismatch.empty()) s.fail(std::move(mismatch));
                else s.control(s.setup.begin(enet_time_get()));
            } else if (s.setup.phase != LinkSetup::Phase::Ready) {
                save::Archive a(packet);
                LinkSetup::Control message;
                a(message.kind, message.value);
                if (!a.finish()) s.fail("MP malformed timing negotiation");
                else {
                    s.control(s.setup.receive(message, enet_time_get()));
                    if (!s.setup.error.empty()) s.fail(s.setup.error);
                }
            } else if (packet.size() == 8 && packet[0] == 'L' && packet[1] == 'O' && packet[2] == 'A' && packet[3] == 'D') {
                save::Archive a(packet);
                std::uint32_t kind = 0, epoch = 0;
                a(kind, epoch);
                if (!a.finish() || !epoch || epoch > s.loadEpoch + 1) s.fail("MP invalid LOADED marker");
                else s.peerLoadedEpoch = epoch; // May arrive before the lobby main thread starts loading.
            } else if (packet.size() > maxPacketBytes) s.fail("MP packet exceeds limit");
            else if (s.queuedBytes + packet.size() > 2 * maxPacketBytes) s.fail("MP receive queue exceeds limit");
            else { s.received.emplace_back(packet.begin(), packet.end()); s.queuedBytes += packet.size(); }
            enet_packet_destroy(event.packet);
            if (!s.error.empty()) break;
        } else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
            s.fail("MP peer disconnected"); break;
        }
    }
    if (s.error.empty() && s.setup.phase == LinkSetup::Phase::Ready && s.loadEpoch) {
        if (s.localLoaded && !s.loadedSent) {
            save::Archive a;
            auto kind = loadedMagic;
            a(kind, s.loadEpoch);
            s.transmit(a.bytes());
            s.loadedSent = true;
        }
        if (s.loaded() && !s.missionTransportReady) {
            enet_peer_timeout(s.peer, 8, 120000, 120000);
            s.missionTransportReady = true;
        }
    }
    enet_host_flush(s.host);
}
std::vector<save::Bytes> Session::pump() {
    auto& s = *impl_;
    std::lock_guard lock(s.mutex);
    // Timing still measures real main-thread frames, not the worker's polling cadence.
    if (s.host && s.error.empty()) s.control(s.setup.frame(enet_time_get()));
    std::vector<save::Bytes> received;
    received.swap(s.received);
    s.queuedBytes = 0;
    return received;
}
void Session::send(std::span<const std::uint8_t> packet) {
    auto& s = *impl_;
    std::lock_guard lock(s.mutex);
    if (!s.error.empty()) return;
    if (s.setup.phase != LinkSetup::Phase::Ready) { s.fail("MP session is not connected"); return; }
    if (packet.empty() || packet.size() > maxPacketBytes) { s.fail("invalid MP packet size"); return; }
    s.transmit(packet);
    enet_host_flush(s.host);
}
bool Session::connected() const { std::lock_guard lock(impl_->mutex); return impl_->setup.phase == LinkSetup::Phase::Ready && impl_->loaded(); }
void Session::missionLoading(bool loading) {
    std::lock_guard lock(impl_->mutex);
    if (loading) {
        ++impl_->loadEpoch;
        impl_->localLoaded = impl_->loadedSent = impl_->missionTransportReady = false;
        if (impl_->peer) enet_peer_timeout(impl_->peer, 8, 120000, 600000);
    } else impl_->localLoaded = true;
}
std::uint32_t Session::inputDelay() const { std::lock_guard lock(impl_->mutex); return impl_->setup.delay; }
std::uint32_t Session::rtt() const { std::lock_guard lock(impl_->mutex); return impl_->peer ? impl_->peer->roundTripTime : 0; }
std::uint32_t Session::jitter() const { std::lock_guard lock(impl_->mutex); return impl_->peer ? impl_->peer->roundTripTimeVariance : 0; }
std::uint32_t Session::measuredRtt() const { std::lock_guard lock(impl_->mutex); return impl_->setup.rtt; }
std::uint32_t Session::measuredJitter() const { std::lock_guard lock(impl_->mutex); return impl_->setup.jitter; }
std::string Session::error() const { std::lock_guard lock(impl_->mutex); return impl_->error; }
}
