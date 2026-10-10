// Gates/gate_types.h -- vocabulary shared by the gate index, table and readers.
//
// Only <cstdint> and <string_view>: this header is pulled into every translation unit
// that reads a gate, so it must stay cheap.
#pragma once

#include <cstdint>
#include <string_view>

namespace mc2::gate {

// What a gate's environment string means. Selects the C++ type gate::get<>() returns:
// Bool -> bool, Tri -> Tri, Int -> std::int32_t, Float -> float, Enum -> the token's
// index in the row's token list. String gates are read with gate::str<>() (gate_cold.h).
enum class Kind : std::uint8_t { Bool, Tri, Int, Float, String, Enum };

// Exact "1" / "0" state of a variable, with no bool conversion on purpose: `if (tri)` would
// silently mean "is set", which is the presence semantics this type exists to prevent.
enum class Tri : std::uint8_t { Unset, Off, On };

// How a Bool gate's string is turned into a value. Each name is the truthiness table of
// one reader idiom already in the tree (probe order: unset, "", "0", "false", "1", "yes",
// "10", "2.5"); the vector in brackets is on(1)/off(0) for those probes.
//   Std        [def def 0 0 1 1 1 1]  0/false/off/no off, 1/true/on/yes on, other non-empty on
//   Presence   [0 1 1 1 1 1 1 1]      `getenv(x) != nullptr`
//   NonEmpty   [0 0 1 1 1 1 1 1]      `v && *v`
//   Exact1     [0 0 0 0 1 0 0 0]      `strcmp(v, "1") == 0`
//   Lead1      [0 0 0 0 1 0 1 0]      first character is '1' (so "10" is on)
//   Lead0      [0 0 0 1 1 1 1 1]      `v && v[0] && v[0] != '0'`
//   Lead0Kill  [1 1 0 1 1 1 1 1]      `!(v && v[0] == '0')`: unset and empty are on
//   Lead0P     [0 1 0 1 1 1 1 1]      `v && v[0] != '0'`: empty is on, unset is off
//   DefOnLead0 [1 0 0 1 1 1 1 1]      `!v || (v[0] && v[0] != '0')`
//   AtoiNZ     [0 0 0 0 1 0 1 1]      `atoi(v) != 0`
//   DefOnAtoiNZ [1 0 0 0 1 0 1 1]     `!v || atoi(v) != 0`
//   Lead0Def   [def def 0 1 1 1 1 1]  `v && v[0] ? v[0] != '0' : dflt`
//   Is01       [0 0 1 0 1 0 0 0]      set to exactly "0" or "1"
//   Prefix     numeric rows: atoi/atof-compatible leading-number parse (see gate.cpp)
enum class Rule : std::uint8_t {
    Std,
    Presence,
    NonEmpty,
    Exact1,
    Lead1,
    Lead0,
    Lead0Kill,
    Lead0P,
    DefOnLead0,
    AtoiNZ,
    DefOnAtoiNZ,
    Lead0Def,
    Is01,
    Prefix,
};

// One line of the generated index (gate_index.gen.h): everything the hot header needs to
// resolve a name at compile time. The full row (default, owner, doc, ...) lives in
// gate_table.cpp.
struct IndexEntry {
    std::string_view name;
    Kind kind;
    Rule rule;
    std::string_view tokens;  // "a|b|c" for Enum rows, empty otherwise
};

}  // namespace mc2::gate
