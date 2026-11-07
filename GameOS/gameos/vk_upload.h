// vk_upload.h -- per-frame and streaming uploads without a CPU wait.
//
// An uploader stages its bytes with stage() (a frame-ring span, or a dedicated host buffer for a request larger than a quarter of a ring slot) and queues
// its copy as a recorder with record(). The context flushes the queue as ONE command buffer, submitted ahead of the next graphics submit of any island (so
// every consumer that submits after the request sees the finished copy) and at the end of the frame at the latest; nothing waits for the GPU. The
// recorders end with their own barriers to the layout and stage the consumer reads in. The staging span belongs to the frame the request was made in,
// and the flush submit lands inside that frame, so the frame's ticket covers it.
//
// Load-time callers (a map's copy-once imports) keep their own one-shot and wait; this is for what happens while frames run.
// The batch is queue-agnostic: with a transfer queue the flush would submit there and the graphics submit would wait its timeline value.

#ifndef MC2_VK_UPLOAD_H
#define MC2_VK_UPLOAD_H


#include <volk.h>
#include <cstdint>
#include <functional>
#include <vector>

#include "vk_owned.h"

namespace mc2vk {

class VulkanContext;

class UploadBatch {
public:
    struct Span {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        void* ptr = nullptr;
        explicit operator bool() const { return buffer != VK_NULL_HANDLE && ptr != nullptr; }
    };

    // `bytes` of host-writable staging, valid until the frame completes. Empty on failure.
    Span stage(VulkanContext& ctx, VkDeviceSize bytes);
    // Queues a recorder; it runs when the batch is flushed, in the batch command buffer. Handles it names must stay valid until then (the same frame).
    void record(std::function<void(VkCommandBuffer)> fn) { recorders_.push_back(std::move(fn)); ++queued_; }
    bool pending() const { return !recorders_.empty(); }

    // Records the queued recorders into one command buffer and returns it with its pool; the context submits it. False when nothing is queued or the
    // command buffer cannot be made (the requests are dropped and counted).
    bool build(const VulkanContext& ctx, OwnedCommandPool& pool, VkCommandBuffer& cb);
    // The dedicated staging buffers of this flush, handed to the context to retire with the frame.
    std::vector<OwnedBuffer> takeDedicated() {
        std::vector<OwnedBuffer> out = std::move(dedicated_);
        dedicated_.clear();
        return out;
    }

    uint64_t flushes() const { return flushes_; }
    uint64_t queued() const { return queued_; }
    uint64_t dropped() const { return dropped_; }
    uint64_t dedicatedStages() const { return dedicatedStages_; }

private:
    std::vector<std::function<void(VkCommandBuffer)>> recorders_;
    std::vector<OwnedBuffer> dedicated_;
    uint64_t flushes_ = 0, queued_ = 0, dropped_ = 0, dedicatedStages_ = 0;
};

}  // namespace mc2vk

#endif  // MC2_VK_UPLOAD_H
