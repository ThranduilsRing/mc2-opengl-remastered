// RenderCore/HandleSlots.h -- slot allocator behind RenderObjectHandle (shared-foundations P3).
//
// A slot is allocated with a generation; release() retires it, bumps the generation and parks the slot on a free list stamped with the frame whose GPU work
// may still name the slot's index (its object-ID pixels, later its rows in the persistent instance tables). alloc() reuses a parked slot only after the
// engine reports that frame completed, so an index is never handed to a new object while the GPU can still be reading the old one. A stale handle (a
// generation that is no longer the slot's) is refused everywhere. Generations are the 12 bits a Handle carries and skip 0 on wrap (0 with index 0 is the
// invalid handle).
//
// Pure standard C++: no engine type, so a doctest drives it with a hand-advanced completed-frame counter.
#pragma once

#include <cstdint>
#include <deque>
#include <vector>

namespace RenderCore {

class HandleSlotTable {
public:
    static constexpr uint32_t kMaxGeneration = 0xFFFu;

    struct Alloc {
        uint32_t slot = 0;
        uint32_t generation = 0;
        bool     ok = false;
    };

    // `capacity` slots at most (the range the handle kind owns).
    void init(uint32_t capacity) { reset(); capacity_ = capacity; }
    bool initialized() const { return capacity_ != 0; }

    // Everything free again, every generation back to 1 (a mission boundary: no GPU work of the old mission may name these indices any more).
    void reset() {
        gen_.clear();
        live_.clear();
        parked_.clear();
        liveCount_ = 0;
    }

    // A free slot: the oldest parked one whose retire frame completed, else a fresh one. Not ok when the range is exhausted.
    Alloc alloc(uint64_t completedFrame) {
        Alloc a;
        if (!parked_.empty() && parked_.front().retireFrame <= completedFrame) {
            a.slot = parked_.front().slot;
            parked_.pop_front();
        } else if (gen_.size() < capacity_) {
            a.slot = static_cast<uint32_t>(gen_.size());
            gen_.push_back(1u);
            live_.push_back(false);
        } else {
            return a;
        }
        live_[a.slot] = true;
        ++liveCount_;
        a.generation = gen_[a.slot];
        a.ok = true;
        return a;
    }

    // Retires a live slot. False for a stale or already retired handle (nothing changes). `retireFrame` is the frame during which the last GPU work that
    // can name the slot was recorded; the slot is reusable once the engine reports that frame completed.
    bool release(uint32_t slot, uint32_t generation, uint64_t retireFrame) {
        if (!isLive(slot, generation)) return false;
        live_[slot] = false;
        --liveCount_;
        uint32_t g = gen_[slot] + 1u;
        if (g > kMaxGeneration) g = 1u;
        gen_[slot] = g;
        parked_.push_back({slot, retireFrame});
        return true;
    }

    bool isLive(uint32_t slot, uint32_t generation) const {
        return slot < gen_.size() && live_[slot] && gen_[slot] == generation;
    }
    // The generation the slot carries now (the one a new allocation of it would get).
    uint32_t generationOf(uint32_t slot) const { return slot < gen_.size() ? gen_[slot] : 0u; }

    uint32_t liveCount() const { return liveCount_; }
    uint32_t parkedCount() const { return static_cast<uint32_t>(parked_.size()); }
    uint32_t highWater() const { return static_cast<uint32_t>(gen_.size()); }

private:
    struct Parked { uint32_t slot; uint64_t retireFrame; };
    uint32_t              capacity_ = 0;
    std::vector<uint32_t> gen_;
    std::vector<bool>     live_;
    std::deque<Parked>    parked_;
    uint32_t              liveCount_ = 0;
};

}  // namespace RenderCore
