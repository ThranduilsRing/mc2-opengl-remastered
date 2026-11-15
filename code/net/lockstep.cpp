#include "lockstep.h"
#include <stdexcept>

namespace mc2::net {
namespace {
constexpr std::uint32_t inputMessage = 1, hashMessage = 2, maxOrders = 128;
void encodeFrame(save::Archive& a, std::uint32_t& kind, sim::Tick& tick,
    std::vector<sim::OrderEnvelope>& orders) {
    a(kind, tick); a.sequence(orders, maxOrders);
}
}
Lockstep::Lockstep(std::uint32_t slot) : slot_(slot) {
    if (slot > 1) throw std::invalid_argument("MP supports exactly two peers");
    // Bootstrap only local empty markers. The other peer must send its own.
    for (sim::Tick tick = 1; tick <= inputDelay; ++tick) sendFrame(tick, {});
}
bool Lockstep::queue(sim::OrderEnvelope envelope) {
    // UI is also serviced during a running step; those inputs belong to the next batch.
    auto target = completed_ + inputDelay + 1 + (executing_ ? 1 : 0);
    if (!error_.empty() || pending_.size() >= maxOrders) return false;
    // A click while waiting for the peer belongs to the next unsealed batch.
    if (frames_[target].orders[slot_]) ++target;
    envelope.applyTick = target;
    envelope.sourceId = peerSourceBase + slot_;
    envelope.sequence = sequence_;
    envelope.issuer = sim::CommanderId{0};
    if (envelope.payload.origin != sim::OrderOrigin::Player) return false;
    save::Archive check;
    check(envelope);
    if (!check.good()) return false;
    pending_.push_back(std::move(envelope));
    ++sequence_;
    return true;
}
void Lockstep::sendFrame(sim::Tick tick, std::vector<sim::OrderEnvelope> orders) {
    save::Archive a;
    auto kind = inputMessage;
    encodeFrame(a, kind, tick, orders);
    if (!a.good() || a.bytes().size() > maxPacketBytes) { error_ = "MP input batch exceeds limit"; return; }
    frames_[tick].orders[slot_] = std::move(orders);
    outgoing_.push_back(a.bytes());
}
void Lockstep::seal() {
    if (!error_.empty() || executing_) return;
    const auto tick = completed_ + inputDelay + 1;
    if (frames_[tick].orders[slot_]) return;
    sendFrame(tick, std::move(pending_));
    pending_.clear();
}
void Lockstep::receive(std::span<const std::uint8_t> packet) {
    if (!error_.empty()) return;
    if (packet.size() > maxPacketBytes) { error_ = "MP packet exceeds limit"; return; }
    save::Archive a(packet);
    std::uint32_t kind = 0;
    sim::Tick tick = 0;
    a(kind, tick);
    // A peer can seal its next batch after receiving our last hash, before we service its hash.
    if (!a.good() || tick == 0 || tick > completed_ + inputDelay + 2) { error_ = "MP invalid tick marker"; return; }
    const auto remote = 1u - slot_;
    if (kind == inputMessage) {
        std::vector<sim::OrderEnvelope> orders;
        a.sequence(orders, maxOrders);
        if (!a.finish() || tick != nextRemoteTick_ || tick <= completed_ || frames_[tick].orders[remote]) {
            error_ = "MP invalid or duplicate input batch"; return;
        }
        auto sequence = remoteSequence_;
        for (const auto& order : orders) {
            if (order.applyTick != tick || order.sourceId != peerSourceBase + remote ||
                order.sequence != sequence++ || order.issuer != sim::OrderIssuer{sim::CommanderId{0}} ||
                order.payload.origin != sim::OrderOrigin::Player) {
                error_ = "MP forged order source, tick or sequence"; return;
            }
        }
        remoteSequence_ = sequence;
        ++nextRemoteTick_;
        frames_[tick].orders[remote] = std::move(orders);
    } else if (kind == hashMessage) {
        StateHash hash{};
        a(hash);
        if (!a.finish() || tick < completed_ || tick > completed_ + 1 || hashes_[tick][remote]) {
            error_ = "MP invalid or duplicate state hash"; return;
        }
        hashes_[tick][remote] = hash;
        compare(tick);
    } else error_ = "MP unknown packet type";
}
std::vector<save::Bytes> Lockstep::takeOutgoing() { return std::exchange(outgoing_, {}); }
bool Lockstep::ready() const {
    if (!error_.empty() || executing_) return false;
    if (completed_) {
        const auto previous = hashes_.find(completed_);
        if (previous == hashes_.end()) return false;
        const auto& [tick, peers] = *previous;
        if (!peers[0] || !peers[1]) return false;
    }
    const auto next = frames_.find(completed_ + 1);
    if (next == frames_.end()) return false;
    const auto& [tick, frame] = *next;
    return frame.orders[0] && frame.orders[1];
}
std::vector<sim::OrderEnvelope> Lockstep::beginTick() {
    if (!ready()) throw std::logic_error("MP tick is not agreed");
    executing_ = true;
    std::vector<sim::OrderEnvelope> result;
    for (auto& orders : frames_.at(completed_ + 1).orders)
        result.insert(result.end(), orders->begin(), orders->end());
    return result;
}
void Lockstep::compare(sim::Tick tick) {
    const auto& hashes = hashes_.at(tick);
    if (!hashes[0] || !hashes[1] || hashes[0] == hashes[1]) return;
    divergence_ = Divergence{tick, *hashes[slot_], *hashes[1u - slot_]};
    error_ = "MP_DESYNC tick=" + std::to_string(tick);
}
void Lockstep::finishTick(StateHash hash) {
    if (!executing_ || !error_.empty()) throw std::logic_error("MP tick completion without agreed step");
    executing_ = false;
    ++completed_;
    frames_.erase(completed_);
    hashes_.erase(completed_ - 1);
    hashes_[completed_][slot_] = hash;
    save::Archive a;
    auto kind = hashMessage;
    a(kind, completed_, hash);
    outgoing_.push_back(a.bytes());
    compare(completed_);
}
}
