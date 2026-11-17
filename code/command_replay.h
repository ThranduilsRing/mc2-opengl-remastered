#pragma once
#include "tactical_order_queue.h"
#include <array>
#include <string_view>

namespace mc2::replay {
inline bool blocksFixtureMutation(std::string_view command, bool recording) {
    const auto verb = command.substr(0, command.find_first_of(" \t"));
    return recording && (verb == "teleport" || verb == "spawn" || verb == "freeze_movers" ||
                         verb == "force_state" || verb == "time_scale" || verb == "end_mission");
}
void begin(std::string_view mission, std::uint32_t seed);
bool active();
bool playing();
bool complete();
bool failed();
std::expected<sim::OrderReceipt, sim::OrderError> submit(sim::TacticalOrderQueue&,
    sim::OrderEnvelope, const sim::OrderAdmissionValidator&);
void pump(sim::TacticalOrderQueue&, const sim::OrderAdmissionValidator&);
void hashes(std::uint64_t tick, const std::array<std::uint64_t, 5>& values);
}
