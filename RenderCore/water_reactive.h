#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mdspan>

namespace mc2::waterreactive {
constexpr uint32_t kCapacity = 128;
constexpr uint32_t kTileSide = 32;
constexpr uint32_t kTiles = kTileSide * kTileSide;
constexpr uint32_t kPerTile = 8;
constexpr float kTileWidth = 256.0f;
constexpr float kReach = 224.0f;
constexpr float kLifetime = 3.0f;

// std430, binding 13 of the water material. XY is MC2 east/north (Z up).
struct alignas(16) Stamp {
    std::array<float, 4> positionRadiusAge{};
    std::array<float, 4> velocityType{};
};
struct alignas(16) GpuFrame {
    std::array<Stamp, kCapacity> stamps{};
    std::array<uint32_t, kTiles> counts{};
    std::array<uint32_t, kTiles * kPerTile> indices{};
};
static_assert(sizeof(Stamp) == 32 && offsetof(Stamp, velocityType) == 16);
static_assert(offsetof(GpuFrame, counts) == 4096 && offsetof(GpuFrame, indices) == 8192);
static_assert(sizeof(GpuFrame) == 40960);

inline uint32_t tile(int x, int y) {
    return (static_cast<uint32_t>(y) & (kTileSide - 1)) * kTileSide
         + (static_cast<uint32_t>(x) & (kTileSide - 1));
}

// Main presentation thread only. Clock/mission resets discard cosmetic history.
class Buffer {
    struct Event { Stamp stamp{}; uintptr_t owner = 0; };
    struct Source {
        uintptr_t key = 0;
        float x = 0, y = 0, seen = 0, emitted = -10;
    };
    std::array<Event, kCapacity> events_{};
    std::array<Source, kCapacity> sources_{};
    uint32_t count_ = 0;
    float time_ = -1;
    uint32_t submits_ = 0, wetVisible_ = 0, emitted_ = 0, wet_ = 0, vis_ = 0;
    float firstX_ = 0, firstY_ = 0, firstVx_ = 0, firstVy_ = 0;

public:
    void clear() { count_ = 0; time_ = -1; sources_ = {}; }
    uint32_t size() const { return count_; }
    // Diagnostics for the once-per-second [WATER_REACTIVE v1] log; reset by takeStats.
    struct Stats { uint32_t submits, wetVisible, emitted, wet, vis; float x, y, vx, vy; };
    Stats takeStats() { Stats s{submits_, wetVisible_, emitted_, wet_, vis_, firstX_, firstY_, firstVx_, firstVy_}; submits_ = wetVisible_ = emitted_ = wet_ = vis_ = 0; return s; }
    void advance(float now) {
        if (!std::isfinite(now)) return;
        if (time_ >= 0 && (now < time_ || now - time_ > 5.0f)) clear();
        const float dt = time_ < 0 ? 0 : now - time_;
        time_ = now;
        uint32_t live = 0;
        for (uint32_t i = 0; i < count_; ++i) {
            events_[i].stamp.positionRadiusAge[3] += dt;
            if (events_[i].stamp.positionRadiusAge[3] < kLifetime)
                events_[live++] = events_[i];
        }
        count_ = live;
    }

    void submit(uintptr_t key, float now, float x, float y, float radius,
                bool wet, bool visible, bool vehicle) {
        if (!key || !std::isfinite(now) || !std::isfinite(x) || !std::isfinite(y)
            || !std::isfinite(radius)) return;
        advance(now);
        ++submits_; wet_ += wet; vis_ += visible;
        auto source = std::find_if(sources_.begin(), sources_.end(),
            [key](const Source& s) { return s.key == key; });
        if (!visible) {
            uint32_t live = 0;
            for (uint32_t i = 0; i < count_; ++i)
                if (events_[i].owner != key) events_[live++] = events_[i];
            count_ = live;
        }
        if (!wet || !visible) {
            if (source != sources_.end()) *source = {};
            return;
        }
        if (source == sources_.end()) {
            source = std::min_element(sources_.begin(), sources_.end(),
                [](const Source& a, const Source& b) { return a.seen < b.seen; });
            // Prefer an empty entry even when time is zero.
            const auto empty = std::find_if(sources_.begin(), sources_.end(),
                [](const Source& s) { return s.key == 0; });
            if (empty != sources_.end()) source = empty;
            *source = {key, x, y, now, -10};
        }
        ++wetVisible_;
        const float dt = now - source->seen;
        const float dx = x - source->x, dy = y - source->y;
        const bool discontinuity = dt > 0.5f || dx * dx + dy * dy > 256.0f * 256.0f;
        float vx = 0, vy = 0;
        if (dt > 0 && !discontinuity) {
            vx = dx / dt; vy = dy / dt;
            const float scale = std::clamp(200.0f / std::max(std::hypot(vx, vy), 0.001f), 0.0f, 1.0f);
            vx *= scale; vy *= scale;
        }
        source->x = x; source->y = y; source->seen = now;
        if (discontinuity) source->emitted = now; // No connecting wake on teleport/reappearance.
        if (now - source->emitted < (vehicle ? 0.25f : 0.55f)) return;
        source->emitted = now;
        ++emitted_; firstX_ = x; firstY_ = y; firstVx_ = vx; firstVy_ = vy;
        if (count_ == kCapacity) {
            for (uint32_t i = 1; i < count_; ++i) events_[i - 1] = events_[i];
            --count_;
        }
        events_[count_++] = {Stamp{{x, y, std::clamp(radius, 8.0f, 40.0f), 0},
                                  {vx, vy, vehicle ? 1.0f : 0.0f, 0}}, key};
    }

    void snapshot(float now, GpuFrame& out) {
        advance(now);
        out = {};
        std::mdspan<uint32_t, std::extents<std::size_t, kTiles, kPerTile>> lists(out.indices.data());
        for (uint32_t i = 0; i < count_; ++i) {
            out.stamps[i] = events_[i].stamp;
            const auto& p = out.stamps[i].positionRadiusAge;
            const int x0 = static_cast<int>(std::floor((p[0] - kReach) / kTileWidth));
            const int x1 = static_cast<int>(std::floor((p[0] + kReach) / kTileWidth));
            const int y0 = static_cast<int>(std::floor((p[1] - kReach) / kTileWidth));
            const int y1 = static_cast<int>(std::floor((p[1] + kReach) / kTileWidth));
            for (int y = y0; y <= y1; ++y) for (int x = x0; x <= x1; ++x) {
                const auto t = tile(x, y);
                auto& n = out.counts[t];
                if (n == kPerTile) {
                    for (uint32_t j = 1; j < n; ++j)
                        lists[t, j - 1] = lists[t, j];
                    --n;
                }
                lists[t, n++] = i;
            }
        }
    }
};

inline Buffer& buffer() { static Buffer value; return value; }
} // namespace mc2::waterreactive
