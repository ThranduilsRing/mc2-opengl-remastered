// Frame-fence wait accounting for the hitch trace under Vulkan. The present path retires the previous frame's
// submit by waiting on one fence; that wait is where a GPU-bound hitch shows up on the CPU, so the trace reports
// it per frame. Header-only and free of Vulkan types so it can be unit-tested.
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "mc2fmt.h"

namespace mc2_hitch {

struct FenceWaitStats {
    double   totalMs = 0.0;
    double   maxMs   = 0.0;
    uint32_t count   = 0;

    // A negative or non-finite duration is a clock fault, not a wait: it is refused so one bad sample cannot
    // poison the frame's total. Returns whether the sample was taken.
    bool record(double ms) {
        if (!std::isfinite(ms) || ms < 0.0) return false;
        totalMs += ms;
        if (ms > maxMs) maxMs = ms;
        ++count;
        return true;
    }

    // The detail line the hitch trace emits for a frame; returns the length written (0 on a format failure).
    int format(char* buf, size_t cap, uint32_t frame) const {
        const int n = mc2::fmt::to_buf_n(buf, cap,
            "[HITCH_VK] frame={:d} fenceWaitMs={:.2f} fenceWaitMax={:.2f} fenceWaits={:d}\n",
            mc2::fmt::as_u32(frame), totalMs, maxMs, mc2::fmt::as_u32(count));
        return n > 0 ? n : 0;
    }
};

} // namespace mc2_hitch
