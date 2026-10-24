#pragma once
#ifdef MC2_VULKAN_ISLAND
#include "vk_compute_pipeline.h"
#include "vulkan_context.h"
#include "gos_terrain_lighting.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>

namespace mc2vk {
// Owned by terrain; all receivers borrow its current-frame descriptors. No
// depth reconstruction, projection convention, light truncation or readback.
class LightLists {
    ComputePipeline pipeline_;
    ComputeSets sets_;
    FrameAlloc params_;
    uint64_t frame_ = ~uint64_t(0), recorded_ = ~uint64_t(0);
    std::array<FrameAlloc, 3> spans_{};
public:
    static constexpr uint32_t kDim = 64;
    float minInv[4]{};
    bool stage(VulkanContext& ctx, const void* pool, uint32_t count, float moverScale, FrameAlloc (&out)[3]) {
        const auto frame = ctx.tracker().frameNo();
        if (frame != frame_) {
            if (!pipeline_ && (!pipeline_.create(ctx.device(), "shaders/vulkan/light_lists.comp.spv",
                {{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC}, {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC},
                 {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC}, {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC}}, "light_lists") ||
                !sets_.create(ctx.device(), pipeline_, 2, "light_lists"))) return false;
            const auto* lights = static_cast<const gos_terrain_lighting::GpuTerrainLight*>(pool);
            float lo[2]{(std::numeric_limits<float>::max)(), (std::numeric_limits<float>::max)()};
            float hi[2]{-lo[0], -lo[1]};
            for (uint32_t i = 0; i < count; ++i) {
                const auto& l = lights[i];
                if (!l.lightType || !(l.farDistance > 0)) continue;
                const float radius = l.farDistance * (l._pad1 > 0.5f && moverScale < 0.999f ? (std::max)(moverScale, 0.05f) : 1.0f);
                for (unsigned a = 0; a < 2; ++a) {
                    lo[a] = (std::min)(lo[a], l.position[a] - radius);
                    hi[a] = (std::max)(hi[a], l.position[a] + radius);
                }
            }
            for (unsigned a = 0; a < 2; ++a) {
                if (!(hi[a] >= lo[a])) lo[a] = hi[a] = 0;
                minInv[a] = lo[a] - 1.0f;
                minInv[a + 2] = float(kDim) / (std::max)(hi[a] - lo[a] + 2.0f, 2.0f);
            }
            spans_[0] = ctx.frameAlloc(VkDeviceSize(count) * 48);
            spans_[1] = ctx.frameAlloc(kDim * kDim * 8);
            spans_[2] = ctx.frameAlloc(VkDeviceSize(kDim) * kDim * count * 4);
            params_ = ctx.frameAlloc(32);
            if (!spans_[0] || !spans_[1] || !spans_[2] || !params_) return false;
            std::memcpy(spans_[0].ptr, pool, size_t(count) * 48);
            std::memcpy(params_.ptr, minInv, 16);
            const uint32_t info[]{kDim, count, std::bit_cast<uint32_t>(moverScale), 0};
            std::memcpy(static_cast<char*>(params_.ptr) + 16, info, 16);
            frame_ = frame;
        }
        std::copy(spans_.begin(), spans_.end(), out);
        return true;
    }
    void record(VulkanContext& ctx, VkCommandBuffer cb, uint32_t parity) {
        if (frame_ != ctx.tracker().frameNo() || recorded_ == frame_) return;
        const FrameAlloc buffers[]{params_, spans_[0], spans_[1], spans_[2]};
        uint32_t offsets[4]{};
        for (uint32_t b = 0; b < 4; ++b) {
            const auto type = b == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
            sets_.write(ctx.device(), parity, b, type, buffers[b].buffer, b == 0 ? 32 : buffers[b].range);
            offsets[b] = uint32_t(buffers[b].offset);
        }
        cmdBarrier(cb, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_WRITE_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        pipeline_.bind(cb, sets_.set(parity), offsets);
        pipeline_.dispatch(cb, kDim * kDim / 64);
        cmdBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        recorded_ = frame_;
    }
};
}
#endif
