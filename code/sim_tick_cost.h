#pragma once
#include <cstdint>

namespace mc2::diagnostic {
// Output-only instrumentation: timings never return to gameplay callers.
class TickCost {
    bool enabled_, running_ = false;
    std::uint64_t started_ = 0;
    double milliseconds_ = 0;
public:
    explicit TickCost(bool enabled) : enabled_(enabled) {}
    void begin();
    void end();
    ~TickCost();
};
}
