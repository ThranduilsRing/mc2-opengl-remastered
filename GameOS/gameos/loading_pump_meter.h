#pragma once
#include <cstdint>
#include <algorithm>
#include <string_view>

// Window-thread accounting, independent of SDL so boundary/throttle rules can be
// proved offline. A checkpoint is credited only after actual event service.
namespace mc2::loadpump {
struct CursorThrottle {
    std::uint64_t last = 0;
    bool applied = false;
    bool due(std::uint64_t now) {
        if (applied && now - last < 50) return false;
        last = now;
        applied = true;
        return true;
    }
};
struct Scope {
    bool active = false;
    bool attempted = false;
    std::uint64_t last = 0, maxGap = 0, services = 0;
    std::string_view phase = "start";
    std::string_view worst = "start";
    void begin(std::uint64_t now, std::string_view name, bool headless) {
        *this = {};
        active = true;
        attempted = headless;
        last = now;
        phase = worst = name;
    }
    void gap(std::uint64_t now) {
        const auto previous = maxGap;
        maxGap = std::max(maxGap, now - last);
        if (maxGap != previous) worst = phase;
    }
    void service(std::uint64_t now, bool attempt) {
        if (!active || attempted != attempt) return;
        gap(now);
        last = now;
        ++services;
    }
    void mark(std::uint64_t now, std::string_view name) {
        if (!active) return;
        gap(now); // A phase mark must not reset the last actual service.
        phase = name;
    }
    void end(std::uint64_t now) {
        if (active) gap(now); // Include last service -> scope end, even with zero services.
        active = false;
    }
};
struct Meter {
    Scope mission, variants;
    std::uint64_t lastService = 0;
    bool hasService = false;
    bool due(std::uint64_t now) const { return !hasService || now - lastService >= 50; }
    void service(std::uint64_t now, bool attempted) {
        if (!attempted) { lastService = now; hasService = true; }
        mission.service(now, attempted);
        variants.service(now, attempted);
    }
};
// MAIN_STALL watchdog, fed the gap since the window thread last pumped: Begin once when a stall begins (gap >= limit), End once when pumping
// resumes, else None. `gap` is the value to report: the starting gap for Begin, the longest gap seen for End.
enum class StallEdge { None, Begin, End };
struct StallWatch {
    std::uint64_t worst = 0, gap = 0;
    StallEdge step(std::uint64_t now_gap, std::uint64_t limit = 4000) {
        if (now_gap >= limit) {
            const bool fresh = worst == 0;
            worst = std::max(worst, now_gap);
            gap = now_gap;
            return fresh ? StallEdge::Begin : StallEdge::None;
        }
        if (worst == 0) return StallEdge::None;
        gap = worst;
        worst = 0;
        return StallEdge::End;
    }
};
// Workers cannot mutate the window thread's scopes or throttle timestamp.
inline thread_local Meter meter;
}
