// vk_record_pools.h -- thread-safe command-buffer recording: one command pool per participant per frame-in-flight.
//
// A Vulkan command pool is externally synchronized, so threads that record in parallel (a Render-domain parallel_for, one island per block) cannot
// share one. Every recording thread gets its own lane: a lane is the (frame slot, participant) pair, with its own command pool and its own list of command
// buffers. The participant is the thread (the main thread and each pool worker); a lane is touched by its owner thread while a frame records and by the main
// thread alone when the slot restarts, so no lane needs a lock. Recording order is the caller's: each island stores its command buffer at its own index and
// main appends them to the submit batch in canonical island order (never in completion order).
//
// RecordPools<Cb> is the bookkeeping with no Vulkan type (doctest); VulkanContext::acquireRecordCommandBuffer adds the VkCommandPools.
// Uploads stay main-only (UploadBatch), and the frame ring / descriptor chains take a lock instead (see FrameRing::alloc, DescPoolChain::allocate).

#ifndef MC2_VK_RECORD_POOLS_H
#define MC2_VK_RECORD_POOLS_H

#include <array>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "vk_frame_ring.h"   // kMaxFramesInFlight

namespace mc2vk {

inline constexpr uint32_t kMaxRecordParticipants = 32;
inline constexpr uint32_t kNoParticipant = UINT32_MAX;

// Hands each recording thread a small stable index, in order of first use. A lookup is under a mutex: it happens once per island command buffer, not per draw.
class ParticipantRegistry {
public:
    uint32_t index() {
        const std::thread::id me = std::this_thread::get_id();
        std::lock_guard<std::mutex> lock(mu_);
        for (uint32_t i = 0; i < count_; ++i)
            if (ids_[i] == me) return i;
        if (count_ == kMaxRecordParticipants) return kNoParticipant;   // more threads than lanes: the caller records on main instead
        ids_[count_] = me;
        return count_++;
    }
    uint32_t count() const { std::lock_guard<std::mutex> lock(mu_); return count_; }

private:
    mutable std::mutex mu_;
    std::array<std::thread::id, kMaxRecordParticipants> ids_{};
    uint32_t count_ = 0;
};

template <class Cb>
class RecordPools {
public:
    static constexpr uint32_t kSlots = kMaxFramesInFlight;

    // Main thread, no recording in flight: the slot's previous user completed (`restart`), so every lane of the slot hands its command buffers out again.
    // resetLane(slot, participant) runs for each lane that owns any (VulkanContext resets that lane's command pool there). Without the proof
    // (restart == false) nothing resets and lanes keep allocating past what the unproven frame recorded.
    template <class ResetLane>
    void beginFrame(uint32_t slot, bool restart, ResetLane&& resetLane) {
        if (!restart) return;
        for (uint32_t p = 0; p < kMaxRecordParticipants; ++p) {
            Lane& lane = lanes_[slot % kSlots][p];
            if (lane.cbs.empty()) continue;
            resetLane(slot % kSlots, p);
            lane.used = 0;
        }
    }

    // Any thread. The next command buffer of the calling thread's lane in `slot`; `create(slot, participant)` makes a new one (from that lane's pool) when
    // the lane has handed out all it owns. Returns Cb{} when there is no lane for this thread (kMaxRecordParticipants exceeded) or create failed.
    template <class Create>
    Cb acquire(uint32_t slot, Create&& create) {
        const uint32_t p = registry_.index();
        if (p == kNoParticipant) return Cb{};
        Lane& lane = lanes_[slot % kSlots][p];
        if (lane.used == lane.cbs.size()) {
            Cb fresh = create(slot % kSlots, p);
            if (fresh == Cb{}) return Cb{};
            lane.cbs.push_back(fresh);
        }
        return lane.cbs[lane.used++];
    }

    uint32_t participantOfCaller() { return registry_.index(); }
    uint32_t participants() const { return registry_.count(); }
    uint32_t used(uint32_t slot, uint32_t participant) const { return lanes_[slot % kSlots][participant].used; }
    uint32_t owned(uint32_t slot, uint32_t participant) const { return static_cast<uint32_t>(lanes_[slot % kSlots][participant].cbs.size()); }
    // Forgets every command buffer (their pools are being destroyed with the device).
    void clear() { for (auto& slot : lanes_) for (auto& lane : slot) { lane.cbs.clear(); lane.used = 0; } }

private:
    struct Lane { std::vector<Cb> cbs; uint32_t used = 0; };
    ParticipantRegistry registry_;
    std::array<std::array<Lane, kMaxRecordParticipants>, kSlots> lanes_;
};

}  // namespace mc2vk

#endif
