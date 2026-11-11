// vulkan_mech_skin.cpp -- see the header. Compiles to nothing unless MC2_VULKAN_ISLAND is defined.

#if defined(_WIN32) && !defined(VK_USE_PLATFORM_WIN32_KHR)
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <volk.h>

#include "vulkan_mech_skin.h"

#include "diagnostic_trace.h"
#include "gos_mech_batcher.h"   // kMechRenderFlagPreskinned, mc2_mech_vertex_stride
#include "mc2fmt.h"
#include "vk_island_util.h"
#include "vulkan_context.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace mc2vk {

namespace {

constexpr uint32_t kVertexBytes = 40;                  // GpuMechVertex
constexpr uint64_t kMaxSkinVertices = 1ull << 21;       // 96 MB per frame slot; an instance past the cap stays on the inline blend
constexpr uint64_t kMaxStorageRange = 128ull << 20;     // the portable maxStorageBufferRange floor: bigger source or skinned buffers stay inline
constexpr uint32_t kInstanceBytes = 64;
constexpr uint32_t kRenderFlagsWord = 3;                // GpuMechInstance::renderFlags

}  // namespace

bool MechSkinPass::init(VkDevice device, VmaAllocator allocator, std::string_view spvDir) {
    device_ = device;
    allocator_ = allocator;
    if (mc2_mech_vertex_stride() != kVertexBytes) return false;   // the shader strides 10 words per vertex
    const std::string path = std::string(spvDir) + "mech_skin.comp.spv";
    const std::vector<ComputeBinding> bindings = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},          // bind-pose vertices
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC},  // bone palette (frame ring)
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},          // jobs
        {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},          // skinned vertices
    };
    if (!pipe_.create(device, path, bindings, "mech.skin", false)) return false;
    if (!sets_.create(device, pipe_, kMaxFramesInFlight, "mech.skin.sets", false)) return false;
    return true;
}

bool MechSkinPass::ensure(uint32_t slot, uint64_t vertices, uint64_t jobs) {
    // The caller has retired every submit that read this slot's buffers.
    if (!skinned_[slot] || vertices > vertexCap_[slot] || usageStale_[slot]) {
        const uint64_t cap = std::min<uint64_t>(kMaxSkinVertices, std::max<uint64_t>(vertices + vertices / 4, 1ull << 14));
        OwnedBuffer fresh;
        if (!fresh.createDeviceLocal(allocator_, device_, cap * kVertexBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | extraUsage_,
                                     "mech.skinned", static_cast<int>(slot))) {
            if (mc2_diag::tagRequested("VK_MECH_SKIN")) mc2::fmt::print_err("[VK_MECH_SKIN] skinned buffer ({} vertices) could not be created; mechs blend inline.\n", mc2::fmt::as_u64(cap));
            return false;
        }
        skinned_[slot] = std::move(fresh);
        vertexCap_[slot] = cap;
        usageStale_[slot] = false;
        stamps_[slot][3].invalidate();  // a recycled VkBuffer handle value must not match the cached key
    }
    if (!jobBuf_[slot] || jobs > jobCap_[slot]) {
        const uint64_t cap = std::max<uint64_t>(jobs + jobs / 2, 256);
        OwnedBuffer fresh;
        if (!fresh.createHostWritable(allocator_, device_, cap * sizeof(Job), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "mech.skin.jobs", static_cast<int>(slot))) return false;
        jobBuf_[slot] = std::move(fresh);
        jobCap_[slot] = cap;
        stamps_[slot][2].invalidate();
    }
    return true;
}

void MechSkinPass::plan(uint32_t slot, std::span<const Mc2MechDrawRun> runSpan, std::span<const uint8_t> srcSpan, std::span<uint8_t> dstSpan, uint64_t vboVertices) {
    const Mc2MechDrawRun* runs = runSpan.data();
    const uint32_t runCount = static_cast<uint32_t>(runSpan.size());
    const uint8_t* srcInstances = srcSpan.data();
    uint8_t* instances = dstSpan.data();
    const uint32_t instanceCount = static_cast<uint32_t>(std::min(srcSpan.size(), dstSpan.size()) / kInstanceBytes);
    active_ = false;
    plannedInstances_ = 0;
    jobs_.clear();
    runOff_.assign(static_cast<size_t>(runCount) + 1, 0);
    if (!ready() || !runs || !srcInstances || !instances || runCount == 0 || instanceCount == 0 || slot >= kMaxFramesInFlight) return;
    if (vboVertices * kVertexBytes > kMaxStorageRange) return;
    slot_ = slot;

    // A run is skinnable when its node vertex range is known and lies inside the VBO; an instance is skinned only when every run that draws it is.
    auto runOk = [&](const Mc2MechDrawRun& r) {
        if (r.indexCount == 0 || r.instanceCount == 0 || r.vertexCount == 0 || r.baseVertex < 0) return false;
        if (static_cast<uint64_t>(r.instanceBase) + r.instanceCount > instanceCount) return false;
        return static_cast<uint64_t>(r.baseVertex) + r.vertexCount <= vboVertices;
    };
    uint32_t total = 0;
    for (uint32_t r = 0; r < runCount; ++r) {
        runOff_[r] = total;
        total += runs[r].instanceCount;
    }
    runOff_[runCount] = total;
    bad_.assign(instanceCount, 0);
    used_.assign(instanceCount, 0);
    for (uint32_t r = 0; r < runCount; ++r) {
        const Mc2MechDrawRun& run = runs[r];
        if (run.indexCount == 0 || run.instanceCount == 0) continue;
        const bool ok = runOk(run);
        const uint32_t last = static_cast<uint32_t>(std::min<uint64_t>(static_cast<uint64_t>(run.instanceBase) + run.instanceCount, instanceCount));
        for (uint32_t i = run.instanceBase; i < last; ++i) { used_[i] = 1; if (!ok) bad_[i] = 1; }
        if (!ok && static_cast<uint64_t>(run.instanceBase) + run.instanceCount > instanceCount)
            for (uint32_t i = run.instanceBase; i < last; ++i) bad_[i] = 1;
    }

    region_.clear();
    uint64_t vertices = 0;
    uint32_t maxCount = 0;
    for (uint32_t r = 0; r < runCount; ++r) {
        const Mc2MechDrawRun& run = runs[r];
        if (!runOk(run)) continue;
        for (uint32_t k = 0; k < run.instanceCount; ++k) {
            const uint32_t i = run.instanceBase + k;
            if (bad_[i]) continue;
            const uint64_t key = (static_cast<uint64_t>(i) << 32) | static_cast<uint32_t>(run.baseVertex);
            if (region_.find(key) != region_.end()) continue;
            if (vertices + run.vertexCount > kMaxSkinVertices) { bad_[i] = 1; continue; }
            // The instance's palette base is its baseBoneOffset (word 1 of the record).
            uint32_t boneBase = 0;
            std::memcpy(&boneBase, srcInstances + static_cast<size_t>(i) * kInstanceBytes + 4, sizeof(boneBase));
            region_[key] = static_cast<uint32_t>(vertices);
            jobs_.push_back({static_cast<uint32_t>(run.baseVertex), run.vertexCount, static_cast<uint32_t>(vertices), boneBase});
            vertices += run.vertexCount;
            maxCount = (std::max)(maxCount, run.vertexCount);
        }
    }
    if (jobs_.empty() || !ensure(slot, vertices, jobs_.size())) { jobs_.clear(); return; }

    dst_.assign(total, -1);
    for (uint32_t r = 0; r < runCount; ++r) {
        const Mc2MechDrawRun& run = runs[r];
        if (!runOk(run)) continue;
        for (uint32_t k = 0; k < run.instanceCount; ++k) {
            const uint32_t i = run.instanceBase + k;
            if (bad_[i]) continue;
            const uint64_t key = (static_cast<uint64_t>(i) << 32) | static_cast<uint32_t>(run.baseVertex);
            if (!region_.contains(key)) continue;
            dst_[runOff_[r] + k] = static_cast<int32_t>(region_.at(key));
        }
    }
    // Flag exactly the instances that every run draws from the skinned region: a half-flagged instance would pose twice.
    for (uint32_t r = 0; r < runCount; ++r) {
        const Mc2MechDrawRun& run = runs[r];
        if (run.indexCount == 0 || run.instanceCount == 0) continue;
        for (uint32_t k = 0; k < run.instanceCount; ++k) {
            const uint32_t i = run.instanceBase + k;
            if (i < instanceCount && dst_[runOff_[r] + k] < 0) bad_[i] = 1;
        }
    }
    for (uint32_t r = 0; r < runCount; ++r) {
        const Mc2MechDrawRun& run = runs[r];
        for (uint32_t k = 0; k < run.instanceCount && run.indexCount; ++k) {
            const uint32_t i = run.instanceBase + k;
            if (i >= instanceCount) break;
            if (bad_[i]) { dst_[runOff_[r] + k] = -1; continue; }
        }
    }
    for (uint32_t i = 0; i < instanceCount; ++i) {
        if (!used_[i] || bad_[i]) continue;
        uint32_t flags = 0;
        // Read from the CPU copy (the destination may be write-combined ring memory), write the one flags word.
        std::memcpy(&flags, srcInstances + static_cast<size_t>(i) * kInstanceBytes + kRenderFlagsWord * sizeof(uint32_t), sizeof(flags));
        flags |= kMechRenderFlagPreskinned;
        std::memcpy(instances + static_cast<size_t>(i) * kInstanceBytes + kRenderFlagsWord * sizeof(uint32_t), &flags, sizeof(flags));
        ++plannedInstances_;
    }
    if (plannedInstances_ == 0) { jobs_.clear(); return; }
    if (!jobBuf_[slot].upload(jobs_.data(), jobs_.size() * sizeof(Job))) { jobs_.clear(); plannedInstances_ = 0; return; }
    maxCount_ = maxCount;
    jobCount_ = static_cast<uint32_t>(jobs_.size());
    active_ = true;
}

void MechSkinPass::setRayInput(bool on) noexcept {
    static_assert(kVertexStride == kVertexBytes);
    const VkBufferUsageFlags want = on ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR : 0;
    if (want == extraUsage_) return;
    extraUsage_ = want;
    for (bool& s : usageStale_) s = true;   // ensure() swaps each slot's buffer the next time it plans that slot (its prior submit has retired then)
}

void MechSkinPass::writeDescriptors(VkBuffer srcVbo, VkDeviceSize srcBytes, VkBuffer bones, VkDeviceSize boneRange) {
    if (!active_ || srcVbo == VK_NULL_HANDLE || bones == VK_NULL_HANDLE) return;
    // Each binding is rewritten only when its buffer or range changed since this slot's set last saw it (the slot's prior submit has retired).
    const VkDescriptorType types[4] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    const VkBuffer bufs[4] = {srcVbo, bones, jobBuf_[slot_].get(), skinned_[slot_].get()};
    const VkDeviceSize ranges[4] = {srcBytes, boneRange, VK_WHOLE_SIZE, VK_WHOLE_SIZE};
    uint32_t issued = 0;
    for (uint32_t b = 0; b < 4; ++b) {
        if (!stamps_[slot_][b].needsWrite({descKeyWord(bufs[b]), ranges[b], b, descKeyWord(sets_.set(slot_))})) continue;
        sets_.write(device_, slot_, b, types[b], bufs[b], ranges[b]);
        ++issued;
    }
    noteDescWrites(issued, 4 - issued);
}

static void dispatchSkin(const ComputePipeline& pipe, VkDescriptorSet set, uint32_t maxCount, uint32_t jobCount, VkCommandBuffer cb, uint32_t boneOffset) {
    pipe.bind(cb, set, std::span<const uint32_t>(&boneOffset, 1));
    vkCmdDispatch(cb, (maxCount + 63u) / 64u, jobCount, 1);
    // The skinned region feeds the vertex-input stage of the depth, colour and shadow passes recorded after this.
    cmdBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT);
}

void MechSkinPass::Frozen::recordDispatch(VkCommandBuffer cb, uint32_t boneOffset) const {
    if (!active || !pipe) return;
    dispatchSkin(*pipe, set, maxCount, jobCount, cb, boneOffset);
}

MechSkinPass::Frozen MechSkinPass::freeze() const {
    Frozen f;
    f.pipe = &pipe_; f.set = sets_.set(slot_); f.active = active_; f.maxCount = maxCount_; f.jobCount = jobCount_;
    f.skinned = skinned_[slot_].get(); f.runOff = runOff_; f.dst = dst_;
    return f;
}

void MechSkinPass::record(VkCommandBuffer cb, VkBuffer srcVbo, VkDeviceSize srcBytes, VkBuffer bones, VkDeviceSize boneRange, uint32_t boneOffset) {
    if (!active_ || srcVbo == VK_NULL_HANDLE || bones == VK_NULL_HANDLE) return;
    writeDescriptors(srcVbo, srcBytes, bones, boneRange);
    dispatchSkin(pipe_, sets_.set(slot_), maxCount_, jobCount_, cb, boneOffset);
}

}  // namespace mc2vk

