// mc2log.cpp -- logging facade implementation (see mc2log.h).
#include "mc2log.h"
#include "Gates/gate.h"
#include "Gates/gate_cold.h"   // str<>()

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>

// Installed by GameOS at startup; standalone tools keep the logger independent.
static std::atomic<void (*)(const char*)> crashSink{nullptr};
extern "C" void mc2_log_set_crash_sink(void (*sink)(const char*)) { crashSink.store(sink); }

#ifdef _WIN32
extern "C" __declspec(dllimport) void __stdcall OutputDebugStringA(const char*);
#endif

namespace mc2::log {

namespace {

constexpr const char* kCatNames[kCatCount] = {
    "core", "io", "render", "vk", "gl", "terrain", "water", "mech", "veg", "ai",
    "sim", "ui", "audio", "net", "shader", "perf", "smoke", "assets", "editor"};
constexpr const char* kLevelNames[] = {"trace", "debug", "info", "warn", "error", "fatal", "off"};

std::atomic<std::FILE*> g_override[3] = {};   // Err, Out, Dbg (Dbg unused)

std::FILE* fileFor(Stream s) noexcept {
    if (s == Stream::Dbg) return nullptr;
    std::FILE* o = g_override[static_cast<size_t>(s)].load(std::memory_order_acquire);
    if (o) return o;
    return s == Stream::Out ? stdout : stderr;
}

// Topic table (rare path; empty by default so topicEnabled() is one atomic load).
std::mutex g_topicMu;
std::vector<ParsedSpec::Topic>& topicTable() { static std::vector<ParsedSpec::Topic> t; return t; }

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}
bool ieq(std::string_view a, std::string_view b) {
    size_t n = b.size();
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; ++i) {
        char c = a[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != b[i]) return false;
    }
    return true;
}
std::optional<Level> parseLevel(std::string_view s) {
    for (size_t i = 0; i < sizeof(kLevelNames) / sizeof(kLevelNames[0]); ++i)
        if (ieq(s, kLevelNames[i])) return Level(i);
    return std::nullopt;
}
std::optional<Cat> parseCat(std::string_view s) {
    for (size_t i = 0; i < kCatCount; ++i)
        if (ieq(s, kCatNames[i])) return Cat(i);
    return std::nullopt;
}

// ---- rate limiter / once table ----
struct Slot {
    std::atomic<uint64_t> key{0};
    std::atomic<int64_t>  next{0};
};
constexpr size_t kSlots = 512;
constexpr size_t kProbe = 8;
Slot g_slots[kSlots];

Slot* claimSlot(uint64_t key) noexcept {
    if (key == 0) key = 1;
    size_t h = static_cast<size_t>((key * 0x9E3779B97F4A7C15ull) >> 40) % kSlots;
    for (size_t p = 0; p < kProbe; ++p) {
        Slot& s = g_slots[(h + p) % kSlots];
        uint64_t k = s.key.load(std::memory_order_acquire);
        if (k == key) return &s;
        if (k == 0) {
            uint64_t expect = 0;
            if (s.key.compare_exchange_strong(expect, key, std::memory_order_acq_rel) || expect == key)
                return &s;
        }
    }
    return nullptr;   // table congested: callers fail open
}

std::atomic<uint64_t> g_frame{0};

}  // namespace

const char* catName(Cat c) noexcept { return static_cast<size_t>(c) < kCatCount ? kCatNames[static_cast<size_t>(c)] : "?"; }
const char* levelName(Level l) noexcept { return static_cast<size_t>(l) < 7 ? kLevelNames[static_cast<size_t>(l)] : "?"; }

bool streamsLiveFor(bool envSet) noexcept { return envSet; }

ParsedSpec parseSpec(std::string_view spec) {
    ParsedSpec r;
    try {
        if (spec.find(':') == std::string_view::npos && spec.find('=') == std::string_view::npos)
            return r;   // legacy ("1", "", anything else): no overrides
        r.isSpec = true;
        size_t pos = 0;
        while (pos <= spec.size()) {
            size_t comma = spec.find(',', pos);
            std::string_view tok = trim(spec.substr(pos, comma == std::string_view::npos
                                                           ? std::string_view::npos : comma - pos));
            pos = (comma == std::string_view::npos) ? spec.size() + 1 : comma + 1;
            if (tok.empty()) continue;
            size_t sep = tok.find_first_of(":=");
            if (sep == std::string_view::npos) { ++r.errors; continue; }
            std::string_view cs = trim(tok.substr(0, sep));
            std::string_view rhs = trim(tok.substr(sep + 1));
            const bool all = (cs == "*");
            const std::optional<Cat> parsedCat = all ? std::optional<Cat>(Cat::Core) : parseCat(cs);
            if (!parsedCat || rhs.empty()) { ++r.errors; continue; }
            const Cat cat = *parsedCat;
            if (const std::optional<Level> parsedLv = parseLevel(rhs)) {
                const Level lv = *parsedLv;
                auto apply = [&](size_t i) {
                    r.levelSet[i] = true; r.level[i] = lv; r.eventOff[i] = (lv == Level::Off);
                };
                if (all) for (size_t i = 0; i < kCatCount; ++i) apply(i);
                else apply(static_cast<size_t>(cat));
            } else if (tok[sep] == '=' && !all) {
                // cat=topic[+topic...]
                size_t tp = 0;
                while (tp <= rhs.size()) {
                    size_t plus = rhs.find('+', tp);
                    std::string_view t = trim(rhs.substr(tp, plus == std::string_view::npos
                                                                ? std::string_view::npos : plus - tp));
                    tp = (plus == std::string_view::npos) ? rhs.size() + 1 : plus + 1;
                    if (!t.empty()) r.topics.push_back({cat, std::string(t)});
                }
            } else {
                ++r.errors;
            }
        }
    } catch (...) { ++r.errors; }
    return r;
}

static void applyParsed(const ParsedSpec& p) {
    for (size_t i = 0; i < kCatCount; ++i) {
        if (p.levelSet[i]) detail::g_threshold[i].store(p.level[i], std::memory_order_relaxed);
        if (p.eventOff[i]) detail::g_eventOn[i].store(false, std::memory_order_relaxed);
        else if (p.levelSet[i]) detail::g_eventOn[i].store(true, std::memory_order_relaxed);
    }
    if (!p.topics.empty()) {
        std::lock_guard<std::mutex> lk(g_topicMu);
        auto& t = topicTable();
        for (const auto& x : p.topics) t.push_back(x);
        detail::g_topicCount.store(static_cast<int>(t.size()), std::memory_order_release);
    }
}

void setSpec(std::string_view spec) {
    try { applyParsed(parseSpec(spec)); } catch (...) {}
}

bool topicEnabled(Cat c, std::string_view topic) noexcept {
    if (detail::g_topicCount.load(std::memory_order_acquire) == 0) return false;
    try {
        std::lock_guard<std::mutex> lk(g_topicMu);
        for (const auto& t : topicTable())
            if (t.cat == c && t.name == topic) return true;
    } catch (...) {}
    return false;
}

bool init() noexcept {
    const bool logSet = mc2::gate::isSet<"MC2_LOG">();
    const std::string logText = logSet ? mc2::gate::str<"MC2_LOG">() : std::string();
    const bool live = streamsLiveFor(logSet);
    detail::g_live.store(live, std::memory_order_relaxed);
    try {
        if (mc2::gate::isSet<"MC2_LOG_DECORATE">()) {
            const std::string d = mc2::gate::str<"MC2_LOG_DECORATE">();
            detail::g_decorate.store(!d.empty() && d[0] == '1', std::memory_order_relaxed);
        }
        if (logSet) {
            ParsedSpec p = parseSpec(std::string_view(logText));
            applyParsed(p);
            if (p.errors > 0) {
                std::string m = "[MC2_LOG] ignored " + std::to_string(p.errors) + " invalid spec token(s)\n";
                writeRaw(Stream::Err, m);
            }
        }
    } catch (...) {}
    return live;
}

void writeRaw(Stream s, std::string_view line) noexcept {
    if (s == Stream::Dbg) {
#ifdef _WIN32
        char buf[1024];
        while (!line.empty()) {
            size_t n = line.size() < sizeof(buf) - 1 ? line.size() : sizeof(buf) - 1;
            std::memcpy(buf, line.data(), n);
            buf[n] = '\0';
            OutputDebugStringA(buf);
            line.remove_prefix(n);
        }
        return;
#else
        s = Stream::Err;
#endif
    }
    std::FILE* f = fileFor(s);
    if (f && !line.empty()) std::fwrite(line.data(), 1, line.size(), f);
}

void write(Cat c, Level l, Stream s, std::string_view text, const std::source_location&) noexcept {
    try {
        std::string line;   // text may alias the thread_local buffer, so build separately
        if (detail::g_decorate.load(std::memory_order_relaxed)) {
            line.append("[").append(levelName(l)).append(" ").append(catName(c)).append("] ");
        }
        line.append(text);
        line.push_back('\n');
        if (auto sink = crashSink.load(std::memory_order_relaxed)) sink(line.c_str());
        writeRaw(s, line);
    } catch (...) {}
}

void vlog(Cat c, Level l, Stream s, const std::source_location& loc,
          std::string_view fmt, std::format_args args) noexcept {
    try {
        std::string& b = detail::tlBuf();
        b.clear();
        std::vformat_to(std::back_inserter(b), fmt, args);
        write(c, l, s, b, loc);
    } catch (const std::exception& e) {
        detail::reportFormatError(c, l, s, fmt, e.what(), loc);
    } catch (...) {}
}

void vevent(Stream s, std::string_view tag, int ver, std::string_view fmt,
            std::format_args args) noexcept {
    try {
        std::string& b = detail::tlBuf();
        b.clear();
        b.push_back('[');
        b.append(tag);
        if (ver >= 0) { b.append(" v"); b.append(std::to_string(ver)); }
        b.append("] ");
        std::vformat_to(std::back_inserter(b), fmt, args);
        b.push_back('\n');
        writeRaw(s, b);
    } catch (...) {}
}

void setStreamOverrideForTest(Stream s, std::FILE* f) noexcept {
    if (static_cast<size_t>(s) < 3) g_override[static_cast<size_t>(s)].store(f, std::memory_order_release);
}

bool allowRateAt(uint64_t key, double perSecond, int64_t nowNs) noexcept {
    if (!(perSecond > 0.0)) return false;
    Slot* s = claimSlot(key);
    if (!s) return true;   // fail open
    const int64_t interval = static_cast<int64_t>(1e9 / perSecond);
    int64_t next = s->next.load(std::memory_order_relaxed);
    for (;;) {
        if (next != 0 && nowNs < next) return false;
        if (s->next.compare_exchange_weak(next, nowNs + interval, std::memory_order_relaxed)) return true;
    }
}

bool allowRate(uint64_t key, double perSecond) noexcept {
    const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count() + 1;
    return allowRateAt(key, perSecond, now);
}

bool allowOnce(uint64_t key) noexcept {
    Slot* s = claimSlot(key ^ 0xA5A5A5A5DEADBEEFull);
    if (!s) return true;
    int64_t expect = 0;
    return s->next.compare_exchange_strong(expect, 1, std::memory_order_relaxed);
}

void tickFrame() noexcept { g_frame.fetch_add(1, std::memory_order_relaxed); }
uint64_t frameNumber() noexcept { return g_frame.load(std::memory_order_relaxed); }

}  // namespace mc2::log
