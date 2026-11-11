#include "vulkan_motion_vectors.h"
#include "vulkan_context.h"
#include "vk_image_resource.h"
#include "vk_island_util.h"
#include "vk_render_infra.h"
#include "vk_gpu_profile.h"
#include "vk_dynamic_rendering.h"
#define MC2_VULKAN_SHARED_SCENE_COLOR_IMPL_ISLAND
#include "vulkan_shared_scene_color.h"
#include "../../RenderCore/FramePackets.h"
#include "../../RenderCore/TemporalInputs.h"
#include <atomic>
extern std::atomic<uint32_t> g_mc2FrameCounter;
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <cstring>
#include <string>
#include <string_view>

namespace mc2vk {
namespace {
bool makeFullscreenPipeline(VkDevice dev, VkPipelineLayout layout, const VkFormat* colorFormats, std::string_view frag,
                            VkColorComponentFlags writeMask, OwnedPipeline& out, uint32_t attachments = 1) {
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
    VkPipelineColorBlendAttachmentState ba[2]{}; ba[0].colorWriteMask = writeMask; ba[1].colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; blend.attachmentCount = attachments; blend.pAttachments = ba;
    VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dyn.dynamicStateCount = 2; dyn.pDynamicStates = states;
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2; gp.pStages = stages; gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp; gp.pRasterizationState = &rs; gp.pMultisampleState = &ms;
    gp.pColorBlendState = &blend; gp.pDynamicState = &dyn; gp.layout = layout;
    const VkPipelineRenderingCreateInfo prci = dynrender::pipelineInfo(attachments, colorFormats, VK_FORMAT_UNDEFINED);
    gp.pNext = &prci; gp.renderPass = VK_NULL_HANDLE;
    return mc2vk::create(out, dev, [&](auto* h) { return infra::graphics(dev, VK_NULL_HANDLE, 1, &gp, nullptr, h); });
}
}

struct MotionVectors::Impl {
    VulkanContext* ctx = nullptr;
    VkImageResource output{};
    VkImageResource reactive{};   // R8, 0 = opaque static; coverage-row writers land with the resolve slices
    OwnedImageView depthView;
    OwnedSampler sampler;
    OwnedDescriptorSetLayout dsl;
    OwnedDescriptorPool pool;
    OwnedPipelineLayout layout;
    OwnedPipeline pipeline;
    VkDescriptorSet set = VK_NULL_HANDLE;       // binding 0 = scene depth
    VkDescriptorSet debugSet = VK_NULL_HANDLE;  // binding 0 = motion output
    VkImage sourceDepth = VK_NULL_HANDLE;
    const VkImageResource* normalImage = nullptr;   // shared GBuffer1, bound at binding 2 for the water reactive class
    // Debug inset over the shared present image (built on first use).
    OwnedPipeline debugPipeline;
    VkImageView debugTarget = VK_NULL_HANDLE;
    bool recorded = false, debugFailed = false;

    ~Impl() {
        if (ctx) { vkImageResourceDestroy(&reactive, ctx->device()); vkImageResourceDestroy(&output, ctx->device()); }
    }
    bool init(VulkanContext& context, const VkImageResource& depth) {
        ctx = &context;
        const auto dev = ctx->device();
        if (!vkImageResourceCreate(&output, dev, ctx->physicalDevice(), depth.w, depth.h,
                VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT, "scene.motionVectors")) return false;
        if (!vkImageResourceCreate(&reactive, dev, ctx->physicalDevice(), depth.w, depth.h,
                VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                "scene.reactiveMask")) return false;
        sourceDepth = depth.image.get();
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = depth.image.get(); vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = depth.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        if (!mc2vk::create(depthView, dev, [&](auto* h) { return vkCreateImageView(dev, &vi, nullptr, h); })) return false;
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.minFilter = si.magFilter = VK_FILTER_NEAREST; si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (!mc2vk::create(sampler, dev, [&](auto* h) { return vkCreateSampler(dev, &si, nullptr, h); })) return false;
        // 0 = scene depth (debug set: motion output), 1 = per-object motion from the mech island (debug set: unused dummy).
        VkDescriptorSetLayoutBinding binding[3] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                                   {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                                   {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
        VkDescriptorSetLayoutCreateInfo di{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        di.bindingCount = 3; di.pBindings = binding;
        if (!mc2vk::create(dsl, dev, [&](auto* h) { return vkCreateDescriptorSetLayout(dev, &di, nullptr, h); })) return false;
        VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6}};
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
        // The mech island's object-motion image is a shared scene target: it lives as long as the depth this was built for.
        const VkImageResource* objectMv = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_OBJECT_MV);
        VkDescriptorImageInfo objectImages[2] = {
            {sampler.get(), objectMv ? objectMv->view.get() : output.view.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {sampler.get(), output.view.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        // Binding 2: shared GBuffer1 (water class in .a). The scene keeps it in COLOR_ATTACHMENT_OPTIMAL; record() brackets the pass.
        normalImage = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_NORMAL);
        VkDescriptorImageInfo normalImages[2] = {
            {sampler.get(), normalImage ? normalImage->view.get() : output.view.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {sampler.get(), output.view.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet writes[6]{};
        for (uint32_t i = 0; i < 6; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet = sets[i & 1];
            writes[i].dstBinding = i >> 1; writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo = (i >> 1) == 2 ? &normalImages[i & 1] : (i >> 1) ? &objectImages[i & 1] : &images[i & 1];
        }
        vkUpdateDescriptorSets(dev, 6, writes, 0, nullptr);
        VkPushConstantRange pc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 124};
        const auto sl = dsl.get();
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.setLayoutCount = 1; li.pSetLayouts = &sl; li.pushConstantRangeCount = 1; li.pPushConstantRanges = &pc;
        if (!mc2vk::create(layout, dev, [&](auto* h) { return vkCreatePipelineLayout(dev, &li, nullptr, h); })) return false;
        // The pass is dynamic rendering: output + reactive, DONT_CARE load / STORE; record() brackets it with the layout barriers the render pass did.
        const VkFormat fmts[2] = {output.format, reactive.format};
        return makeFullscreenPipeline(dev, layout.get(), fmts, "motion_vectors.frag.spv",
                                      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT, pipeline, 2);
    }
    bool initDebug(VkImageView target, uint32_t w, uint32_t h) {
        const auto dev = ctx->device();
        if (!debugPipeline) {   // LOAD pass over the present image, no layout change; recordDebug brackets it with the stage/access dependencies
            const VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
            if (!makeFullscreenPipeline(dev, layout.get(), &fmt, "motion_vectors_debug.frag.spv",
                    VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
                    debugPipeline)) return false;
        }
        debugTarget = target;
        return true;
    }
};

MotionVectors::MotionVectors() = default;
MotionVectors::~MotionVectors() = default;
void MotionVectors::reset() { impl_.reset(); }
VkImageView MotionVectors::view() const { return impl_ ? impl_->output.view.get() : VK_NULL_HANDLE; }
VkImage MotionVectors::image() const { return impl_ ? impl_->output.image.get() : VK_NULL_HANDLE; }
VkImage MotionVectors::reactiveImage() const { return impl_ ? impl_->reactive.image.get() : VK_NULL_HANDLE; }
VkImageView MotionVectors::reactiveView() const { return impl_ ? impl_->reactive.view.get() : VK_NULL_HANDLE; }
bool MotionVectors::prepare(VulkanContext& ctx, const VkImageResource& depth) {
    if (impl_ && impl_->sourceDepth == depth.image.get() && impl_->output.w == depth.w && impl_->output.h == depth.h) return true;
    reset();
    auto state = std::make_unique<Impl>();
    if (!state->init(ctx, depth)) return false;
    impl_ = std::move(state);
    return true;
}
void MotionVectors::record(VkCommandBuffer cb, const VkImageResource& depth, uint32_t renderFrame, bool objectMotionWritten) {
    if (!impl_) return;
    auto& s = *impl_;
    const auto main = RenderCore::framePackets().copyMainView();
    // ViewUniforms bytes read as the column-major mat4 the native shaders use. The producer
    // (GameCamera::renderViewUniforms) publishes previous == current on a history reset.
    const glm::mat4 current = glm::make_mat4(main.uniforms.worldToClipGL);
    const glm::mat4 previous = glm::make_mat4(main.uniforms.previousWorldToClipGL);
    // Exact identity avoids inversion roundoff on still cameras and on reset; an all-zero
    // previous (producers without history) gives w == 0 and therefore zero motion.
    // Render jitter of this frame (zero unless a temporal consumer is registered). The depth was
    // drawn with it; the shader removes it before reprojecting.
    float jitterClip[2];
    RenderCore::temporal::frameJitterClip(renderFrame, depth.w, depth.h, depth.w, jitterClip);
    glm::mat4 reprojection{1.0f};
    glm::vec4 skyRows[3] = {};   // x, y, w rows of the rotation-only (translation-free) reprojection for sky pixels
    if (std::memcmp(glm::value_ptr(previous), glm::value_ptr(current), sizeof(glm::mat4)) != 0 &&
        glm::determinant(current) != 0.0f) {
        const glm::mat4 inverseCurrent = glm::inverse(current);
        reprojection = previous * inverseCurrent;
        // A depth-0 pixel is a direction at infinity: drop the camera translation, keep the previous view rotation.
        glm::mat4 previousRotation = previous;
        previousRotation[3] = glm::vec4(0.0f);
        const glm::vec3 cameraPos(main.uniforms.cameraWorldPos[0], main.uniforms.cameraWorldPos[1], main.uniforms.cameraWorldPos[2]);
        glm::mat4 toCamera(1.0f);   // column-convention (clip-side) translation by -cameraPos: column 3
        toCamera[3] = glm::vec4(-cameraPos, 1.0f);
        const glm::mat4 sky = previousRotation * toCamera * inverseCurrent;
        // The eye lies on the clip w = 0 plane; if the published position does not satisfy that (different world
        // convention) the sky keeps zero motion rather than a wrong vector.
        const float eyeW = std::abs((current * glm::vec4(cameraPos, 1.0f)).w);
        const float eyeScale = (std::abs(current[0][3]) + std::abs(current[1][3]) + std::abs(current[2][3])) * (1.0f + glm::length(cameraPos));
        if (eyeW <= 1e-4f * eyeScale + 1e-3f)
        for (int r = 0; r < 3; ++r) skyRows[r] = glm::vec4(sky[0][r == 2 ? 3 : r], sky[1][r == 2 ? 3 : r], sky[2][r == 2 ? 3 : r], sky[3][r == 2 ? 3 : r]);
    }
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
    VkImageMemoryBarrier nb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    const bool normalReadable = s.normalImage && s.normalImage->image;
    if (normalReadable) {
        nb.srcQueueFamilyIndex = nb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        nb.image = s.normalImage->image.get();
        nb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        nb.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        nb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        nb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        nb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &nb);
    }
    {
        MC2_VK_GPU_SCOPE(timer, cb, "VKIsl.Post.MotionVectors");
        // The render pass's initial layout move + external dependency: both targets UNDEFINED -> COLOR_ATTACHMENT, after the previous frame's readers.
        constexpr VkPipelineStageFlags2 kPrevReaders = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        dynrender::colorImageBarrier(*s.ctx, cb, s.output.image.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                     kPrevReaders, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        dynrender::colorImageBarrier(*s.ctx, cb, s.reactive.image.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                     kPrevReaders, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        {
            dynrender::Pass pass;
            pass.area = {{0, 0}, {s.output.w, s.output.h}};
            pass.colorCount = 2;
            pass.color[0] = dynrender::attachment(s.output.view.get(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE);
            pass.color[1] = dynrender::attachment(s.reactive.view.get(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE);
            dynrender::begin(*s.ctx, cb, pass);
        }
        VkViewport vp{0, 0, static_cast<float>(s.output.w), static_cast<float>(s.output.h), 0, 1}; VkRect2D sc{{0, 0}, {s.output.w, s.output.h}};
        vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &sc);
        infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s.pipeline.get());
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s.layout.get(), 0, 1, &s.set, 0, nullptr);
        vkCmdPushConstants(cb, s.layout.get(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64, glm::value_ptr(reprojection));
        vkCmdPushConstants(cb, s.layout.get(), VK_SHADER_STAGE_FRAGMENT_BIT, 64, 48, skyRows);
        vkCmdPushConstants(cb, s.layout.get(), VK_SHADER_STAGE_FRAGMENT_BIT, 112, 8, jitterClip);
        const uint32_t objectValid = (objectMotionWritten ? 1u : 0u) | (normalReadable ? 2u : 0u);
        vkCmdPushConstants(cb, s.layout.get(), VK_SHADER_STAGE_FRAGMENT_BIT, 120, 4, &objectValid);
        vkCmdDraw(cb, 3, 1, 0, 0); dynrender::end(*s.ctx, cb);
        // The render pass's final layout + external dependency: COLOR_ATTACHMENT -> SHADER_READ_ONLY for the later fragment-shader reads.
        dynrender::colorImageBarrier(*s.ctx, cb, s.output.image.get(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        dynrender::colorImageBarrier(*s.ctx, cb, s.reactive.image.get(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    }
    if (normalReadable) {
        nb.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        nb.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        nb.srcAccessMask = 0;
        nb.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &nb);
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
    if (s.debugTarget != target || !s.debugPipeline) {
        if (!s.initDebug(target, w, h)) { s.debugFailed = true; return false; }
    }
    MC2_VK_GPU_SCOPE(timer, cb, "VKIsl.Present.MotionVectorsDebug");
    // LOAD pass with no layout transition: chain against the previous writer of the present image and its later readers explicitly.
    constexpr VkPipelineStageFlags2 kAround = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    constexpr VkAccessFlags2 kAroundAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    dynrender::memoryBarrier(*s.ctx, cb, kAround, kAroundAccess, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    {
        dynrender::Pass pass;
        pass.area = {{0, 0}, {w, h}};
        pass.colorCount = 1;
        pass.color[0] = dynrender::attachment(target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE);
        dynrender::begin(*s.ctx, cb, pass);
    }
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
    dynrender::end(*s.ctx, cb);
    dynrender::memoryBarrier(*s.ctx, cb, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, kAround, kAroundAccess);
    return true;
}
}
