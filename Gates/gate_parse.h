// Gates/gate_parse.h -- the string-to-slot parser, exposed so tests/unit can drive it with
// arbitrary rows and text instead of mutating the process environment. Internal: callers
// outside Gates/ and tests use gate.h / gate_cold.h.
#pragma once

#include <cstdint>

#include "gate.h"
#include "gate_row.h"

namespace mc2::gate::detail {

// Classification flags (detail::Flag) of a raw environment string; null = unset.
[[nodiscard]] std::uint32_t classify(const char* value) noexcept;

// Parse `value` (null = unset) as row `row`: the slot word without the resolved / pre-main
// bits. kBadBit marks a string that did not parse as the row's kind.
[[nodiscard]] std::uint64_t parseRow(const Row& row, const char* value) noexcept;

}  // namespace mc2::gate::detail
