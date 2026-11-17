#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <future>
#include <utility>

namespace mc2::terrain {
// Only for common fragment UBOs produced by fill_frame_ubo: lighting/detail
// producer constraints also govern road shading outside the lighting block.
// Two bits per field preserve 0/1/2 readiness, 0/1/3 detail and 0/2 visualMode.
struct TerrainSpecKey {
    uint32_t bits = 0;
    std::array<int32_t, 12> values() const {
        std::array<int32_t, 12> out{};
        for (unsigned i = 0; i < out.size(); ++i) out[i] = (bits >> (2 * i)) & 3u;
        return out;
    }
    bool operator==(TerrainSpecKey b) const { return bits == b.bits; }
    bool operator!=(TerrainSpecKey b) const { return !(*this == b); }
};
template<class Ubo> TerrainSpecKey terrainSpecKey(const Ubo& u) {
    std::array<int32_t, 12> v{{u.colormapReady, u.useCement, u.useTransitionMask,
        u.cementDiagConnect, u.lightingEnabled, u.enableShadows, u.detailEnabled,
        u.detailReady, u.concreteDetail, u.visualMode, u.shorelineOn, u.shorelineHasMask}};
    if (!v[1]) v[2] = v[3] = 0;
    if (!v[4]) v[5] = v[6] = v[7] = v[8] = v[11] = 0;
    if (!v[6]) v[7] = v[8] = 0;
    if (!v[10]) v[11] = 0;
    TerrainSpecKey key;
    for (unsigned i = 0; i < v.size(); ++i) key.bits |= static_cast<uint32_t>(v[i]) << (2 * i);
    return key;
}

// Render-owner table; the worker owns its callable and returns an owned pipe.
// Completion never selects a pipeline: selection uses this frame's exact key.
// Destruction drains before the island's shader/layout/device owners disappear.
template<class Pipe> class TerrainSpecVariants {
    struct Entry { TerrainSpecKey key; Pipe pipe; uint64_t used = 0; };
    struct Result { TerrainSpecKey key; uint64_t generation; Pipe pipe; };
    std::array<Entry, 4> entries_{};
    std::future<Result> pending_;
    TerrainSpecKey observed_{};
    unsigned stable_ = 0;
    uint64_t clock_ = 0, generation_ = 0;
    bool failed_ = false;
public:
    ~TerrainSpecVariants() { drain(); }
    void drain() { if (pending_.valid()) { try { pending_.get(); } catch (...) {} } }
    // Never blocks; completion from an old generation is discarded.
    void invalidate() { ++generation_; stable_ = 0; }
    bool compiling() const { return pending_.valid(); }
    bool failed() const { return failed_; }
    template<class Compile, class Retire>
    Pipe* select(TerrainSpecKey key, bool enabled, Compile compile, Retire retire) {
        ++clock_;
        if (pending_.valid() && pending_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try {
                auto result = pending_.get();
                if (result.generation == generation_) {
                    if (!result.pipe) failed_ = true;
                    else {
                        auto* dest = &entries_[0];
                        for (auto& e : entries_) if (!e.pipe || (dest->pipe && e.used < dest->used)) dest = &e;
                        if (dest->pipe) retire(std::move(dest->pipe));
                        dest->key = result.key; dest->pipe = std::move(result.pipe); dest->used = clock_;
                    }
                }
            } catch (...) { failed_ = true; }
        }
        if (!stable_ || key != observed_) { observed_ = key; stable_ = 1; }
        else if (stable_ < 30) ++stable_;
        if (!enabled) return nullptr;
        for (auto& e : entries_) if (e.pipe && e.key == key) { e.used = clock_; return &e.pipe; }
        if (stable_ >= 30 && !failed_ && !pending_.valid()) {
            const auto generation = generation_;
            try {
                pending_ = std::async(std::launch::async, [key, generation, compile = std::move(compile)]() mutable {
                    return Result{key, generation, compile()};
                });
            } catch (...) { failed_ = true; }
        }
        return nullptr;
    }
};
} // namespace mc2::terrain
