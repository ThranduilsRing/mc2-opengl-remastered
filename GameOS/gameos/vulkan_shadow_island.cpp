// VK-REGION-SHADOW-1 (sub-slice A) -- reusable DEPTH-ONLY INSTANCED shadow-caster
// pipeline builder. This is the WRITE side of the shadow path (the comparesampler /
// csmarray fixtures proved the READ side). Entire TU compiles to nothing unless
// MC2_VULKAN is defined (CMake option, default OFF), and is ADDITIVE island code --
// it does NOT touch the GL shadow producer (gameos_graphics.cpp
// beginDynamicShadowPass/endDynamicShadowPass/drawShadowBatchTessellated), which
// stays the direct-GL path untouched by this seam.
//
// The pipeline:
//   - NO color attachment (depth-only render pass; caller owns the pass).
//   - depthTestEnable + depthWriteEnable = TRUE, depthCompareOp = LESS.
//   - vertex input = one vec3 position stream (a unit quad), drawn INSTANCED.
//   - descriptor set0 = per-frame view/cascade UBO (lightSpace[] + cascadeIndex).
//   - descriptor set1 = the instance ring, bound STORAGE_BUFFER_DYNAMIC so the
//     caller supplies a dynamic offset = slot*capacity*stride (inventory hard-case
//     #2: the 3-frames-in-flight persistent-mapped ring).
//
// Fail-soft everywhere: any VkResult error logs a reason + returns false and leaves
// the out struct zeroed (the caller then declines / falls back). Never crashes.

#include "vulkan_backend_skeleton.h"

#ifdef MC2_VULKAN

#include <volk.h>

#include <cstdarg>
#include <cstdio>

namespace {

void shadowlog(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "[VULKAN_SHADOW] ");
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
    va_end(ap);
}

}  // namespace

bool mc2_vulkan_build_shadow_depth_pipeline(VkDevice device, VkRenderPass renderPass,
                                            VkShaderModule vert, VkShaderModule frag,
                                            Mc2VkShadowDepthPipeline* out) {
    if (!out) return false;
    *out = Mc2VkShadowDepthPipeline{};
    if (device == VK_NULL_HANDLE || renderPass == VK_NULL_HANDLE ||
        vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE) {
        shadowlog("build-pipeline: null argument. fail-soft.");
        return false;
    }

    VkResult r;

    // ---- set0: per-frame view/cascade UBO ----------------------------------
    {
        VkDescriptorSetLayoutBinding b{};
        b.binding = 0;
        b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = 1;
        lci.pBindings = &b;
        r = vkCreateDescriptorSetLayout(device, &lci, nullptr, &out->frameSetLayout);
        if (r != VK_SUCCESS) { shadowlog("build-pipeline: create set0 layout failed (%d). fail-soft.", (int)r); goto fail; }
    }

    // ---- set1: instance ring, STORAGE_BUFFER_DYNAMIC -----------------------
    {
        VkDescriptorSetLayoutBinding b{};
        b.binding = 0;
        b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = 1;
        lci.pBindings = &b;
        r = vkCreateDescriptorSetLayout(device, &lci, nullptr, &out->instanceSetLayout);
        if (r != VK_SUCCESS) { shadowlog("build-pipeline: create set1 layout failed (%d). fail-soft.", (int)r); goto fail; }
    }

    // ---- pipeline layout (set0, set1) --------------------------------------
    {
        VkDescriptorSetLayout sets[2] = { out->frameSetLayout, out->instanceSetLayout };
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 2;
        plci.pSetLayouts = sets;
        r = vkCreatePipelineLayout(device, &plci, nullptr, &out->layout);
        if (r != VK_SUCCESS) { shadowlog("build-pipeline: create pipeline layout failed (%d). fail-soft.", (int)r); goto fail; }
    }

    // ---- graphics pipeline (depth-only, instanced) -------------------------
    {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert;
        stages[0].pName  = "main";
        stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag;
        stages[1].pName  = "main";

        // One vertex buffer: vec3 position at location 0 (a unit quad).
        VkVertexInputBindingDescription vib{};
        vib.binding = 0;
        vib.stride = sizeof(float) * 3;
        vib.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        VkVertexInputAttributeDescription via{};
        via.location = 0;
        via.binding = 0;
        via.format = VK_FORMAT_R32G32B32_SFLOAT;
        via.offset = 0;
        VkPipelineVertexInputStateCreateInfo vin{};
        vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vin.vertexBindingDescriptionCount = 1;
        vin.pVertexBindingDescriptions = &vib;
        vin.vertexAttributeDescriptionCount = 1;
        vin.pVertexAttributeDescriptions = &via;

        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vps{};
        vps.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vps.viewportCount = 1;
        vps.scissorCount  = 1;

        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode    = VK_CULL_MODE_NONE;   // caster geometry is single-sided in the fixture
        rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth   = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // Depth-only: test + write ON, compare LESS. No stencil.
        VkPipelineDepthStencilStateCreateInfo ds{};
        ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthTestEnable  = VK_TRUE;
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp   = VK_COMPARE_OP_LESS;
        ds.depthBoundsTestEnable = VK_FALSE;
        ds.stencilTestEnable     = VK_FALSE;

        // NO color attachment -> a color-blend state with zero attachments.
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 0;
        cb.pAttachments    = nullptr;

        VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dsci{};
        dsci.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dsci.dynamicStateCount = 2;
        dsci.pDynamicStates = dyn;

        VkGraphicsPipelineCreateInfo gp{};
        gp.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gp.stageCount          = 2;
        gp.pStages             = stages;
        gp.pVertexInputState   = &vin;
        gp.pInputAssemblyState = &ia;
        gp.pViewportState      = &vps;
        gp.pRasterizationState = &rs;
        gp.pMultisampleState   = &ms;
        gp.pDepthStencilState  = &ds;
        gp.pColorBlendState    = &cb;
        gp.pDynamicState       = &dsci;
        gp.layout              = out->layout;
        gp.renderPass          = renderPass;
        gp.subpass             = 0;
        r = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, nullptr, &out->pipeline);
        if (r != VK_SUCCESS) { shadowlog("build-pipeline: vkCreateGraphicsPipelines failed (%d). fail-soft.", (int)r); goto fail; }
    }

    return true;

fail:
    mc2_vulkan_destroy_shadow_depth_pipeline(device, out);
    return false;
}

void mc2_vulkan_destroy_shadow_depth_pipeline(VkDevice device, Mc2VkShadowDepthPipeline* p) {
    if (!p || device == VK_NULL_HANDLE) return;
    if (p->pipeline)          vkDestroyPipeline(device, p->pipeline, nullptr);
    if (p->layout)            vkDestroyPipelineLayout(device, p->layout, nullptr);
    if (p->instanceSetLayout) vkDestroyDescriptorSetLayout(device, p->instanceSetLayout, nullptr);
    if (p->frameSetLayout)    vkDestroyDescriptorSetLayout(device, p->frameSetLayout, nullptr);
    *p = Mc2VkShadowDepthPipeline{};
}

#endif // MC2_VULKAN
