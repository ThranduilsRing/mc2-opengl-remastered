#pragma once
// Per-destination cache for descriptor writes/copies whose inputs did not change (VK descriptor plan, Astra step 1).
// A stamp belongs to ONE destination (set + binding, or set + binding group). It is keyed on what the write would
// put there (handles, layout, source revision, dummy-vs-live mode) plus the global descriptor epoch; the epoch is
// bumped at every lifetime event that can recycle a handle value (set allocation, ring growth, shadow island or bake
// product replacement, mission reset), so a recycled handle never matches a stale key. Callers consult a stamp only
// at the existing write points, after the existing slot waits: the cache never makes a write earlier, it only skips one.
// Pure logic, no Vulkan: tests/unit/test_vk_desc_write_cache.cpp.
#include <array>
#include <atomic>
#include <cstdint>
#include <type_traits>

namespace mc2vk {

inline std::atomic<uint64_t>& descEpochCounter() { static std::atomic<uint64_t> e{1}; return e; }
inline uint64_t descEpoch() { return descEpochCounter().load(std::memory_order_acquire); }
inline void bumpDescEpoch() { descEpochCounter().fetch_add(1, std::memory_order_acq_rel); }

inline uint64_t nextDescRevision() { static std::atomic<uint64_t> r{0}; return r.fetch_add(1, std::memory_order_relaxed) + 1; }

using DescKey = std::array<uint64_t, 4>;

// A Vulkan handle (pointer or uint64 typedef) or enum as a key word.
template <class T> uint64_t descKeyWord(T v) {
    if constexpr (std::is_pointer_v<T>) return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(v));
    else return static_cast<uint64_t>(v);
}

struct DescWriteStamp {
    DescKey key{};
    uint64_t epoch = 0;
    uint64_t rev = 0;      // changes whenever the destination's content changes (a consumer can key on it)
    bool valid = false;

    // true = the caller must issue the write now (the stamp already records it as written).
    bool needsWrite(const DescKey& k, uint64_t ep) {
        if (valid && epoch == ep && key == k) return false;
        key = k; epoch = ep; valid = true; rev = nextDescRevision();
        return true;
    }
    bool needsWrite(const DescKey& k) { return needsWrite(k, descEpoch()); }
    void invalidate() { valid = false; rev = nextDescRevision(); }
};

// Write counter: issued = descriptors written/copied, skipped = writes the stamps avoided. issued+skipped is what the
// uncached path issued per frame. Reported by mc2vk::reportDescWrites() once per frame.
struct DescWriteCounts {
    std::atomic<uint64_t> issued{0};
    std::atomic<uint64_t> skipped{0};
};
inline DescWriteCounts& descWriteCounts() { static DescWriteCounts c; return c; }
inline void noteDescWrites(uint32_t issued, uint32_t skipped) {
    if (issued) descWriteCounts().issued.fetch_add(issued, std::memory_order_relaxed);
    if (skipped) descWriteCounts().skipped.fetch_add(skipped, std::memory_order_relaxed);
}
// Tracy plot every frame; [VK_DESC_WRITES v1] every 600 frames when MC2_DIAG_TAGS has VK_DESC_WRITES (vulkan_mover_shadow.cpp).
void reportDescWrites();

}  // namespace mc2vk
