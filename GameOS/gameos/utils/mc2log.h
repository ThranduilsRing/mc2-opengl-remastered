// mc2log.h -- logging facade (phases P0/P1 of .claude/cleanup/logging-facade-plan.md,
// including the 2026-10-09 amendments A2..A8).  namespace mc2::log.
//
// Contract highlights:
//  * Level/category check happens INSIDE the macro, before the format string or
//    any argument expression is evaluated (A7).
//  * Compile-time literals use std::format_string; runtime/data-supplied
//    templates go through MC2_LOGV (vformat + try/catch, never throws) (A4).
//  * Thresholds / event enables are constinit with legacy defaults (A2); init()
//    only overrides, so logging before main() is safe.
//  * MC2_EVENT emits exactly "[TAG vN] " + body + "\n" on the same FILE* the
//    replaced fprintf/printf used; no decoration, one fwrite per line (A8).
//    Call sites keep their own fflush() exactly as the legacy code had it.
//  * Nothing here throws.  Fatal/assert/STOP paths are not migrated (A9).
#pragma once
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <format>
#include <iterator>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mc2::log {

enum class Level : uint8_t { Trace, Debug, Info, Warn, Error, Fatal, Off };
enum class Cat : uint8_t { Core, Io, Render, Vk, Gl, Terrain, Water, Mech, Veg, Ai, Sim,
                           Ui, Audio, Net, Shader, Perf, Smoke, Assets, Editor, Count };
// Err/Out = the process stderr/stdout FILE* (so the MC2_LOG-unset NUL redirect
// keeps working).  Dbg = OutputDebugStringA (stderr off Windows).
enum class Stream : uint8_t { Err, Out, Dbg };

inline constexpr size_t kCatCount = size_t(Cat::Count);

#ifndef MC2_LOG_MIN_LEVEL
#define MC2_LOG_MIN_LEVEL 0   // CMake may raise this; sites below it become dead code
#endif
inline constexpr Level kMinLevel = Level(MC2_LOG_MIN_LEVEL);

// ---- constinit state (A2): legacy defaults, no dynamic initialisation ----------
namespace detail {
#define MC2LOG_T3 Level::Info, Level::Info, Level::Info
#define MC2LOG_B3 true, true, true
inline constinit std::atomic<Level> g_threshold[kCatCount] = {
    MC2LOG_T3, MC2LOG_T3, MC2LOG_T3, MC2LOG_T3, MC2LOG_T3, MC2LOG_T3, Level::Info};
inline constinit std::atomic<bool> g_eventOn[kCatCount] = {
    MC2LOG_B3, MC2LOG_B3, MC2LOG_B3, MC2LOG_B3, MC2LOG_B3, MC2LOG_B3, true};
#undef MC2LOG_T3
#undef MC2LOG_B3
inline constinit std::atomic<bool> g_decorate{false};
inline constinit std::atomic<int>  g_topicCount{0};
static_assert(kCatCount == 19, "update the constinit initialiser lists");
}  // namespace detail

// Whole disabled-path cost: one relaxed load + compare.
inline bool enabled(Cat c, Level l) noexcept {
    return l >= detail::g_threshold[size_t(c)].load(std::memory_order_relaxed);
}
// Legacy-gate equivalent for structured events: default on (always-on stays
// always-on); only a spec "cat:off" turns a category's events off.
inline bool eventEnabled(Cat c) noexcept {
    return detail::g_eventOn[size_t(c)].load(std::memory_order_relaxed);
}
bool topicEnabled(Cat c, std::string_view topic) noexcept;   // "terrain=indirect+surface"

const char* catName(Cat c) noexcept;
const char* levelName(Level l) noexcept;

// ---- spec parsing (pure; unit tested) -------------------------------------------
// Grammar: MC2_LOG unset  -> legacy quiet (streams redirected to NUL by gameosmain)
//          non-spec value -> legacy loud ("1", "", anything without ':' or '=')
//          spec           -> comma list of  cat:level | cat=level | cat=topic[+topic...]
//                            cat may be '*'.  A spec implies live streams.
//          level "off" on a category also disables that category's MC2_EVENTs.
struct ParsedSpec {
    bool isSpec = false;                 // contained ':' or '='
    int  errors = 0;                     // tokens ignored as invalid
    bool levelSet[kCatCount] = {};
    Level level[kCatCount] = {};
    bool eventOff[kCatCount] = {};
    struct Topic { Cat cat; std::string name; };
    std::vector<Topic> topics;
};
ParsedSpec parseSpec(std::string_view spec);      // never throws
void setSpec(std::string_view spec);              // parse + apply (runtime / dev shell)
// Call from gameosmain BEFORE the MC2_LOG NUL redirect (A3).  Reads MC2_LOG and
// MC2_LOG_DECORATE; returns true when stdout/stderr must stay live (== MC2_LOG is
// set to anything, exactly the legacy rule).
bool init() noexcept;
bool streamsLiveFor(const char* mc2LogEnvValue) noexcept;   // pure helper (nullptr -> false)

// ---- sinks ----------------------------------------------------------------------
// One fwrite per line.  `line` must include its trailing newline.  No allocation.
void writeRaw(Stream s, std::string_view line) noexcept;
// Free-form line (adds "[LEVEL cat] " only when MC2_LOG_DECORATE=1) + '\n'.
void write(Cat c, Level l, Stream s, std::string_view text, const std::source_location& loc) noexcept;
// Test hook: route a stream to a given FILE* (nullptr restores stdout/stderr).
void setStreamOverrideForTest(Stream s, std::FILE* f) noexcept;

// ---- rate limit / once ----------------------------------------------------------
bool allowRate(uint64_t siteKey, double perSecond) noexcept;
bool allowRateAt(uint64_t siteKey, double perSecond, int64_t nowNs) noexcept;   // injectable clock
bool allowOnce(uint64_t siteKey) noexcept;
inline uint64_t siteKey(const std::source_location& l) noexcept {
    return (uint64_t(reinterpret_cast<uintptr_t>(l.file_name())) * 1099511628211ull)
         ^ (uint64_t(l.line()) << 20) ^ l.column();
}
// Frame counter for MC2_LOG_EVERY_N_FRAMES (the engine may call tickFrame once per frame).
void tickFrame() noexcept;
uint64_t frameNumber() noexcept;

namespace detail {
inline std::string& tlBuf() { thread_local std::string b; return b; }

inline void reportFormatError(Cat c, Level l, Stream s, std::string_view tmpl, const char* what,
                              const std::source_location& loc) noexcept {
    try {
        std::string m(tmpl);
        m += "\n[LOG FORMAT ERROR] ";
        m += what ? what : "?";
        write(c, l, s, m, loc);
    } catch (...) {}
}
}  // namespace detail

// ---- compile-time-checked free-form API ------------------------------------------
template <class... A>
void logFmt(Cat c, Level l, Stream s, const std::source_location& loc,
            std::format_string<A...> f, A&&... a) noexcept {
    try {
        std::string& b = detail::tlBuf();
        b.clear();
        std::format_to(std::back_inserter(b), f, std::forward<A>(a)...);
        write(c, l, s, b, loc);
    } catch (const std::exception& e) {
        detail::reportFormatError(c, l, s, f.get(), e.what(), loc);
    } catch (...) {}
}

// ---- runtime format strings (A4): never throws ------------------------------------
template <class... A>
void logRuntime(Cat c, Level l, Stream s, const std::source_location& loc,
                std::string_view tmpl, A&&... a) noexcept {
    try {
        std::string& b = detail::tlBuf();
        b = std::vformat(tmpl, std::make_format_args(a...));
        write(c, l, s, b, loc);
    } catch (const std::exception& e) {      // includes std::format_error
        detail::reportFormatError(c, l, s, tmpl, e.what(), loc);
    } catch (...) {}
}

// ---- structured events (A8) --------------------------------------------------------
template <size_t N> struct Lit {
    char v[N];
    consteval Lit(const char (&s)[N]) { for (size_t i = 0; i < N; ++i) v[i] = s[i]; }
};
// Emits "[TAG vVER] " + body + "\n" in ONE writeRaw.  Ver < 0 -> "[TAG] " (unversioned).
template <Lit Tag, int Ver, class... A>
void event(Stream s, std::format_string<A...> f, A&&... a) noexcept {
    try {
        std::string& b = detail::tlBuf();
        b.clear();
        b.push_back('[');
        b.append(Tag.v);
        if constexpr (Ver >= 0) { b.append(" v"); b.append(std::to_string(Ver)); }
        b.append("] ");
        std::format_to(std::back_inserter(b), f, std::forward<A>(a)...);
        b.push_back('\n');
        writeRaw(s, b);
    } catch (...) {}
}

}  // namespace mc2::log

// ---- macros: check BEFORE evaluating format/args (A7) --------------------------------
#define MC2_LOG_IMPL_(stream, cat, lvl, ...)                                                  \
    do {                                                                                      \
        if constexpr (::mc2::log::Level::lvl >= ::mc2::log::kMinLevel) {                      \
            if (::mc2::log::enabled(::mc2::log::Cat::cat, ::mc2::log::Level::lvl))            \
                ::mc2::log::logFmt(::mc2::log::Cat::cat, ::mc2::log::Level::lvl,              \
                                   ::mc2::log::Stream::stream,                                \
                                   std::source_location::current(), __VA_ARGS__);             \
        }                                                                                     \
    } while (0)
#define MC2_LOG(cat, lvl, ...)     MC2_LOG_IMPL_(Err, cat, lvl, __VA_ARGS__)
#define MC2_LOG_OUT(cat, lvl, ...) MC2_LOG_IMPL_(Out, cat, lvl, __VA_ARGS__)
#define MC2_LOG_DBG(cat, lvl, ...) MC2_LOG_IMPL_(Dbg, cat, lvl, __VA_ARGS__)

// Runtime (non-literal) template: MC2_LOGV(cat, lvl, tmplStringView, args...)
#define MC2_LOGV(cat, lvl, ...)                                                               \
    do {                                                                                      \
        if constexpr (::mc2::log::Level::lvl >= ::mc2::log::kMinLevel) {                      \
            if (::mc2::log::enabled(::mc2::log::Cat::cat, ::mc2::log::Level::lvl))            \
                ::mc2::log::logRuntime(::mc2::log::Cat::cat, ::mc2::log::Level::lvl,          \
                                       ::mc2::log::Stream::Err,                               \
                                       std::source_location::current(), __VA_ARGS__);         \
        }                                                                                     \
    } while (0)

#define MC2_LOG_ONCE(cat, lvl, ...)                                                           \
    do {                                                                                      \
        static ::std::atomic<bool> mc2LogOnce_{false};                                        \
        if (::mc2::log::enabled(::mc2::log::Cat::cat, ::mc2::log::Level::lvl) &&              \
            !mc2LogOnce_.exchange(true, ::std::memory_order_relaxed))                         \
            MC2_LOG(cat, lvl, __VA_ARGS__);                                                   \
    } while (0)

#define MC2_LOG_EVERY(cat, lvl, hz, ...)                                                      \
    do {                                                                                      \
        if (::mc2::log::enabled(::mc2::log::Cat::cat, ::mc2::log::Level::lvl) &&              \
            ::mc2::log::allowRate(::mc2::log::siteKey(std::source_location::current()), (hz)))\
            MC2_LOG(cat, lvl, __VA_ARGS__);                                                   \
    } while (0)

#define MC2_LOG_EVERY_N_FRAMES(cat, lvl, n, ...)                                              \
    do {                                                                                      \
        if (::mc2::log::enabled(::mc2::log::Cat::cat, ::mc2::log::Level::lvl) &&              \
            (::mc2::log::frameNumber() % uint64_t(n)) == 0)                                   \
            MC2_LOG(cat, lvl, __VA_ARGS__);                                                   \
    } while (0)

#define MC2_LOG_TOPIC(cat, topic, lvl, ...)                                                   \
    do {                                                                                      \
        if (::mc2::log::topicEnabled(::mc2::log::Cat::cat, (topic)))                          \
            MC2_LOG(cat, lvl, __VA_ARGS__);                                                   \
    } while (0)

// Structured lines.  tag is a string literal, ver an int literal (use _NOVER for
// legacy unversioned tags).  Events bypass the level filter; they obey only the
// category toggle.  Stream matches the replaced call (Err = fprintf(stderr), Out = printf).
#define MC2_EVENT_IMPL_(stream, cat, tag, ver, ...)                                           \
    do {                                                                                      \
        if (::mc2::log::eventEnabled(::mc2::log::Cat::cat))                                   \
            ::mc2::log::event<tag, ver>(::mc2::log::Stream::stream, __VA_ARGS__);             \
    } while (0)
#define MC2_EVENT(cat, tag, ver, ...)           MC2_EVENT_IMPL_(Err, cat, tag, ver, __VA_ARGS__)
#define MC2_EVENT_OUT(cat, tag, ver, ...)       MC2_EVENT_IMPL_(Out, cat, tag, ver, __VA_ARGS__)
#define MC2_EVENT_NOVER(cat, tag, ...)          MC2_EVENT_IMPL_(Err, cat, tag, -1, __VA_ARGS__)
#define MC2_EVENT_NOVER_OUT(cat, tag, ...)      MC2_EVENT_IMPL_(Out, cat, tag, -1, __VA_ARGS__)
