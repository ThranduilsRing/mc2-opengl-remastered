#pragma once
#include "session.h"
#include "tactical_order_queue.h"
#include <map>

namespace mc2::net {
inline constexpr sim::Tick inputDelay = 3;
inline constexpr std::uint32_t peerSourceBase = 100;
using StateHash = std::array<std::uint64_t, 5>;
struct Divergence {
    sim::Tick tick = 0;
    StateHash local{}, remote{};
};
// Pure protocol state. Transport scheduling and local frame timing cannot advance it.
class Lockstep {
public:
    explicit Lockstep(std::uint32_t slot);
    bool queue(sim::OrderEnvelope envelope);
    void stop(std::string_view reason) { if (error_.empty()) error_ = reason; }
    void seal();
    void receive(std::span<const std::uint8_t> packet);
    std::vector<save::Bytes> takeOutgoing();
    bool ready() const;
    std::vector<sim::OrderEnvelope> beginTick();
    void finishTick(StateHash hash);
    sim::Tick completed() const { return completed_; }
    const std::string& error() const { return error_; }
    const std::optional<Divergence>& divergence() const { return divergence_; }
private:
    struct Frame { std::array<std::optional<std::vector<sim::OrderEnvelope>>, 2> orders; };
    std::uint32_t slot_;
    sim::Tick completed_ = 0;
    std::uint64_t sequence_ = 0, remoteSequence_ = 0;
    sim::Tick nextRemoteTick_ = 1;
    bool executing_ = false;
    std::map<sim::Tick, Frame> frames_;
    std::map<sim::Tick, std::array<std::optional<StateHash>, 2>> hashes_;
    std::vector<sim::OrderEnvelope> pending_;
    std::vector<save::Bytes> outgoing_;
    std::string error_;
    std::optional<Divergence> divergence_;
    void sendFrame(sim::Tick tick, std::vector<sim::OrderEnvelope> orders);
    void compare(sim::Tick tick);
};
// Initial Team 0 roster indices; runtime additions belong to slot 0.
constexpr std::uint32_t moverOwner(std::uint32_t index, std::uint32_t /*count*/, bool initial = true) {
    return initial && index > 0 ? 1u : 0u;
}
}
