// VK water island: init_island: the shared-device borrow, the GBuffer / depth images, buffers, render pass and pipeline.
// Declared in the class headers; shared names in vulkan_water_island_internal.h.
#include "vk_owned.h"
#include "vulkan_water_island_internal.h"

#ifdef MC2_VULKAN_ISLAND


namespace vk_water_detail {

// One-time init: borrow the shared device, build the exportable color seam,
// gbuffer1 + depth images, recipe/thin/UBO buffers, render pass, pipeline.
bool init_island(WaterIsland* s, int w, int h) {
    s->w = w; s->h = h;
    ++g_health.attempted;

    // VK-P9a: bring up the shared engine device on-demand, DECOUPLED from present.
    // If MC2_RENDER_BACKEND=vulkan already born it, this is a no-op; otherwise it
    // inits the SAME shared device (one volkLoadDevice, interop exts, NO swapchain)
    // so the water region runs on the GL-present path (SSAO model, shared device).
    // Fail-soft -> GL water draw.
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
    // the GL scene renders into. Fail-soft: not ready => GL water draw.
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

    // ---- Render pass: color0 (shared, LOAD scene color, alpha-blend, STORE) +
    // color1 (gbuffer1, CLEAR, STORE local) + depth (LOAD imported, GEQUAL, write). ----
    {
        VkAttachmentDescription att[3]{};
        // RUNG 6 tier1: LOAD-preserve pass over the shared scene GBuffer. Water
        // runs after terrain/props/mechs wrote scene depth and must GEQUAL-test
        // against it, exactly like the GL water draw does.
        att[0].format = s->sColorRes->format; att[0].samples = VK_SAMPLE_COUNT_1_BIT;
        att[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[0].finalLayout   = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[1].format = s->sNormalRes->format; att[1].samples = VK_SAMPLE_COUNT_1_BIT;
        att[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[1].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[1].finalLayout   = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[2].format = s->sDepthRes->format; att[2].samples = VK_SAMPLE_COUNT_1_BIT;
        att[2].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[2].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        att[2].finalLayout   = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference colorRefs[2]{
            {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
            {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
        VkAttachmentReference depthRef{2, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 2; sub.pColorAttachments = colorRefs;
        sub.pDepthStencilAttachment = &depthRef;

        // Incoming + outgoing external deps — interop cross-frame WAW serialize (SSAO
        // P1.5 pattern). WAW FIX: the shared color image is written every frame (CLEAR +
        // water draw + STORE) across two parity slots; without an INCOMING external dep
        // sync-val flags a WRITE_AFTER_WRITE between frame N's storeOp and frame N+1's
        // color write (the GL semaphore serializes it functionally, but sync-val can't
        // see across the GL boundary). deps[0]: order THIS frame's color+depth writes
        // AFTER any prior color/depth write on the shared/local images. srcAccessMask
        // includes COLOR_ATTACHMENT_WRITE so the prior store is a visible hazard source.
        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL; deps[0].dstSubpass = 0;
        deps[0].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[0].dependencyFlags = 0;
        // deps[1]: OUTGOING — the color store completes before the cross-API signal
        // hands the image back to GL (dst = BOTTOM_OF_PIPE, no dstAccess).
        deps[1].srcSubpass = 0; deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstStageMask  = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        deps[1].dstAccessMask = 0;

        VkRenderPassCreateInfo rpci{};
        rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpci.attachmentCount = 3; rpci.pAttachments = att;
        rpci.subpassCount = 1; rpci.pSubpasses = &sub;
        rpci.dependencyCount = 2; rpci.pDependencies = deps;
        if (!mc2vk::create(s->rpass, s->device, [&](VkRenderPass* h) { return vkCreateRenderPass(s->device, &rpci, nullptr, h); })) {
            vlog("vkCreateRenderPass failed."); return false;
        }
        VkImageView fbViews[3] = {
            s->sColorRes->view.get(),   // shared scene color0 (MRT0)
            s->sNormalRes->view.get(),  // shared scene normal (MRT1)
            s->sDepthRes->view.get()    // shared scene depth
        };
        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = s->rpass.get(); fci.attachmentCount = 3; fci.pAttachments = fbViews;
        fci.width = static_cast<uint32_t>(w); fci.height = static_cast<uint32_t>(h); fci.layers = 1;
        if (!mc2vk::create(s->fb, s->device, [&](VkFramebuffer* h) { return vkCreateFramebuffer(s->device, &fci, nullptr, h); })) {
            vlog("vkCreateFramebuffer failed."); return false;
        }
    }

    // ---- Descriptor layout/pool/set (2 UBO + 2 SSBO), both stages. ----
    {
        VkDescriptorSetLayoutBinding b[15]{};
        b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
        b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        // HORIZON-WAVE-1: the VS reads the camera position from this block too (grazing-angle wave fade).
        b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
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
        for (int k=0;k<4;++k) {
            b[10+k].binding=8+k;
            b[10+k].descriptorType=k==3 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[10+k].descriptorCount=1;
            b[10+k].stageFlags=k==0 ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_COMPUTE_BIT;
        }
        // The reflection compute's list of 8x8 half-res tiles that contain water (water_cull_pack).
        b[14].binding=12; b[14].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[14].descriptorCount=1; b[14].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = 15; lci.pBindings = b;
        if (!mc2vk::create(s->dsl, s->device, [&](VkDescriptorSetLayout* h) { return vkCreateDescriptorSetLayout(s->device, &lci, nullptr, h); })) {
            vlog("vkCreateDescriptorSetLayout failed."); return false;
        }
        // One descriptor set per frame slot, including the reflection bindings.
        VkDescriptorPoolSize sizes[5]{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; sizes[0].descriptorCount = 2 * mc2vk::kMaxFramesInFlight;
        sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; sizes[1].descriptorCount = 3 * mc2vk::kMaxFramesInFlight;
        sizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sizes[2].descriptorCount = 8 * mc2vk::kMaxFramesInFlight;
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
        gp.layout = s->playout.get(); gp.renderPass = s->rpass.get(); gp.subpass = 0;
        if (!mc2vk::create(s->pipe, s->device, [&](VkPipeline* h) { return mc2vk::infra::graphics(s->device, VK_NULL_HANDLE, 1, &gp, nullptr, h); })) {
            vlog("vkCreateGraphicsPipelines failed."); return false;
        }
        mc2vk::nameObject(s->device, s->pipe.get(), "water.pipe");
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

#endif // MC2_VULKAN_ISLAND
