#pragma once
#include "tactical_order_queue.h"
#include <array>
#include <string_view>

namespace mc2::replay {
inline bool blocksFixtureMutation(std::string_view command, bool recording) {
    const auto verb = command.substr(0, command.find_first_of(" \t"));
    // Fail closed: new fixture verbs must be audited before recording can use them.
    return recording && !(verb == "select" || verb == "order" || verb == "status" ||
        verb == "list" || verb == "unit" || verb == "mission_status" || verb == "light_state" ||
        verb == "camera" || verb == "camera_follow" || verb == "debugview" || verb == "screenshot");
}
void begin(std::string_view mission, std::uint32_t seed);
bool active();
bool playing();
bool complete();
bool failed();
void failRecording(std::string_view reason);
std::expected<sim::OrderReceipt, sim::OrderError> submit(sim::TacticalOrderQueue&,
    sim::OrderEnvelope, const sim::OrderAdmissionValidator&);
void pump(sim::TacticalOrderQueue&, const sim::OrderAdmissionValidator&);
void hashes(std::uint64_t tick, const std::array<std::uint64_t, 5>& values);
}
