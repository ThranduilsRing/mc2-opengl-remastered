// RenderCore/FrameFenceCore.h -- the parity and "may this slot's fence be waited" decision of a double-buffered frame resource (vk_image_resource).
//
// WHAT: begin() hands out the slot to use this frame and flips the parity; cancel() gives the slot back after a frame that submitted nothing.
// WHY: BeginFrame resets the slot's fence and only a submit signals it again, so a frame that returns early must re-create the fence signaled
// (cancel(slot, true)). When the re-create fails (device loss, out of memory) the old fence stays reset with nothing in flight on it: waiting it
// would block forever, so the resource is disabled and begin() never allows a wait again. Pure logic, offline-testable.
#pragma once
#include <cstdint>

namespace RenderCore {

struct FrameFenceCore {
    struct Begin {
        uint32_t slot = 0;
        bool mayWait = false;   // false: the resource is disabled, wait on nothing and submit nothing
    };

    Begin begin() {
        if (disabled_) return {parity_, false};
        const Begin b{parity_, true};
        parity_ ^= 1u;
        return b;
    }

    // `recreated`: the slot's fence now exists again, signaled. Otherwise the resource is disabled for good.
    void cancel(uint32_t slot, bool recreated) {
        if (slot > 1u) return;
        parity_ = slot;
        if (!recreated) disabled_ = true;
    }

    uint32_t parity() const { return parity_; }
    bool disabled() const { return disabled_; }

private:
    uint32_t parity_ = 0;
    bool disabled_ = false;
};

}  // namespace RenderCore
