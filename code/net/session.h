#pragma once
#include "../save_archive.h"
#include <memory>
#include <optional>

namespace mc2::net {
inline constexpr std::uint32_t protocolVersion = 3;
inline constexpr std::size_t maxPacketBytes = 256 * 1024;
struct Identity {
    save::Digest executable{}, content{};
    std::string mission;
    std::uint32_t seed = 0;
    bool operator==(const Identity&) const = default;
};
save::Bytes hello(const Identity& identity);
save::Digest contentIdentity(const save::Digest& files, bool lowCost, bool remasteredBuildings);
// Returns the specific incompatible field, or empty on an exact match.
std::string checkHello(std::span<const std::uint8_t> packet, const Identity& local);

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
    return ageMs >= (connected ? 15000u : 60000u)
        ? (connected ? "MP peer stalled (15 seconds without an agreed tick)" : "MP handshake timeout") : nullptr;
}

// Separate from the retail MultiPlayer object: no client/server sim forks.
class Session {
public:
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    // An empty host endpoint binds loopback:23460. Only numeric IPv4 is accepted.
    void open(bool host, std::string_view endpoint, Identity identity);
    std::vector<save::Bytes> pump();
    void send(std::span<const std::uint8_t> packet);
    bool connected() const;
    std::uint32_t inputDelay() const;
    std::uint32_t rtt() const;
    std::uint32_t jitter() const;
    std::uint32_t measuredRtt() const;
    std::uint32_t measuredJitter() const;
    const std::string& error() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
