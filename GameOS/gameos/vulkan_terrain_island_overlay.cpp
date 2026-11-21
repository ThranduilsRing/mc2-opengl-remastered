// VK terrain island: the terrain overlay and decal island: textures, pipeline init, submit, run, reset and the island destroy.
// Declared in the class headers; shared names in vulkan_terrain_island_internal.h.
#include "vk_owned.h"
#include "vk_island_fail_policy.h"
#include "vulkan_terrain_island_internal.h"
#include "vulkan_mover_shadow.h"
#include "vk_dynamic_rendering.h"
#include "../../RenderCore/TemporalInputs.h"
extern uint32_t g_mc2RenderFrame;   // mclib/tgl.cpp
#include "vk_gpu_profile.h"   // VK-GPU-ZONES: MC2_VK_GPU_SCOPE
#include "texture_decode.h"   // gos_GetTextureTexels
#include "../../mclib/battle_bench_wait.h"



// ===========================================================================
// VK-PARITY-OVERLAY-1: VK owner of the two GL world-space overlay draw groups that
// renderLists flushes into the (VK-discarded) GL scene buffer:
//   kind 0  overlays.terrain_overlays_static  (gos_terrain_indirect::DrawDecalStatic: the
//           mission-static cement / pad / runway / road authored-art overlay bake, plus the
//           legacy per-frame gos_DrawTerrainOverlays batch) -> terrain_overlay.frag port
//   kind 1  overlays.decals  (gos_DrawDecals: craters, footprints, contact blobs, dynamic
//           decals)                                                -> decal.frag port
// The GL draws are replaced (when the terrain island is armed) by queueing their vertex
// data here; mc2_vk_overlay_run (called right after Render.Decals in renderLists) draws the
// queue into the SHARED scene GBuffer (LOAD color0 + normal + depth, GEQUAL reverse-Z) with
// the terrain island's own set 0 (UBO slot 0 + cement/pool/shadow bindings), so overlays are
// lit by exactly the inputs the terrain beneath them used. Textures are copy-once mip'd
// imports of the handle's decoded texels (gos_GetTextureTexels), keyed on the gos texture handle.
// ===========================================================================

namespace vk_terrain_detail {


bool g_ovlDeclined = false;
std::vector<OvlPending> g_ovlPending;

void ovl_destroy_tex(OvlTex& t) {
    t.texture={};
    t.set[0]=t.set[1]=VK_NULL_HANDLE;   // freed with the pool
    t.source.reset();
}

// Idles the GPU, then releases the overlay objects (the state's destructor does it, in reverse declaration order).
void ovl_destroy(TerrainIsland* s) {
    if (!s || !s->ovl) return;
    mc2vk::getOrCreateEngineVulkanContext().waitIdle("terrain_overlay_destroy");
    s->ovl.reset();
}

bool ovl_alloc_tex_set(TerrainIsland* s, OvlState* o, OvlTex& t) {
    VkDescriptorSetLayout layouts[mc2vk::kMaxFramesInFlight]; for (auto& layout : layouts) layout = o->texDsl.get();
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorSetCount=mc2vk::kMaxFramesInFlight; ai.pSetLayouts=layouts;
    if (MC2_VK_ALLOC_SETS_CHAIN(o->texPool,ai,t.set)!=VK_SUCCESS) return false;
    for (uint32_t par=0;par<mc2vk::kMaxFramesInFlight;++par) {
        VkDescriptorImageInfo info{o->samp.get(),t.texture.view(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w.dstSet=t.set[par]; w.descriptorCount=1;
        w.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo=&info;
        vkUpdateDescriptorSets(s->device,1,&w,0,nullptr); t.bound[par]=info.imageView;
    }
    return true;
}

// Sampled image (copy-once import): RGBA8 gets a full blit-generated mip chain; BC7 blocks are copied as they are into a single
// level (a compressed image cannot be a blit target, and the GL texture is single-level too).
bool ovl_upload_tex(TerrainIsland* s, OvlTex& e, uint32_t w, uint32_t h, VkFormat fmt, const void* bytes, VkDeviceSize byteCount) {
    auto ref=mc2vk::getOrCreateEngineVulkanContext().textures().acquirePixels(w,h,fmt,bytes,static_cast<size_t>(byteCount),true);
    if (!ref) return false;
    e.texture=std::move(ref); return true;
}

// The overlay samples level 0 only: a BC7 chain from the legacy sidecar path is cut to its first level.
size_t ovl_level0_bytes(const GosTexels& texels) {
    return texels.mipCount > 1 ? mc2tex::bc7LevelBytes(texels.width, texels.height) : texels.bytes->size();
}

// Resolve (importing copy-once on first sight) the texture for a gos handle. nullptr = texels still
// pending or the import failed (caller skips the draw this frame). Untextured (0 / 0xffffffff, or a
// handle the producer reports has no CPU texels) binds the dummy, as the producer contract says.
OvlTex* ovl_get_tex(TerrainIsland* s, OvlState* o, uint32_t gosHandle) {
    if (gosHandle == 0 || gosHandle == 0xffffffffu) return &o->dummy;
    GosTexels texels = gos_GetTextureTexels(gosHandle);
    if (texels.status == GosTexels::Status::Unavailable) return &o->dummy;
    if (texels.status != GosTexels::Status::Ready) return nullptr;
    const uint32_t w = texels.width, h = texels.height;
    auto it = o->texs.find(gosHandle);
    if (it != o->texs.end()) {
        auto& [handle, texture] = *it;
        if (texture.source == texels.bytes) return &texture;
        if (!ovl_upload_tex(s,texture,w,h,mc2vk::texelFormat(texels.vkFormat),texels.bytes->data(),ovl_level0_bytes(texels))) return nullptr;
        texture.source=texels.bytes; return &texture;
    }
    ZoneScopedN("VKIsl.Overlay.TextureFirstUse");

    if (w == 0 || h == 0 || w > 8192 || h > 8192) return nullptr;

    OvlTex t;
    if (!ovl_upload_tex(s, t, w, h, mc2vk::texelFormat(texels.vkFormat), texels.bytes->data(),
                        static_cast<VkDeviceSize>(ovl_level0_bytes(texels))) ||
        !ovl_alloc_tex_set(s, o, t)) {
        // The upload of this texture may already be queued: the image leaves with the frame, not now.
        mc2vk::getOrCreateEngineVulkanContext().retire(std::move(t));
        return nullptr;
    }
    t.source = std::move(texels.bytes);
    vlog("[VK_TERRAIN_OVERLAY] texture cached: handle=%u %ux%u format=%u (%zu, pools=%zu).",
         gosHandle, w, h, texels.vkFormat, o->texs.size() + 1, o->texPool.length());
    const auto [cachedIt, inserted] = o->texs.emplace(gosHandle, std::move(t));
    auto& [handle, texture] = *cachedIt;
    return &texture;
}

bool ovl_prepare_static(TerrainIsland* s, OvlState* o, const std::vector<OvlPending>& pending) {
    for (const OvlPending& p : pending) {
        if (!p.isStatic) continue;
        auto& slot = o->statics[p.kind];
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(p.vertCount) * kOvlVertStride;
        if (slot.version == p.version && slot.buffer && bytes <= slot.capacity) continue;
        ZoneScopedN("VKIsl.Overlay.StaticUpload");
        if (!slot.buffer || slot.used || bytes > slot.capacity) {
            // A replaced bake may still be referenced by the preceding frame.
            mc2vk::getOrCreateEngineVulkanContext().retire(std::move(slot.buffer), slot.capacity);
            slot.capacity = bytes + 256;
            if (!make_buffer(s, slot.buffer, slot.capacity, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true, "terrain.overlay.stat")) return false;
            slot.used = false;
        }
        void* dst = nullptr;
        if (vmaMapMemory(s->allocator, slot.buffer.allocation(), &dst) != VK_SUCCESS) return false;
        std::memcpy(dst, p.staticPtr, static_cast<size_t>(bytes));
        vmaUnmapMemory(s->allocator, slot.buffer.allocation());
        slot.version = p.version;
    }
    return true;
}

void ovl_resolve_items(TerrainIsland* s, OvlState* o, const OvlPending& p, uint32_t base, std::vector<OvlItem>& items) {
    auto resolve = [&](std::vector<OvlItem>& out) {
        bool ready = true;
        for (const Mc2VkOverlayDraw& d : p.draws) {
            if (d.vertCount == 0 || d.firstVert + d.vertCount > p.vertCount) continue;
            OvlTex* t = ovl_get_tex(s, o, d.texHandle);
            if (!t) { ready = false; continue; } // retry incomplete imports next frame
            out.push_back({base + d.firstVert, d.vertCount, p.kind | (p.isStatic ? 4u : 0u), t});
        }
        return ready;
    };
    if (p.isStatic) {
        auto& slot = o->statics[p.kind];
        const auto generation = gos_GetTextureGeneration();
        if (slot.resolvedVersion != p.version || slot.textureGeneration != generation) {
            slot.items.clear();
            slot.resolvedVersion = resolve(slot.items) ? p.version : UINT64_MAX;
            slot.textureGeneration = generation;
        }
        items.insert(items.end(), slot.items.begin(), slot.items.end());
    } else {
        resolve(items);
    }
}

bool ovl_init(TerrainIsland* s, OvlState* o, int w, int h) {
    o->w = w; o->h = h;
    VkImageResource* colorRes  = s->colorRes;
    VkImageResource* normalRes = s->normalRes;
    VkImageResource* depthRes  = s->depthRes;
    if (!colorRes || !normalRes || !depthRes) return false;

    // The scene pass is a dynamic rendering pass (LOAD + STORE over the 3 shared views; vk_dynamic_rendering.h beginSceneLoad): no render pass object.

    {   // texture set layout / pool / sampler / dummy white
        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.minLod = 0.0f; sci.maxLod = 16.0f;
        mc2vk::infra::materialSampler(sci);
        o->sampBias = sci.mipLodBias;
        if (!mc2vk::create(o->samp, s->device, [&](auto* h) { return vkCreateSampler(s->device, &sci, nullptr, h); })) return false;
        VkDescriptorSetLayoutBinding b{};
        b.binding = 0; b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1; b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = 1; lci.pBindings = &b;
        if (!mc2vk::create(o->texDsl, s->device, [&](auto* h) { return vkCreateDescriptorSetLayout(s->device, &lci, nullptr, h); })) return false;
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, mc2vk::kMaxFramesInFlight*(kOvlTexPerPool + 2)};
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets = mc2vk::kMaxFramesInFlight*(kOvlTexPerPool + 2); pci.poolSizeCount = 1; pci.pPoolSizes = &ps;
        if (o->texPool.init(s->device, pci) != VK_SUCCESS) return false;
        const uint8_t px[4] = {255, 255, 255, 255};
        if (!ovl_upload_tex(s, o->dummy, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, px, sizeof(px)) || !ovl_alloc_tex_set(s, o, o->dummy)) return false;
    }

    {   // command buffers (own pair, from the island's pool)
        for (uint32_t i = 0; i < mc2vk::kMaxFramesInFlight; ++i) {
            VkCommandPoolCreateInfo pci{};
            pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            pci.queueFamilyIndex = s->gfxFamily;
            if (!mc2vk::create(o->cpool[i], s->device, [&](auto* h) { return vkCreateCommandPool(s->device, &pci, nullptr, h); })) return false;
            VkCommandBufferAllocateInfo cbi{};
            cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            cbi.commandPool = o->cpool[i].get(); cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount = 1;
            if (vkAllocateCommandBuffers(s->device, &cbi, &o->cbuf[i]) != VK_SUCCESS) return false;
        }
    }

    {   // pipelines
        const char* vn[2] = {"terrain_overlay_tier1.vert.spv", "terrain_decal_tier1.vert.spv"};
        const char* fn[2] = {"terrain_overlay_tier1.frag.spv", "terrain_decal_tier1.frag.spv"};
        for (int k = 0; k < 2; ++k) {
            o->vert[k] = load_spv(s->device, spv_dir() + vn[k]);
            o->frag[k] = load_spv(s->device, spv_dir() + fn[k]);
            if (!o->vert[k] || !o->frag[k]) return false;
        }
        VkDescriptorSetLayout sets[2] = { s->dsl.get(), o->texDsl.get() };
        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT; pcr.offset = 0; pcr.size = sizeof(OvlPC);
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 2; plci.pSetLayouts = sets;
        plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
        if (!mc2vk::create(o->playout, s->device, [&](auto* h) { return vkCreatePipelineLayout(s->device, &plci, nullptr, h); })) return false;

        // WorldOverlayVert (28B): pos vec3@0, uv vec2@12, fog float@20, argb u8x4 unorm@24.
        VkVertexInputBindingDescription vib{};
        vib.binding = 0; vib.stride = kOvlVertStride; vib.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        VkVertexInputAttributeDescription via[4]{};
        via[0].location = 0; via[0].binding = 0; via[0].format = VK_FORMAT_R32G32B32_SFLOAT; via[0].offset = 0;
        via[1].location = 1; via[1].binding = 0; via[1].format = VK_FORMAT_R32G32_SFLOAT;    via[1].offset = 12;
        via[2].location = 2; via[2].binding = 0; via[2].format = VK_FORMAT_R32_SFLOAT;       via[2].offset = 20;
        via[3].location = 3; via[3].binding = 0; via[3].format = VK_FORMAT_R8G8B8A8_UNORM;   via[3].offset = 24;
        VkPipelineVertexInputStateCreateInfo vin{};
        vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vin.vertexBindingDescriptionCount = 1; vin.pVertexBindingDescriptions = &vib;
        vin.vertexAttributeDescriptionCount = 4; vin.pVertexAttributeDescriptions = via;
        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vps{};
        vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vps.viewportCount = 1; vps.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dsci{};
        dsci.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dsci.dynamicStateCount = 2; dsci.pDynamicStates = dyn;
        const VkColorComponentFlags all = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        for (int k = 0; k < 2; ++k) {
            // kind 0 (TerrainOverlay row): opaque, depth test + WRITE, GEQUAL.
            // kind 1 (TerrainDecal row):  SRC_ALPHA/ONE_MINUS_SRC_ALPHA on MRT0, depth test, NO write, GEQUAL.
            // MRT1 is replaced (GL: shadowHandled value with alpha 1 => SRC_ALPHA blend == replace).
            VkPipelineDepthStencilStateCreateInfo ds{};
            ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
            ds.depthTestEnable = VK_TRUE;
            ds.depthWriteEnable = (k == 0) ? VK_TRUE : VK_FALSE;
            ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
            VkPipelineColorBlendAttachmentState cba[2]{};
            cba[0].colorWriteMask = all; cba[1].colorWriteMask = all;
            if (k == 1) {
                cba[0].blendEnable = VK_TRUE;
                cba[0].srcColorBlendFactor = cba[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                cba[0].dstColorBlendFactor = cba[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                cba[0].colorBlendOp = cba[0].alphaBlendOp = VK_BLEND_OP_ADD;
            }
            VkPipelineColorBlendStateCreateInfo cb{};
            cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            cb.attachmentCount = 2; cb.pAttachments = cba;
            VkPipelineShaderStageCreateInfo stages[2]{};
            stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = o->vert[k].get(); stages[0].pName = "main";
            stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = o->frag[k].get(); stages[1].pName = "main";
            VkGraphicsPipelineCreateInfo gp{};
            gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            gp.stageCount = 2; gp.pStages = stages;
            gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia;
            gp.pViewportState = &vps; gp.pRasterizationState = &rs;
            gp.pMultisampleState = &ms; gp.pDepthStencilState = &ds; gp.pColorBlendState = &cb;
            gp.pDynamicState = &dsci;
            const VkFormat sceneFmts[2] = { colorRes->format, normalRes->format };
            const VkPipelineRenderingCreateInfo prci = mc2vk::dynrender::pipelineInfo(2, sceneFmts, depthRes->format);
            gp.pNext = &prci;
            gp.layout = o->playout.get(); gp.renderPass = VK_NULL_HANDLE; gp.subpass = 0;
            if (!mc2vk::create(o->pipe[k], s->device, [&](auto* h) { return mc2vk::infra::graphics(s->device, VK_NULL_HANDLE, 1, &gp, nullptr, h); })) return false;
            mc2vk::nameObjectAt(s->device, o->pipe[k].get(), "terrain.overlayPipe", static_cast<int>(k));
        }
    }
    o->inited = true;
    vlog("[VK_TERRAIN_OVERLAY] island initialized %dx%d (cement/road overlays + decals -> shared GBuffer; GEQUAL reverse-Z).", w, h);
    return true;
}

}  // namespace

extern "C" int mc2_vk_overlay_enabled() {
    return g_ovlDeclined ? 0 : 1;
}

extern "C" void mc2_vk_overlay_submit(uint32_t kind, const void* verts, uint32_t vertCount,
                                      const Mc2VkOverlayDraw* draws, uint32_t drawCount, uint64_t staticVersion) {
    if (!verts || vertCount == 0 || !draws || drawCount == 0) return;
    OvlPending p;
    if (kind > 2u) return;
    p.kind = kind;
    p.vertCount = vertCount;
    p.draws.assign(draws, draws + drawCount);
    if (staticVersion != 0) {
        p.isStatic = true; p.version = staticVersion;
        p.staticPtr = static_cast<const uint8_t*>(verts);
    } else {
        p.verts.assign(static_cast<const uint8_t*>(verts),
                       static_cast<const uint8_t*>(verts) + static_cast<size_t>(vertCount) * kOvlVertStride);
    }
    g_ovlPending.push_back(std::move(p));
}

extern "C" int mc2_vk_overlay_prewarm(int w,int h) {
    TerrainIsland* s=parkedIsland();
    if(!s || !s->inited || !mc2_vk_overlay_enabled()) return 0;
    if(!s->ovl) {
        s->ovl=std::make_unique<OvlState>();
        if(!ovl_init(s,s->ovl.get(),w,h)) { ovl_destroy(s); return 0; }
    }
    ZoneScopedN("VKIsl.Overlay.WarmStatic");
    const void* verts = nullptr;
    const Mc2VkOverlayDraw* draws = nullptr;
    uint32_t vertCount = 0, drawCount = 0;
    uint64_t version = 0;
    if (!mc2_terrain_overlay_get_load_stream(&verts, &vertCount, &draws, &drawCount, &version)) return 0;
    std::vector<OvlPending> staging;
    if (vertCount && drawCount) {
        OvlPending p;
        p.isStatic = true; p.version = version; p.vertCount = vertCount;
        p.staticPtr = static_cast<const uint8_t*>(verts);
        p.draws.assign(draws, draws + drawCount);
        staging.push_back(std::move(p));
    }
    bool ready = ovl_prepare_static(s, s->ovl.get(), staging);
    for (const auto& p : staging)
        for (const auto& d : p.draws)
            if (d.vertCount && !ovl_get_tex(s, s->ovl.get(), d.texHandle)) ready = false;
    vlog("[VK_TERRAIN_OVERLAY] prestage verts=%u draws=%u textures=%zu ready=%d.",
        vertCount, drawCount, s->ovl->texs.size(), static_cast<int>(ready));
    return ready ? 1 : 0;
}

extern "C" int mc2_vk_overlay_run(int w, int h) {
    ZoneScopedN("VK-terrain-overlay");
    std::vector<OvlPending> pending;
    pending.swap(g_ovlPending);                       // always consume
    if (pending.empty() || !mc2_vk_overlay_enabled() || w <= 0 || h <= 0) return 0;
    TerrainIsland* s = parkedIsland();
    if (!s || !s->inited || s->disabled) return 0;    // terrain island not up: nothing to share state with

    if (s->ovl && (s->ovl->w != w || s->ovl->h != h)) ovl_destroy(s);
    if (!s->ovl) {
        s->ovl = std::make_unique<OvlState>();
        if (!ovl_init(s, s->ovl.get(), w, h)) {
            vlog("[VK_TERRAIN_OVERLAY] init failed -- overlays/decals will not draw under VK (permanent for this session).");
            g_ovlDeclined = true;
            ovl_destroy(s);
            return 0;
        }
    }
    OvlState* o = s->ovl.get();
    if (o->disabled) return 0;

    Mc2TerrainTier1Feed feed{};
    if (!mc2_terrain_tier1_get_frame_feed(&feed) || !feed.patches || feed.patchCount == 0) return 0;

    // ---- resolve textures (GL calls) and size the vertex streams BEFORE recording ----
    using Item = OvlItem; // low two kind bits select source, bit2 selects static stream
    std::vector<Item> items;
    VkDeviceSize dynBytes = 0;
    for (const OvlPending& p : pending) if (!p.isStatic) dynBytes += static_cast<VkDeviceSize>(p.vertCount) * kOvlVertStride;

    // Persistent static bake: upload only changed vertices, retiring the preceding version.
    if (!ovl_prepare_static(s, o, pending)) { o->disabled = true; return 0; }
    // Draw list in submission order; dynamic verts packed back to back.
    VkDeviceSize dynOff = 0;
    std::vector<std::pair<const OvlPending*, uint32_t>> dynRuns;   // (pending, first vertex in the dyn stream)
    for (const OvlPending& p : pending) {
        uint32_t base = 0;
        if (!p.isStatic) { base = static_cast<uint32_t>((dynOff / kOvlVertStride)); dynRuns.push_back({&p, base}); dynOff += static_cast<VkDeviceSize>(p.vertCount) * kOvlVertStride; }
        ovl_resolve_items(s, o, p, base, items);
    }
    if (items.empty()) return 0;

    Mc2VkOverlayFrame fr{};
    mc2_vk_overlay_fill_frame(&fr);

    // Per-frame data goes into the engine frame ring BEFORE beginFrame (the slot belongs to the frame, not to the parity): the one-slot UBO, the light pool
    // + grid spans and the dynamic vertex stream. The overlay stages its own copies; the terrain pass's spans are not shared.
    mc2vk::VulkanContext& rctx = mc2vk::getOrCreateEngineVulkanContext();
    const mc2vk::FrameAlloc uboSpan = rctx.frameAlloc(sizeof(TerrainFrameUBO));
    int32_t vkPoolLights = feed.numPoolLights;
    int32_t vkGridDim    = feed.lightGridDim;
    mc2vk::FrameAlloc lpSpans[3];
    mc2vk::FrameAlloc vtxSpan;
    if (!uboSpan || !stage_light_pool(s, feed, vkPoolLights, vkGridDim, lpSpans) || (dynBytes != 0 && !(vtxSpan = rctx.frameAlloc(dynBytes, 16)))) {
        return 0;
    }
    {
        TerrainFrameUBO u{};
        fill_frame_ubo(s, feed, feed.patches[0], vkPoolLights, vkGridDim, u);
        std::memcpy(uboSpan.ptr, &u, sizeof(u));
    }
    for (const auto& [pending, firstVertex] : dynRuns)
        std::memcpy(static_cast<uint8_t*>(vtxSpan.ptr) + static_cast<size_t>(firstVertex) * kOvlVertStride, pending->verts.data(), static_cast<size_t>(pending->vertCount) * kOvlVertStride);

    // ---- sync: same shared parity-slot fence discipline as the other islands ----
    // GPU-FRONTIER-1: engine frame slot + this island's own previous submit. The overlay draws through the terrain run's set[par]; when the run already
    // rewrote that set this frame the overlay leaves it alone (same values, and the run's submit may still be in flight). Otherwise the overlay is the
    // first user of the set this frame and waits the run's previous submit at this slot before rewriting it.
    const uint32_t par = rctx.frameContext().slot;
    const uint64_t frameNo = rctx.frameContext().frame;
    // The later graphics ticket covers both prior users. A set already bound this frame stays untouched, so exclude its live terrain submit.
    const auto slotTicket = s->setBoundFrame[par] == frameNo ? s->ovlSubmitted[par] : mc2vk::GpuTicket::both(s->ovlSubmitted[par], s->submitted[par]);
    { ZoneScopedN("VKIsl.Overlay.FenceWait"); mc2::battlebench::WaitScope gpuWait1(mc2::battlebench::WaitGpuMainNs);
      if (!vk_island_util::waitStrike(o->waitStrikes, o->disabled, rctx.wait(slotTicket, 5000000000ull, "overlay submission slot"), "VK_TERRAIN_OVERLAY")) { vlog("[VK_TERRAIN_OVERLAY] LOUD: overlay submission slot wait timed out (5 s); skipping this frame, will retry next frame."); return 0; } }
    if (s->setBoundFrame[par] != frameNo) {
        if (!mc2vk::bindMoverShadowDescriptors(s->device,s->set[par],&s->moverStamp[par])) return 0;
        if (!bind_environment(s,par)) return 0;
        refresh_biased_samplers(s);
        // Overlays can be the first terrain consumer of this parity (including load warmup).
        // Initialize the shared set after its wait even when the terrain draw was skipped.
        const std::array<VkImageView,7> views{s->colormapTexture.view(),s->cementTexture.view(),s->shoreMaskTexture.view(),s->matNormalTexture.view(),s->materialTexture.view(),s->macroGradientTexture.view(),s->staticVisibilityDummy.view()};
        if (s->descriptorsDirty[par] || s->boundTextures[par]!=views) {
            write_descriptors(s,static_cast<int>(par)); s->boundTextures[par]=views;
        }
        s->setBoundFrame[par] = frameNo;
    }
    if (mc2vk::infra::materialBiasStale(o->sampBias)) {   // FSR/AA resolve state changed: rebuild the sampler, rewrite this slot's texture sets
        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.minLod = 0.0f; sci.maxLod = 16.0f;
        mc2vk::infra::materialSampler(sci);
        mc2vk::OwnedSampler fresh;
        if (mc2vk::create(fresh, s->device, [&](auto* h) { return vkCreateSampler(s->device, &sci, nullptr, h); })) {
            rctx.retire(std::move(o->samp));
            o->samp = std::move(fresh);
            o->sampBias = sci.mipLodBias;
            ++o->sampGen;
            mc2vk::bumpDescEpoch();
        }
    }
    if (o->sampBoundGen[par] != o->sampGen) {
        for (auto& [key, tex] : o->texs) { (void)key; tex.bound[par] = VK_NULL_HANDLE; }
        o->dummy.bound[par] = VK_NULL_HANDLE;
        o->sampBoundGen[par] = o->sampGen;
    }
    for (const auto& item : items) {
        auto& t=*item.tex; const auto view=t.texture.view();
        if (t.bound[par]==view) continue;
        VkDescriptorImageInfo info{o->samp.get(),view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w.dstSet=t.set[par]; w.descriptorCount=1;
        w.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo=&info;
        vkUpdateDescriptorSets(s->device,1,&w,0,nullptr); t.bound[par]=view;
    }

    sync_ring_bindings(s, par, uboSpan, lpSpans);

    VkCommandBuffer cb = o->cbuf[par];
    vkResetCommandPool(s->device, o->cpool[par].get(), 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) { o->disabled = true; return 0; }
    {
        MC2_VK_GPU_SCOPE(listsScope, cb, "VKIsl.LightLists");
        s->lightLists.record(rctx, cb, par);
    }
    MC2_VK_GPU_SCOPE(gpuScope, cb, "VKIsl.Terrain.Overlay");
    mc2vk::dynrender::beginSceneLoad(rctx, cb, static_cast<uint32_t>(w), static_cast<uint32_t>(h), s->colorRes->view.get(), s->normalRes->view.get(),
                                     s->depthRes->view.get(), s->depthRes->format);
    VkViewport vp{0.0f, 0.0f, static_cast<float>(w),   // overlay quads (selection, fog of war) stay unjittered
 static_cast<float>(h), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &scissor);

    OvlPC pc{};
    for (int k = 0; k < 4; ++k) pc.fogColor[k] = fr.fogColor[k];
    pc.artFlatExempt = fr.artFlatExempt; pc.mapHalfExtent = fr.mapHalfExtent;
    pc.biasLegacy = fr.biasLegacy; pc.cementAdopt = fr.cementAdopt; pc.seamFix = fr.seamFix;
    const uint32_t dynOffsets[4] = {static_cast<uint32_t>(uboSpan.offset), static_cast<uint32_t>(lpSpans[0].offset),
                                    static_cast<uint32_t>(lpSpans[1].offset), static_cast<uint32_t>(lpSpans[2].offset)};
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, o->playout.get(), 0, 1, &s->set[par], 4, dynOffsets);

    int boundKind = -1, boundSource = -1; VkBuffer boundBuffer = VK_NULL_HANDLE; VkDescriptorSet boundTex = VK_NULL_HANDLE;
    unsigned long drawn[2] = {0, 0}, verts = 0;
    uint32_t mineDraws = 0;
    // Draw order is the blend order, so only ADJACENT items merge: same kind (pipeline, static/stream buffer, source) and texture set, and the
    // vertex ranges abut. One vkCmdDraw then covers exactly the vertices the separate draws would have, in the same order.
    uint32_t pendFirst = 0, pendCount = 0;
    auto flushPending = [&]() {
        if (pendCount == 0) return;
        mc2::battlebench::add(mc2::battlebench::DrawTerrain), vkCmdDraw(cb, pendCount, 1, pendFirst, 0);
        pendCount = 0;
    };
    const Item* prevItem = nullptr;
    for (const Item& it : items) {
        if (prevItem && pendCount != 0 && it.kind == prevItem->kind && it.tex->set[par] == prevItem->tex->set[par] && it.firstVtx == pendFirst + pendCount) {
            pendCount += it.count;
            const int mk = (it.kind & 3u) == 1u ? 1 : 0;
            ++drawn[mk]; verts += it.count;
            if ((it.kind & 3u) == 2u) ++mineDraws;
            prevItem = &it;
            continue;
        }
        flushPending();
        const int sourceKind = static_cast<int>(it.kind & 3u);
        const int kind = sourceKind == 1 ? 1 : 0;
        if (sourceKind != boundSource) {
            pc._pad[0] = sourceKind == 2 ? 2 : 0;
            vkCmdPushConstants(cb, o->playout.get(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
            boundSource = sourceKind;
        }
        const int isStat = (it.kind & 4u) ? 1 : 0;
        if (kind != boundKind) { mc2vk::infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, o->pipe[kind].get()); boundKind = kind; }
        VkBuffer vb = isStat ? o->statics[sourceKind].buffer.get() : vtxSpan.buffer;
        if (vb != boundBuffer) {
            VkDeviceSize vbOff = isStat ? 0 : vtxSpan.offset;
            vkCmdBindVertexBuffers(cb, 0, 1, &vb, &vbOff);
            boundBuffer = vb;
        }
        if (it.tex->set[par] != boundTex) {
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, o->playout.get(), 1, 1, &it.tex->set[par], 0, nullptr);
            boundTex = it.tex->set[par];
        }
        pendFirst = it.firstVtx; pendCount = it.count; prevItem = &it;
        ++drawn[kind]; verts += it.count;
        if (sourceKind == 2) ++mineDraws;
    }
    flushPending();
    mc2vk::dynrender::end(rctx, cb);
    gpuScope.end();
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) { o->disabled = true; return 0; }
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    { ZoneScopedN("VKIsl.Overlay.Submit");
      if (mc2vk::submitGraphics(si, VK_NULL_HANDLE) != VK_SUCCESS) { o->disabled = true; return 0; } }
    s->ovlSubmitted[par] = rctx.tracker().now();
    for (const auto& item : items)
        if (item.kind & 4u) o->statics[item.kind & 3u].used = true;

    static bool s_firstOk = false;
    static unsigned long s_frames = 0;
    ++s_frames;
    static bool mineWitness = false;
    if (mineDraws && !mineWitness) {
        vlog("[VK_TERRAIN_OVERLAY] native_mines draws=%u", mineDraws);
        mineWitness = true;
    }
    if (!s_firstOk) {
        vlog("[VK_TERRAIN_OVERLAY] first overlay co-write: overlay_draws=%lu decal_draws=%lu verts=%lu textures=%zu.",
             drawn[0], drawn[1], verts, o->texs.size());
        s_firstOk = true;
    }
    return 1;
}

extern "C" void mc2_vk_overlay_reset() {
    mc2vk::resetAllIslandInitFailures();  // mission boundary: a new mission starts with fresh retry allowances
    g_ovlPending.clear();
    TerrainIsland* s = parkedIsland();
    OvlState* o = s ? s->ovl.get() : nullptr;
    if (!o || !o->inited) return;
    // gos texture handles are reused by the next mission: drop the copy-once cache + static bake.
    mc2vk::getOrCreateEngineVulkanContext().waitIdle("terrain_overlay_reset");
    for (auto& [handle, tex] : o->texs) ovl_destroy_tex(tex);
    o->texs.clear();
    for (auto& slot : o->statics) slot = {};
    o->texPool.logChain("overlay");
    if (o->texPool) o->texPool.reset();
    o->dummy.set[0]=o->dummy.set[1]=VK_NULL_HANDLE;
    if (!ovl_alloc_tex_set(s, o, o->dummy)) { o->disabled = true; vlog("[VK_TERRAIN_OVERLAY] reset: dummy set failed -- disabled."); }
    vlog("[VK_TERRAIN_OVERLAY] texture cache + static bake reset on mission unload.");
}

extern "C" void mc2_vk_terrain_island_destroy() {
    vlog("[VK_TERRAIN_TIER1] vulkan_available=%d used_vulkan=%lu interop_active=%d vk2gl_sem=%d "
         "patch_records=%lu height_snapshots=%lu fallback_reason=\"%s\" vk_draws=%lu "
         "forbidden_getTexImage=%lu forbidden_readPixels=%lu stage_us=%.1f draw_us=%.1f total_us=%.1f",
         g_health.vulkanAvailable, g_health.usedVulkan, g_health.interopActive, g_health.vk2glSemActive,
         g_health.patchRecords, g_health.heightSnapshots, g_health.fallbackReason, g_health.vkDraws,
         g_forbidden.glGetTexImage, g_forbidden.glReadPixels,
         g_health.stageUs, g_health.drawUs, g_health.totalUs);
    // VK-DRAW-PERF-1: GPU-timestamp segment breakdown (only when MC2_VK_GPU_TIMESTAMPS
    // armed and supported; gap1 = setup+clear/load, gap2 = patch+skirt draws).
    TerrainIsland* island = parkedIsland();
    if (island && island->gpuTimer.supported && island->gpuTimer.samples > 0) {
        vlog("GPU_TIMESTAMPS samples=%llu gpu_pass_us=%.1f gpu_setup_us=%.1f gpu_draw_us=%.1f "
             "(cpu_draw_us=%.1f incl. record+submit+fence-wait)",
             static_cast<unsigned long long>(island->gpuTimer.samples),
             island->gpuTimer.avgTotalUs(),
             island->gpuTimer.avgGapUs(1),
             island->gpuTimer.avgGapUs(2),
             g_health.drawUs);
    }
    mc2vk::getOrCreateEngineVulkanContext().retireIsland(mc2vk::IslandSlot::Terrain);
}

extern "C" void mc2_vk_terrain_set_native_prefix_clear_color(float r, float g, float b, float a)
{
    g_prefixClearColor[0] = r;
    g_prefixClearColor[1] = g;
    g_prefixClearColor[2] = b;
    g_prefixClearColor[3] = a;
}

