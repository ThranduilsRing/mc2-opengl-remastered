#include "vulkan_motion_vectors.h"
#ifdef MC2_VULKAN_ISLAND
#include "vulkan_context.h"
#include "vk_image_resource.h"
#include "vk_island_util.h"
#include "vk_render_infra.h"
#include "vk_gpu_profile.h"
#include "../../RenderCore/FramePackets.h"
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <cstring>
#include <string>
#include <string_view>

namespace mc2vk {
namespace {
bool makeFullscreenPipeline(VkDevice dev, VkPipelineLayout layout, VkRenderPass rp, std::string_view frag,
                            VkColorComponentFlags writeMask, OwnedPipeline& out) {
    OwnedShaderModule vs(dev, vk_island_util::loadSpv(dev, vk_island_util::spvDir() + "postprocess.vert.spv", nullptr));
    OwnedShaderModule fs(dev, vk_island_util::loadSpv(dev, vk_island_util::spvDir() + std::string(frag), nullptr));
    if (!vs || !fs) return false;
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (auto& stage : stages) { stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; stage.pName = "main"; }
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vs.get();
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs.get();
    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba{}; ba.colorWriteMask = writeMask;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; blend.attachmentCount = 1; blend.pAttachments = &ba;
    VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dyn.dynamicStateCount = 2; dyn.pDynamicStates = states;
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2; gp.pStages = stages; gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp; gp.pRasterizationState = &rs; gp.pMultisampleState = &ms;
    gp.pColorBlendState = &blend; gp.pDynamicState = &dyn; gp.layout = layout; gp.renderPass = rp;
    return mc2vk::create(out, dev, [&](auto* h) { return infra::graphics(dev, VK_NULL_HANDLE, 1, &gp, nullptr, h); });
}
}

struct MotionVectors::Impl {
    VulkanContext* ctx = nullptr;
    VkImageResource output{};
    OwnedImageView depthView;
    OwnedSampler sampler;
    OwnedDescriptorSetLayout dsl;
    OwnedDescriptorPool pool;
    OwnedPipelineLayout layout;
    OwnedPipeline pipeline;
    OwnedRenderPass renderPass;
    OwnedFramebuffer framebuffer;
    VkDescriptorSet set = VK_NULL_HANDLE;       // binding 0 = scene depth
    VkDescriptorSet debugSet = VK_NULL_HANDLE;  // binding 0 = motion output
    VkImage sourceDepth = VK_NULL_HANDLE;
    // Debug inset over the shared present image (built on first use).
    OwnedRenderPass debugPass;
    OwnedPipeline debugPipeline;
    OwnedFramebuffer debugFramebuffer;
    VkImageView debugTarget = VK_NULL_HANDLE;
    bool recorded = false, debugFailed = false;

    ~Impl() {
        debugFramebuffer.reset();
        framebuffer.reset();
        if (ctx) vkImageResourceDestroy(&output, ctx->device());
    }
    bool init(VulkanContext& context, const VkImageResource& depth) {
        ctx = &context;
        const auto dev = ctx->device();
        if (!vkImageResourceCreate(&output, dev, ctx->physicalDevice(), depth.w, depth.h,
                VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT, "scene.motionVectors")) return false;
        sourceDepth = depth.image.get();
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = depth.image.get(); vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = depth.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        if (!mc2vk::create(depthView, dev, [&](auto* h) { return vkCreateImageView(dev, &vi, nullptr, h); })) return false;
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.minFilter = si.magFilter = VK_FILTER_NEAREST; si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (!mc2vk::create(sampler, dev, [&](auto* h) { return vkCreateSampler(dev, &si, nullptr, h); })) return false;
        VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo di{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        di.bindingCount = 1; di.pBindings = &binding;
        if (!mc2vk::create(dsl, dev, [&](auto* h) { return vkCreateDescriptorSetLayout(dev, &di, nullptr, h); })) return false;
        VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2}};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 2; pi.poolSizeCount = 1; pi.pPoolSizes = sizes;
        if (!mc2vk::create(pool, dev, [&](auto* h) { return vkCreateDescriptorPool(dev, &pi, nullptr, h); })) return false;
        const VkDescriptorSetLayout layouts[2] = {dsl.get(), dsl.get()};
        VkDescriptorSet sets[2]{};
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool.get(); ai.descriptorSetCount = 2; ai.pSetLayouts = layouts;
        if (vkAllocateDescriptorSets(dev, &ai, sets) != VK_SUCCESS) return false;
        set = sets[0]; debugSet = sets[1];
        VkDescriptorImageInfo images[] = {
            {sampler.get(), depthView.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {sampler.get(), output.view.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet writes[2]{};
        for (uint32_t i = 0; i < 2; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet = sets[i];
            writes[i].dstBinding = 0; writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[i].pImageInfo = &images[i];
        }
        vkUpdateDescriptorSets(dev, 2, writes, 0, nullptr);
        VkPushConstantRange pc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64};
        const auto sl = dsl.get();
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.setLayoutCount = 1; li.pSetLayouts = &sl; li.pushConstantRangeCount = 1; li.pPushConstantRanges = &pc;
        if (!mc2vk::create(layout, dev, [&](auto* h) { return vkCreatePipelineLayout(dev, &li, nullptr, h); })) return false;
        VkAttachmentDescription ad{};
        ad.format = output.format; ad.samples = VK_SAMPLE_COUNT_1_BIT;
        ad.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; ad.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ad.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; ad.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        ad.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; ad.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{}; sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1; sub.pColorAttachments = &ref;
        // In: the previous frame's readers of the output (debug inset, later the resolve) are done.
        // Out: fragment-shader reads later in this submit.
        VkSubpassDependency deps[] = {
            {VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0},
            {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, 0}};
        VkRenderPassCreateInfo ri{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        ri.attachmentCount = 1; ri.pAttachments = &ad; ri.subpassCount = 1; ri.pSubpasses = &sub;
        ri.dependencyCount = 2; ri.pDependencies = deps;
        if (!mc2vk::create(renderPass, dev, [&](auto* h) { return vkCreateRenderPass(dev, &ri, nullptr, h); })) return false;
        const VkImageView target = output.view.get();
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = renderPass.get(); fi.attachmentCount = 1; fi.pAttachments = &target;
        fi.width = output.w; fi.height = output.h; fi.layers = 1;
        if (!mc2vk::create(framebuffer, dev, [&](auto* h) { return vkCreateFramebuffer(dev, &fi, nullptr, h); })) return false;
        return makeFullscreenPipeline(dev, layout.get(), renderPass.get(), "motion_vectors.frag.spv",
                                      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT, pipeline);
    }
    bool initDebug(VkImageView target, uint32_t w, uint32_t h) {
        const auto dev = ctx->device();
        if (!debugPass) {
            VkAttachmentDescription ad{};
            ad.format = VK_FORMAT_R8G8B8A8_UNORM; ad.samples = VK_SAMPLE_COUNT_1_BIT;
            ad.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ad.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            ad.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; ad.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            ad.initialLayout = ad.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
            VkSubpassDescription sub{}; sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            sub.colorAttachmentCount = 1; sub.pColorAttachments = &ref;
            // LOAD pass with no layout transition: chain against the previous writer of the
            // present image and its later readers explicitly.
            const VkPipelineStageFlags around = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
            const VkAccessFlags aroundAccess = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT |
                VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            VkSubpassDependency deps[] = {
                {VK_SUBPASS_EXTERNAL, 0, around, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, aroundAccess,
                 VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0},
                {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, around,
                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, aroundAccess, 0}};
            VkRenderPassCreateInfo ri{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
            ri.attachmentCount = 1; ri.pAttachments = &ad; ri.subpassCount = 1; ri.pSubpasses = &sub;
            ri.dependencyCount = 2; ri.pDependencies = deps;
            if (!mc2vk::create(debugPass, dev, [&](auto* hh) { return vkCreateRenderPass(dev, &ri, nullptr, hh); })) return false;
            if (!makeFullscreenPipeline(dev, layout.get(), debugPass.get(), "motion_vectors_debug.frag.spv",
                    VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
                    debugPipeline)) return false;
        }
        debugFramebuffer.reset();
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = debugPass.get(); fi.attachmentCount = 1; fi.pAttachments = &target;
        fi.width = w; fi.height = h; fi.layers = 1;
        if (!mc2vk::create(debugFramebuffer, dev, [&](auto* hh) { return vkCreateFramebuffer(dev, &fi, nullptr, hh); })) return false;
        debugTarget = target;
        return true;
    }
};

MotionVectors::MotionVectors() = default;
MotionVectors::~MotionVectors() = default;
void MotionVectors::reset() { impl_.reset(); }
VkImageView MotionVectors::view() const { return impl_ ? impl_->output.view.get() : VK_NULL_HANDLE; }
bool MotionVectors::prepare(VulkanContext& ctx, const VkImageResource& depth) {
    if (impl_ && impl_->sourceDepth == depth.image.get() && impl_->output.w == depth.w && impl_->output.h == depth.h) return true;
    reset();
    auto state = std::make_unique<Impl>();
    if (!state->init(ctx, depth)) return false;
    impl_ = std::move(state);
    return true;
}
void MotionVectors::record(VkCommandBuffer cb, const VkImageResource& depth) {
    if (!impl_) return;
    auto& s = *impl_;
    const auto main = RenderCore::framePackets().copyMainView();
    // ViewUniforms bytes read as the column-major mat4 the native shaders use. The producer
    // (GameCamera::renderViewUniforms) publishes previous == current on a history reset.
    const glm::mat4 current = glm::make_mat4(main.uniforms.worldToClipGL);
    const glm::mat4 previous = glm::make_mat4(main.uniforms.previousWorldToClipGL);
    // Exact identity avoids inversion roundoff on still cameras and on reset; an all-zero
    // previous (producers without history) gives w == 0 and therefore zero motion.
    glm::mat4 reprojection{1.0f};
    if (std::memcmp(glm::value_ptr(previous), glm::value_ptr(current), sizeof(glm::mat4)) != 0 &&
        glm::determinant(current) != 0.0f)
        reprojection = previous * glm::inverse(current);
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = depth.image.get();
    b.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
    b.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    {
        MC2_VK_GPU_SCOPE(timer, cb, "VKIsl.Post.MotionVectors");
        VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rp.renderPass = s.renderPass.get(); rp.framebuffer = s.framebuffer.get(); rp.renderArea.extent = {s.output.w, s.output.h};
        vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport vp{0, 0, static_cast<float>(s.output.w), static_cast<float>(s.output.h), 0, 1}; VkRect2D sc{{0, 0}, {s.output.w, s.output.h}};
        vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &sc);
        infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s.pipeline.get());
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s.layout.get(), 0, 1, &s.set, 0, nullptr);
        vkCmdPushConstants(cb, s.layout.get(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64, glm::value_ptr(reprojection));
        vkCmdDraw(cb, 3, 1, 0, 0); vkCmdEndRenderPass(cb);
    }
    b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    b.srcAccessMask = 0;   // read-after-read: the transition only has to wait for the shader read
    b.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);
    s.recorded = true;
}
bool MotionVectors::recordDebug(VkCommandBuffer cb, VkImageView target, uint32_t w, uint32_t h) {
    if (!impl_ || !impl_->recorded || impl_->debugFailed || target == VK_NULL_HANDLE || w < 64 || h < 64) return false;
    auto& s = *impl_;
    s.recorded = false;
    if (s.debugTarget != target || !s.debugFramebuffer) {
        if (!s.initDebug(target, w, h)) { s.debugFailed = true; return false; }
    }
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = s.debugPass.get(); rp.framebuffer = s.debugFramebuffer.get(); rp.renderArea.extent = {w, h};
    MC2_VK_GPU_SCOPE(timer, cb, "VKIsl.Present.MotionVectorsDebug");
    vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
    // Top-right inset, one third of the present image.
    const uint32_t iw = w / 3, ih = h / 3, margin = 16;
    const uint32_t x0 = w - iw - margin;
    VkViewport vp{static_cast<float>(x0), static_cast<float>(margin), static_cast<float>(iw), static_cast<float>(ih), 0, 1};
    VkRect2D sc{{static_cast<int32_t>(x0), static_cast<int32_t>(margin)}, {iw, ih}};
    vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &sc);
    infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s.debugPipeline.get());
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s.layout.get(), 0, 1, &s.debugSet, 0, nullptr);
    const float pc[4] = {static_cast<float>(s.output.w), static_cast<float>(s.output.h), 8.0f, 0.0f};   // render size, full-scale pixels
    vkCmdPushConstants(cb, s.layout.get(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb);
    return true;
}
}
#endif
