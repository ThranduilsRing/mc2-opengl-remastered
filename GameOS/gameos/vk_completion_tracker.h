// vk_completion_tracker.h -- the ticket arithmetic behind "has the GPU finished X".
//
// No Vulkan type in here: the engine context owns one timeline semaphore per queue, hands every submit a value from noteSubmit(), and feeds the
// semaphore's counter back through setCompleted(). A ticket is a pair of those values (gfx, xfer; 0 = no dependency on that queue), complete when
// every non-zero component is at or below the queue's completed counter. One value per queue is enough because a timeline signal covers every
// submit that precedes it on the queue, which is why every submit goes through the context's submit() and none through a raw vkQueueSubmit.
//
// A frame is opened by beginFrame(), sealed by endFrame() with the last graphics value submitted so far, and answered by frameTicket(n). A frame
// the ring no longer remembers, or one that was never sealed, answers the empty ticket: complete.

#ifndef MC2_VK_COMPLETION_TRACKER_H
#define MC2_VK_COMPLETION_TRACKER_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include "vk_frame_ring.h"

namespace mc2vk {

enum class QueueKind : uint8_t { Graphics = 0, Transfer = 1 };

struct GpuTicket {
    uint64_t gfx  = 0;
    uint64_t xfer = 0;
    // The later of two tickets, component by component: complete only when both are.
    static GpuTicket both(const GpuTicket& a, const GpuTicket& b) {
        return {a.gfx > b.gfx ? a.gfx : b.gfx, a.xfer > b.xfer ? a.xfer : b.xfer};
    }
};

class CompletionTracker {
public:
    static constexpr uint32_t kMaxFramesInFlight = mc2vk::kMaxFramesInFlight;
    static constexpr uint32_t kFrameRing = 8;   // sealed frames remembered; older ones answer "complete"

    // The next value for a submit on `q`. The caller signals exactly this value on the queue's timeline semaphore, in submission order.
    uint64_t noteSubmit(QueueKind q) { return ++last_[idx(q)]; }
    // The submit that took `value` was refused by the driver, so nothing will signal it. Only the newest value can be returned.
    void unnoteSubmit(QueueKind q, uint64_t value) { if (last_[idx(q)] == value && value > 0) --last_[idx(q)]; }
    uint64_t lastSubmitted(QueueKind q) const { return last_[idx(q)]; }

    // The semaphore's counter, read by the context. Monotonic: a smaller value is ignored.
    void setCompleted(QueueKind q, uint64_t v) { done_[idx(q)] = std::max(done_[idx(q)], v); }
    uint64_t completed(QueueKind q) const { return done_[idx(q)]; }

    bool isComplete(const GpuTicket& t) const { return t.gfx <= done_[0] && t.xfer <= done_[1]; }
    // Everything submitted so far, as a ticket.
    GpuTicket now() const { return {last_[0], last_[1]}; }

    // Frame bookkeeping. frameNo() is the OPEN frame once beginFrame() ran (0 before the first).
    void beginFrame() { ++frame_; open_ = true; }
    void endFrame() {
        if (!open_) return;
        sealed_[frame_ % kFrameRing] = {frame_, now()};
        open_ = false;
    }
    uint64_t frameNo() const { return frame_; }
    bool frameOpen() const { return open_; }
    GpuTicket frameTicket(uint64_t frameNo) const {
        const Sealed& s = sealed_[frameNo % kFrameRing];
        return s.frame == frameNo ? s.ticket : GpuTicket{};
    }

private:
    static constexpr size_t idx(QueueKind q) { return static_cast<size_t>(q); }
    struct Sealed { uint64_t frame = ~0ull; GpuTicket ticket; };
    std::array<uint64_t, 2> last_{};
    std::array<uint64_t, 2> done_{};
    std::array<Sealed, kFrameRing> sealed_{};
    uint64_t frame_ = 0;
    bool open_ = false;
};

}  // namespace mc2vk

#endif  // MC2_VK_COMPLETION_TRACKER_H
