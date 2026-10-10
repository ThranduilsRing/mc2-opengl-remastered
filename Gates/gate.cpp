// Gates/gate.cpp -- parse, resolve, invalidate and dump for the typed gate table.
//
// This TU is the only place in the tree that may read MC2_* variables with raw getenv /
// _environ (the LG19 ratchet exempts Gates/). It sits below mc2log: the dump goes to a
// FILE* the caller passes, never through the logging facade.
#include "gate_cold.h"
#include "gate_parse.h"
#include "gate_row.h"

#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if !defined(_WIN32)
extern char** environ;
#endif

namespace mc2::gate {

namespace detail {

// Constant-initialised, so readers running during other TUs' static initialisation are safe.
std::atomic<std::uint64_t> g_slot[kGateCount]{};

namespace {

std::atomic<bool> g_mainEntered{false};

// Slots resolved before main, captured by invalidateAll() for the dump's premain warning.
struct Snapshot {
    std::size_t idx;
    std::uint64_t slot;
};

std::vector<Snapshot>& snapshots() {
    static std::vector<Snapshot> s;
    return s;
}

constexpr std::uint64_t kValueMask = 0xFFFFFFFFull | kRowBit;

char lowerAscii(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

// `lit` must already be lower case.
bool equalsNoCase(const char* v, const char* lit) noexcept {
    for (;; ++v, ++lit) {
        const char a = lowerAscii(*v);
        if (a != *lit) return false;
        if (a == '\0') return true;
    }
}

// Token index of `v` (case-insensitive) in an "a|b|c" list, or -1.
int enumIndex(std::string_view list, const char* v) noexcept {
    int idx = 0;
    std::size_t start = 0;
    while (start <= list.size()) {
        std::size_t end = list.find('|', start);
        if (end == std::string_view::npos) end = list.size();
        const std::string_view tok = list.substr(start, end - start);
        std::size_t i = 0;
        while (i < tok.size() && lowerAscii(v[i]) == lowerAscii(tok[i])) ++i;
        if (i == tok.size() && v[i] == '\0') return idx;
        ++idx;
        start = end + 1;
    }
    return -1;
}

std::uint32_t floatBits(float f) noexcept { return __builtin_bit_cast(std::uint32_t, f); }

// `rg.lo` is exclusive when rg.loExcl (a reader that tests `f > lo`).
bool outOfRange(const Range& rg, double d) noexcept {
    return d < rg.lo || (rg.loExcl && d == rg.lo) || d > rg.hi;
}

}  // namespace

std::uint32_t classify(const char* v) noexcept {
    if (v == nullptr) return 0;
    std::uint32_t f = fSet;
    if (v[0] != '\0') f |= fNonEmpty;
    if (v[0] == '0') f |= fFirst0;
    if (v[0] == '1') f |= fFirst1;
    if (v[0] == '0' && v[1] == '\0') f |= fExact0;
    if (v[0] == '1' && v[1] == '\0') f |= fExact1;
    if (equalsNoCase(v, "0") || equalsNoCase(v, "false") || equalsNoCase(v, "off") || equalsNoCase(v, "no")) f |= fFalseTok;
    if (equalsNoCase(v, "1") || equalsNoCase(v, "true") || equalsNoCase(v, "on") || equalsNoCase(v, "yes")) f |= fTrueTok;
    if (std::atoi(v) != 0) f |= fAtoiNZ;
    return f;
}

// Parse the environment text `v` (may be null) as row `r`; returns the slot without the
// resolved / pre-main bits.
std::uint64_t parseRow(const Row& r, const char* v) noexcept {
    const std::uint32_t f = classify(v);
    std::uint64_t s = static_cast<std::uint64_t>(f) << kFlagShift;
    bool bad = false;
    const bool hasText = (f & fNonEmpty) != 0;

    switch (r.kind) {
        case Kind::Bool: {
            if (r.defBool) s |= kDefaultBit;
            if (r.rule == Rule::Std && hasText && (f & (fFalseTok | fTrueTok)) == 0) bad = true;
            if (applyRule(r.rule, f, r.defBool)) s |= kRowBit;
            break;
        }
        case Kind::Tri: {
            const std::uint32_t t = (f & fExact1) ? 2u : (f & fExact0) ? 1u : 0u;
            s |= t;
            break;
        }
        case Kind::Int: {
            long long value = r.defInt;
            if (hasText) {
                bool parsed = true;
                if (r.rule == Rule::Prefix) {
                    char* end = nullptr;
                    value = std::strtol(v, &end, 10);
                    if (end == v) bad = true;  // atoi() would also say 0
                } else {
                    std::int32_t x = 0;
                    const char* last = v + std::strlen(v);
                    const auto res = std::from_chars(v, last, x);
                    if (res.ec == std::errc{} && res.ptr == last) value = x;
                    else { bad = true; parsed = false; }
                }
                if (parsed && r.range.has) {
                    const double d = static_cast<double>(value);
                    if (outOfRange(r.range, d)) {
                        if (r.range.clamp) value = static_cast<long long>(d < r.range.lo ? r.range.lo : r.range.hi);
                        else { value = r.defInt; bad = true; }
                    }
                }
            }
            if (value > INT32_MAX) value = INT32_MAX;
            if (value < INT32_MIN) value = INT32_MIN;
            s |= static_cast<std::uint32_t>(static_cast<std::int32_t>(value));
            break;
        }
        case Kind::Float: {
            double value = r.defFloat;
            if (hasText) {
                bool parsed = true;
                if (r.rule == Rule::Prefix) {
                    char* end = nullptr;
                    value = std::strtod(v, &end);
                    if (end == v) bad = true;  // atof() would also say 0
                } else {
                    float x = 0.0f;
                    const char* last = v + std::strlen(v);
                    const auto res = std::from_chars(v, last, x);
                    if (res.ec == std::errc{} && res.ptr == last) value = x;
                    else { bad = true; parsed = false; }
                }
                // The legacy readers hold the parsed number in a float before testing it.
                value = static_cast<double>(static_cast<float>(value));
                // atof/strtod compatible rows let +-inf reach the range test (a clamp turns it
                // into the bound, as the old readers did); NaN never parses.
                const bool finiteOk = std::isfinite(value) || (r.rule == Rule::Prefix && std::isinf(value));
                if (parsed && !finiteOk) { bad = true; parsed = false; value = r.defFloat; }
                if (parsed && r.range.has) {
                    if (outOfRange(r.range, value)) {
                        if (r.range.clamp) value = value < r.range.lo ? r.range.lo : r.range.hi;
                        else { value = r.defFloat; bad = true; }
                    }
                }
            }
            s |= floatBits(static_cast<float>(value));
            break;
        }
        case Kind::Enum: {
            int idx = enumIndex(r.tokens, r.defStr.data());
            if (hasText) {
                const int hit = enumIndex(r.tokens, v);
                if (hit >= 0) idx = hit;
                else bad = true;
            }
            s |= static_cast<std::uint32_t>(idx < 0 ? 0 : idx);
            break;
        }
        case Kind::String:
            break;
    }
    if (bad) s |= kBadBit;
    return s;
}

namespace {

const char* envOf(const Row& r) noexcept {
    // Row names come from string literals, so name.data() is NUL-terminated.
    return std::getenv(r.name.data());
}

const char* ruleName(Rule r) noexcept {
    switch (r) {
        case Rule::Std:        return "std";
        case Rule::Presence:   return "presence";
        case Rule::NonEmpty:   return "nonempty";
        case Rule::Exact1:     return "exact1";
        case Rule::Lead1:      return "lead1";
        case Rule::Lead0:      return "lead0";
        case Rule::Lead0Kill:  return "lead0kill";
        case Rule::Lead0P:     return "lead0p";
        case Rule::DefOnLead0: return "defonlead0";
        case Rule::AtoiNZ:     return "atoinz";
        case Rule::DefOnAtoiNZ: return "defonatoinz";
        case Rule::Lead0Def:   return "lead0def";
        case Rule::Is01:       return "is01";
        case Rule::Prefix:     return "prefix";
    }
    return "?";
}

const char* kindName(Kind k) noexcept {
    switch (k) {
        case Kind::Bool:   return "bool";
        case Kind::Tri:    return "tri";
        case Kind::Int:    return "int";
        case Kind::Float:  return "float";
        case Kind::String: return "string";
        case Kind::Enum:   return "enum";
    }
    return "?";
}

void appendEscaped(std::string& out, const char* v) {
    if (v == nullptr) return;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(v); *p; ++p) {
        if (*p == '"' || *p == '\\') { out += '\\'; out += static_cast<char>(*p); }
        else if (*p < 0x20 || *p >= 0x7F) {
            char b[8];
            std::snprintf(b, sizeof b, "\\x%02X", *p);
            out += b;
        } else out += static_cast<char>(*p);
    }
}

std::string tokenAt(std::string_view list, int want) {
    int idx = 0;
    std::size_t start = 0;
    while (start <= list.size()) {
        std::size_t end = list.find('|', start);
        if (end == std::string_view::npos) end = list.size();
        if (idx == want) return std::string(list.substr(start, end - start));
        ++idx;
        start = end + 1;
    }
    return {};
}

// Printable value of a parsed slot.
std::string valueText(const Row& r, std::uint64_t s, const char* raw) {
    char b[40];
    switch (r.kind) {
        case Kind::Bool:  return (s & kRowBit) ? "1" : "0";
        case Kind::Tri: {
            const std::uint32_t t = static_cast<std::uint32_t>(s & 0xFFu);
            return t == 2 ? "1" : t == 1 ? "0" : "unset";
        }
        case Kind::Int:
            std::snprintf(b, sizeof b, "%d", static_cast<std::int32_t>(static_cast<std::uint32_t>(s)));
            return b;
        case Kind::Float:
            std::snprintf(b, sizeof b, "%g", static_cast<double>(__builtin_bit_cast(float, static_cast<std::uint32_t>(s))));
            return b;
        case Kind::Enum:
            return tokenAt(r.tokens, static_cast<std::int32_t>(static_cast<std::uint32_t>(s)));
        case Kind::String: {
            const std::string text = (raw && raw[0]) ? std::string(raw) : std::string(r.defStr);
            std::string t = "\"";
            appendEscaped(t, text.c_str());
            t += '"';
            return t;
        }
    }
    return {};
}

std::string defaultText(const Row& r) {
    char b[40];
    switch (r.kind) {
        case Kind::Bool:  return r.defBool ? "1" : "0";
        case Kind::Tri:   return "unset";
        case Kind::Int:   std::snprintf(b, sizeof b, "%d", r.defInt); return b;
        case Kind::Float: std::snprintf(b, sizeof b, "%g", static_cast<double>(r.defFloat)); return b;
        case Kind::Enum:  return std::string(r.defStr);
        case Kind::String: {
            std::string t = "\"";
            appendEscaped(t, std::string(r.defStr).c_str());
            t += '"';
            return t;
        }
    }
    return {};
}

bool isDefault(const Row& r, std::uint64_t s) {
    switch (r.kind) {
        case Kind::Bool:  return ((s & kRowBit) != 0) == r.defBool;
        case Kind::Tri:   return (s & 0xFFu) == 0;
        case Kind::Int:   return static_cast<std::int32_t>(static_cast<std::uint32_t>(s)) == r.defInt;
        case Kind::Float: return static_cast<std::uint32_t>(s) == floatBits(r.defFloat);
        case Kind::Enum:  return static_cast<std::int32_t>(static_cast<std::uint32_t>(s)) == enumIndex(r.tokens, r.defStr.data());
        case Kind::String: return true;
    }
    return true;
}

void writeLine(std::FILE* out, const std::string& line) noexcept {
    std::fwrite(line.data(), 1, line.size(), out);
}

char** environBlock() noexcept {
#if defined(_WIN32)
    return _environ;
#else
    return environ;
#endif
}

}  // namespace

std::uint64_t resolve(std::size_t idx) noexcept {
    if (idx >= tableSize()) return kResolvedBit;
    const Row& r = tableData()[idx];
    std::uint64_t s = parseRow(r, envOf(r)) | kResolvedBit;
    if (!g_mainEntered.load(std::memory_order_relaxed)) s |= kPreMainBit;
    // A row read fresh after in-process _putenv (reread) is never cached: every read parses again.
    if (r.reread) return s;
    // Two threads racing on the first read agree on the winner's value. invalidate()/invalidateAll()
    // are for the single-threaded phases that call _putenv (startup, mod picker, harness setup): a read on
    // another thread that straddles an invalidate may cache the pre-_putenv text until the next invalidate.
    std::uint64_t expected = 0;
    if (!g_slot[idx].compare_exchange_strong(expected, s, std::memory_order_relaxed)) return expected;
    return s;
}

std::string strRead(std::size_t idx) {
    if (idx >= tableSize()) return {};
    const Row& r = tableData()[idx];
    const char* v = envOf(r);
    if (v != nullptr && v[0] != '\0') return v;
    return std::string(r.defStr);
}

}  // namespace detail

bool parseBool(const char* v, bool dflt, bool* bad) noexcept {
    if (bad != nullptr) *bad = false;
    if (v == nullptr || v[0] == '\0') return dflt;
    if (detail::equalsNoCase(v, "0") || detail::equalsNoCase(v, "false") ||
        detail::equalsNoCase(v, "off") || detail::equalsNoCase(v, "no")) return false;
    if (detail::equalsNoCase(v, "1") || detail::equalsNoCase(v, "true") ||
        detail::equalsNoCase(v, "on") || detail::equalsNoCase(v, "yes")) return true;
    if (bad != nullptr) *bad = true;
    return true;
}

std::optional<NameInfo> lookupName(std::string_view name) noexcept {
    const Row* t = tableData();
    std::size_t lo = 0;
    std::size_t hi = tableSize();
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        const int c = t[mid].name.compare(name);
        if (c == 0) return NameInfo{mid, t[mid].kind, t[mid].rule};
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return std::nullopt;
}

void invalidate(std::string_view name) noexcept {
    if (const auto info = lookupName(name)) {
        detail::g_slot[info->index].store(0, std::memory_order_relaxed);
    }
}

void invalidateAll() noexcept {
    auto& snaps = detail::snapshots();
    for (std::size_t i = 0; i < detail::kGateCount; ++i) {
        const std::uint64_t s = detail::g_slot[i].load(std::memory_order_relaxed);
        if ((s & detail::kResolvedBit) && (s & detail::kPreMainBit)) {
            bool known = false;
            for (const auto& k : snaps) known = known || (k.idx == i);
            if (!known) snaps.push_back({i, s});
        }
        detail::g_slot[i].store(0, std::memory_order_relaxed);
    }
}

void markMainEntered() noexcept { detail::g_mainEntered.store(true, std::memory_order_relaxed); }

DumpOptions dumpOptionsFromEnv() noexcept {
    DumpOptions o;
    const char* d = std::getenv("MC2_CONFIG_DUMP");
    if (d != nullptr) {
        o.all = detail::equalsNoCase(d, "all");
        o.none = detail::equalsNoCase(d, "none");
    }
    o.strict = parseBool(std::getenv("MC2_CONFIG_STRICT"), false);
    return o;
}

DumpSummary dumpConfigWith(std::FILE* out, const DumpOptions& opts) noexcept {
    using namespace detail;
    DumpSummary sum;
    if (out == nullptr) return sum;
    const Row* rows = tableData();
    const std::size_t n = tableSize();
    sum.total = n;

    std::vector<std::string> warnings;
    for (std::size_t i = 0; i < n; ++i) {
        const Row& r = rows[i];
        const char* raw = envOf(r);
        const std::uint64_t s = parseRow(r, raw);
        const bool set = raw != nullptr;
        if (set) ++sum.set;
        if (!isDefault(r, s)) ++sum.nondefault;
        if (set && (s & kBadBit)) {
            if (r.verified) ++sum.badvalue; else ++sum.badvalueUnverified;
            std::string w = "[CONFIG v1] warn=badvalue name=";
            w += r.name;
            w += " kind=";
            w += kindName(r.kind);
            w += " raw=\"";
            appendEscaped(w, raw);
            w += r.verified ? "\"\n" : "\" verified=0\n";
            warnings.push_back(std::move(w));
        }
        if (opts.none || !(opts.all || set)) continue;
        std::string line = "[CONFIG v1] name=";
        line += r.name;
        line += " kind=";
        line += kindName(r.kind);
        line += " value=";
        line += valueText(r, s, raw);
        line += set ? " src=env" : " src=default";
        line += " raw=\"";
        appendEscaped(line, raw);
        line += "\" default=";
        line += defaultText(r);
        line += " rule=";
        line += ruleName(r.rule);
        line += " owner=";
        line += r.owner;
        line += '\n';
        writeLine(out, line);
    }

    if (char** env = environBlock()) {
        for (; *env != nullptr; ++env) {
            const char* e = *env;
            if (std::strncmp(e, "MC2_", 4) != 0) continue;
            const char* eq = std::strchr(e, '=');
            const std::string name(e, eq ? static_cast<std::size_t>(eq - e) : std::strlen(e));
            if (!lookupName(name)) {
                ++sum.unregistered;
                warnings.push_back("[CONFIG v1] warn=unregistered name=" + name + "\n");
            }
        }
    }

    for (const auto& snap : snapshots()) {
        if (snap.idx >= n) continue;
        const Row& r = rows[snap.idx];
        const std::uint64_t now = parseRow(r, envOf(r));
        if (((now ^ snap.slot) & kValueMask) != 0) {
            ++sum.premainStale;
            std::string w = "[CONFIG v1] warn=premain name=";
            w += r.name;
            w += " premain_value=" + valueText(r, snap.slot, nullptr);
            w += " env_now=" + valueText(r, now, envOf(r));
            w += '\n';
            warnings.push_back(std::move(w));
        }
    }

    for (const auto& w : warnings) writeLine(out, w);

    char b[200];
    std::snprintf(b, sizeof b,
                  "[CONFIG v1] summary total=%zu set=%zu nondefault=%zu unregistered=%zu badvalue=%zu premain_stale=%zu"
                  " badvalue_unverified=%zu\n",
                  sum.total, sum.set, sum.nondefault, sum.unregistered, sum.badvalue, sum.premainStale,
                  sum.badvalueUnverified);
    writeLine(out, b);
    std::fflush(out);

    if (opts.strict && opts.exitOnStrict && (sum.unregistered != 0 || sum.badvalue != 0)) {
        std::snprintf(b, sizeof b,
                      "[CONFIG v1] fatal=strict unregistered=%zu badvalue=%zu (MC2_CONFIG_STRICT=1)\n",
                      sum.unregistered, sum.badvalue);
        writeLine(out, b);
        std::fflush(out);
        std::_Exit(78);
    }
    return sum;
}

DumpSummary dumpConfig(std::FILE* out) noexcept { return dumpConfigWith(out, dumpOptionsFromEnv()); }

}  // namespace mc2::gate
