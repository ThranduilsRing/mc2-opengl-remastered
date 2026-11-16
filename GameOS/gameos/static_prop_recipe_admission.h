#pragma once
#include "../../RenderWorld/RenderWorld.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace GpuStaticPropRegistry {
// Admission owns the exhaustion episode; storage still owns append/LIFO reuse.
class StaticPropRecipeAdmission {
public:
    template<class Emit>
    int32_t admit(bool eligible, size_t rangeCount, bool recycle,
                  const std::vector<uint32_t>& freeList, Emit emit) {
        if (!eligible) return -1;
        const size_t index = recycle && !freeList.empty() ? freeList.back() : rangeCount;
        if (RenderWorld::detail::staticPropRecipeIndexFits(index))
            return static_cast<int32_t>(index);
        if (!exhausted_) {
            exhausted_ = true;
            emit(index);
        }
        return -1;
    }

    // Only a live slot returned to the enabled free list ends an episode.
    bool recycleSlot(bool enabled, bool wasLive) {
        if (!enabled || !wasLive) return false;
        reset();
        return true;
    }

    void reset() { exhausted_ = false; }

private:
    bool exhausted_ = false;
};
} // namespace GpuStaticPropRegistry
