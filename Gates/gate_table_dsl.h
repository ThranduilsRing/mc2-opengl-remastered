// Gates/gate_table_dsl.h -- the row builders and the compile-time checks behind
// gate_table.cpp. Only that TU (and the tests that build their own tables) include this.
//
//   G("MC2_NAME", Bool, true, "owner", "", "one-line doc")               // Std rule
//   L(Presence, "MC2_NAME", Bool, false, "owner", "2026-12-31", "doc")   // legacy rule
//   G("MC2_NAME", Int, 64, "owner", "", "doc", range(16, 256))           // + options
//   G("MC2_NAME", Enum, "1", "owner", "", "doc", tokens("1|2|agx"))
//
// Defaults are typed by overload: bool, int, double (Float), string literal (String, and
// the default token of an Enum). A Tri row passes `false` and means "unset".
#pragma once

#include <cstddef>
#include <string_view>

#include "gate_row.h"

namespace mc2::gate::rows {

using enum Kind;
using enum Rule;

// Optional row fields, combined with `|`: `reread | passthrough | range(1, 8)`.
struct Opts {
    bool reread = false;
    bool divergent = false;
    bool passthrough = false;
    Range range;
    std::string_view tokens;
    std::string_view logTopic;
};

constexpr Opts operator|(Opts a, const Opts& b) {
    a.reread = a.reread || b.reread;
    a.divergent = a.divergent || b.divergent;
    a.passthrough = a.passthrough || b.passthrough;
    if (b.range.has) a.range = b.range;
    if (!b.tokens.empty()) a.tokens = b.tokens;
    if (!b.logTopic.empty()) a.logTopic = b.logTopic;
    return a;
}

inline constexpr Opts reread{.reread = true};
inline constexpr Opts divergent{.divergent = true};
inline constexpr Opts passthrough{.passthrough = true};
// Out-of-range values fall back to the default.
constexpr Opts range(double lo, double hi) { return Opts{.range = Range{true, false, lo, hi}}; }
// Out-of-range values are clamped: only where the old reader clamped.
constexpr Opts clampRange(double lo, double hi) { return Opts{.range = Range{true, true, lo, hi}}; }
constexpr Opts tokens(std::string_view list) { return Opts{.tokens = list}; }
constexpr Opts topic(std::string_view t) { return Opts{.logTopic = t}; }

namespace detail {
constexpr Row make(Rule rule, std::string_view name, Kind kind, std::string_view owner,
                   std::string_view retireBy, std::string_view doc, const Opts& o) {
    Row r;
    r.name = name;
    r.kind = kind;
    r.rule = rule;
    r.owner = owner;
    r.retireBy = retireBy;
    r.doc = doc;
    r.range = o.range;
    r.tokens = o.tokens;
    r.logTopic = o.logTopic;
    r.reread = o.reread;
    r.divergent = o.divergent;
    r.smokePassthrough = o.passthrough;
    return r;
}
}  // namespace detail

constexpr Row L(Rule rule, std::string_view name, Kind kind, bool dflt, std::string_view owner,
                std::string_view retireBy, std::string_view doc, const Opts& o = {}) {
    Row r = detail::make(rule, name, kind, owner, retireBy, doc, o);
    r.defBool = dflt;
    return r;
}
constexpr Row L(Rule rule, std::string_view name, Kind kind, int dflt, std::string_view owner,
                std::string_view retireBy, std::string_view doc, const Opts& o = {}) {
    Row r = detail::make(rule, name, kind, owner, retireBy, doc, o);
    r.defInt = dflt;
    return r;
}
constexpr Row L(Rule rule, std::string_view name, Kind kind, double dflt, std::string_view owner,
                std::string_view retireBy, std::string_view doc, const Opts& o = {}) {
    Row r = detail::make(rule, name, kind, owner, retireBy, doc, o);
    r.defFloat = static_cast<float>(dflt);
    return r;
}
constexpr Row L(Rule rule, std::string_view name, Kind kind, const char* dflt, std::string_view owner,
                std::string_view retireBy, std::string_view doc, const Opts& o = {}) {
    Row r = detail::make(rule, name, kind, owner, retireBy, doc, o);
    r.defStr = dflt;
    return r;
}

template <typename D>
constexpr Row G(std::string_view name, Kind kind, D dflt, std::string_view owner,
                std::string_view retireBy, std::string_view doc, const Opts& o = {}) {
    return L(Rule::Std, name, kind, dflt, owner, retireBy, doc, o);
}
// Numeric rows with a legacy atoi/atof-compatible parse.
template <typename D>
constexpr Row P(std::string_view name, Kind kind, D dflt, std::string_view owner,
                std::string_view retireBy, std::string_view doc, const Opts& o = {}) {
    return L(Rule::Prefix, name, kind, dflt, owner, retireBy, doc, o);
}

// ---- compile-time table checks ------------------------------------------------------------

// What the rule says about an unset variable. Bool rows other than Std must declare it.
constexpr bool ruleImpliesUnsetOn(Rule r) {
    return r == Rule::Lead0Kill || r == Rule::DefOnLead0 || r == Rule::DefOnAtoiNZ;
}
constexpr bool ruleTakesDefault(Rule r) { return r == Rule::Std || r == Rule::Lead0Def; }

constexpr bool nameIsSortedUnique(const Row* t, std::size_t n) {
    for (std::size_t i = 1; i < n; ++i)
        if (!(t[i - 1].name < t[i].name)) return false;
    return true;
}

// Per-row sanity independent of the index.
constexpr bool rowIsSane(const Row& r) {
    if (r.name.size() < 5 || r.name.substr(0, 4) != "MC2_") return false;
    if (r.doc.size() > kMaxDocLen) return false;
    if (!r.retireBy.empty() && r.retireBy.size() != 10) return false;
    if (r.kind == Kind::Bool && !ruleTakesDefault(r.rule) && r.rule != Rule::Prefix &&
        r.defBool != ruleImpliesUnsetOn(r.rule))
        return false;  // a Bool default must agree with what the rule does for "unset"
    if (r.kind == Kind::Bool && r.rule == Rule::Prefix) return false;
    if ((r.kind == Kind::Int || r.kind == Kind::Float) && r.rule != Rule::Std && r.rule != Rule::Prefix) return false;
    if (r.kind == Kind::Enum && r.tokens.empty()) return false;
    if (r.kind != Kind::Enum && !r.tokens.empty()) return false;
    if (r.range.has && !(r.range.lo <= r.range.hi)) return false;
    if (r.range.has && r.kind != Kind::Int && r.kind != Kind::Float) return false;
    return true;
}

constexpr bool allRowsSane(const Row* t, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i)
        if (!rowIsSane(t[i])) return false;
    return true;
}

// First row that fails rowIsSane, or n when all pass (so a static_assert message can be
// found in the compiler output by a debugger-free binary search of the table).
constexpr std::size_t firstInsaneRow(const Row* t, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i)
        if (!rowIsSane(t[i])) return i;
    return n;
}

}  // namespace mc2::gate::rows
