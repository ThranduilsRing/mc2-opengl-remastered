// vk_shared_light_feed.cpp -- VK-LIGHTFEED-DEDUP-1. See the header for the design. #ifdef MC2_VULKAN_ISLAND (whole TU).


#include <volk.h>

#include "vk_shared_light_feed.h"
#include "Gates/gate.h"                // gate::get<> (MC2_VK_LIGHTFEED_DEDUP)

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <bit>
#include <vk_mem_alloc.h>
#include <tracy/Tracy.hpp>

#include "vulkan_context.h"
#include "mc2_diag_verbose.h"
#include "../../mclib/vk_object_lights.h"
#include "../../RenderCore/gpu_abi/GpuObjectLights.h"
#include "mc2fmt.h"

bool mc2vkSharedLightFeedEnabled()
{
    // Default-ON killswitch (fence-defer template): explicit "0" reverts to the per-island copy paths; unset / "1" = dedup armed.
    static int s_state = -1;
    if (s_state == -1)
        s_state = mc2::gate::get<"MC2_VK_LIGHTFEED_DEDUP">() ? 1 : 0;
    return s_state == 1;
}

mc2vk::FrameAlloc mc2vkSharedLightFeedAcquire(const mc2::vkLights::Feed& feed)
{
    if (!mc2vkSharedLightFeedEnabled()) return {};
    mc2vk::VulkanContext& ctx = mc2vk::getOrCreateEngineVulkanContext();
    if (!ctx.valid()) return {};
    return ctx.lightFeed().acquire(ctx, feed);
}

namespace mc2vk {

FrameAlloc SharedLightFeed::acquire(VulkanContext& ctx, const mc2::vkLights::Feed& feed)
{
    const void* cpuData = feed.data;
    const uint64_t cpuBytes = feed.bytes;
    if (cpuData == nullptr || cpuBytes < 3600ull) return {};
    const auto generations = feed.generations;
    constexpr size_t rowBytes = sizeof(GpuObjectLights);
    if (!feed.owner || cpuBytes != generations.size() * rowBytes) return {};
    const uint64_t revision = feed.revision;

    const uint64_t frame = ctx.tracker().frameNo();
    if (frame == frame_) {
        // Second+ consumer this frame: reuse the already-copied span. If the CPU array GREW between two same-frame island flushes (mid-frame dynamic-light
        // append), fail soft: this caller does its own copy (rare, content-safe; next frame's copy picks up the full array).
        if (!span_ || static_cast<VkDeviceSize>(cpuBytes) > bytes_ || revision != revision_ || feed.packetKey != packetKey_) { ++fallbacks_; return {}; }
        ++hits_;
        return span_;
    }
    frame_ = frame;
    span_ = FrameAlloc{};
    revision_ = revision;
    packetKey_ = feed.packetKey;

    {
        ZoneScopedN("VKIsl.Shared.LightFeed");
        // beginFrame retired this frame slot. Its persistent rows are never
        // changed again after the first acquire; same-frame edits fall back.
        auto& slot = slots_[ctx.frameContext().slot];
        if (slot.capacity < cpuBytes) {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(ctx.physicalDevice(), &properties);
            const VkDeviceSize limit = properties.limits.maxStorageBufferRange;
            if (cpuBytes > limit) { ++fallbacks_; return {}; }
            const VkDeviceSize capacity = (std::min)(std::bit_ceil(cpuBytes), limit);
            OwnedBuffer fresh;
            VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bci.size = capacity;
            bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_AUTO;
            aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            aci.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            aci.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            if (!fresh.create(ctx.allocator(), ctx.device(), bci, aci, "lightfeed")) { ++fallbacks_; return {}; }
            VmaAllocationInfo info{};
            vmaGetAllocationInfo(ctx.allocator(), fresh.allocation(), &info);
            if (!info.pMappedData) { ++fallbacks_; return {}; }
            if (slot.buffer) ctx.retire(std::move(slot.buffer), slot.capacity);
            slot.buffer = std::move(fresh);
            slot.mapped = info.pMappedData;
            slot.capacity = capacity;
            slot.generations.assign(generations.size(), ~0ull);
        }
        slot.generations.resize(generations.size(), ~0ull);
        const auto* src = static_cast<const uint8_t*>(cpuData);
        auto* dst = static_cast<uint8_t*>(slot.mapped);
        for (size_t i = 0; i < generations.size();) {
            if (slot.generations[i] == generations[i]) { ++i; continue; }
            const size_t first = i;
            do { slot.generations[i] = generations[i]; ++i; }
            while (i < generations.size() && slot.generations[i] != generations[i]);
            std::memcpy(dst + first * rowBytes, src + first * rowBytes, (i - first) * rowBytes);
        }
        span_ = {slot.buffer.get(), 0, slot.mapped, cpuBytes, slot.capacity};
    }
    bytes_ = static_cast<VkDeviceSize>(cpuBytes);
    ++copies_;

    if (copies_ == 1 && mc2DiagVerbose()) {
        mc2::fmt::print_err("[VK_LIGHTFEED_DEDUP v2] armed: persistent dirty-row LightsData per frame slot ({:d} B) -- killswitch MC2_VK_LIGHTFEED_DEDUP=0\n",
            mc2::fmt::as_u64(static_cast<unsigned long long>(cpuBytes)));
        std::fflush(stderr);
    }
    if (copies_ == 600 && mc2DiagVerbose()) {   // ~10 s receipt: hits ~= copies when both islands share
        mc2::fmt::print_err("[VK_LIGHTFEED_DEDUP v1] 600 copies: sharedHits={:d} fallbacks={:d} bytes/frame={:d}\n",
            mc2::fmt::as_ulong(hits_), mc2::fmt::as_ulong(fallbacks_), mc2::fmt::as_u64(static_cast<unsigned long long>(bytes_)));
        std::fflush(stderr);
    }

    return span_;
}

}  // namespace mc2vk

