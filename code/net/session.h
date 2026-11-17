#pragma once
#include "../save_archive.h"
#include <memory>
#include <optional>

namespace mc2::net {
inline constexpr std::uint32_t protocolVersion = 5;
inline constexpr std::size_t maxPacketBytes = 256 * 1024;
struct Identity {
    save::Digest executable{}, content{};
    std::string mission;
    std::uint32_t seed = 0;
    struct Component {
        std::string name;
        save::Digest digest{};
        bool operator==(const Component&) const = default;
    };
    std::vector<Component> components;
    bool operator==(const Identity&) const = default;
};
save::Bytes hello(const Identity& identity);
std::string digestText(const save::Digest& digest);
save::Digest contentIdentity(const save::Digest& files, bool lowCost, bool remasteredBuildings);
// Returns the specific incompatible field, or empty on an exact match.
std::string checkHello(std::span<const std::uint8_t> packet, const Identity& local);

struct LobbyState {
    bool hosting = false, offered = false, localReady = false, peerReady = false, started = false;
    std::string mission = "mc2_01", error;
    bool canStart() const { return offered && localReady && peerReady && !started && error.empty(); }
    save::Bytes offer();
    save::Bytes ready(bool value);
    save::Bytes start();
    void receive(std::span<const std::uint8_t> packet);
};

// Both peers settle their frame cadence before four reliable timing probes.
struct LinkSetup {
    enum class Phase { Identity, Settling, Measuring, Ready, Stopped };
    struct Control { std::uint32_t kind = 0, value = 0; };
    bool hosting = false;
    Phase phase = Phase::Identity;
    std::uint32_t probe = 0, samples = 0, rtt = 0, jitter = 0, previous = 0, delay = 3;
    std::uint32_t lastFrame = 0, settledFrames = 0;
    bool localSettled = false, peerSettled = false;
    std::string error;
    std::optional<Control> begin(std::uint32_t now);
    std::optional<Control> frame(std::uint32_t now);
    std::optional<Control> receive(Control message, std::uint32_t now);
    void stop(std::string_view reason);
};
std::uint32_t delayForRtt(std::uint32_t rtt, std::uint32_t jitter);
constexpr const char* progressTimeout(bool connected, std::uint64_t ageMs) {
    return ageMs >= (connected ? 120000u : 600000u)
        ? (connected ? "MP peer stalled (120 seconds without an agreed tick)" : "MP loading timeout (600 seconds)") : nullptr;
}

// Separate from the retail MultiPlayer object: no client/server sim forks.
class Session {
public:
    // Transport silence is independent of the 120-second agreed-tick policy.
    struct Timeouts {
        std::uint32_t minimum = 3000, maximum = 8000, teardown = 3000;
    };
    Session();
    explicit Session(Timeouts timeouts);
    virtual ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    // An empty host endpoint binds loopback:23460. Only numeric IPv4 is accepted.
    void open(bool host, std::string_view endpoint, Identity identity);
    virtual std::vector<save::Bytes> pump();
    virtual void send(std::span<const std::uint8_t> packet);
    // Transport negotiated, and (in a mission) both LOADED markers sent and received.
    virtual bool connected() const;
    struct LoadSnapshot {
        bool phaseReady, localLoaded, loadedSent;
        std::uint32_t loadEpoch, peerLoadedEpoch;
    };
    LoadSnapshot loadSnapshot() const;
    virtual void missionLoading(bool loading);
    std::uint32_t inputDelay() const;
    std::uint32_t rtt() const;
    std::uint32_t jitter() const;
    std::uint32_t measuredRtt() const;
    std::uint32_t measuredJitter() const;
    virtual std::string error() const;
    bool transportClosed() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
