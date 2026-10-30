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

namespace mc2vk {
struct MotionVectors::Impl {
    VulkanContext* ctx = nullptr;
    VkImageResource output{};
    VkImageResource* correction = nullptr;
    OwnedImageView depthView;
    OwnedSampler sampler;
    OwnedDescriptorSetLayout dsl;
    OwnedDescriptorPool pool;
    OwnedPipelineLayout layout;
    OwnedPipeline pipeline;
    OwnedRenderPass renderPass;
    OwnedFramebuffer framebuffer;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkImage sourceDepth = VK_NULL_HANDLE, targetMotion = VK_NULL_HANDLE;
    glm::mat4 previous{1.0f}, pending{1.0f};
    uint64_t previousFrame = 0, pendingFrame = 0;
    bool havePrevious = false;

    ~Impl() {
        framebuffer.reset();
        if (ctx) vkImageResourceDestroy(&output, ctx->device());
    }
    bool init(VulkanContext& context, const VkImageResource& depth, VkImageResource& motion) {
        ctx = &context;
        const auto dev = ctx->device();
        correction = &motion;
        if (!vkImageResourceCreate(&output, dev, ctx->physicalDevice(), depth.w, depth.h,
                VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT, "scene.motionVectors")) return false;
        sourceDepth = depth.image.get(); targetMotion = motion.image.get();
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = depth.image.get(); vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = depth.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        if (!mc2vk::create(depthView, dev, [&](auto* h) { return vkCreateImageView(dev, &vi, nullptr, h); })) return false;
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.minFilter = si.magFilter = VK_FILTER_NEAREST; si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (!mc2vk::create(sampler, dev, [&](auto* h) { return vkCreateSampler(dev, &si, nullptr, h); })) return false;
        VkDescriptorSetLayoutBinding bindings[] = {
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
        VkDescriptorSetLayoutCreateInfo di{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        di.bindingCount = 2; di.pBindings = bindings;
        if (!mc2vk::create(dsl, dev, [&](auto* h) { return vkCreateDescriptorSetLayout(dev, &di, nullptr, h); })) return false;
        VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2}};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 1; pi.poolSizeCount = 1; pi.pPoolSizes = sizes;
        if (!mc2vk::create(pool, dev, [&](auto* h) { return vkCreateDescriptorPool(dev, &pi, nullptr, h); })) return false;
        const auto sl = dsl.get();
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool.get(); ai.descriptorSetCount = 1; ai.pSetLayouts = &sl;
        if (vkAllocateDescriptorSets(dev, &ai, &set) != VK_SUCCESS) return false;
        VkDescriptorImageInfo images[] = {
            {sampler.get(), depthView.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {sampler.get(), motion.view.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet writes[2]{};
        for (uint32_t i = 0; i < 2; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet = set;
            writes[i].dstBinding = i; writes[i].descriptorCount = 1;
            writes[i].descriptorType = bindings[i].descriptorType; writes[i].pImageInfo = &images[i];
        }
        vkUpdateDescriptorSets(dev, 2, writes, 0, nullptr);
        VkPushConstantRange pc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64};
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.setLayoutCount = 1; li.pSetLayouts = &sl; li.pushConstantRangeCount = 1; li.pPushConstantRanges = &pc;
        if (!mc2vk::create(layout, dev, [&](auto* h) { return vkCreatePipelineLayout(dev, &li, nullptr, h); })) return false;
        VkAttachmentDescription ad{};
        ad.format = output.format; ad.samples = VK_SAMPLE_COUNT_1_BIT;
        ad.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; ad.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ad.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; ad.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{}; sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1; sub.pColorAttachments = &ref;
        VkSubpassDependency deps[] = {
            {VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
             VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0},
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
        OwnedShaderModule vs(dev, vk_island_util::loadSpv(dev, vk_island_util::spvDir() + "postprocess.vert.spv", nullptr));
        OwnedShaderModule fs(dev, vk_island_util::loadSpv(dev, vk_island_util::spvDir() + "motion_vectors.frag.spv", nullptr));
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
        VkPipelineColorBlendAttachmentState ba{}; ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; blend.attachmentCount = 1; blend.pAttachments = &ba;
        VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dyn.dynamicStateCount = 2; dyn.pDynamicStates = states;
        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.stageCount = 2; gp.pStages = stages; gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vp; gp.pRasterizationState = &rs; gp.pMultisampleState = &ms;
        gp.pColorBlendState = &blend; gp.pDynamicState = &dyn; gp.layout = layout.get(); gp.renderPass = renderPass.get();
        return mc2vk::create(pipeline, dev, [&](auto* h) { return infra::graphics(dev, VK_NULL_HANDLE, 1, &gp, nullptr, h); });
    }
};

MotionVectors::MotionVectors() = default;
MotionVectors::~MotionVectors() = default;
void MotionVectors::reset() { impl_.reset(); }
VkImageView MotionVectors::view() const { return impl_ ? impl_->output.view.get() : VK_NULL_HANDLE; }
bool MotionVectors::prepare(VulkanContext& ctx, const VkImageResource& depth, VkImageResource& motion) {
    if (impl_ && impl_->sourceDepth == depth.image.get() && impl_->targetMotion == motion.image.get()) return true;
    reset();
    auto state = std::make_unique<Impl>();
    if (!state->init(ctx, depth, motion)) return false;
    impl_ = std::move(state);
    return true;
}
void MotionVectors::record(VkCommandBuffer cb, const VkImageResource& depth) {
    if (!impl_) return;
    auto& s = *impl_;
    const auto main = RenderCore::framePackets().copyMainView();
    // These are already the exact raw bytes the native shaders read column-major.
    s.pending = glm::make_mat4(main.uniforms.worldToClipGL);
    s.pendingFrame = main.frame;
    // Exact identity avoids inversion roundoff on still cameras and on history reset.
    glm::mat4 reprojection{1.0f};
    if (s.havePrevious && main.frame == s.previousFrame + 1 &&
        std::memcmp(glm::value_ptr(s.previous), glm::value_ptr(s.pending), sizeof(glm::mat4)) != 0) {
        const float det = glm::determinant(s.pending);
        if (det != 0.0f) reprojection = s.previous * glm::inverse(s.pending);
    }
    VkImageMemoryBarrier b[2]{};
    for (auto& barrier : b) {
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    }
    b[0].image = depth.image.get();
    b[0].subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
    b[0].oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    b[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b[0].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    b[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b[1].image = s.correction->image.get(); b[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b[1].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; b[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b[1].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 2, b);
    {
        MC2_VK_GPU_SCOPE(timer, cb, "VKIsl.Post.MotionVectors");
        VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rp.renderPass = s.renderPass.get(); rp.framebuffer = s.framebuffer.get(); rp.renderArea.extent = {depth.w, depth.h};
        vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport vp{0, 0, float(depth.w), float(depth.h), 0, 1}; VkRect2D sc{{0, 0}, {depth.w, depth.h}};
        vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &sc);
        infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s.pipeline.get());
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s.layout.get(), 0, 1, &s.set, 0, nullptr);
        vkCmdPushConstants(cb, s.layout.get(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64, glm::value_ptr(reprojection));
        vkCmdDraw(cb, 3, 1, 0, 0); vkCmdEndRenderPass(cb);
    }
    b[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b[0].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    b[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    b[1].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; b[1].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, nullptr, 0, nullptr, 2, b);
}
void MotionVectors::submitted() {
    if (!impl_) return;
    impl_->previous = impl_->pending; impl_->previousFrame = impl_->pendingFrame; impl_->havePrevious = true;
}
}
#endif
