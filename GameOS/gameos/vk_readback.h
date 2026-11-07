// vk_readback.h -- GPU -> CPU readbacks that never block the frame.
//
// A consumer asks request(tag, bytes) for a slot, records a copy into the slot buffer, submits it, and seals the slot with the ticket of that submit.
// Frames later it asks poll(token, expectedTag): the bytes when the ticket completed, nothing yet while it has not. A result whose slot was recycled, or
// whose tag is not the one the consumer expects (the view, cursor or frame it was requested for changed), is Stale and is discarded, never used.
// Only captures and explicit diagnostics may wait (wait()).
//
// ReadbackCore is the slot/ticket/tag logic with no Vulkan type (doctest); ReadbackRing adds the host-cached buffers.

#ifndef MC2_VK_READBACK_H
#define MC2_VK_READBACK_H

#include <cstdint>
#include <type_traits>

#include "vk_completion_tracker.h"

#ifdef MC2_VULKAN   // the no-Vulkan unit-test tree (test_readback.cpp) uses only ReadbackCore
#include <volk.h>
#include "vk_owned.h"
#endif

namespace mc2vk {

struct ReadbackToken {
    uint32_t slot = ~0u;
    uint32_t gen = 0;
    explicit operator bool() const { return slot != ~0u; }
};

enum class ReadbackState : uint8_t { Pending, Ready, Stale };

// A completion predicate: whether the host may treat the ticket as done. The tracker overloads below answer from the polled counter (doctest,
// non-Vulkan callers); ReadbackRing passes VulkanContext::hostSynced, a host sync point, because a slot is read or its buffer reused right after.
template <class Complete>
concept ReadbackComplete = std::is_invocable_r_v<bool, Complete&, const GpuTicket&>;

class ReadbackCore {
public:
    static constexpr uint32_t kSlots = 4;

    // A free slot, tagged; an empty token when every slot is in use. A discarded slot whose copy is still in flight is NOT free: it returns to
    // the pool only once its ticket completed, so its buffer is never replaced or reused under a pending GPU write.
    ReadbackToken request(uint64_t tag, const CompletionTracker& ct) { return request(tag, [&](const GpuTicket& t) { return ct.isComplete(t); }); }
    template <ReadbackComplete Complete>
    ReadbackToken request(uint64_t tag, Complete&& done) {
        for (uint32_t i = 0; i < kSlots; ++i) {
            Slot& s = slots_[i];
            if (s.state == State::Discarded && done(s.ticket)) s.state = State::Free;
            if (s.state != State::Free) continue;
            s.state = State::Requested;
            s.tag = tag;
            s.ticket = GpuTicket{};
            return {i, s.gen};
        }
        return {};
    }
    // The submit that carries the copy has been made: the slot becomes readable once `t` completes.
    void seal(ReadbackToken tok, const GpuTicket& t) {
        if (!valid(tok) || slots_[tok.slot].state != State::Requested) return;
        slots_[tok.slot].ticket = t;
        slots_[tok.slot].state = State::Sealed;
    }
    // Never blocks. Ready only for a sealed slot whose ticket completed and whose tag is the one expected.
    ReadbackState poll(ReadbackToken tok, const CompletionTracker& ct, uint64_t expectedTag) const { return poll(tok, [&](const GpuTicket& t) { return ct.isComplete(t); }, expectedTag); }
    template <ReadbackComplete Complete>
    ReadbackState poll(ReadbackToken tok, Complete&& done, uint64_t expectedTag) const {
        if (!valid(tok)) return ReadbackState::Stale;
        const Slot& s = slots_[tok.slot];
        if (s.tag != expectedTag) return ReadbackState::Stale;
        if (s.state != State::Sealed || !done(s.ticket)) return ReadbackState::Pending;
        return ReadbackState::Ready;
    }
    // The consumer is done with the slot (read it or discarded it): every older token for it is stale. The slot is free again when its copy
    // completed (or was never submitted); a sealed copy still in flight parks the slot as Discarded until its ticket completes.
    void release(ReadbackToken tok, const CompletionTracker& ct) { release(tok, [&](const GpuTicket& t) { return ct.isComplete(t); }); }
    template <ReadbackComplete Complete>
    void release(ReadbackToken tok, Complete&& done) {
        if (!valid(tok)) return;
        Slot& s = slots_[tok.slot];
        s.state = (s.state == State::Sealed && !done(s.ticket)) ? State::Discarded : State::Free;
        ++s.gen;
    }
    uint64_t tagOf(ReadbackToken tok) const { return valid(tok) ? slots_[tok.slot].tag : 0; }
    GpuTicket ticketOf(ReadbackToken tok) const { return valid(tok) ? slots_[tok.slot].ticket : GpuTicket{}; }
    uint32_t inUse() const { uint32_t n = 0; for (const Slot& s : slots_) n += s.state != State::Free; return n; }

private:
    enum class State : uint8_t { Free, Requested, Sealed, Discarded };
    struct Slot { State state = State::Free; uint32_t gen = 0; uint64_t tag = 0; GpuTicket ticket; };
    bool valid(ReadbackToken tok) const { return tok.slot < kSlots && slots_[tok.slot].gen == tok.gen; }
    Slot slots_[kSlots];
};

#ifdef MC2_VULKAN
class VulkanContext;

class ReadbackRing {
public:
    // Reserves a slot of `bytes` and returns the buffer the caller records its copy into (offset 0). An empty token: all slots busy or no memory.
    ReadbackToken request(VulkanContext& ctx, VkDeviceSize bytes, uint64_t tag, VkBuffer* dst);
    // The copy has been submitted by the submit that signalled `t`.
    void seal(ReadbackToken tok, const GpuTicket& t) { core_.seal(tok, t); }
    // The mapped bytes when the slot completed and carries `expectedTag`; null while pending. A stale token is released here and reported through *stale.
    const void* poll(VulkanContext& ctx, ReadbackToken tok, uint64_t expectedTag, bool* stale = nullptr);
    // Captures and diagnostics only: blocks until the slot completed.
    const void* wait(VulkanContext& ctx, ReadbackToken tok, uint64_t expectedTag, uint64_t timeoutNs);
    void release(VulkanContext& ctx, ReadbackToken tok);
    void releaseAll() { for (auto& b : buf_) b.reset(); }
    GpuTicket ticketOf(ReadbackToken tok) const { return core_.ticketOf(tok); }
    uint32_t inUse() const { return core_.inUse(); }
    uint64_t requests() const { return requests_; }
    uint64_t stale() const { return stale_; }

private:
    ReadbackCore core_;
    OwnedBuffer buf_[ReadbackCore::kSlots];
    VkDeviceSize cap_[ReadbackCore::kSlots] = {};
    void* mapped_[ReadbackCore::kSlots] = {};
    uint64_t requests_ = 0, stale_ = 0;
};
#endif

}  // namespace mc2vk

#endif  // MC2_VK_READBACK_H
