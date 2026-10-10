// Gates/gate_row.h -- one row of the gate table, and the builders gate_table.cpp uses.
//
// Not a hot header: only gate.cpp, gate_table.cpp, tools/gate_dump and the tests include it.
// The row is a literal type so gate_table.cpp can static_assert on the whole table.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "gate_types.h"

namespace mc2::gate {

// Numeric bounds. Out of range falls back to the default (Reject) unless `clamp` is set,
// which is allowed only where the old reader clamped.
struct Range {
    bool has = false;
    bool clamp = false;
    double lo = 0.0;
    double hi = 0.0;
};

struct Row {
    std::string_view name;
    Kind kind = Kind::Bool;
    Rule rule = Rule::Std;
    bool defBool = false;
    std::int32_t defInt = 0;
    float defFloat = 0.0f;
    std::string_view defStr;     // String default, or the default token of an Enum row
    std::string_view owner;
    std::string_view retireBy;   // "" or an ISO date
    std::string_view doc;        // one line, at most 160 characters
    Range range;
    std::string_view tokens;     // "a|b|c" for Enum rows
    std::string_view logTopic;
    bool reread = false;         // read fresh after in-process _putenv (invalidate on write)
    bool divergent = false;      // readers deliberately disagree; owner decision pending
    bool smokePassthrough = false;  // run_smoke.py forwards it to the game
};

inline constexpr std::size_t kMaxDocLen = 160;

// The compiled table, defined in gate_table.cpp (sorted by name, the same order as the
// generated index).
const Row* tableData() noexcept;
std::size_t tableSize() noexcept;

}  // namespace mc2::gate
