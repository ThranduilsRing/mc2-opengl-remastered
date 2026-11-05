// vk_deferred_destroy.h -- destroy a GPU object once the GPU has finished with it, without idling the device.
//
// retire(std::move(owner)) parks an owner (OwnedBuffer, OwnedImage, a container of them, a shared_ptr to CPU texels a copy still reads) until a
// ticket completes; its destructor then runs. Vulkan-free: the engine context feeds it the CompletionTracker. An entry retired while a frame is open
// is sealed with that frame's ticket at endFrame (the open frame may still record a use before its submits); one retired outside a frame takes the
// ticket of everything submitted so far, which is right because load-time code submits before it returns.

#ifndef MC2_VK_DEFERRED_DESTROY_H
#define MC2_VK_DEFERRED_DESTROY_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "vk_completion_tracker.h"

namespace mc2vk {

class DeferredDestroy {
public:
    struct Stats {
        uint32_t entries = 0;
        uint64_t bytes = 0;
        uint64_t oldestAgeFrames = 0;
    };

    // Moves the owner in; it is destroyed by collect() once the open frame's ticket (or, outside a frame, everything submitted now) completes.
    template <class Owner>
    void retire(const CompletionTracker& ct, Owner&& owner, uint64_t bytes = 0) {
        add(ct, std::make_unique<Box<std::decay_t<Owner>>>(std::forward<Owner>(owner)), bytes, ct.now(), !ct.frameOpen());
    }
    // The same with an explicit ticket (an upload or readback that is not tied to the frame).
    template <class Owner>
    void retireAfter(const CompletionTracker& ct, const GpuTicket& t, Owner&& owner, uint64_t bytes = 0) {
        add(ct, std::make_unique<Box<std::decay_t<Owner>>>(std::forward<Owner>(owner)), bytes, t, true);
    }
    // Seals the entries retired during the frame that just ended.
    void sealFrame(const GpuTicket& t) {
        for (Entry& e : entries_) if (!e.sealed) { e.ticket = t; e.sealed = true; }
    }
    // Destroys every sealed entry whose ticket completed, newest insertion first. Returns how many were destroyed.
    uint32_t collect(const CompletionTracker& ct) {
        uint32_t n = 0;
        for (size_t i = entries_.size(); i-- > 0;) {
            Entry& e = entries_[i];
            if (e.sealed && ct.isComplete(e.ticket)) { bytes_ -= e.bytes; entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i)); ++n; }
        }
        return n;
    }
    // Destroys everything. The caller has waited for the device (mission unload, swapchain recreate, teardown).
    uint32_t drainAll() {
        const uint32_t n = static_cast<uint32_t>(entries_.size());
        while (!entries_.empty()) entries_.pop_back();   // reverse insertion order
        bytes_ = 0;
        return n;
    }
    // The newest ticket among the entries still parked: waiting for it makes the whole queue collectable.
    GpuTicket newestTicket() const {
        GpuTicket t;
        for (const Entry& e : entries_) if (e.sealed) t = GpuTicket::both(t, e.ticket);
        return t;
    }
    Stats stats(const CompletionTracker& ct) const {
        Stats s;
        s.entries = static_cast<uint32_t>(entries_.size());
        s.bytes = bytes_;
        for (const Entry& e : entries_) {
            const uint64_t age = ct.frameNo() - e.frame;
            s.oldestAgeFrames = (std::max)(s.oldestAgeFrames, age);
        }
        return s;
    }
    uint64_t bytes() const { return bytes_; }
    size_t size() const { return entries_.size(); }

private:
    struct Holder { virtual ~Holder() = default; };
    template <class T> struct Box final : Holder {
        T value;
        explicit Box(T&& v) : value(std::move(v)) {}
        explicit Box(const T& v) : value(v) {}
    };
    struct Entry {
        GpuTicket ticket;
        bool sealed = false;
        uint64_t frame = 0;
        uint64_t bytes = 0;
        std::unique_ptr<Holder> holder;
    };
    void add(const CompletionTracker& ct, std::unique_ptr<Holder> h, uint64_t bytes, const GpuTicket& t, bool sealed) {
        Entry e;
        e.ticket = t;
        e.sealed = sealed;
        e.frame = ct.frameNo();
        e.bytes = bytes;
        e.holder = std::move(h);
        bytes_ += bytes;
        entries_.push_back(std::move(e));
    }
    std::vector<Entry> entries_;
    uint64_t bytes_ = 0;
};

}  // namespace mc2vk

#endif  // MC2_VK_DEFERRED_DESTROY_H
