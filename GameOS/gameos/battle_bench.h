// Buffered frame evidence, armed only by the test command channel.
#pragma once
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>

namespace mc2::battlebench {
using Clock = std::chrono::steady_clock;
inline FILE* output = nullptr;
inline uint64_t frame = 0;
inline unsigned ticks = 0;
inline double tickMs = 0;
inline bool attack = false;
inline double milliseconds(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
inline bool arm(const char* path) {
    if (output) return false;
    output = std::fopen(path, "w");
    if (!output) return false;
    std::setvbuf(output, nullptr, _IOFBF, 1024 * 1024);
    std::atexit([] { if (output) { std::fclose(output); output = nullptr; } });
    std::fprintf(output, "{\"type\":\"clock\",\"model\":\"mission_update_per_render_frame\"}\n");
    return true;
}
inline void beginFrame() { ++frame; ticks = 0; tickMs = 0; }
struct Tick {
    bool enabled = output != nullptr;
    Clock::time_point start = enabled ? Clock::now() : Clock::time_point{};
    ~Tick() {
        if (!enabled) return;
        const double ms = milliseconds(start);
        ++ticks; tickMs += ms;
        std::fprintf(output, "{\"type\":\"tick\",\"frame\":%llu,\"ms\":%.6f}\n",
                     static_cast<unsigned long long>(frame), ms);
    }
};
inline void endFrame(double ms) {
    if (output) std::fprintf(output,
        "{\"type\":\"cpu\",\"frame\":%llu,\"ms\":%.6f,\"ticks\":%u,\"tick_total_ms\":%.6f}\n",
        static_cast<unsigned long long>(frame), ms, ticks, tickMs);
}
} // namespace mc2::battlebench
