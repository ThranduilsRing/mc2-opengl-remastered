// Gates/gate.h -- the hot reader API of the typed gate table.
//
//   if (gate::get<"MC2_TERRAIN_LOD_CHUNK">()) ...        // Bool row -> bool
//   int dim = gate::get<"MC2_LIGHT_GRID_DIM">();          // Int row  -> std::int32_t
//   gate::Tri t = gate::get<"MC2_VK_SHARED_GBUFFER">();   // Tri row  -> Tri (never a bool)
//   bool legacy = gate::get<"MC2_X", gate::Rule::Presence>();  // per-site rule override
//
// A misspelt name is a compile error. The first read of a name parses the environment
// (out of line, gate.cpp) and caches the answer in one atomic word, so every later read is
// a relaxed load, a test and a branch. There is no function-local static, hence no
// init-order dependence and no thread-safe-init guard.
//
// Only <atomic> <cstdint> <string_view> (and gate_types.h / gate_index.gen.h) are included
// here; strings, FILE* and the dump live in gate_cold.h.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "gate_types.h"
#include "gate_index.gen.h"

namespace mc2::gate {

// A string literal as a template argument: gate::get<"MC2_X">().
template <std::size_t N>
struct Name {
    char v[N]{};
    consteval Name(const char (&s)[N]) {
        for (std::size_t i = 0; i < N; ++i) v[i] = s[i];
    }
    constexpr std::string_view sv() const noexcept { return {v, N - 1}; }
};

namespace detail {

inline constexpr std::size_t kNpos = ~static_cast<std::size_t>(0);

// Binary search of the sorted generated index; evaluated at compile time.
consteval std::size_t indexOf(std::string_view name) noexcept {
    std::size_t lo = 0;
    std::size_t hi = kGateCount;
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        const int c = kIndex[mid].name.compare(name);
        if (c == 0) return mid;
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return kNpos;
}

// Position of `tok` in an "a|b|c" token list, or -1.
consteval int tokenIndex(std::string_view list, std::string_view tok) noexcept {
    int idx = 0;
    std::size_t start = 0;
    while (start <= list.size()) {
        std::size_t end = list.find('|', start);
        if (end == std::string_view::npos) end = list.size();
        if (list.substr(start, end - start) == tok) return idx;
        ++idx;
        start = end + 1;
    }
    return -1;
}

// Slot layout (one std::uint64_t per gate):
//   bits  0..31  payload: Int value, Float bits, Enum token index, Tri (0 Unset 1 Off 2 On)
//   bits 32..47  classification flags of the raw string (below), valid for every kind
//   bit  48      Bool value under the row's own rule
//   bit  49      the row's default (Bool)
//   bit  61      the string did not parse as this kind (value fell back to the default)
//   bit  62      resolved before gate::markMainEntered() (static-init time)
//   bit  63      resolved
inline constexpr std::uint64_t kResolvedBit = 1ull << 63;
inline constexpr std::uint64_t kPreMainBit  = 1ull << 62;
inline constexpr std::uint64_t kBadBit      = 1ull << 61;
inline constexpr std::uint64_t kRowBit      = 1ull << 48;
inline constexpr std::uint64_t kDefaultBit  = 1ull << 49;
inline constexpr unsigned      kFlagShift   = 32;

enum Flag : std::uint32_t {
    fSet      = 1u << 0,  // variable present (maybe empty)
    fNonEmpty = 1u << 1,
    fFirst0   = 1u << 2,  // v[0] == '0'
    fFirst1   = 1u << 3,  // v[0] == '1'
    fExact0   = 1u << 4,  // v == "0"
    fExact1   = 1u << 5,  // v == "1"
    fFalseTok = 1u << 6,  // 0 / false / off / no, case-insensitive
    fTrueTok  = 1u << 7,  // 1 / true / on / yes, case-insensitive
    fAtoiNZ   = 1u << 8,  // atoi(v) != 0
};

constexpr std::uint32_t flagsOf(std::uint64_t slot) noexcept {
    return static_cast<std::uint32_t>(slot >> kFlagShift) & 0xFFFFu;
}

// The Bool truthiness table (see Rule in gate_types.h) as one function of the flags.
constexpr bool applyRule(Rule r, std::uint32_t f, bool dflt) noexcept {
    const bool set      = (f & fSet) != 0;
    const bool nonEmpty = (f & fNonEmpty) != 0;
    const bool first0   = (f & fFirst0) != 0;
    switch (r) {
        case Rule::Std:        return nonEmpty ? (f & fFalseTok) == 0 : dflt;
        case Rule::Presence:   return set;
        case Rule::NonEmpty:   return nonEmpty;
        case Rule::Exact1:     return (f & fExact1) != 0;
        case Rule::Lead1:      return (f & fFirst1) != 0;
        case Rule::Lead0:      return nonEmpty && !first0;
        case Rule::Lead0Kill:  return !(set && first0);
        case Rule::Lead0P:     return set && !first0;
        case Rule::DefOnLead0: return !set || (nonEmpty && !first0);
        case Rule::AtoiNZ:     return set && (f & fAtoiNZ) != 0;
        case Rule::DefOnAtoiNZ: return !set || (f & fAtoiNZ) != 0;
        case Rule::Lead0Def:   return nonEmpty ? !first0 : dflt;
        case Rule::Is01:       return (f & (fExact0 | fExact1)) != 0;
        case Rule::Prefix:     return dflt;  // numeric rows only
    }
    return dflt;
}

// The hot path relies on a plain load/store: a lock-based atomic would turn every read into a call.
static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "gate slots need lock-free 64-bit atomics");

extern std::atomic<std::uint64_t> g_slot[kGateCount];

// Parse the environment for gate `idx`, publish and return the slot. gate.cpp.
std::uint64_t resolve(std::size_t idx) noexcept;

[[nodiscard]] inline std::uint64_t load(std::size_t idx) noexcept {
    std::uint64_t s = g_slot[idx].load(std::memory_order_relaxed);
    if (!(s & kResolvedBit)) [[unlikely]] s = resolve(idx);
    return s;
}

}  // namespace detail

// The row's value, typed by its kind.
template <Name n>
[[nodiscard]] inline auto get() noexcept {
    constexpr std::size_t idx = detail::indexOf(n.sv());
    static_assert(idx != detail::kNpos, "gate::get<>: name is not in the gate table (Gates/gate_table.cpp)");
    constexpr Kind k = detail::kIndex[idx].kind;
    static_assert(k != Kind::String, "gate::get<>: String rows are read with gate::str<>() (gate_cold.h)");
    const std::uint64_t s = detail::load(idx);
    if constexpr (k == Kind::Bool) {
        return (s & detail::kRowBit) != 0;
    } else if constexpr (k == Kind::Tri) {
        return static_cast<Tri>(s & 0xFFu);
    } else if constexpr (k == Kind::Float) {
        return __builtin_bit_cast(float, static_cast<std::uint32_t>(s));
    } else {  // Int, Enum
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(s));
    }
}

// A Bool row read under a different rule than the row's own. The escape hatch for a site
// whose old idiom differs from the row and was not unified; counted by gate_gen --check.
template <Name n, Rule r>
[[nodiscard]] inline bool get() noexcept {
    constexpr std::size_t idx = detail::indexOf(n.sv());
    static_assert(idx != detail::kNpos, "gate::get<>: name is not in the gate table (Gates/gate_table.cpp)");
    static_assert(detail::kIndex[idx].kind == Kind::Bool, "gate::get<name, Rule>: only Bool rows take a rule override");
    static_assert(r != Rule::Prefix, "gate::get<name, Rule>: Prefix is a numeric rule");
    const std::uint64_t s = detail::load(idx);
    return detail::applyRule(r, detail::flagsOf(s), (s & detail::kDefaultBit) != 0);
}

// The raw exact-"1" / exact-"0" state of a Bool or Tri row, for readers that must tell
// "unset" from "off" (the VK umbrella helpers).
template <Name n>
[[nodiscard]] inline Tri state() noexcept {
    constexpr std::size_t idx = detail::indexOf(n.sv());
    static_assert(idx != detail::kNpos, "gate::state<>: name is not in the gate table (Gates/gate_table.cpp)");
    static_assert(detail::kIndex[idx].kind == Kind::Bool || detail::kIndex[idx].kind == Kind::Tri,
                  "gate::state<>: only Bool and Tri rows");
    const std::uint32_t f = detail::flagsOf(detail::load(idx));
    if (f & detail::fExact1) return Tri::On;
    if (f & detail::fExact0) return Tri::Off;
    return Tri::Unset;
}

// Presence of the variable, whatever the row's kind (MC2_LOG, MC2_NO_LAUNCHER).
template <Name n>
[[nodiscard]] inline bool isSet() noexcept {
    constexpr std::size_t idx = detail::indexOf(n.sv());
    static_assert(idx != detail::kNpos, "gate::isSet<>: name is not in the gate table (Gates/gate_table.cpp)");
    return (detail::flagsOf(detail::load(idx)) & detail::fSet) != 0;
}

// Enum row: does the current value equal token `tok`? The token is checked at compile time.
template <Name n, Name tok>
[[nodiscard]] inline bool is() noexcept {
    constexpr std::size_t idx = detail::indexOf(n.sv());
    static_assert(idx != detail::kNpos, "gate::is<>: name is not in the gate table (Gates/gate_table.cpp)");
    static_assert(detail::kIndex[idx].kind == Kind::Enum, "gate::is<>: only Enum rows");
    constexpr int want = detail::tokenIndex(detail::kIndex[idx].tokens, tok.sv());
    static_assert(want >= 0, "gate::is<>: token is not in the row's token list");
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(detail::load(idx))) == want;
}

}  // namespace mc2::gate
