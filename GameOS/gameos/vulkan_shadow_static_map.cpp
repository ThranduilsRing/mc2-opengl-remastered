// vulkan_shadow_static_map.cpp -- see the header. Compiles to nothing unless MC2_VULKAN_ISLAND is defined.
#ifdef MC2_VULKAN_ISLAND

#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <volk.h>

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include <vk_mem_alloc.h>

#include "vulkan_shadow_static_map.h"

#include "../../RenderCore/gpu_abi/GpuStaticPropInstance.h"
#include "gos_static_prop_registry.h"
#include "mc2fmt.h"
#include "static_shadow_spec.h"
#include "terrain_field_store.h"
#include "vk_depth_caster_pipeline.h"
#include "vk_island_util.h"
#include "vulkan_context.h"
#include "vk_gpu_profile.h"   // VK-GPU-ZONES: MC2_VK_GPU_SCOPE

#include <algorithm>
#include <cstring>
#include <string>

namespace mc2vk {

namespace {

constexpr VkFormat kDepthFmt = VK_FORMAT_D32_SFLOAT;
// The terrain grid spacing (GL: Terrain::worldUnitsPerVertex, 128 on every map).
constexpr float kWorldUnitsPerVertex = 128.0f;
// The largest map edge (in texels) and terrain side (in vertices) accepted; anything larger is a malformed input, not a map.
constexpr int kMaxMapSize = 16384;
constexpr int kMaxTerrainSide = 4097;

// LOCKSTEP with shadow_prop.vert / shadow_terrain_static.vert ShadowFrameUBO (std140).
struct StaticFrameUBO {
    float lightSpaceMatrix[16];
    int32_t instBase;
    int32_t pad[3];
};
static_assert(sizeof(StaticFrameUBO) == 80, "StaticFrameUBO std140 size (shadow_prop.vert)");

void slog(const char* what) { mc2::fmt::print_err("[VK_SHADOW_STATIC] {}\n", what); }

}  // namespace

bool ShadowStaticMap::ensureBuffer(OwnedBuffer& buffer, VkDeviceSize& capacity, VkDeviceSize bytes, VkBufferUsageFlags usage, std::string_view name) {
    if (buffer && bytes <= capacity) return true;
    // The caller idled the device before a rebuild, so no submit still reads the buffer being replaced.
    const VkDeviceSize cap = bytes + bytes / 4 + 256;
    OwnedBuffer fresh;
    if (!fresh.createHostWritable(allocator_, device_, cap, usage, name)) {
        mc2::fmt::print_err("[VK_SHADOW_STATIC] buffer '{}' ({} bytes) could not be created.\n", name, mc2::fmt::as_u64(cap));
        return false;
    }
    buffer = std::move(fresh);
    capacity = cap;
    return true;
}

// Each vertex shader is loaded just before the pipeline that names its vertex layout, so the offline layout checker pairs the terrain stride
// with shadow_terrain_static.vert and the building stride with shadow_prop.vert.
bool ShadowStaticMap::init(VkDevice device, VmaAllocator allocator, VkRenderPass pass, std::string_view spvDir) {
    device_ = device;
    allocator_ = allocator;
    pass_ = pass;
    const std::string dir(spvDir);
    frag_ = OwnedShaderModule(device_, vk_island_util::loadSpv(device_, dir + "shadow_prop.frag.spv", nullptr));
    if (!frag_) { slog("shadow_prop.frag SPIR-V missing: no static map."); return false; }

    VkDescriptorSetLayoutBinding b[2]{};
    b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayoutCreateInfo lci{};
    lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    lci.bindingCount = 2; lci.pBindings = b;
    if (!create(dsl_, device_, [&](auto* h) { return vkCreateDescriptorSetLayout(device_, &lci, nullptr, h); })) { slog("descriptor layout failed."); return false; }
    VkDescriptorPoolSize sizes[2]{};
    sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; sizes[0].descriptorCount = 1;
    sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; sizes[1].descriptorCount = 1;
    VkDescriptorPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pci.maxSets = 1; pci.poolSizeCount = 2; pci.pPoolSizes = sizes;
    if (!create(pool_, device_, [&](auto* h) { return vkCreateDescriptorPool(device_, &pci, nullptr, h); })) { slog("descriptor pool failed."); return false; }
    const VkDescriptorSetLayout dslHandle = dsl_.get();
    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = pool_.get(); dsai.descriptorSetCount = 1; dsai.pSetLayouts = &dslHandle;
    if (vkAllocateDescriptorSets(device_, &dsai, &set_) != VK_SUCCESS) { slog("descriptor set allocation failed."); return false; }
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1; plci.pSetLayouts = &dslHandle;
    if (!create(layout_, device_, [&](auto* h) { return vkCreatePipelineLayout(device_, &plci, nullptr, h); })) { slog("pipeline layout failed."); return false; }

    // Terrain: a packed world-space position per vertex.
    terrainVert_ = OwnedShaderModule(device_, vk_island_util::loadSpv(device_, dir + "shadow_terrain_static.vert.spv", nullptr));
    if (!terrainVert_) { slog("shadow_terrain_static.vert SPIR-V missing: no static map."); return false; }
    VkVertexInputBindingDescription vib{};
    vib.binding = 0; vib.stride = 12; vib.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription tvia[1]{};
    tvia[0].location = 0; tvia[0].binding = 0; tvia[0].format = VK_FORMAT_R32G32B32_SFLOAT; tvia[0].offset = 0;
    if (!createDepthCasterPipeline(terrainPipe_, device_, pass_, layout_.get(), terrainVert_.get(), frag_.get(), vib, tvia)) { slog("terrain pipeline failed."); return false; }
    nameObject(device_, terrainPipe_.get(), "shadow.static.terrainPipe");

    // Buildings: the shared static-prop vertex (position at 0; the UV at 24 rides along unread, as in the island's opaque caster).
    propVert_ = OwnedShaderModule(device_, vk_island_util::loadSpv(device_, dir + "shadow_prop.vert.spv", nullptr));
    if (!propVert_) { slog("shadow_prop.vert SPIR-V missing: no static map."); return false; }
    VkVertexInputBindingDescription pvib{};
    pvib.binding = 0; pvib.stride = mc2_static_prop_vertex_stride(); pvib.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription pvia[2]{};
    pvia[0].location = 0; pvia[0].binding = 0; pvia[0].format = VK_FORMAT_R32G32B32_SFLOAT; pvia[0].offset = 0;
    pvia[1].location = 2; pvia[1].binding = 0; pvia[1].format = VK_FORMAT_R32G32_SFLOAT;    pvia[1].offset = 24;
    if (!createDepthCasterPipeline(buildingPipe_, device_, pass_, layout_.get(), propVert_.get(), frag_.get(), pvib, pvia)) { slog("building pipeline failed."); return false; }
    nameObject(device_, buildingPipe_.get(), "shadow.static.buildingPipe");

    if (!ubo_.createHostWritable(allocator_, device_, sizeof(StaticFrameUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, "shadow.static.ubo")) { slog("UBO failed."); return false; }
    inited_ = true;
    return true;
}

bool ShadowStaticMap::stale() const {
    if (!inited_) return false;
    StaticShadowSpec spec;
    return gos_GetStaticShadowSpec(&spec) && spec.generation != builtGeneration_ && spec.generation != pendingGeneration_
        && spec.generation != failedGeneration_;
}

bool ShadowStaticMap::ensureTarget(int size, VkRenderPass pass) {
    if (image_ && size_ == size) return true;
    builtGeneration_ = 0;   // a new image holds no build until the one being recorded is submitted
    fb_.reset();   // the framebuffer and the view go before the image they cover
    view_.reset();
    image_.reset();
    size_ = 0;
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = kDepthFmt;
    ici.extent = {static_cast<uint32_t>(size), static_cast<uint32_t>(size), 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    if (!image_.create(allocator_, device_, ici, aci, "shadow.staticMap")) { slog("image creation failed."); return false; }
    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = image_.get();
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = kDepthFmt;
    vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    if (!create(view_, device_, [&](auto* h) { return vkCreateImageView(device_, &vci, nullptr, h); })) { slog("view creation failed."); return false; }
    VkFramebufferCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    const VkImageView attachment = view_.get();
    fci.renderPass = pass; fci.attachmentCount = 1; fci.pAttachments = &attachment;
    fci.width = static_cast<uint32_t>(size); fci.height = static_cast<uint32_t>(size); fci.layers = 1;
    if (!create(fb_, device_, [&](auto* h) { return vkCreateFramebuffer(device_, &fci, nullptr, h); })) { slog("framebuffer creation failed."); return false; }
    size_ = size;
    return true;
}

// The whole height field as GL's static build draws it: one quad per grid cell, split along the diagonal the scene uses for that cell's parity.
bool ShadowStaticMap::buildTerrainMesh() {
    const mc2terrain::TerrainFieldStore* store = gos_TerrainFieldStore();
    if (!store) { slog("no terrain field store: terrain casts nothing."); terrainIndexCount_ = 0; return true; }
    if (terrainIndexCount_ != 0 && store->heightVersion() == terrainHeightVersion_) return true;
    const int side = store->mapSide();
    const std::vector<float>& height = store->height();
    if (side < 2 || side > kMaxTerrainSide || height.size() != static_cast<size_t>(side) * static_cast<size_t>(side)) {
        mc2::fmt::print_err("[VK_SHADOW_STATIC] terrain field rejected: side {} with {} heights.\n", side, mc2::fmt::as_size(height.size()));
        terrainIndexCount_ = 0;
        return true;
    }
    const int half = side >> 1;
    std::vector<float> positions(static_cast<size_t>(side) * side * 3);
    for (int ay = 0; ay < side; ++ay) {
        for (int ax = 0; ax < side; ++ax) {
            const size_t v = static_cast<size_t>(ax) + static_cast<size_t>(ay) * side;
            positions[v * 3 + 0] = static_cast<float>(ax - half) * kWorldUnitsPerVertex;
            positions[v * 3 + 1] = static_cast<float>(half - ay) * kWorldUnitsPerVertex;
            positions[v * 3 + 2] = height[v];
        }
    }
    std::vector<uint32_t> indices;
    indices.reserve(static_cast<size_t>(side - 1) * (side - 1) * 6);
    for (int ay = 0; ay < side - 1; ++ay) {
        for (int ax = 0; ax < side - 1; ++ax) {
            const uint32_t v0 = static_cast<uint32_t>(ax + ay * side), v1 = v0 + 1, v3 = v0 + static_cast<uint32_t>(side), v2 = v3 + 1;
            if ((ax & 1) == (ay & 1)) indices.insert(indices.end(), {v0, v1, v2, v0, v2, v3});
            else                      indices.insert(indices.end(), {v0, v1, v3, v1, v2, v3});
        }
    }
    const VkDeviceSize vboBytes = positions.size() * sizeof(float), iboBytes = indices.size() * sizeof(uint32_t);
    if (!ensureBuffer(terrainVbo_, terrainVboCap_, vboBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, "shadow.static.terrainVbo")) return false;
    if (!ensureBuffer(terrainIbo_, terrainIboCap_, iboBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, "shadow.static.terrainIbo")) return false;
    if (!terrainVbo_.upload(positions.data(), vboBytes) || !terrainIbo_.upload(indices.data(), iboBytes)) { slog("terrain upload failed."); return false; }
    terrainIndexCount_ = static_cast<uint32_t>(indices.size());
    terrainHeightVersion_ = store->heightVersion();
    return true;
}

// Every registry building, sorted by type so each type is one instanced draw per packet.
bool ShadowStaticMap::stageBuildings(const StaticPropGeometryView& geometry) {
    buildingRuns_.clear();
    std::vector<GpuStaticPropInstance> instances;
    GpuStaticPropRegistry::getBuildingShadowInstances(instances);
    if (instances.empty()) return true;
    std::vector<uint32_t> order(instances.size());
    for (uint32_t k = 0; k < order.size(); ++k) order[k] = k;
    std::sort(order.begin(), order.end(), [&instances](uint32_t a, uint32_t b) { return instances[a].typeID < instances[b].typeID; });
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(instances.size()) * sizeof(GpuStaticPropInstance);
    if (!ensureBuffer(instBuf_, instCap_, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "shadow.static.instances")) return false;
    // memcpy per record: a mapped allocation may be less aligned than the instance's alignas(16).
    std::vector<uint8_t> packed(static_cast<size_t>(bytes));
    for (size_t k = 0; k < order.size(); ++k) std::memcpy(packed.data() + k * sizeof(GpuStaticPropInstance), &instances[order[k]], sizeof(GpuStaticPropInstance));
    if (!instBuf_.upload(packed.data(), bytes)) { slog("building instance upload failed."); return false; }
    std::vector<uint32_t> sortedTypes(order.size());
    for (size_t k = 0; k < order.size(); ++k) sortedTypes[k] = instances[order[k]].typeID;
    uint32_t dropped = 0;
    buildingRuns_ = planShadowBuildingRuns(sortedTypes, std::min(geometry.typeFirstPacket.size(), geometry.typePacketCount.size()), dropped);
    if (dropped) mc2::fmt::print_err("[VK_SHADOW_STATIC] {} building instances name a type the geometry does not have; dropped.\n", dropped);
    return true;
}

bool ShadowStaticMap::record(VkCommandBuffer cb, const StaticPropGeometryView& geometry) {
    if (!inited_) return false;
    StaticShadowSpec spec;
    if (!gos_GetStaticShadowSpec(&spec)) return false;
    // A generation that fails is not retried: the retry would idle the device every frame. The next generation (map load, rebake) tries again.
    failedGeneration_ = spec.generation;
    if (spec.size <= 0 || spec.size > kMaxMapSize) { mc2::fmt::print_err("[VK_SHADOW_STATIC] map size {} rejected.\n", spec.size); return false; }
    // A rebuild rewrites buffers and descriptors an earlier build's submit may still read; rebuilds happen once per map or per destroy.
    getOrCreateEngineVulkanContext().waitIdle("shadow_static_rebuild");
    if (!ensureTarget(spec.size, pass_)) return false;
    if (!buildTerrainMesh()) return false;
    if (!instBuf_ && !ensureBuffer(instBuf_, instCap_, sizeof(GpuStaticPropInstance), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "shadow.static.instances")) return false;
    if (!stageBuildings(geometry)) return false;
    {   // the terrain draw binds the same set, so both bindings are written even with no building
        const VkDescriptorBufferInfo uboInfo{ubo_.get(), 0, sizeof(StaticFrameUBO)};
        const VkDescriptorBufferInfo instInfo{instBuf_.get(), 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w[2]{};
        for (uint32_t i = 0; i < 2; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = set_; w[i].dstBinding = i; w[i].descriptorCount = 1;
            w[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        }
        w[0].pBufferInfo = &uboInfo;
        w[1].pBufferInfo = &instInfo;
        vkUpdateDescriptorSets(device_, 2, w, 0, nullptr);
    }
    StaticFrameUBO u{};
    std::memcpy(u.lightSpaceMatrix, spec.lightSpace, sizeof(u.lightSpaceMatrix));
    if (!ubo_.upload(&u, sizeof(u))) { slog("UBO upload failed."); return false; }

    VkClearValue clear{};
    clear.depthStencil = {1.0f, 0};   // forward-Z, as GL clears its static map
    VkRenderPassBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = pass_; rbi.framebuffer = fb_.get();
    rbi.renderArea = {{0, 0}, {static_cast<uint32_t>(size_), static_cast<uint32_t>(size_)}};
    rbi.clearValueCount = 1; rbi.pClearValues = &clear;
    MC2_VK_GPU_SCOPE(gpuScope, cb, "VKIsl.Shadow.Static");
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport vp{0.0f, 0.0f, static_cast<float>(size_), static_cast<float>(size_), 0.0f, 1.0f};   // not flipped, as the island's passes
    const VkRect2D scissor{{0, 0}, {static_cast<uint32_t>(size_), static_cast<uint32_t>(size_)}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &scissor);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_.get(), 0, 1, &set_, 0, nullptr);
    const VkDeviceSize zero = 0;

    // GL draws the terrain with no polygon offset and the buildings with its shadow bias (glPolygonOffset(factor, units) = depthBias(units, slope factor)).
    uint32_t terrainTris = 0, buildingDraws = 0, buildingInstances = 0;
    if (terrainIndexCount_ > 0) {
        vkCmdSetDepthBias(cb, 0.0f, 0.0f, 0.0f);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, terrainPipe_.get());
        const VkBuffer vbo = terrainVbo_.get();
        vkCmdBindVertexBuffers(cb, 0, 1, &vbo, &zero);
        vkCmdBindIndexBuffer(cb, terrainIbo_.get(), 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cb, terrainIndexCount_, 1, 0, 0, 0);
        terrainTris = terrainIndexCount_ / 3;
    }
    if (!buildingRuns_.empty() && geometry.vbo != VK_NULL_HANDLE && geometry.ibo != VK_NULL_HANDLE) {
        vkCmdSetDepthBias(cb, spec.biasUnits, 0.0f, spec.biasFactor);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, buildingPipe_.get());
        vkCmdBindVertexBuffers(cb, 0, 1, &geometry.vbo, &zero);
        vkCmdBindIndexBuffer(cb, geometry.ibo, 0, VK_INDEX_TYPE_UINT32);
        for (const ShadowBuildingRun& run : buildingRuns_) {
            const uint32_t first = geometry.typeFirstPacket[run.typeID], count = geometry.typePacketCount[run.typeID];
            for (uint32_t k = 0; k < count; ++k) {
                const uint32_t pk = first + k;
                if (pk >= geometry.packets.size()) break;
                const Mc2ShadowPacket& pkt = geometry.packets[pk];
                vkCmdDrawIndexed(cb, pkt.indexCount, run.count, pkt.firstIndex, static_cast<int32_t>(pkt.baseVertex), run.firstInstance);
                ++buildingDraws;
            }
            buildingInstances += run.count;
        }
    }
    vkCmdEndRenderPass(cb);
    gpuScope.end();
    pendingGeneration_ = spec.generation;
    failedGeneration_ = 0;
    mc2::fmt::print_err("[VK_SHADOW_STATIC] build recorded: generation={} size={} terrain_tris={} building_instances={} building_draws={}\n",
                        mc2::fmt::as_u64(spec.generation), spec.size, terrainTris, buildingInstances, buildingDraws);
    return true;
}

}  // namespace mc2vk

#endif  // MC2_VULKAN_ISLAND
