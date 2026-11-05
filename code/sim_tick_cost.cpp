#include "sim_tick_cost.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>
#include "mc2fmt.h"

void mc2::diagnostic::TickCost::begin() {
    if (enabled_ && !running_) {
        started_ = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        running_ = true;
    }
}
void mc2::diagnostic::TickCost::end() {
    if (!running_) return;
    const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    milliseconds_ += static_cast<double>(now - started_) / 1000000.0;
    running_ = false;
}
mc2::diagnostic::TickCost::~TickCost() {
    if (!enabled_) return;
    end();
    static std::vector<double> samples;
    samples.push_back(milliseconds_);
    if (samples.size() < 180) return;
    double total = 0;
    for (auto value : samples) total += value;
    std::sort(samples.begin(), samples.end());
    mc2::fmt::print_out("[SIM_TICK_COST v1] ticks={:d} avg_ms={:.4f} p95_ms={:.4f} p99_ms={:.4f} max_ms={:.4f}\n",
        mc2::fmt::as_size(samples.size()), total / samples.size(), samples[samples.size() * 95 / 100],
        samples[samples.size() * 99 / 100], samples.back());
    std::fflush(stdout);
    samples.clear();
}
