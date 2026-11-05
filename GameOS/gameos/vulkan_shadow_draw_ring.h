// vulkan_shadow_draw_ring.h -- the static-prop shadow casters' draws as device-address records plus one indirect multi-draw per pipeline, instead of one
// vkCmdDrawIndexed per (run, packet).
//
// WHAT: a caller lists its draws (a packet's index range, an instance range, and for an alpha-tested packet its layer and UV scale). `record` writes the draw
// commands and the per-draw alpha rows into a host-visible ring, then issues one vkCmdDrawIndexedIndirect for the opaque draws and one for the alpha draws.
// The alpha rows are read through buffer device address (the push block carries the row array's address, the shader indexes it by packetBase + gl_DrawID).
// WHY: the draw order of a depth-only pass is irrelevant, so the opaque and alpha draws split into two multi-draws; the old loop switched pipeline and pushed
// constants per packet.
// Frame safety: the ring holds one slice per frame parity; the caller `begin`s the parity after that frame's fence has retired its previous use.
#ifndef MC2_VK_SHADOW_DRAW_RING_H
#define MC2_VK_SHADOW_DRAW_RING_H

#ifdef MC2_VULKAN_ISLAND

#include <volk.h>
#include <vk_mem_alloc.h>

#include "vk_owned.h"
#include "vk_frame_ring.h"
#include "vk_render_infra.h"
#include "../../mclib/battle_bench_sink.h"

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace mc2vk {

// One caster draw: `pkt` of the shared index buffer, `instCount` instances from `firstInstance`; alpha draws carry the layer and UV scale of the packet.
struct ShadowDraw {
    uint32_t indexCount, firstIndex, baseVertex;
    uint32_t firstInstance, instCount;
    int32_t  layer;          // < 0: opaque
    float    scaleX, scaleY;
};

// Matches the shader row (shadow_prop_alpha.frag AlphaRow, 16 B std430).
struct ShadowAlphaRow { float uvScale[2]; int32_t layer; int32_t pad; };
// Matches the push block (vertex and fragment stages): the row array's device address and the first row of this multi-draw.
struct ShadowDrawPush { VkDeviceAddress rows; uint32_t packetBase; uint32_t pad; };
static_assert(sizeof(ShadowAlphaRow) == 16 && sizeof(ShadowDrawPush) == 16, "shadow draw records match the shader blocks");

class ShadowDrawRing {
public:
    // `capDraws` draws per parity slice (commands and rows share indices).
    bool init(VmaAllocator allocator, VkDevice device, uint32_t capDraws) {
        cap_ = (std::max)(capDraws, 256u);
        for (uint32_t p = 0; p < kMaxFramesInFlight; ++p) {
            if (!cmd_[p].createHostWritable(allocator, device, static_cast<VkDeviceSize>(cap_) * sizeof(VkDrawIndexedIndirectCommand), VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                                            "shadow.drawCmds", static_cast<int>(p)) ||
                !rows_[p].createHostWritable(allocator, device, static_cast<VkDeviceSize>(cap_) * sizeof(ShadowAlphaRow), VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                             "shadow.drawRows", static_cast<int>(p)) ||
                rows_[p].deviceAddress() == 0) { reset(); return false; }
        }
        return true;
    }
    void reset() { for (uint32_t p = 0; p < kMaxFramesInFlight; ++p) { cmd_[p].reset(); rows_[p].reset(); } cap_ = 0; }
    bool ready() const { return cap_ != 0; }
    // Starts the parity's slice over (its previous frame has retired).
    void begin(uint32_t parity) { parity_ = parity % kMaxFramesInFlight; cursor_ = 0; }

    // Records `draws` with the layout's set 0 (and the vertex/index buffers) already bound. Opaque draws use `opaque`, alpha draws `alpha` with `alphaSet` at
    // set 1. Returns the multi-draw calls issued. The pipeline left bound is `opaque`.
    uint32_t record(VkCommandBuffer cb, VkPipelineLayout layout, VkPipeline opaque, VkPipeline alpha, VkDescriptorSet alphaSet, std::span<const ShadowDraw> draws) {
        if (draws.empty() || !ready()) return 0;
        const uint32_t room = cap_ - (std::min)(cursor_, cap_);
        const uint32_t n = static_cast<uint32_t>((std::min<size_t>)(draws.size(), room));   // a run list holds at most one draw per packet; the ring is sized for that
        cmds_.clear(); rows_s_.clear();
        for (uint32_t i = 0; i < n; ++i)
            if (draws[i].layer < 0) cmds_.push_back({draws[i].indexCount, draws[i].instCount, draws[i].firstIndex, static_cast<int32_t>(draws[i].baseVertex), draws[i].firstInstance});
        const uint32_t nOpaque = static_cast<uint32_t>(cmds_.size());
        for (uint32_t i = 0; i < n; ++i)
            if (draws[i].layer >= 0) {
                cmds_.push_back({draws[i].indexCount, draws[i].instCount, draws[i].firstIndex, static_cast<int32_t>(draws[i].baseVertex), draws[i].firstInstance});
                rows_s_.push_back({{draws[i].scaleX, draws[i].scaleY}, draws[i].layer, 0});
            }
        const uint32_t nAlpha = n - nOpaque;
        const uint32_t base = cursor_;
        if (!cmd_[parity_].upload(cmds_.data(), static_cast<VkDeviceSize>(n) * sizeof(VkDrawIndexedIndirectCommand), static_cast<VkDeviceSize>(base) * sizeof(VkDrawIndexedIndirectCommand))) return 0;
        if (nAlpha && !rows_[parity_].upload(rows_s_.data(), static_cast<VkDeviceSize>(nAlpha) * sizeof(ShadowAlphaRow), static_cast<VkDeviceSize>(base + nOpaque) * sizeof(ShadowAlphaRow))) return 0;
        cursor_ += n;
        const VkBuffer buf = cmd_[parity_].get();
        uint32_t calls = 0;
        if (nOpaque) {
            infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, opaque);
            mc2::battlebench::add(mc2::battlebench::DrawShadow);
            vkCmdDrawIndexedIndirect(cb, buf, static_cast<VkDeviceSize>(base) * sizeof(VkDrawIndexedIndirectCommand), nOpaque, sizeof(VkDrawIndexedIndirectCommand));
            ++calls;
        }
        if (nAlpha) {
            infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, alpha);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &alphaSet, 0, nullptr);
            const ShadowDrawPush push{rows_[parity_].deviceAddress(), base + nOpaque, 0};
            vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
            mc2::battlebench::add(mc2::battlebench::DrawShadow);
            vkCmdDrawIndexedIndirect(cb, buf, static_cast<VkDeviceSize>(base + nOpaque) * sizeof(VkDrawIndexedIndirectCommand), nAlpha, sizeof(VkDrawIndexedIndirectCommand));
            ++calls;
            infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, opaque);
        }
        return calls;
    }

private:
    OwnedBuffer cmd_[kMaxFramesInFlight], rows_[kMaxFramesInFlight];
    std::vector<VkDrawIndexedIndirectCommand> cmds_;
    std::vector<ShadowAlphaRow> rows_s_;
    uint32_t cap_ = 0, cursor_ = 0, parity_ = 0;
};

}  // namespace mc2vk

#endif  // MC2_VULKAN_ISLAND
#endif  // MC2_VK_SHADOW_DRAW_RING_H
