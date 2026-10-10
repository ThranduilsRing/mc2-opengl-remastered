// Gates/gate_cold.h -- the cold half of the gate API: strings, name lookup, invalidation,
// the [CONFIG v1] dump and the one shared boolean parser.
//
// Heavier than gate.h (<string>, <cstdio>, <optional>); include it only where needed.
// Deliberately below mc2log: the dump writes with fwrite to a stream the caller passes.
#pragma once

#include <cstddef>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

#include "gate.h"

namespace mc2::gate {

// The one boolean parser: 0/false/off/no (case-insensitive) -> false, 1/true/on/yes ->
// true, unset or empty -> `dflt`, any other non-empty string -> true and *bad = true.
// HostServices::EnvConfig::flag and the Std rule of every Bool row use it.
[[nodiscard]] bool parseBool(const char* value, bool dflt, bool* bad = nullptr) noexcept;

struct NameInfo {
    std::size_t index;
    Kind kind;
    Rule rule;
};

// Table lookup by runtime name; nullopt for a name that is not a gate.
[[nodiscard]] std::optional<NameInfo> lookupName(std::string_view name) noexcept;

namespace detail {
// The variable's environment text as the C runtime reports it (nullptr when unset), for any row. Never cached.
[[nodiscard]] const char* rawEnv(std::size_t idx) noexcept;
// Current text of gate `idx`: the environment value, or the row's default string when the
// variable is unset or empty. Never cached (String rows are cold).
[[nodiscard]] std::string strRead(std::size_t idx);
}  // namespace detail

// A String row's current text (re-read on every call).
template <Name n>
[[nodiscard]] inline std::string str() {
    constexpr std::size_t idx = detail::indexOf(n.sv());
    static_assert(idx != detail::kNpos, "gate::str<>: name is not in the gate table (Gates/gate_table.cpp)");
    static_assert(detail::kIndex[idx].kind == Kind::String, "gate::str<>: only String rows");
    return detail::strRead(idx);
}

// The variable's current environment text, or nullptr when it is unset: for the diagnostic lines that echo
// what the user typed next to the parsed value. Cold and uncached; decisions read get<>() instead.
template <Name n>
[[nodiscard]] inline const char* raw() noexcept {
    constexpr std::size_t idx = detail::indexOf(n.sv());
    static_assert(idx != detail::kNpos, "gate::raw<>: name is not in the gate table (Gates/gate_table.cpp)");
    return detail::rawEnv(idx);
}

// The current text of an environment variable by RUNTIME name, nullptr when unset: for the one caller that
// walks names it does not know at compile time (the launcher_env.json apply asks "did the shell already set
// this key?"). No table lookup and no cache; every other reader names its gate at compile time.
[[nodiscard]] const char* envText(const char* name) noexcept;

// Forget the cached value of one gate / of every gate, so the next read parses the
// environment again. Call after an in-process _putenv. invalidateAll() first snapshots the
// values that were resolved before main so the dump can warn about stale ones.
void invalidate(std::string_view name) noexcept;
void invalidateAll() noexcept;

// First line of main()/InitInstance: reads resolved after this are not "pre-main".
void markMainEntered() noexcept;

struct DumpOptions {
    bool all = false;           // every row, not only the ones the environment sets
    bool none = false;          // summary line only
    bool strict = false;        // badvalue / unregistered are fatal
    bool exitOnStrict = true;   // tests turn this off
};

struct DumpSummary {
    std::size_t total = 0;
    std::size_t set = 0;
    std::size_t nondefault = 0;
    std::size_t unregistered = 0;
    std::size_t badvalue = 0;            // bad value on a verified row (fatal under strict)
    std::size_t premainStale = 0;
    std::size_t badvalueUnverified = 0;  // bad value on a row no typed reader has proven yet (warning only)
};

// MC2_CONFIG_DUMP (default|all|none) and MC2_CONFIG_STRICT from the environment.
[[nodiscard]] DumpOptions dumpOptionsFromEnv() noexcept;

// Write the [CONFIG v1] lines (one fwrite each) and return the summary.
DumpSummary dumpConfigWith(std::FILE* out, const DumpOptions& opts) noexcept;

// dumpConfigWith(out, dumpOptionsFromEnv()). Under MC2_CONFIG_STRICT=1 a bad value or an
// unregistered MC2_* variable ends the process with exit code 78 after printing why.
DumpSummary dumpConfig(std::FILE* out) noexcept;

}  // namespace mc2::gate
