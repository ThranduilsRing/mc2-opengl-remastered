// vulkan_mech_skin.h -- the once-per-frame GPU pre-skin of the mech vertex stream (compute), shared by the colour/depth island and the shadow casters.
//
// WHAT: reads the bind-pose mech VBO (copy-once, device-local) and a bone palette, and writes one skinned region per mech instance (an instance is one node
// of one actor) into a per-frame-slot device-local buffer that keeps the 48 B GpuMechVertex layout. The consumers (depth, colour, shadow) bind that buffer
// instead of the VBO for a flagged instance and skip their per-vertex bone blend, so a mech node is skinned once per frame instead of once per pass.
// WHY: each pass blended four bone matrices per vertex; the depth pre-pass, the colour pass and every shadow cascade repeated the same maths.
//
// HOW: `plan` flags the skinnable instances (renderFlags bit 7, in the caller's mutable copy of the instance records only), assigns each a region and writes
// the job list; `record` dispatches the compute and the barrier; `drawRun` emits the draws of one run (a flagged instance is its own draw, reading its region
// through baseVertex; unflagged neighbours stay one instanced draw on the bind-pose VBO). An instance whose run has no known node vertex range
// (Mc2MechDrawRun::vertexCount 0: dedupe skipped, a foreign range), or that would overflow the buffer cap, stays unflagged and is drawn exactly as before,
// so rigid meshes, vehicles and mod mechs never get worse. Each frame slot owns its buffers: no frame in flight is overwritten.
#ifndef MC2_VULKAN_MECH_SKIN_H
#define MC2_VULKAN_MECH_SKIN_H


#include <volk.h>

#include "vk_compute_pipeline.h"
#include "vk_desc_write_cache.h"
#include "vk_frame_ring.h"
#include "vk_owned.h"
#include "vulkan_mech_island.h"   // Mc2MechDrawRun

#include <cstdint>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mc2vk {

class MechSkinPass {
public:
    // Builds the compute pipeline from `<spvDir>mech_skin.comp.spv` and one descriptor set per frame slot. False (logged) leaves the pass unusable; every
    // plan() is then a no-op and the consumers blend bones as before.
    bool init(VkDevice device, VmaAllocator allocator, std::string_view spvDir);
    bool ready() const noexcept { return static_cast<bool>(pipe_) && sets_.set(0) != VK_NULL_HANDLE; }

    // Plans the frame of `slot` (the caller has retired the slot's previous submit). `srcInstances` is the CPU copy of the 64 B instance records and `instances` the
    // destination copy (maybe write-combined: only flag words are written); flagged ones get kMechRenderFlagPreskinned there. `vboVertices` is the vertex count of the bind-pose VBO. Afterwards active() says whether record() has work.
    void plan(uint32_t slot, std::span<const Mc2MechDrawRun> runs, std::span<const uint8_t> srcInstances, std::span<uint8_t> instances, uint64_t vboVertices);
    bool active() const noexcept { return active_; }
    void cancel() noexcept { active_ = false; }   // a frame that stops before plan() must not draw last frame regions

    // Records the dispatch and the compute -> vertex-input barrier into `cb` (outside any render pass). `bones` is the palette buffer with `boneRange` valid
    // from every dynamic offset and `boneOffset` the byte offset of this frame's palette.
    void record(VkCommandBuffer cb, VkBuffer srcVbo, VkDeviceSize srcBytes, VkBuffer bones, VkDeviceSize boneRange, uint32_t boneOffset);

    // Emits the draws of run `r` (the index into the list given to plan). `draw(instanceCount, firstInstance, baseVertex)` is called once per draw, after
    // the right vertex buffer is bound; `boundVertexBuffer` tracks what is bound so consecutive draws rebind only on a change.
    template <class CountFn>
    uint32_t drawRun(VkCommandBuffer cb, uint32_t r, const Mc2MechDrawRun& run, VkBuffer bindPoseVbo, VkBuffer& boundVertexBuffer, CountFn&& issue) const {
        uint32_t draws = 0;
        const int32_t* dst = active_ && r + 1 < runOff_.size() ? dst_.data() + runOff_[r] : nullptr;
        uint32_t k = 0;
        while (k < run.instanceCount) {
            const bool skinned = dst && dst[k] >= 0;
            uint32_t n = 1;
            if (!skinned) while (k + n < run.instanceCount && dst && dst[k + n] < 0) ++n;
            if (!skinned && !dst) n = run.instanceCount - k;
            const VkBuffer want = skinned ? skinned_[slot_].get() : bindPoseVbo;
            if (want != boundVertexBuffer) {
                const VkDeviceSize zero = 0;
                vkCmdBindVertexBuffers(cb, 0, 1, &want, &zero);
                boundVertexBuffer = want;
            }
            issue(n, run.instanceBase + k, skinned ? dst[k] : run.baseVertex);
            ++draws;
            k += n;
        }
        return draws;
    }

    uint32_t plannedInstances() const noexcept { return plannedInstances_; }

    // RT movers: the skinned buffers also carry SHADER_DEVICE_ADDRESS | ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY so a mover BLAS builds straight from
    // the skin output (no readback). Set once after init, before the first plan(); buffers created earlier are recreated on the next plan of their slot.
    void setRayInput(bool on) noexcept;
    static constexpr uint32_t kVertexStride = 40;   // GpuMechVertex; position R32G32B32_SFLOAT at offset 0
    // First skinned vertex of instance `k` of run `r` this frame, -1 when it draws from the bind-pose VBO (or nothing is active).
    int32_t region(uint32_t r, uint32_t k) const noexcept {
        if (!active_ || r + 1 >= runOff_.size() || k >= runOff_[r + 1] - runOff_[r]) return -1;
        return dst_[runOff_[r] + k];
    }
    // Device address of this frame's skinned buffer (0 without setRayInput or when nothing is active).
    VkDeviceAddress skinnedAddress() const noexcept { return active_ ? skinned_[slot_].deviceAddress() : 0; }
    uint32_t slot() const noexcept { return slot_; }

private:
    struct Job { uint32_t srcFirst, count, dstFirst, boneBase; };

    bool ensure(uint32_t slot, uint64_t vertices, uint64_t jobs);

    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = nullptr;
    uint32_t slot_ = 0;
    bool active_ = false;
    uint32_t plannedInstances_ = 0;
    uint32_t maxCount_ = 0;
    uint32_t jobCount_ = 0;
    VkBufferUsageFlags extraUsage_ = 0;             // setRayInput
    bool usageStale_[kMaxFramesInFlight] = {};      // the slot's skinned buffer predates extraUsage_

    std::vector<Job> jobs_;
    std::vector<uint32_t> runOff_;     // per run: offset of its instances in dst_ (size runCount + 1)
    std::vector<int32_t> dst_;         // per run instance: first vertex of its skinned region, -1 = drawn from the bind-pose VBO
    std::vector<uint8_t> bad_, used_;
    std::unordered_map<uint64_t, uint32_t> region_;

    uint64_t vertexCap_[kMaxFramesInFlight] = {}, jobCap_[kMaxFramesInFlight] = {};
    OwnedBuffer skinned_[kMaxFramesInFlight], jobBuf_[kMaxFramesInFlight];
    ComputePipeline pipe_;
    ComputeSets sets_;
    DescWriteStamp stamps_[kMaxFramesInFlight][4];  // per slot, per binding 0..3: skip a write whose buffer/range the set already holds
};

}  // namespace mc2vk

#endif  // MC2_VULKAN_MECH_SKIN_H
