// VK water island: init_island: the shared-device borrow, the GBuffer / depth images, buffers, render pass and pipeline.
// Declared in the class headers; shared names in vulkan_water_island_internal.h.
#include "vk_owned.h"
#include "vulkan_water_island_internal.h"
#include "vk_dynamic_rendering.h"
int gos_GetWaterFsDebugMode();



namespace vk_water_detail {

int g_waterInitReachedBuild = 0;

// One-time init: borrow the shared device, build the exportable color seam,
// gbuffer1 + depth images, recipe/thin/UBO buffers, render pass, pipeline.
bool init_island(WaterIsland* s, int w, int h) {
    s->w = w; s->h = h;
    ++g_health.attempted;
    g_waterInitReachedBuild = 0;

    // Bring up the shared engine device on demand (a no-op once born); not ready => retry next frame.
    if (!mc2vk::ensureEngineVulkanContext()) {
        setFallback("engine_vk_context_birth_failed"); return false;
    }
    mc2vk::VulkanContext& ctx = mc2vk::getOrCreateEngineVulkanContext();
    if (!ctx.valid()) { setFallback("engine_vk_context_not_valid"); return false; }
    s->instance  = ctx.instance();
    s->phys      = ctx.physicalDevice();
    s->device    = ctx.device();
    s->queue     = ctx.graphicsQueue();
    s->gfxFamily = ctx.graphicsFamily();
    s->allocator = ctx.allocator();
    if (s->device == VK_NULL_HANDLE || s->allocator == VK_NULL_HANDLE) {
        setFallback("engine_vk_device_or_allocator_null"); return false;
    }
    g_health.vulkanAvailable = 1;
    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(s->phys, &props);
        std::strncpy(g_health.deviceName, props.deviceName, sizeof(g_health.deviceName) - 1);
        g_health.deviceName[sizeof(g_health.deviceName) - 1] = '\0';
    }

    // ---- RUNG 6 (tier1): borrow the SHARED scene GBuffer (color0+normal+depth)
    // the scene renders into. Not ready => retry next frame.
    // VK-RUNG8-ITEM5: fire the one-shot MC2_VK_WATER_DIRECT_CHAIN deprecation note if
    // the retired gate was explicitly set (result ignored — always direct-chain-native).
    (void)waterDirectChainRequested();

    s->tier1 = true;
    s->sColorRes  = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_COLOR0);
    s->sNormalRes = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_NORMAL);
    s->sDepthRes  = mc2SharedSceneDepthGetResource();
    // VK-RUNG8-ITEM5: direct-chain-native only. The shared GBuffer is VK-only and
    // left Vulkan-owned by the upstream first-owner; only the VkImageResources are
    // required (no GL interop view, no GL<->VK semaphore handoff).
    if (!s->sColorRes || !s->sNormalRes || !s->sDepthRes) {
        setFallback("shared_gbuffer_not_ready");
        return false;
    }
    g_waterInitReachedBuild = 1;
    s->useInterop = false;
    g_health.interopActive = 0;
    g_health.vk2glSemActive = 0;
    vlog("TIER-1 shared scene GBuffer seam ACTIVE - in-place water co-write "
         "(color0+normal+depth, no composite, direct-chain native, no interop).");
    // ---- Recipe SSBO (mission-static, copied once at first run). Sized to the
    // current recipe count; if empty now, deferred to run() first frame. ----
    {
        const uint32_t rc = WaterStream::GetRecipeCount();
        // 64B/recipe (WaterRecipe). Allocate at least 1 to keep the descriptor valid.
        s->recipeBytes = static_cast<VkDeviceSize>((rc ? rc : 1)) * 64;
        if (!make_buffer(s, s->recipeBuf, s->recipeBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "water.recipeBuf")) return false;
    }
    // Thin SSBOs (per parity slot) — start with a small ring; grows in run().
    if (!ensure_thin_capacity(s, 0, 64 * 1024)) return false; // ~1365 records to start
    if (!ensure_thin_capacity(s, 1, 64 * 1024)) return false;
    // ---- Scene pass (VK_KHR_dynamic_rendering, no VkRenderPass/VkFramebuffer): color0 (shared, LOAD scene color, alpha-blend, STORE) + normal (LOAD,
    // STORE) + depth (LOAD, GEQUAL, write). Water runs after terrain/props/mechs wrote scene depth. The incoming WAW dependency of the old render pass
    // (this frame's colour/depth writes after the prior frame's stores) is the memory barrier recorded by beginSceneLoad. ----
    s->sceneFmts[0] = s->sColorRes->format; s->sceneFmts[1] = s->sNormalRes->format; s->sceneDepthFmt = s->sDepthRes->format;

    // ---- Descriptor layout/pool/set (2 UBO + 2 SSBO), both stages. ----
    {
        VkDescriptorSetLayoutBinding b[13]{};
        b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        // HORIZON-WAVE-1: the VS reads the camera position from this block too (grazing-angle wave fade).
        b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        b[2].binding = 5; b[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[2].descriptorCount = 1; b[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        b[3].binding = 6; b[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[3].descriptorCount = 1; b[3].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        // RUNG-6.5: HDRI equirect (copy-once import), FS-only.
        b[4].binding = 2; b[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[4].descriptorCount = 1; b[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        // VK-PARITY-WATER-SHORE-1: shore height (3) + shore mask (4), FS-only.
        for (int k = 0; k < 2; ++k) {
            b[5 + k].binding = 3 + k; b[5 + k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[5 + k].descriptorCount = 1; b[5 + k].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        // The static shadow bake's mask (7), FS-only: the water surface reads the same static shadow as the ground under it.
        b[7].binding = 7; b[7].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[7].descriptorCount = 1; b[7].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        b[8].binding=24; b[8].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[8].descriptorCount=1; b[8].stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;
        b[9].binding=25; b[9].descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b[9].descriptorCount=1; b[9].stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;
        for (int k=0;k<2;++k) {
            b[10+k].binding=k==0 ? 8 : 11;
            b[10+k].descriptorType=k==1 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[10+k].descriptorCount=1;
            b[10+k].stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        b[12].binding=13; b[12].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[12].descriptorCount=1; b[12].stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = 13; lci.pBindings = b;
        if (!mc2vk::create(s->dsl, s->device, [&](VkDescriptorSetLayout* h) { return vkCreateDescriptorSetLayout(s->device, &lci, nullptr, h); })) {
            vlog("vkCreateDescriptorSetLayout failed."); return false;
        }
        // One descriptor set per frame slot, including the reflection bindings.
        VkDescriptorPoolSize sizes[5]{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; sizes[0].descriptorCount = 2 * mc2vk::kMaxFramesInFlight;
        sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; sizes[1].descriptorCount = 3 * mc2vk::kMaxFramesInFlight;
        sizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sizes[2].descriptorCount = 6 * mc2vk::kMaxFramesInFlight;
        sizes[3].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; sizes[3].descriptorCount = 1 * mc2vk::kMaxFramesInFlight;
        sizes[4].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; sizes[4].descriptorCount = mc2vk::kMaxFramesInFlight;
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets = mc2vk::kMaxFramesInFlight; pci.poolSizeCount = 5; pci.pPoolSizes = sizes;
        if (!mc2vk::create(s->pool, s->device, [&](VkDescriptorPool* h) { return vkCreateDescriptorPool(s->device, &pci, nullptr, h); })) {
            vlog("vkCreateDescriptorPool failed."); return false;
        }
        VkDescriptorSetLayout layouts[mc2vk::kMaxFramesInFlight]; for (auto& layout : layouts) layout = s->dsl.get();
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = s->pool.get(); dsai.descriptorSetCount = mc2vk::kMaxFramesInFlight; dsai.pSetLayouts = layouts;
        if (MC2_VK_ALLOC_SETS(s->device, &dsai, s->set) != VK_SUCCESS) {
            vlog("vkAllocateDescriptorSets failed."); return false;
        }
        write_descriptors(s);
    }

    // ---- Pipeline (procedural VS, no vertex input; GEQUAL depth on+write on;
    // cull none; MRT alpha-blend SRC_ALPHA/ONE_MINUS_SRC_ALPHA on both color atts). ----
    {
        s->vert = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "water_geom.vert.spv"));
        s->frag = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "water_geom.frag.spv"));
        if (!s->vert || !s->frag) { setFallback("spv_load_failed"); return false; }

        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        if (!s->groundLighting.init(s->device)) return false;
        const VkDescriptorSetLayout layouts[]{s->dsl.get(), s->groundLighting.layout()};
        plci.setLayoutCount = 2; plci.pSetLayouts = layouts;
        if (!mc2vk::create(s->playout, s->device, [&](VkPipelineLayout* h) { return vkCreatePipelineLayout(s->device, &plci, nullptr, h); })) {
            vlog("vkCreatePipelineLayout failed."); return false;
        }

        VkPipelineVertexInputStateCreateInfo vin{};
        vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO; // count=0 (procedural)
        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        VkPhysicalDeviceFeatures waterFeatures{};
        vkGetPhysicalDeviceFeatures(s->phys, &waterFeatures);
        rs.depthClampEnable = waterFeatures.depthClamp;
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // Depth: reverse-Z GEQUAL, test on, WRITE ON (PipelineRegistry WaterArmed row).
        VkPipelineDepthStencilStateCreateInfo ds{};
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;

        // RUNG 6 tier1 blend: in-place over the LOADed scene color. The FS emits
        // premultiplied (col*a, a), producing the same RGB result as GL's
        // straight-alpha SRC_ALPHA/ONE_MINUS_SRC_ALPHA water pass.
        VkPipelineColorBlendAttachmentState cba[2]{};
        cba[0].blendEnable = VK_TRUE;
        cba[0].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cba[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba[0].colorBlendOp = VK_BLEND_OP_ADD;
        cba[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cba[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba[0].alphaBlendOp = VK_BLEND_OP_ADD;
        cba[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        // Attachment 1 = shared scene normal. GL's water GBuffer1 output has src.a
        // 0, so under GL's SRC_ALPHA blend it contributes nothing; writeMask=0 is
        // the exact no-op without requiring independentBlend.
        cba[1].blendEnable = VK_FALSE;
        // Keep normal RGB masked. Water shades its own light, so its alpha marker
        // must prevent the screen resolve from darkening the reflected sky again.
        cba[1].colorWriteMask = VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 2; cb.pAttachments = cba;

        VkViewport vp{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}};
        VkPipelineViewportStateCreateInfo vps{};
        vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &scissor;

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = s->vert.get(); stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = s->frag.get(); stages[1].pName = "main";
        VkGraphicsPipelineCreateInfo gp{};
        gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gp.stageCount = 2; gp.pStages = stages;
        gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia;
        const VkDynamicState dynViewport = VK_DYNAMIC_STATE_VIEWPORT;   // temporal AA jitters the viewport per frame
        VkPipelineDynamicStateCreateInfo dyns{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dyns.dynamicStateCount = 1; dyns.pDynamicStates = &dynViewport;
        gp.pDynamicState = &dyns;
        gp.pViewportState = &vps; gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms; gp.pDepthStencilState = &ds; gp.pColorBlendState = &cb;
        gp.layout = s->playout.get(); gp.renderPass = VK_NULL_HANDLE; gp.subpass = 0;
        const VkPipelineRenderingCreateInfo sceneRendering = mc2vk::dynrender::pipelineInfo(2, s->sceneFmts, s->sceneDepthFmt);
        gp.pNext = &sceneRendering;
        if (!mc2vk::create(s->pipe, s->device, [&](VkPipeline* h) { return mc2vk::infra::graphics(s->device, VK_NULL_HANDLE, 1, &gp, nullptr, h); })) {
            vlog("vkCreateGraphicsPipelines failed."); return false;
        }
        mc2vk::nameObject(s->device, s->pipe.get(), "water.pipe");
        if (gos_GetWaterFsDebugMode() == 10) {
            const VkSpecializationMapEntry vsEntry{12, 0, sizeof(int32_t)};
            const VkSpecializationMapEntry fsEntry{13, 0, sizeof(int32_t)};
            cba[0].blendEnable = VK_FALSE;
            for (int32_t pass = 1; pass <= 3; ++pass) {
                const int32_t physical = pass != 2;
                const VkSpecializationInfo vsSpec{1, &vsEntry, sizeof(physical), &physical};
                const VkSpecializationInfo fsSpec{1, &fsEntry, sizeof(pass), &pass};
                stages[0].pSpecializationInfo = &vsSpec;
                stages[1].pSpecializationInfo = &fsSpec;
                ds.depthWriteEnable = pass == 3 ? VK_TRUE : VK_FALSE;
                cba[1].colorWriteMask = pass == 3 ? VK_COLOR_COMPONENT_A_BIT : 0;
                if (!mc2vk::create(s->debugWater[pass - 1], s->device, [&](VkPipeline* h) {
                    return mc2vk::infra::graphics(s->device, VK_NULL_HANDLE, 1, &gp, nullptr, h);
                })) return false;
            }
            stages[0].pSpecializationInfo = nullptr;
        }
        const int32_t coverage = 1;
        const VkSpecializationMapEntry entry{11, 0, sizeof(coverage)};
        const VkSpecializationInfo spec{1, &entry, sizeof(coverage), &coverage};
        stages[1].pSpecializationInfo = &spec;
        const VkFormat maskFormats[2]{VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_UNDEFINED};
        const auto maskRendering = mc2vk::dynrender::pipelineInfo(2, maskFormats, s->sceneDepthFmt);
        gp.pNext = &maskRendering;
        ds.depthWriteEnable = VK_FALSE;
        cba[0].blendEnable = VK_FALSE; cba[1].colorWriteMask = 0;
        if (!mc2vk::create(s->maskCoverage, s->device, [&](VkPipeline* h) { return mc2vk::infra::graphics(s->device, VK_NULL_HANDLE, 1, &gp, nullptr, h); })) return false;
    }

    // ---- Command pool + 2 command buffers (per parity) + fence. ----
    {
        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = 0;
        pci.queueFamilyIndex = s->gfxFamily;
        for (uint32_t i = 0; i < mc2vk::kMaxFramesInFlight; ++i) {
            if (!mc2vk::create(s->cpool[i], s->device, [&](VkCommandPool* h) { return vkCreateCommandPool(s->device, &pci, nullptr, h); })) {
                vlog("vkCreateCommandPool failed."); return false;
            }
            VkCommandBufferAllocateInfo cai{};
            cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            cai.commandPool = s->cpool[i].get(); cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
            if (vkAllocateCommandBuffers(s->device, &cai, &s->cbuf[i]) != VK_SUCCESS) {
                vlog("vkAllocateCommandBuffers failed."); return false;
            }
        }
    }

    if (!init_reflection(s)) return false;
    s->inited = true;
    setFallback("");
    vlog("island initialized %dx%d (borrowed shared device; shared scene GBuffer; "
         "procedural water VS; GEQUAL reverse-Z depth-write-on; "
         "MRT alpha-blend).", w, h);
    return true;
}

} // namespace vk_water_detail

