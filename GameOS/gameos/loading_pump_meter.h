#pragma once
#include <cstdint>
#include <algorithm>
#include <string_view>

// Window-thread accounting, independent of SDL so boundary/throttle rules can be
// proved offline. A checkpoint is credited only after actual event service.
namespace mc2::loadpump {
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
// Workers cannot mutate the window thread's scopes or throttle timestamp.
inline thread_local Meter meter;
}
