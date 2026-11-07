// Spatial post-AA on display-encoded scene color, before either HUD path.
#include "vulkan_spatial_aa.h"
#include "vulkan_context.h"
#include "vk_image_resource.h"
#include "vk_island_util.h"
#include "vk_gpu_profile.h"
#include "vk_render_infra.h"
#include "vk_resource_graph.h"
#include "../../3rdparty/smaa/AreaTex.h"
#include "../../3rdparty/smaa/SearchTex.h"
#include <vk_mem_alloc.h>
#include <array>
#include "mc2fmt.h"
#include <cstring>

namespace mc2vk {
namespace {
void imageBarrier(VkCommandBuffer cb, VkImage image, VkImageLayout before, VkImageLayout after,
                  VkAccessFlags src, VkAccessFlags dst,
                  VkPipelineStageFlags from, VkPipelineStageFlags to) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src; b.dstAccessMask = dst;
    b.oldLayout = before; b.newLayout = after;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, from, to, 0, 0, nullptr, 0, nullptr, 1, &b);
}
void memoryBarrier(VkCommandBuffer cb, VkAccessFlags src, VkAccessFlags dst,
                   VkPipelineStageFlags from, VkPipelineStageFlags to) {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; b.srcAccessMask = src; b.dstAccessMask = dst;
    vkCmdPipelineBarrier(cb, from, to, 0, 1, &b, 0, nullptr, 0, nullptr);
}
struct Texture {
    OwnedImage image;
    OwnedImageView view;
    bool create(VulkanContext& ctx, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage, bool mutableFormat = false) {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.flags = mutableFormat ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
        ci.imageType = VK_IMAGE_TYPE_2D; ci.format = fmt; ci.extent = {w, h, 1};
        ci.mipLevels = ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = usage;
        VmaAllocationCreateInfo ai{}; ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (!image.create(ctx.allocator(), ctx.device(), ci, ai, "spatialAA.image")) return false;
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = image.get(); vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        return mc2vk::create(view, ctx.device(), [&](auto* hnd) { return vkCreateImageView(ctx.device(), &vi, nullptr, hnd); });
    }
};
}

struct SpatialAA::Impl {
    VulkanContext* ctx = nullptr;
    VkDevice dev = VK_NULL_HANDLE;
    uint32_t w = 0, h = 0;
    int mode = 0;
    bool uploaded = false;
    bool lookupPending = false;
    Texture input, area, search, cmaaEdges, heads;
    std::vector<Texture> transientSlots;
    OwnedImageView inputSrgb;
    OwnedBuffer lookupUpload;
    std::array<OwnedBuffer, 5> work; // candidates, locations, items, control, indirect
    OwnedSampler sampler;
    OwnedRenderPass renderPass;
    OwnedDescriptorSetLayout dsl;
    OwnedDescriptorPool pool;
    OwnedPipelineLayout layout;
    std::array<OwnedFramebuffer, 3> framebuffers;
    std::array<OwnedPipeline, 3> graphics;
    std::array<VkDescriptorSet, 3> sets{};
    std::array<OwnedPipeline, 4> compute;
    OwnedDescriptorSetLayout computeDsl;
    OwnedPipelineLayout computeLayout;
    VkDescriptorSet computeSet = VK_NULL_HANDLE;
    graph::Plan graphPlan;
    std::vector<graph::Binding> graphBindings;

    Texture& graphTexture(unsigned id) { return transientSlots[graphPlan.lifetimes[id].slot]; }
    Texture& inputTexture() { return mode == 1 ? graphTexture(1) : input; }

    bool initGraph(const VkImageResource& output) {
        using namespace graph;
        const State color{VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, true};
        const State sampledState{VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 VK_ACCESS_SHADER_READ_BIT, false};
        const State copyRead{VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, false};
        const State copyWrite{VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, true};
        const auto key = [&](uint32_t usage, uint32_t flags) {
            return std::array<uint32_t, 6>{w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, usage, flags};
        };
        std::vector<Image> images{
            {{}, true, color},
            {key(VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT)},
            {key(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0)},
            {key(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0)},
            {{}, true, sampledState}, {{}, true, sampledState}};
        graphPlan = compile(images, {
            {{{0, copyRead}, {1, copyWrite}}},
            {{{1, sampledState}, {2, color}}},
            {{{2, sampledState}, {4, sampledState}, {5, sampledState}, {3, color}}},
            {{{1, sampledState}, {3, sampledState}, {0, color}}}});
        if (!graphPlan.valid) return false;
        transientSlots.resize(graphPlan.slots);
        for (unsigned i = 1; i <= 3; ++i) {
            auto& texture = graphTexture(i);
            const auto& k = images[i].key;
            if (!texture.image && !texture.create(*ctx, w, h, VkFormat(k[2]), k[4], k[5] != 0)) return false;
        }
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        graphBindings = {{output.image.get(), range}, {graphTexture(1).image.get(), range},
                         {graphTexture(2).image.get(), range}, {graphTexture(3).image.get(), range},
                         {VK_NULL_HANDLE, range}, {VK_NULL_HANDLE, range}};
        mc2::fmt::print_err("[VK_RESOURCE_GRAPH] chain=SMAA passes=4 transient_images=3 slots={} alias_savings={}\n",
                            graphPlan.slots, 3u - graphPlan.slots);
        return true;
    }

    bool allocateSet(VkDescriptorSetLayout sl, VkDescriptorSet& set) {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool.get(); ai.descriptorSetCount = 1; ai.pSetLayouts = &sl;
        return MC2_VK_ALLOC_SETS(dev, &ai, &set) == VK_SUCCESS;
    }
    void sampled(VkDescriptorSet set, uint32_t binding, VkImageView view, bool separate = false) {
        VkDescriptorImageInfo ii{sampler.get(), view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        wr.dstSet = set; wr.dstBinding = binding; wr.descriptorCount = 1;
        wr.descriptorType = separate ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr.pImageInfo = &ii; vkUpdateDescriptorSets(dev, 1, &wr, 0, nullptr);
    }
    bool init(VulkanContext& context, const VkImageResource& output, int selected) {
        ctx = &context; dev = ctx->device(); w = output.w; h = output.h; mode = selected;
        if (mode == 1 && !initGraph(output)) return false;
        if (mode != 1 && !input.create(*ctx, w, h, mode == 3 ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM,
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, mode == 1)) return false;
        if (mode == 1) {
            // SMAA edges read encoded color; its neighborhood filter reads the same bytes in linear light.
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = inputTexture().image.get(); vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = VK_FORMAT_R8G8B8A8_SRGB;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            if (!mc2vk::create(inputSrgb, dev, [&](auto* hnd) { return vkCreateImageView(dev, &vi, nullptr, hnd); })) return false;
        }
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.minFilter = si.magFilter = VK_FILTER_LINEAR; si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (!mc2vk::create(sampler, dev, [&](auto* hnd) { return vkCreateSampler(dev, &si, nullptr, hnd); })) return false;
        VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 9}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3},
                                      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1}};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 4; pi.poolSizeCount = 4; pi.pPoolSizes = sizes;
        if (!mc2vk::create(pool, dev, [&](auto* hnd) { return vkCreateDescriptorPool(dev, &pi, nullptr, hnd); })) return false;
        if (mode == 3) return initCompute(output);
        return initGraphics(output);
    }
    bool initGraphics(const VkImageResource& output) {
        VkAttachmentDescription ad{};
        ad.format = VK_FORMAT_R8G8B8A8_UNORM; ad.samples = VK_SAMPLE_COUNT_1_BIT;
        ad.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; ad.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ad.initialLayout = mode == 1 ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
        ad.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{}; sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1; sub.pColorAttachments = &ref;
        VkRenderPassCreateInfo ri{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        ri.attachmentCount = 1; ri.pAttachments = &ad; ri.subpassCount = 1; ri.pSubpasses = &sub;
        VkSubpassDependency dep{VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0};
        ri.dependencyCount = 1; ri.pDependencies = &dep;
        if (!mc2vk::create(renderPass, dev, [&](auto* hnd) { return vkCreateRenderPass(dev, &ri, nullptr, hnd); })) return false;
        VkDescriptorSetLayoutBinding bindings[3]{};
        for (uint32_t i = 0; i < 3; ++i) bindings[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo di{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; di.bindingCount = 3; di.pBindings = bindings;
        if (!mc2vk::create(dsl, dev, [&](auto* hnd) { return vkCreateDescriptorSetLayout(dev, &di, nullptr, hnd); })) return false;
        VkPushConstantRange pc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
        VkDescriptorSetLayout sl = dsl.get();
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.setLayoutCount = 1; li.pSetLayouts = &sl; li.pushConstantRangeCount = 1; li.pPushConstantRanges = &pc;
        if (!mc2vk::create(layout, dev, [&](auto* hnd) { return vkCreatePipelineLayout(dev, &li, nullptr, hnd); })) return false;
        const bool smaa = mode == 1;
        if (smaa) {
            if (!area.create(*ctx, AREATEX_WIDTH, AREATEX_HEIGHT, VK_FORMAT_R8G8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT) ||
                !search.create(*ctx, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) return false;
            std::array<unsigned char, AREATEX_SIZE + SEARCHTEX_SIZE> bytes{};
            std::memcpy(bytes.data(), areaTexBytes, AREATEX_SIZE); std::memcpy(bytes.data() + AREATEX_SIZE, searchTexBytes, SEARCHTEX_SIZE);
            if (!lookupUpload.createFilled(ctx->allocator(), dev, bytes.data(), bytes.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, "spatialAA.lookups")) return false;
        }
        const char* names[] = {smaa ? "aa_smaa_edges.frag.spv" : "aa_fxaa.frag.spv", "aa_smaa_weights.frag.spv", "aa_smaa_blend.frag.spv"};
        VkImageView targets[] = {smaa ? graphTexture(2).view.get() : output.view.get(),
                                smaa ? graphTexture(3).view.get() : VK_NULL_HANDLE, output.view.get()};
        // loadSpv registers every AA module with infra::shader for reload and breadcrumbs.
        OwnedShaderModule vs(dev, vk_island_util::loadSpv(dev, vk_island_util::spvDir() + "postprocess.vert.spv", nullptr));
        if (!vs) return false;
        for (uint32_t i = 0; i < (smaa ? 3u : 1u); ++i) {
            VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fi.renderPass = renderPass.get(); fi.attachmentCount = 1; fi.pAttachments = &targets[i]; fi.width = w; fi.height = h; fi.layers = 1;
            if (!mc2vk::create(framebuffers[i], dev, [&](auto* hnd) { return vkCreateFramebuffer(dev, &fi, nullptr, hnd); })) return false;
            if (!allocateSet(dsl.get(), sets[i])) return false;
            // All bindings are initialized, even when a particular shader omits one.
            for (uint32_t j = 0; j < 3; ++j) sampled(sets[i], j, inputTexture().view.get());
            OwnedShaderModule fs(dev, vk_island_util::loadSpv(dev, vk_island_util::spvDir() + names[i], nullptr));
            if (!fs) return false;
            graphics[i] = makeGraphics(vs.get(), fs.get());
            if (!graphics[i]) return false;
        }
        if (smaa) {
            sampled(sets[1], 0, graphTexture(2).view.get()); sampled(sets[1], 1, area.view.get()); sampled(sets[1], 2, search.view.get());
            sampled(sets[2], 0, inputSrgb.get());
            sampled(sets[2], 1, graphTexture(3).view.get());
            graphBindings[4].image = area.image.get(); graphBindings[5].image = search.image.get();
        }
        return true;
    }
    OwnedPipeline makeGraphics(VkShaderModule vs, VkShaderModule fs) {
        VkPipelineShaderStageCreateInfo stages[2]{};
        for (auto & stage : stages) { stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; stage.pName = "main"; }
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vs;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs;
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vp.viewportCount = vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState ba{}; ba.colorWriteMask = 15;
        VkPipelineColorBlendStateCreateInfo bs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; bs.attachmentCount = 1; bs.pAttachments = &ba;
        VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; ds.dynamicStateCount = 2; ds.pDynamicStates = states;
        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        ci.stageCount = 2; ci.pStages = stages; ci.pVertexInputState = &vi; ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vp; ci.pRasterizationState = &rs; ci.pMultisampleState = &ms; ci.pColorBlendState = &bs; ci.pDynamicState = &ds;
        ci.layout = layout.get(); ci.renderPass = renderPass.get();
        OwnedPipeline pipe;
        mc2vk::create(pipe, dev, [&](auto* hnd) { return infra::graphics(dev, VK_NULL_HANDLE, 1, &ci, nullptr, hnd); });
        return pipe;
    }
    bool initCompute(const VkImageResource& output) {
        if (w > 16384 || h > 16384) return false; // reference candidate coordinate packing
        if (!cmaaEdges.create(*ctx, (w + 1) / 2, h, VK_FORMAT_R32_UINT, VK_IMAGE_USAGE_STORAGE_BIT) ||
            !heads.create(*ctx, (w + 1) / 2, (h + 1) / 2, VK_FORMAT_R32_UINT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return false;
        const VkDeviceSize pixels = static_cast<VkDeviceSize>(w) * h;
        const VkDeviceSize bytes[] = {pixels * 4, static_cast<VkDeviceSize>((w + 1) / 2) * ((h + 1) / 2) * 4, pixels * 16, 64, 12};
        VkPhysicalDeviceProperties props{}; vkGetPhysicalDeviceProperties(ctx->physicalDevice(), &props);
        if ((pixels + 127) / 128 > props.limits.maxComputeWorkGroupCount[0] ||
            (bytes[1] / 4 + 31) / 32 > props.limits.maxComputeWorkGroupCount[1]) return false;
        for (int i = 0; i < 5; ++i) {
            if (bytes[i] > props.limits.maxStorageBufferRange) return false;
            VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = bytes[i];
            bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | (i == 4 ? VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT : 0);
            VmaAllocationCreateInfo ai{}; ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            if (!work[i].create(ctx->allocator(), dev, bi, ai, "spatialAA.cmaaWork", i)) return false;
        }
        VkDescriptorSetLayoutBinding bindings[9]{};
        for (uint32_t i = 0; i < 9; ++i) {
            auto type = (i == 0 || i == 1 || i == 5) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            if (i == 8) type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            bindings[i] = {i, type, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        }
        VkDescriptorSetLayoutCreateInfo di{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; di.bindingCount = 9; di.pBindings = bindings;
        if (!mc2vk::create(computeDsl, dev, [&](auto* hnd) { return vkCreateDescriptorSetLayout(dev, &di, nullptr, hnd); }) || !allocateSet(computeDsl.get(), computeSet)) return false;
        VkDescriptorSetLayout sl = computeDsl.get();
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; li.setLayoutCount = 1; li.pSetLayouts = &sl;
        if (!mc2vk::create(computeLayout, dev, [&](auto* hnd) { return vkCreatePipelineLayout(dev, &li, nullptr, hnd); })) return false;
        VkImageView views[] = {output.view.get(), cmaaEdges.view.get(), heads.view.get()};
        const uint32_t imageBindings[] = {0, 1, 5};
        for (int i = 0; i < 3; ++i) {
            VkDescriptorImageInfo ii{VK_NULL_HANDLE, views[i], VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; wr.dstSet = computeSet; wr.dstBinding = imageBindings[i];
            wr.descriptorCount = 1; wr.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; wr.pImageInfo = &ii; vkUpdateDescriptorSets(dev, 1, &wr, 0, nullptr);
        }
        const uint32_t bufferBindings[] = {2, 3, 4, 6, 7};
        for (int i = 0; i < 5; ++i) {
            VkDescriptorBufferInfo bi{work[i].get(), 0, bytes[i]};
            VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; wr.dstSet = computeSet; wr.dstBinding = bufferBindings[i];
            wr.descriptorCount = 1; wr.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wr.pBufferInfo = &bi; vkUpdateDescriptorSets(dev, 1, &wr, 0, nullptr);
        }
        sampled(computeSet, 8, input.view.get(), true);
        const char* entries[] = {"EdgesColor2x2CS", "ComputeDispatchArgsCS", "ProcessCandidatesCS", "DeferredColorApply2x2CS"};
        for (int i = 0; i < 4; ++i) {
            OwnedShaderModule mod(dev, vk_island_util::loadSpv(dev, vk_island_util::spvDir() + "aa_cmaa2_" + entries[i] + ".comp.spv", nullptr));
            if (!mod) return false;
            VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            ci.layout = computeLayout.get(); ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; ci.stage.module = mod.get(); ci.stage.pName = entries[i];
            if (!mc2vk::create(compute[i], dev, [&](auto* hnd) { return infra::compute(dev, VK_NULL_HANDLE, 1, &ci, nullptr, hnd); })) return false;
        }
        return true;
    }
    void uploadLookup(VkCommandBuffer cb, Texture& tex, uint32_t width, uint32_t height, VkDeviceSize offset) {
        imageBarrier(cb, tex.image.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                     (VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT), VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy{}; copy.bufferOffset = offset; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {width, height, 1};
        vkCmdCopyBufferToImage(cb, lookupUpload.get(), tex.image.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        imageBarrier(cb, tex.image.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }
    void recordGraphics(VkCommandBuffer cb, uint32_t i = 0) {
            VkClearValue clear{};
            VkRenderPassBeginInfo ri{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; ri.renderPass = renderPass.get(); ri.framebuffer = framebuffers[i].get();
            ri.renderArea.extent = {w, h}; ri.clearValueCount = 1; ri.pClearValues = &clear;
            vkCmdBeginRenderPass(cb, &ri, VK_SUBPASS_CONTENTS_INLINE);
            VkViewport vp{0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1}; VkRect2D sc{{0, 0}, {w, h}};
            vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &sc);
            infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, graphics[i].get());
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout.get(), 0, 1, &sets[i], 0, nullptr);
            float metrics[] = {1.0f / static_cast<float>(w), 1.0f / static_cast<float>(h), static_cast<float>(w), static_cast<float>(h)};
            vkCmdPushConstants(cb, layout.get(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(metrics), metrics);
            vkCmdDraw(cb, 3, 1, 0, 0); vkCmdEndRenderPass(cb);
    }
    void recordGraph(VkCommandBuffer cb, const VkImageResource& output) {
        if (!uploaded) {
            uploadLookup(cb, area, AREATEX_WIDTH, AREATEX_HEIGHT, 0);
            uploadLookup(cb, search, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, AREATEX_SIZE);
            lookupPending = true;
        }
        for (unsigned p = 0; p < graphPlan.barriers.size(); ++p) {
            graph::recordBarriers(cb, graphPlan, p, graphBindings);
            if (p == 0) {
                VkImageCopy copy{};
                copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy.extent = {w, h, 1};
                vkCmdCopyImage(cb, output.image.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               inputTexture().image.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            } else recordGraphics(cb, p - 1);
        }
    }
    void recordCompute(VkCommandBuffer cb, const VkImageResource& output) {
        imageBarrier(cb, cmaaEdges.image.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
            0, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, (VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        imageBarrier(cb, heads.image.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT, (VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT), VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearColorValue emptyHeads{};
        for (auto& value : emptyHeads.uint32) value = UINT32_MAX;
        VkImageSubresourceRange headRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, heads.image.get(), VK_IMAGE_LAYOUT_GENERAL, &emptyHeads, 1, &headRange);
        imageBarrier(cb, output.image.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        vkCmdFillBuffer(cb, work[3].get(), 0, VK_WHOLE_SIZE, 0);
        memoryBarrier(cb, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, computeLayout.get(), 0, 1, &computeSet, 0, nullptr);
        auto dispatch = [&](int pipeline, uint32_t x, uint32_t y) {
            infra::bind(cb, VK_PIPELINE_BIND_POINT_COMPUTE, compute[pipeline].get()); vkCmdDispatch(cb, x, y, 1);
            memoryBarrier(cb, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT);
        };
        dispatch(0, (w + 27) / 28, (h + 27) / 28);
        dispatch(1, 2, 1);
        infra::bind(cb, VK_PIPELINE_BIND_POINT_COMPUTE, compute[2].get()); vkCmdDispatchIndirect(cb, work[4].get(), 0);
        memoryBarrier(cb, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT,
                      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        dispatch(1, 1, 2);
        infra::bind(cb, VK_PIPELINE_BIND_POINT_COMPUTE, compute[3].get()); vkCmdDispatchIndirect(cb, work[4].get(), 0);
        imageBarrier(cb, output.image.get(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    }
};

SpatialAA::SpatialAA() = default;
SpatialAA::~SpatialAA() = default;
void SpatialAA::reset() { impl_.reset(); failedMode_ = -1; }
void SpatialAA::submitted() {
    if (impl_ && impl_->lookupPending) impl_->uploaded = true;
}
bool SpatialAA::prepare(VulkanContext& ctx, const VkImageResource& output, int mode) {
    if (mode < 0 || mode > 3) mode = 1;
    if (failedMode_ != mode) failedMode_ = -1;
    if (failedMode_ == mode && failedW_ == output.w && failedH_ == output.h) return false;
    if (impl_ && impl_->w == output.w && impl_->h == output.h && impl_->mode == mode) return true;
    if (impl_) { ctx.waitIdle("spatial_aa_change"); reset(); }
    if (!mode) return true;
    auto next = std::make_unique<Impl>();
    if (!next->init(ctx, output, mode)) {
        failedMode_ = mode; failedW_ = output.w; failedH_ = output.h;
        mc2::fmt::print_err("[SPATIAL_AA] shader_error=1 mode={} initialization failed; scene retained\n", mode);
        return false;
    }
    mc2::fmt::print_err("[SPATIAL_AA] mode={} extent={}x{} spatial=1 before_hud=1\n", mode, output.w, output.h);
    impl_ = std::move(next);
    failedMode_ = -1;
    return true;
}
void SpatialAA::record(VkCommandBuffer cb, const VkImageResource& output) {
    if (!impl_) return;
    auto& s = *impl_;
    const char* names[] = {"VKIsl.AA.Off", "VKIsl.AA.SMAA1x", "VKIsl.AA.FXAA", "VKIsl.AA.CMAA2"};
    MC2_VK_GPU_SCOPE(scope, cb, names[s.mode]);
    // Scratch images are shared across frame slots on one queue; retire prior reads before overwriting them.
    // Explicit stage set (was ALL_COMMANDS->ALL_COMMANDS): the scene's color writes plus the previous frame's AA reads/writes of the scratch images.
    constexpr VkPipelineStageFlags kAaStages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
    memoryBarrier(cb, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, kAaStages, kAaStages);
    if (s.mode == 1) { s.recordGraph(cb, output); return; }
    imageBarrier(cb, output.image.get(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    imageBarrier(cb, s.input.image.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                 (VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT), VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageCopy copy{}; copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.extent = {s.w, s.h, 1};
    vkCmdCopyImage(cb, output.image.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.input.image.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    imageBarrier(cb, s.input.image.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 s.mode == 3 ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    if (s.mode == 3) s.recordCompute(cb, output);
    else {
        // The final AA render pass discards the prior attachment contents after the copy finishes.
        memoryBarrier(cb, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        s.recordGraphics(cb);
    }
}
}
