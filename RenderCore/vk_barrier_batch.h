#pragma once
// Vulkan-free synchronization2 barrier batching (vk-sync-arch, vk-sync-opusA item 3/R6).
//
// BarrierBatch collects the image, buffer and memory barriers of one sync point as plain records; the Vulkan adapter
// (GameOS/gameos/vk_barrier_batch_vk.h) turns them into ONE vkCmdPipelineBarrier2. ImageTracker remembers the state of one
// image per mip and derives the source of every barrier from the PREVIOUS use, so a call site names only what it is about to
// do: tracker.use(batch, {layout, stages, access, write}). The tracker lives as long as the image, so the state carries across frames.
// Masks are VkPipelineStageFlags2 / VkAccessFlags2 values; layouts are VkImageLayout values (0 = UNDEFINED).
#include "vk_resource_graph.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace mc2vk::sync {
using graph::State;

struct ImageBarrierRec {
    uint64_t image = 0;   // VkImage handle value
    uint32_t aspect = 0, baseMip = 0, mipCount = 1, baseLayer = 0, layerCount = 1;
    State before, after;
};
struct BufferBarrierRec {
    uint64_t buffer = 0;  // VkBuffer handle value
    uint64_t offset = 0, size = ~0ull;
    uint64_t srcStages = 0, srcAccess = 0, dstStages = 0, dstAccess = 0;
};
struct MemoryBarrierRec { uint64_t srcStages = 0, srcAccess = 0, dstStages = 0, dstAccess = 0; };

class BarrierBatch {
public:
    void image(const ImageBarrierRec& r) { images_.push_back(r); }
    void growLastImage(uint32_t mipCount) { images_.back().mipCount = mipCount; }
    void buffer(const BufferBarrierRec& r) { buffers_.push_back(r); }
    // Global memory barriers of one batch merge into one (the union of the source and the destination scopes).
    void memory(uint64_t srcStages, uint64_t srcAccess, uint64_t dstStages, uint64_t dstAccess) {
        memory_.srcStages |= srcStages; memory_.srcAccess |= srcAccess;
        memory_.dstStages |= dstStages; memory_.dstAccess |= dstAccess;
        hasMemory_ = true;
    }
    bool empty() const { return images_.empty() && buffers_.empty() && !hasMemory_; }
    size_t size() const { return images_.size() + buffers_.size() + (hasMemory_ ? 1 : 0); }
    void clear() { images_.clear(); buffers_.clear(); memory_ = {}; hasMemory_ = false; }
    const std::vector<ImageBarrierRec>& images() const { return images_; }
    const std::vector<BufferBarrierRec>& buffers() const { return buffers_; }
    const MemoryBarrierRec* memoryBarrier() const { return hasMemory_ ? &memory_ : nullptr; }
private:
    std::vector<ImageBarrierRec> images_;
    std::vector<BufferBarrierRec> buffers_;
    MemoryBarrierRec memory_;
    bool hasMemory_ = false;
};

// Tracks the layout, last access and visibility of one image, per mip level.
class ImageTracker {
public:
    ImageTracker() = default;
    ImageTracker(uint64_t image, uint32_t aspect, uint32_t mips = 1, uint32_t layers = 1, State initial = {}) {
        reset(image, aspect, mips, layers, initial);
    }
    void reset(uint64_t image, uint32_t aspect, uint32_t mips = 1, uint32_t layers = 1, State initial = {}) {
        image_ = image; aspect_ = aspect; layers_ = std::max(layers, 1u);
        cur_.assign(std::max(mips, 1u), initial);
        vis_.assign(cur_.size(), initial.write ? State{} : initial);
    }
    uint64_t image() const { return image_; }
    const State& state(uint32_t mip = 0) const { return cur_[mip]; }
    // The contents are discarded (resize, recreate, the image went back to UNDEFINED outside the tracker).
    void invalidate() { std::fill(cur_.begin(), cur_.end(), State{}); std::fill(vis_.begin(), vis_.end(), State{}); }
    // A producer outside the tracker (a render pass final layout, a subpass dependency) left the image in `s`: no barrier is emitted.
    void assume(const State& s, uint32_t baseMip = 0, uint32_t mipCount = 0) {
        forEach(baseMip, mipCount, [&](uint32_t m) { cur_[m] = s; vis_[m] = s.write ? State{} : s; });
    }
    // Records the barrier(s) from the previous use to `dst` into `batch`. Returns the number of barrier records added (0 when the previous
    // use already made the contents visible: same layout, both reads, the stages and accesses covered).
    unsigned use(BarrierBatch& batch, const State& dst, uint32_t baseMip = 0, uint32_t mipCount = 0) {
        unsigned added = 0;
        bool open = false;
        forEach(baseMip, mipCount, [&](uint32_t m) {
            State& before = cur_[m];
            State& vis = vis_[m];
            const State src = before;
            bool need = false;
            if (before.layout != dst.layout || before.write || dst.write) {
                need = true;
                before = dst;
                vis = dst;
            } else {
                if ((dst.stages & ~vis.stages) || (dst.access & ~vis.access)) {
                    need = true;
                    vis.stages |= dst.stages;
                    vis.access |= dst.access;
                }
                before.stages |= dst.stages;
                before.access |= dst.access;
            }
            if (!need) { open = false; return; }
            if (open && graph::sameState(last_.before, src) && last_.baseMip + last_.mipCount == m) {
                ++last_.mipCount;
                batch.growLastImage(last_.mipCount);
            } else {
                last_ = {image_, aspect_, m, 1, 0, layers_, src, dst};
                batch.image(last_);
                open = true;
                ++added;
            }
        });
        return added;
    }
private:
    template <class F> void forEach(uint32_t baseMip, uint32_t mipCount, F&& f) {
        const uint32_t n = uint32_t(cur_.size());
        const uint32_t end = mipCount ? std::min(baseMip + mipCount, n) : n;
        for (uint32_t m = baseMip; m < end; ++m) f(m);
    }
    uint64_t image_ = 0;
    uint32_t aspect_ = 0, layers_ = 1;
    std::vector<State> cur_, vis_;
    ImageBarrierRec last_;
};
}  // namespace mc2vk::sync
