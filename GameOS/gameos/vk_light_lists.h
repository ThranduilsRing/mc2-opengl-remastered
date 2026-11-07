#pragma once
#include "vk_compute_pipeline.h"
#include "vulkan_context.h"
#include "gos_terrain_lighting.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <limits>
#include "mc2fmt.h"

namespace mc2vk {
// Owned by terrain; all receivers borrow its current-frame descriptors. No
// depth reconstruction, projection convention, light truncation or readback.
class LightLists {
    ComputePipeline pipeline_;
    ComputeSets sets_;
    FrameAlloc params_;
    uint64_t frame_ = ~uint64_t(0), recorded_ = ~uint64_t(0);
    uint64_t attempted_ = ~uint64_t(0), retryAt_ = 0, failures_ = 0;
    uint32_t retryDelay_ = 1;
    const char* failure_ = nullptr;
    std::array<FrameAlloc, 3> spans_{};
    uint32_t poolWords_ = 0;
public:
    static constexpr uint32_t kDim = 64;
    float minInv[4]{};
    bool stage(VulkanContext& ctx, const void* pool, uint32_t count, float moverScale, FrameAlloc (&out)[3], const char*& failure) {
        const auto frame = ctx.tracker().frameNo();
        if (frame != frame_) {
            failure = failure_;
            if (attempted_ == frame || frame < retryAt_) return false;
            attempted_ = frame;
            const bool logFailures = failures_ == 0;
            auto fail = [&] {
                failure_ = failure;
                ++failures_;
                retryAt_ = frame + retryDelay_;
                retryDelay_ = (std::min)(retryDelay_ * 2, 256u);
                if (logFailures)
                    mc2::fmt::print_err("[VK_TERRAIN_ISLAND] light lists failed ({}); using linear lighting, count=1 (retries back off up to 256 frames).\n", mc2::fmt::safe(failure));
                return false;
            };
            failure = "pipeline";
            if (!pipeline_ && !pipeline_.create(ctx.device(), "shaders/vulkan/light_lists.comp.spv",
                {{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC}, {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC},
                 {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC}, {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC}}, "light_lists", logFailures)) return fail();
            failure = "descriptors";
            if (!sets_.set(0) && !sets_.create(ctx.device(), pipeline_, kMaxFramesInFlight, "light_lists", logFailures)) return fail();
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
                minInv[a + 2] = static_cast<float>(kDim) / (std::max)(hi[a] - lo[a] + 2.0f, 2.0f);
            }
            failure = "frame_ring_allocation";
            // The index buffer dominates. Refuse it before consuming any smaller
            // spans, and never repeat partial allocations in the same frame.
            // Pooled: one shared index pool (word 0 is its allocation cursor) instead of a full count-entry slice per cell. The pool is sized from the
            // exact worst case: the cells each light's bounding box touches (a superset of the shader's circle test), so no cell is ever cut short.
            uint32_t entries = 0;
            for (uint32_t i = 0; i < count; ++i) {
                const auto& l = lights[i];
                if (!l.lightType || !(l.farDistance > 0)) continue;
                const float r = l.farDistance * 1.001f + 0.01f;
                uint32_t span[2];
                for (unsigned a = 0; a < 2; ++a) {
                    const float c0 = std::floor((l.position[a] - r - minInv[a]) * minInv[a + 2]), c1 = std::floor((l.position[a] + r - minInv[a]) * minInv[a + 2]);
                    span[a] = c1 < 0.0f || c0 > static_cast<float>(kDim - 1) ? 0u : static_cast<uint32_t>((std::min)(c1, static_cast<float>(kDim - 1)) - (std::max)(c0, 0.0f)) + 1u;
                }
                entries += span[0] * span[1];
            }
            poolWords_ = 1 + (std::min)(entries, kDim * kDim * count);
            if (!(spans_[2] = ctx.frameAlloc(VkDeviceSize(poolWords_) * 4)) ||
                !(spans_[0] = ctx.frameAlloc(VkDeviceSize(count) * 48)) ||
                !(spans_[1] = ctx.frameAlloc(kDim * kDim * 8)) ||
                !(params_ = ctx.frameAlloc(32))) return fail();
            std::memcpy(spans_[0].ptr, pool, static_cast<size_t>(count) * 48);
            *static_cast<uint32_t*>(spans_[2].ptr) = 1;
            std::memcpy(params_.ptr, minInv, 16);
            const uint32_t info[]{kDim, count, std::bit_cast<uint32_t>(moverScale), poolWords_};
            std::memcpy(static_cast<char*>(params_.ptr) + 16, info, 16);
            frame_ = frame;
            if (failures_)
                mc2::fmt::print_err("[VK_TERRAIN_ISLAND] light lists recovered failed_attempts={:d}.\n", mc2::fmt::as_u64(static_cast<unsigned long long>(failures_)));
            failures_ = 0; retryDelay_ = 1; failure_ = nullptr;
        }
        std::copy(spans_.begin(), spans_.end(), out);
        failure = nullptr;
        return true;
    }
    void record(VulkanContext& ctx, VkCommandBuffer cb, uint32_t parity) {
        if (frame_ != ctx.tracker().frameNo() || recorded_ == frame_) return;
        const FrameAlloc buffers[]{params_, spans_[0], spans_[1], spans_[2]};
        uint32_t offsets[4]{};
        for (uint32_t b = 0; b < 4; ++b) {
            const auto type = b == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
            sets_.write(ctx.device(), parity, b, type, buffers[b].buffer, b == 0 ? 32 : buffers[b].range);
            offsets[b] = static_cast<uint32_t>(buffers[b].offset);
        }
        // No HOST->COMPUTE barrier: host writes before vkQueueSubmit are made visible by the submit itself.
        pipeline_.bind(cb, sets_.set(parity), offsets);
        pipeline_.dispatch(cb, kDim * kDim / 64);
        cmdBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        recorded_ = frame_;
    }
};
}
