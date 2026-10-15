// vulkan_shadow_mech_casters.cpp -- see the header. Compiles to nothing unless MC2_VULKAN_ISLAND is defined.
#ifdef MC2_VULKAN_ISLAND

#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <volk.h>

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include <vk_mem_alloc.h>

#include "vulkan_shadow_mech_casters.h"

#include "vk_island_util.h"
#include "vulkan_context.h"

#include <cstdarg>
#include <cstring>
#include <string>

namespace mc2vk {

namespace {

void vlog(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vk_island_util::vlogv("[VK_SHADOW_MECH] ", fmt, ap);
    va_end(ap);
}

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

// Copies `bytes` into a host-visible buffer and flushes the write so the GPU sees it on memory that is not coherent. False when the map fails.
bool writeBuffer(VmaAllocator allocator, const OwnedBuffer& buffer, const void* src, VkDeviceSize bytes) {
    void* dst = buffer.map();
    if (!dst) return false;
    std::memcpy(dst, src, static_cast<size_t>(bytes));
    vmaFlushAllocation(allocator, buffer.allocation(), 0, VK_WHOLE_SIZE);
    buffer.unmap();
    return true;
}

}  // namespace

bool ShadowMechCasters::ensureBuffer(OwnedBuffer& buffer, VkDeviceSize& capacity, VkDeviceSize bytes, VkBufferUsageFlags usage, std::string_view name) {
    if (buffer && bytes <= capacity) return true;
    // The caller has retired every submit that read this buffer (the frame fence for a parity buffer, an idle device for the geometry).
    const VkDeviceSize cap = bytes + bytes / 2 + 4096;
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = cap;
    bci.usage = usage;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    OwnedBuffer fresh;
    if (!fresh.create(allocator_, device_, bci, aci, name)) {
        vlog("buffer '%.*s' (%llu bytes) could not be created.", static_cast<int>(name.size()), name.data(), static_cast<unsigned long long>(cap));
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
    vert_ = OwnedShaderModule(device_, vk_island_util::loadSpv(device_, dir + "shadow_mech.vert.spv", vlog));
    frag_ = OwnedShaderModule(device_, vk_island_util::loadSpv(device_, dir + "shadow_mech.frag.spv", vlog));
    if (!vert_ || !frag_) return false;

    VkDescriptorSetLayoutBinding b[3]{};
    b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    b[2].binding = 2; b[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; b[2].descriptorCount = 1; b[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayoutCreateInfo lci{};
    lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    lci.bindingCount = 3;
    lci.pBindings = b;
    if (!create(dsl_, device_, [&](auto* h) { return vkCreateDescriptorSetLayout(device_, &lci, nullptr, h); })) { vlog("descriptor layout failed."); return false; }

    VkDescriptorPoolSize sizes[2]{};
    sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; sizes[0].descriptorCount = 2;
    sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; sizes[1].descriptorCount = 4;
    VkDescriptorPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pci.maxSets = 2;
    pci.poolSizeCount = 2;
    pci.pPoolSizes = sizes;
    if (!create(pool_, device_, [&](auto* h) { return vkCreateDescriptorPool(device_, &pci, nullptr, h); })) { vlog("descriptor pool failed."); return false; }
    const VkDescriptorSetLayout dslHandle = dsl_.get();
    const VkDescriptorSetLayout setLayouts[2] = {dslHandle, dslHandle};
    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = pool_.get();
    dsai.descriptorSetCount = 2;
    dsai.pSetLayouts = setLayouts;
    if (vkAllocateDescriptorSets(device_, &dsai, set_) != VK_SUCCESS) { vlog("descriptor set allocation failed."); return false; }

    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &dslHandle;
    if (!create(layout_, device_, [&](auto* h) { return vkCreatePipelineLayout(device_, &plci, nullptr, h); })) { vlog("pipeline layout failed."); return false; }

    VkVertexInputBindingDescription vib{};
    vib.binding = 0;
    vib.stride = mc2_mech_vertex_stride();   // the batcher owns the vertex layout
    vib.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    // The mech vertex: position at 0, the four bone indices at 32, their weights at 36 (the rest of the stride is not read here).
    VkVertexInputAttributeDescription via[3]{};
    via[0].location = 0; via[0].binding = 0; via[0].format = VK_FORMAT_R32G32B32_SFLOAT; via[0].offset = 0;
    via[1].location = 3; via[1].binding = 0; via[1].format = VK_FORMAT_R8G8B8A8_UINT;    via[1].offset = 32;
    via[2].location = 4; via[2].binding = 0; via[2].format = VK_FORMAT_R8G8B8A8_UNORM;   via[2].offset = 36;
    VkPipelineVertexInputStateCreateInfo vin{};
    vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vin.vertexBindingDescriptionCount = 1; vin.pVertexBindingDescriptions = &vib;
    vin.vertexAttributeDescriptionCount = 3; vin.pVertexAttributeDescriptions = via;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vps{};
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1; vps.scissorCount = 1;
    // The state of the island's static-prop caster pipeline: no culling (GL's shadow pass draws both faces), depth bias on, forward-Z LESS.
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.depthBiasEnable = VK_TRUE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 0;
    const VkDynamicState dyn[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS};
    VkPipelineDynamicStateCreateInfo dsci{};
    dsci.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dsci.dynamicStateCount = 3; dsci.pDynamicStates = dyn;
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vert_.get(); stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = frag_.get(); stages[1].pName = "main";
    VkGraphicsPipelineCreateInfo gp{};
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount = 2; gp.pStages = stages;
    gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia; gp.pViewportState = &vps; gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms; gp.pDepthStencilState = &ds; gp.pColorBlendState = &cb; gp.pDynamicState = &dsci;
    gp.layout = layout_.get(); gp.renderPass = pass; gp.subpass = 0;
    if (!create(pipeline_, device_, [&](auto* h) { return vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gp, nullptr, h); })) {
        vlog("pipeline creation failed.");
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
        if (!logged) { logged = true; vlog("mech geometry refused: stride %u (want %u), %llu vertex bytes, %llu index bytes.",
                                           feed.vertexStride, vertexStride_, static_cast<unsigned long long>(feed.vboBytes), static_cast<unsigned long long>(feed.iboBytes)); }
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
        if (!writeBuffer(allocator_, vbo_, feed.vboCpu, feed.vboBytes) || !writeBuffer(allocator_, ibo_, feed.iboCpu, feed.iboBytes)) {
            geomUploaded_ = false;
            return;
        }
        geomGen_ = feed.geomGen;
        geomUploaded_ = true;
        iboIndexCount_ = feed.iboBytes / sizeof(uint32_t);
        vlog("mech caster geometry uploaded: %llu vertices, %llu indices (generation %u).",
             static_cast<unsigned long long>(feed.vboBytes / vertexStride_), static_cast<unsigned long long>(iboIndexCount_), geomGen_);
    }

    const uint32_t parity = static_cast<uint32_t>(epoch & 1u);
    const VkDeviceSize instBytes = static_cast<VkDeviceSize>(feed.instanceCount) * kMechInstanceBytes;
    const VkDeviceSize boneBytes = static_cast<VkDeviceSize>(feed.boneCount) * kMechBoneBytes;
    const VkBuffer oldInst = instBuf_[parity].get(), oldBone = boneBuf_[parity].get();
    if (!ensureBuffer(instBuf_[parity], instCap_[parity], instBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "shadow.mechInst") ||
        !ensureBuffer(boneBuf_[parity], boneCap_[parity], boneBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "shadow.mechBone")) return;
    if (instBuf_[parity].get() != oldInst || boneBuf_[parity].get() != oldBone) writeDescriptors(parity);
    if (!writeBuffer(allocator_, instBuf_[parity], feed.instances64, instBytes) || !writeBuffer(allocator_, boneBuf_[parity], feed.bones64, boneBytes)) return;

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
        if (!logged) { logged = true; vlog("%u of %u mech runs point outside the staged buffers and are not drawn.", dropped, feed.runCount); }
    }
}

void ShadowMechCasters::record(VkCommandBuffer cb, uint32_t parity, uint32_t uboOffset) const {
    if (!hasCasters() || !geomUploaded_ || !pipeline_) return;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.get());
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
