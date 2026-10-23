#pragma once
#include "tactical_order_queue.h"
#include <array>
#include <string_view>

namespace mc2::replay {
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
