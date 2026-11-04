// vk_one_shot.cpp -- see vk_one_shot.h.
#ifdef MC2_VULKAN_ISLAND
#include "vk_one_shot.h"

#include <cstdio>
#include <string>

#include "hitch_stats.h"
#include "vk_owned.h"
#include "vulkan_context.h"

namespace mc2vk {

bool beginTransientCommandBuffer(const VulkanContext& ctx, OwnedCommandPool& pool, VkCommandBuffer& cb) {
    const VkDevice device = ctx.device();
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pci.queueFamilyIndex = ctx.graphicsFamily();
    if (!create(pool, device, [&](VkCommandPool* h) { return vkCreateCommandPool(device, &pci, nullptr, h); })) return false;
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool.get();
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device, &ai, &cb) != VK_SUCCESS) return false;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return vkBeginCommandBuffer(cb, &bi) == VK_SUCCESS;
}

bool submitAndWait(const VulkanContext& ctx, const VkSubmitInfo& si, uint64_t timeoutNs, std::string_view why) {
    uint64_t value = 0;
    const uint64_t pending = mc2_hitch2::pendingUploadBytes();   // the upload batch ctx.submit flushes ahead of this submit
    if (ctx.submit(si, VK_NULL_HANDLE, &value) != VK_SUCCESS) return false;
    const GpuTicket t{value, 0};
    bool done;
    {
        mc2_hitch2::FenceWaitScope waitScope(why, mc2_hitch2::kOneShot, pending);   // closed before the idle fallback, which waitidle_ms counts
        done = ctx.wait(t, timeoutNs, why);
    }
    if (done) return true;
    ctx.waitIdle(why);
    ctx.poll();
    return ctx.isComplete(t);
}

bool submitOneShot(const VulkanContext& ctx, const std::function<void(VkCommandBuffer)>& record, std::string_view why) {
    const VkDevice device = ctx.device();
    if (device == VK_NULL_HANDLE) return false;
    OwnedCommandPool pool;
    OwnedFence fence;
    bool ok = false;
    bool submitted = false;   // the GPU owns the fence and the pool from the submit until the fence signals
    do {
        VkCommandBuffer cb = VK_NULL_HANDLE;
        if (!beginTransientCommandBuffer(ctx, pool, cb)) break;
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (!create(fence, device, [&](VkFence* h) { return vkCreateFence(device, &fci, nullptr, h); })) break;
        record(cb);
        if (vkEndCommandBuffer(cb) != VK_SUCCESS) break;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        const uint64_t pending = mc2_hitch2::pendingUploadBytes();   // flushed ahead of this submit
        if (ctx.submit(si, fence.get()) != VK_SUCCESS) break;
        submitted = true;
        const VkFence fenceHandle = fence.get();
        mc2_hitch2::FenceWaitScope waitScope(why, mc2_hitch2::kOneShot, pending);
        ok = vkWaitForFences(device, 1, &fenceHandle, VK_TRUE, 5ull * 1000 * 1000 * 1000) == VK_SUCCESS;
    } while (false);
    if (submitted && !ok) {
        // The GPU may still be executing the submit, and the fence and the pool are destroyed on return: wait for the device
        // first. A hung device is turned into a lost one by the driver, which ends the wait.
        const std::string w(why);
        std::fprintf(stderr, "[VK_ONE_SHOT] %s: the fence did not signal in time; waiting for the device before releasing it\n", w.c_str());
        ctx.waitIdle(why);
    }
    return ok;
}

}  // namespace mc2vk
#endif  // MC2_VULKAN_ISLAND
