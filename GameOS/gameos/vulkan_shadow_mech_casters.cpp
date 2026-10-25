// vulkan_shadow_mech_casters.cpp -- see the header. Compiles to nothing unless MC2_VULKAN_ISLAND is defined.
#ifdef MC2_VULKAN_ISLAND

#if defined(_WIN32) && !defined(VK_USE_PLATFORM_WIN32_KHR)
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <volk.h>

#include "vulkan_shadow_mech_casters.h"

#include "mc2fmt.h"
#include "vk_depth_caster_pipeline.h"
#include "vk_island_util.h"
#include "vulkan_context.h"

#include <cstring>
#include <string>

namespace mc2vk {

namespace {

constexpr uint32_t kMechInstanceBytes = 64;
constexpr uint32_t kMechBoneBytes = 64;

// A run that points outside the buffers it was staged with would make the GPU read past them; it is dropped with a log line instead.
bool runFits(const Mc2MechDrawRun& r, uint64_t indexCount, uint64_t vertexCount, uint32_t instanceCount) {
    if (r.indexCount == 0 || r.instanceCount == 0) return false;
    if (static_cast<uint64_t>(r.firstIndex) + r.indexCount > indexCount) return false;
    if (static_cast<uint64_t>(r.instanceBase) + r.instanceCount > instanceCount) return false;
    if (r.baseVertex < 0 || static_cast<uint64_t>(r.baseVertex) >= vertexCount) return false;
    return true;
}

}  // namespace

bool ShadowMechCasters::ensureBuffer(OwnedBuffer& buffer, VkDeviceSize& capacity, VkDeviceSize bytes, VkBufferUsageFlags usage, std::string_view name) {
    if (buffer && bytes <= capacity) return true;
    // The caller has retired every submit that read this buffer (the frame fence for a parity buffer, an idle device for the geometry).
    const VkDeviceSize cap = bytes + bytes / 2 + 4096;
    OwnedBuffer fresh;
    if (!fresh.createHostWritable(allocator_, device_, cap, usage, name)) {
        mc2::fmt::print_err("[VK_SHADOW_MECH] buffer '{}' ({} bytes) could not be created.\n", name, mc2::fmt::as_u64(cap));
        return false;
    }
    buffer = std::move(fresh);
    capacity = cap;
    return true;
}

void ShadowMechCasters::writeDescriptors(uint32_t parity) {
    if (set_[parity] == VK_NULL_HANDLE || !instBuf_[parity] || !boneBuf_[parity]) return;
    const VkDescriptorBufferInfo uboInfo{frameUbo_, 0, uboRange_};
    const VkDescriptorBufferInfo instInfo{instBuf_[parity].get(), 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo boneInfo{boneBuf_[parity].get(), 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = set_[parity];
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = (i == 0) ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    }
    w[0].pBufferInfo = &uboInfo;
    w[1].pBufferInfo = &instInfo;
    w[2].pBufferInfo = &boneInfo;
    vkUpdateDescriptorSets(device_, 3, w, 0, nullptr);
}

bool ShadowMechCasters::init(VkDevice device, VmaAllocator allocator, VkRenderPass pass, VkBuffer frameUbo, VkDeviceSize uboRange, std::string_view spvDir) {
    device_ = device;
    allocator_ = allocator;
    frameUbo_ = frameUbo;
    uboRange_ = uboRange;
    vertexStride_ = mc2_mech_vertex_stride();

    const std::string dir(spvDir);
    vert_ = OwnedShaderModule(device_, vk_island_util::loadSpv(device_, dir + "shadow_mech.vert.spv", nullptr));
    frag_ = OwnedShaderModule(device_, vk_island_util::loadSpv(device_, dir + "shadow_mech.frag.spv", nullptr));
    if (!vert_ || !frag_) {
        mc2::fmt::print_err("[VK_SHADOW_MECH] shadow_mech SPIR-V missing under '{}'.\n", dir);
        return false;
    }

    VkDescriptorSetLayoutBinding b[3]{};
    b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    b[2].binding = 2; b[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; b[2].descriptorCount = 1; b[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayoutCreateInfo lci{};
    lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    lci.bindingCount = 3;
    lci.pBindings = b;
    if (!create(dsl_, device_, [&](auto* h) { return vkCreateDescriptorSetLayout(device_, &lci, nullptr, h); })) { mc2::fmt::print_err("[VK_SHADOW_MECH] descriptor layout failed.\n"); return false; }

    VkDescriptorPoolSize sizes[2]{};
    sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; sizes[0].descriptorCount = 2;
    sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; sizes[1].descriptorCount = 4;
    VkDescriptorPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pci.maxSets = 2;
    pci.poolSizeCount = 2;
    pci.pPoolSizes = sizes;
    if (!create(pool_, device_, [&](auto* h) { return vkCreateDescriptorPool(device_, &pci, nullptr, h); })) { mc2::fmt::print_err("[VK_SHADOW_MECH] descriptor pool failed.\n"); return false; }
    const VkDescriptorSetLayout dslHandle = dsl_.get();
    const VkDescriptorSetLayout setLayouts[2] = {dslHandle, dslHandle};
    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = pool_.get();
    dsai.descriptorSetCount = 2;
    dsai.pSetLayouts = setLayouts;
    if (vkAllocateDescriptorSets(device_, &dsai, set_) != VK_SUCCESS) { mc2::fmt::print_err("[VK_SHADOW_MECH] descriptor set allocation failed.\n"); return false; }

    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &dslHandle;
    if (!create(layout_, device_, [&](auto* h) { return vkCreatePipelineLayout(device_, &plci, nullptr, h); })) { mc2::fmt::print_err("[VK_SHADOW_MECH] pipeline layout failed.\n"); return false; }

    // The mech vertex: position at 0, the four bone indices at 32, their weights at 36 (the rest of the stride is not read here).
    VkVertexInputBindingDescription vib{};
    vib.binding = 0; vib.stride = mc2_mech_vertex_stride();   // the batcher owns the vertex layout
    vib.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription via[3]{};
    via[0].location = 0; via[0].binding = 0; via[0].format = VK_FORMAT_R32G32B32_SFLOAT; via[0].offset = 0;
    via[1].location = 3; via[1].binding = 0; via[1].format = VK_FORMAT_R8G8B8A8_UINT;    via[1].offset = 32;
    via[2].location = 4; via[2].binding = 0; via[2].format = VK_FORMAT_R8G8B8A8_UNORM;   via[2].offset = 36;
    if (!createDepthCasterPipeline(pipeline_, device_, pass, layout_.get(), vert_.get(), frag_.get(), vib, via)) {
        mc2::fmt::print_err("[VK_SHADOW_MECH] pipeline creation failed.\n");
        return false;
    }
    nameObject(device_, pipeline_.get(), "shadow.mechPipe");
    inited_ = true;
    return true;
}

void ShadowMechCasters::stage(uint64_t epoch) {
    if (!inited_ || epoch == stagedEpoch_) return;
    stagedEpoch_ = epoch;
    runs_.clear();
    stagedInstances_ = 0;

    Mc2MechShadowFeed feed{};
    if (!mc2_mech_island_get_shadow_feed(&feed)) return;
    if (feed.vertexStride != vertexStride_ || feed.vboBytes % vertexStride_ != 0 || feed.iboBytes % sizeof(uint32_t) != 0) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            mc2::fmt::print_err("[VK_SHADOW_MECH] mech geometry refused: stride {} (want {}), {} vertex bytes, {} index bytes.\n", feed.vertexStride, vertexStride_,
                                mc2::fmt::as_u64(feed.vboBytes), mc2::fmt::as_u64(feed.iboBytes));
        }
        return;
    }

    if (!geomUploaded_ || feed.geomGen != geomGen_) {
        // The geometry is shared by both parities and may still be read by a frame in flight: a map change or a normals rebuild is rare, so the device idles.
        getOrCreateEngineVulkanContext().waitIdle("shadow_mech_geometry");
        if (!ensureBuffer(vbo_, vboCap_, feed.vboBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, "shadow.mechVbo") ||
            !ensureBuffer(ibo_, iboCap_, feed.iboBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, "shadow.mechIbo")) {
            geomUploaded_ = false;
            return;
        }
        if (!vbo_.upload(feed.vboCpu, feed.vboBytes) || !ibo_.upload(feed.iboCpu, feed.iboBytes)) {
            geomUploaded_ = false;
            return;
        }
        geomGen_ = feed.geomGen;
        geomUploaded_ = true;
        iboIndexCount_ = feed.iboBytes / sizeof(uint32_t);
        mc2::fmt::print_err("[VK_SHADOW_MECH] mech caster geometry uploaded: {} vertices, {} indices (generation {}).\n", mc2::fmt::as_u64(feed.vboBytes / vertexStride_),
                            mc2::fmt::as_u64(iboIndexCount_), geomGen_);
    }

    const uint32_t parity = static_cast<uint32_t>(epoch & 1u);
    const VkDeviceSize instBytes = static_cast<VkDeviceSize>(feed.instanceCount) * kMechInstanceBytes;
    const VkDeviceSize boneBytes = static_cast<VkDeviceSize>(feed.boneCount) * kMechBoneBytes;
    const VkBuffer oldInst = instBuf_[parity].get(), oldBone = boneBuf_[parity].get();
    if (!ensureBuffer(instBuf_[parity], instCap_[parity], instBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "shadow.mechInst") ||
        !ensureBuffer(boneBuf_[parity], boneCap_[parity], boneBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "shadow.mechBone")) return;
    if (instBuf_[parity].get() != oldInst || boneBuf_[parity].get() != oldBone) writeDescriptors(parity);
    if (!instBuf_[parity].upload(feed.instances64, instBytes) || !boneBuf_[parity].upload(feed.bones64, boneBytes)) return;

    const uint64_t vertexCount = feed.vboBytes / vertexStride_;
    uint32_t dropped = 0;
    runs_.reserve(feed.runCount);
    for (uint32_t k = 0; k < feed.runCount; ++k) {
        if (runFits(feed.runs[k], iboIndexCount_, vertexCount, feed.instanceCount)) {
            runs_.push_back(feed.runs[k]);
            stagedInstances_ += feed.runs[k].instanceCount;
        } else {
            ++dropped;
        }
    }
    if (dropped != 0) {
        static bool logged = false;
        if (!logged) { logged = true; mc2::fmt::print_err("[VK_SHADOW_MECH] {} of {} mech runs point outside the staged buffers and are not drawn.\n", dropped, feed.runCount); }
    }
}

void ShadowMechCasters::record(VkCommandBuffer cb, uint32_t parity, uint32_t uboOffset) const {
    if (!hasCasters() || !geomUploaded_ || !pipeline_) return;
    mc2vk::infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.get());
    const VkDescriptorSet set = set_[parity & 1u];
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_.get(), 0, 1, &set, 1, &uboOffset);
    const VkBuffer vertexBuf = vbo_.get();
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cb, 0, 1, &vertexBuf, &offset);
    vkCmdBindIndexBuffer(cb, ibo_.get(), 0, VK_INDEX_TYPE_UINT32);
    for (const Mc2MechDrawRun& r : runs_)
        vkCmdDrawIndexed(cb, r.indexCount, r.instanceCount, r.firstIndex, r.baseVertex, r.instanceBase);
}

}  // namespace mc2vk

#endif  // MC2_VULKAN_ISLAND
