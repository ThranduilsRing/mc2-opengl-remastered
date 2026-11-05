// RenderCore/FrameArena.h
//
// FrameArena: typed linear allocator for L2 (frame-lifetime) records.
// Non-owning: caller provides the backing buffer via init().
// Invariants: reset exactly once per frame; no individual free();
// all allocated types must be trivially destructible.
// Overflow fails closed: returns null/empty and increments overflowCount.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace RenderCore {

struct FrameArenaStats {
    uint32_t frameIndex     = 0;
    size_t   capacityBytes  = 0;
    size_t   usedBytes      = 0;
    size_t   highWaterBytes = 0;  // lifetime peak; persists across resets
    uint32_t allocCount     = 0;
    uint32_t overflowCount  = 0;
};

// Span<T>: non-owning view over a contiguous array of T.
// count is uint32_t; the caller supplies the per-slot byte budget.
template <typename T>
struct Span {
    T*       data  = nullptr;
    uint32_t count = 0;

    Span() = default;
    Span(T* d, uint32_t c) : data(d), count(c) {}

    uint32_t size()  const { return count; }
    bool     empty() const { return data == nullptr || count == 0; }

    T*       begin()       { return data; }
    T*       end()         { return data + count; }
    const T* begin() const { return data; }
    const T* end()   const { return data + count; }

    T&       operator[](uint32_t i)       { return data[i]; }
    const T& operator[](uint32_t i) const { return data[i]; }
};

// FrameArena: bump allocator over an externally-owned byte buffer.
// Call init() once with the backing buffer, then reset() at the start of each
// frame. allocArray<T>() carves out aligned arrays; returns empty Span on
// overflow. stats().highWaterBytes tracks the peak across all resets.
class FrameArena {
public:
    void init(void* memory, size_t capacityBytes) {
        base_     = static_cast<uint8_t*>(memory);
        capacity_ = capacityBytes;
        offset_   = 0;
        stats_    = {};
        stats_.capacityBytes = capacityBytes;
    }

    // Reset for the new frame. Clears per-frame counters; highWaterBytes persists.
    void reset(uint32_t frameIndex) {
        offset_              = 0;
        stats_.frameIndex    = frameIndex;
        stats_.usedBytes     = 0;
        stats_.allocCount    = 0;
        stats_.overflowCount = 0;
    }

    void* allocBytes(size_t bytes, size_t alignment, const char* /*tag*/) {
        if (!base_ || bytes == 0) return nullptr;
        size_t aligned = (offset_ + alignment - 1u) & ~(alignment - 1u);
        if (aligned + bytes > capacity_) {
            ++stats_.overflowCount;
            return nullptr;
        }
        void* p  = base_ + aligned;
        offset_  = aligned + bytes;
        ++stats_.allocCount;
        stats_.usedBytes = offset_;
        stats_.highWaterBytes = (std::max)(stats_.highWaterBytes, offset_);
        return p;
    }

    template <typename T>
    Span<T> allocArray(uint32_t count, const char* tag) {
        static_assert(std::is_trivially_destructible<T>::value,
                      "FrameArena only supports trivially-destructible records");
        if (count == 0) return {};
        void* p = allocBytes(static_cast<size_t>(count) * sizeof(T), alignof(T), tag);
        if (!p) return {};
        return Span<T>(static_cast<T*>(p), count);
    }

    const FrameArenaStats& stats()     const { return stats_; }
    bool                   overflowed() const { return stats_.overflowCount != 0; }

    size_t bytesUsed()  const { return stats_.usedBytes; }
    size_t capacity()   const { return capacity_; }
    size_t remaining()  const { return capacity_ - offset_; }

private:
    uint8_t*        base_     = nullptr;
    size_t          capacity_ = 0;
    size_t          offset_   = 0;
    FrameArenaStats stats_{};
};

} // namespace RenderCore
