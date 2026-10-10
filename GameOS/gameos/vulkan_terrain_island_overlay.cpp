// VK terrain island: the terrain overlay and decal island: textures, pipeline init, submit, run, reset and the island destroy.
// Declared in the class headers; shared names in vulkan_terrain_island_internal.h.
#include "vulkan_terrain_island_internal.h"

#ifdef MC2_VULKAN_ISLAND


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
// imports of the GL textures, keyed on the gos texture handle.
// ===========================================================================
uint32_t gos_GetGLTextureId(uint32_t gosHandle);   // gameos_graphics.cpp

namespace vk_terrain_detail {

OvlState* g_ovl = nullptr;
bool g_ovlDeclined = false;
std::vector<OvlPending> g_ovlPending;

void ovl_destroy_tex(TerrainIsland* s, OvlTex& t) {
    if (t.view) vkDestroyImageView(s->device, t.view, nullptr);
    if (t.img)  vmaDestroyImage(s->allocator, t.img, t.alloc);
    t = OvlTex{};
}

void ovl_destroy(TerrainIsland* s) {
    OvlState* o = g_ovl;
    if (!o || o->owner != s) return;
    VkDevice d = s->device;
    if (d != VK_NULL_HANDLE) vkDeviceWaitIdle(d);
    for (int k = 0; k < 2; ++k) {
        if (o->pipe[k]) vkDestroyPipeline(d, o->pipe[k], nullptr);
        if (o->vert[k]) vkDestroyShaderModule(d, o->vert[k], nullptr);
        if (o->frag[k]) vkDestroyShaderModule(d, o->frag[k], nullptr);
        if (o->vtx[k])  vmaDestroyBuffer(s->allocator, o->vtx[k], o->vtxAlloc[k]);
    }
    if (o->stat)    vmaDestroyBuffer(s->allocator, o->stat, o->statAlloc);
    if (o->fb)      vkDestroyFramebuffer(d, o->fb, nullptr);
    if (o->rpass)   vkDestroyRenderPass(d, o->rpass, nullptr);
    if (o->playout) vkDestroyPipelineLayout(d, o->playout, nullptr);
    for (auto& kv : o->texs) ovl_destroy_tex(s, kv.second);
    ovl_destroy_tex(s, o->dummy);
    if (o->texPool) vkDestroyDescriptorPool(d, o->texPool, nullptr);
    if (o->texDsl)  vkDestroyDescriptorSetLayout(d, o->texDsl, nullptr);
    if (o->samp)    vkDestroySampler(d, o->samp, nullptr);
    delete o;
    g_ovl = nullptr;
}

bool ovl_alloc_tex_set(TerrainIsland* s, OvlState* o, OvlTex& t) {
    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = o->texPool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &o->texDsl;
    if (vkAllocateDescriptorSets(s->device, &dsai, &t.set) != VK_SUCCESS) { t.set = VK_NULL_HANDLE; return false; }
    VkDescriptorImageInfo ii{};
    ii.sampler = o->samp; ii.imageView = t.view; ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet w{};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet = t.set; w.dstBinding = 0;
    w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &ii;
    vkUpdateDescriptorSets(s->device, 1, &w, 0, nullptr);
    return true;
}

// RGBA8 sampled image with a full blit-generated mip chain (copy-once import).
bool ovl_upload_tex(TerrainIsland* s, OvlTex& e, uint32_t w, uint32_t h, const void* bytes, VkDeviceSize byteCount) {
    uint32_t mips = 1;
    { uint32_t m = (w > h) ? w : h; while (m > 1) { m >>= 1; ++mips; } }
    VkBuffer stg = VK_NULL_HANDLE; VmaAllocation stgAlloc = VK_NULL_HANDLE;
    if (!make_buffer(s, byteCount, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &stg, &stgAlloc, true)) return false;
    { void* p = nullptr;
      if (vmaMapMemory(s->allocator, stgAlloc, &p) != VK_SUCCESS) { vmaDestroyBuffer(s->allocator, stg, stgAlloc); return false; }
      std::memcpy(p, bytes, (size_t)byteCount); vmaUnmapMemory(s->allocator, stgAlloc); }

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {w, h, 1}; ici.mipLevels = mips; ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo aci{}; aci.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(s->allocator, &ici, &aci, &e.img, &e.alloc, nullptr) != VK_SUCCESS) {
        vmaDestroyBuffer(s->allocator, stg, stgAlloc); return false;
    }
    VkCommandBuffer cb = begin_oneshot(s);
    if (cb == VK_NULL_HANDLE) { vmaDestroyBuffer(s->allocator, stg, stgAlloc); return false; }

    auto barrier = [&](uint32_t baseMip, uint32_t count, VkImageLayout oldL, VkImageLayout newL,
                       VkAccessFlags srcA, VkAccessFlags dstA, VkPipelineStageFlags srcS, VkPipelineStageFlags dstS) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = oldL; b.newLayout = newL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = e.img;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, baseMip, count, 0, 1};
        b.srcAccessMask = srcA; b.dstAccessMask = dstA;
        vkCmdPipelineBarrier(cb, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    barrier(0, mips, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy rgn{};
    rgn.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rgn.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cb, stg, e.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rgn);
    int32_t mw = (int32_t)w, mh = (int32_t)h;
    for (uint32_t i = 1; i < mips; ++i) {
        barrier(i - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        const int32_t nw = (mw > 1) ? mw / 2 : 1, nh = (mh > 1) ? mh / 2 : 1;
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1};
        blit.srcOffsets[1] = {mw, mh, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
        blit.dstOffsets[1] = {nw, nh, 1};
        vkCmdBlitImage(cb, e.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, e.img,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        mw = nw; mh = nh;
    }
    if (mips > 1)
        barrier(0, mips - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    barrier(mips - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    end_oneshot(s, cb);
    vmaDestroyBuffer(s->allocator, stg, stgAlloc);

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = e.img; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
    return vkCreateImageView(s->device, &vci, nullptr, &e.view) == VK_SUCCESS;
}

// Resolve (importing copy-once on first sight) the texture for a gos handle. nullptr = not
// resident / cannot import (caller skips the draw this frame). 0 / 0xffffffff = untextured.
OvlTex* ovl_get_tex(TerrainIsland* s, OvlState* o, uint32_t gosHandle) {
    if (gosHandle == 0 || gosHandle == 0xffffffffu) return &o->dummy;
    const uint32_t glTex = gos_GetGLTextureId(gosHandle);
    if (glTex == 0) return nullptr;
    auto it = o->texs.find(gosHandle);
    if (it != o->texs.end()) {
        if (it->second.glTex == glTex) return &it->second;
        // The GL texture behind this handle changed (reloaded): re-import.
        vkDeviceWaitIdle(s->device);
        ovl_destroy_tex(s, it->second);
        o->texs.erase(it);
        // (the old descriptor set leaks in the pool until the next mission reset; rare)
    }
    if (o->texs.size() >= kOvlMaxTex) return nullptr;

    GLint prevTex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glBindTexture(GL_TEXTURE_2D, (GLuint)glTex);
    GLint w = 0, h = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) {
        glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
        return nullptr;
    }
    std::vector<uint8_t> blob((size_t)w * (size_t)h * 4u);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, blob.data());
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);

    OvlTex t;
    t.glTex = glTex;
    if (!ovl_upload_tex(s, t, (uint32_t)w, (uint32_t)h, blob.data(), (VkDeviceSize)blob.size()) ||
        !ovl_alloc_tex_set(s, o, t)) {
        ovl_destroy_tex(s, t);
        return nullptr;
    }
    vlog("[VK_TERRAIN_OVERLAY] texture cached: handle=%u glTex=%u %dx%d (%zu/%u).",
         gosHandle, glTex, (int)w, (int)h, o->texs.size() + 1, kOvlMaxTex);
    return &o->texs.emplace(gosHandle, t).first->second;
}

bool ovl_init(TerrainIsland* s, OvlState* o, int w, int h) {
    o->owner = s; o->w = w; o->h = h;
    VkImageResource* colorRes  = s->colorRes;
    VkImageResource* normalRes = s->normalRes;
    VkImageResource* depthRes  = s->depthRes;
    if (!colorRes || !normalRes || !depthRes) return false;

    {   // LOAD-preserve render pass + framebuffer over the 3 shared views (same as legacy island)
        VkAttachmentDescription att[3]{};
        att[0].format = colorRes->format;  att[0].samples = VK_SAMPLE_COUNT_1_BIT;
        att[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; att[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[1].format = normalRes->format; att[1].samples = VK_SAMPLE_COUNT_1_BIT;
        att[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[1].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; att[1].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[2].format = depthRes->format;  att[2].samples = VK_SAMPLE_COUNT_1_BIT;
        att[2].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[2].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL; att[2].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference colorRefs[2] = {
            {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL} };
        VkAttachmentReference depthRef{2, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 2; sub.pColorAttachments = colorRefs;
        sub.pDepthStencilAttachment = &depthRef;
        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL; deps[0].dstSubpass = 0;
        deps[0].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[1].srcSubpass = 0; deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[1].dstStageMask  = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT; deps[1].dstAccessMask = 0;
        VkRenderPassCreateInfo rpci{};
        rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpci.attachmentCount = 3; rpci.pAttachments = att;
        rpci.subpassCount = 1; rpci.pSubpasses = &sub;
        rpci.dependencyCount = 2; rpci.pDependencies = deps;
        if (vkCreateRenderPass(s->device, &rpci, nullptr, &o->rpass) != VK_SUCCESS) return false;
        VkImageView fbViews[3] = { colorRes->view, normalRes->view, depthRes->view };
        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = o->rpass; fci.attachmentCount = 3; fci.pAttachments = fbViews;
        fci.width = (uint32_t)w; fci.height = (uint32_t)h; fci.layers = 1;
        if (vkCreateFramebuffer(s->device, &fci, nullptr, &o->fb) != VK_SUCCESS) return false;
    }

    {   // texture set layout / pool / sampler / dummy white
        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.minLod = 0.0f; sci.maxLod = 16.0f;
        if (vkCreateSampler(s->device, &sci, nullptr, &o->samp) != VK_SUCCESS) return false;
        VkDescriptorSetLayoutBinding b{};
        b.binding = 0; b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1; b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = 1; lci.pBindings = &b;
        if (vkCreateDescriptorSetLayout(s->device, &lci, nullptr, &o->texDsl) != VK_SUCCESS) return false;
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kOvlMaxTex + 2};
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets = kOvlMaxTex + 2; pci.poolSizeCount = 1; pci.pPoolSizes = &ps;
        if (vkCreateDescriptorPool(s->device, &pci, nullptr, &o->texPool) != VK_SUCCESS) return false;
        const uint8_t px[4] = {255, 255, 255, 255};
        if (!ovl_upload_tex(s, o->dummy, 1, 1, px, sizeof(px)) || !ovl_alloc_tex_set(s, o, o->dummy)) return false;
    }

    {   // command buffers (own pair, from the island's pool)
        VkCommandBufferAllocateInfo cbi{};
        cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbi.commandPool = s->cpool; cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount = 2;
        if (vkAllocateCommandBuffers(s->device, &cbi, o->cbuf) != VK_SUCCESS) return false;
    }

    {   // pipelines
        const char* vn[2] = {"terrain_overlay_tier1.vert.spv", "terrain_decal_tier1.vert.spv"};
        const char* fn[2] = {"terrain_overlay_tier1.frag.spv", "terrain_decal_tier1.frag.spv"};
        for (int k = 0; k < 2; ++k) {
            o->vert[k] = load_spv(s->device, spv_dir() + vn[k]);
            o->frag[k] = load_spv(s->device, spv_dir() + fn[k]);
            if (o->vert[k] == VK_NULL_HANDLE || o->frag[k] == VK_NULL_HANDLE) return false;
        }
        VkDescriptorSetLayout sets[2] = { s->dsl, o->texDsl };
        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT; pcr.offset = 0; pcr.size = sizeof(OvlPC);
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 2; plci.pSetLayouts = sets;
        plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
        if (vkCreatePipelineLayout(s->device, &plci, nullptr, &o->playout) != VK_SUCCESS) return false;

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
            stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = o->vert[k]; stages[0].pName = "main";
            stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = o->frag[k]; stages[1].pName = "main";
            VkGraphicsPipelineCreateInfo gp{};
            gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            gp.stageCount = 2; gp.pStages = stages;
            gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia;
            gp.pViewportState = &vps; gp.pRasterizationState = &rs;
            gp.pMultisampleState = &ms; gp.pDepthStencilState = &ds; gp.pColorBlendState = &cb;
            gp.pDynamicState = &dsci;
            gp.layout = o->playout; gp.renderPass = o->rpass; gp.subpass = 0;
            if (vkCreateGraphicsPipelines(s->device, VK_NULL_HANDLE, 1, &gp, nullptr, &o->pipe[k]) != VK_SUCCESS) return false;
        }
    }
    for (int i = 0; i < 2; ++i) {
        o->vtxCap[i] = 256 * 1024;
        if (!make_buffer(s, o->vtxCap[i], VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &o->vtx[i], &o->vtxAlloc[i], true)) return false;
    }
    o->inited = true;
    vlog("[VK_TERRAIN_OVERLAY] island initialized %dx%d (cement/road overlays + decals -> shared GBuffer; GEQUAL reverse-Z).", w, h);
    return true;
}

}  // namespace

extern "C" int mc2_vk_overlay_enabled() {
    return (terrainTier1Requested() && !g_terrainSessionDeclined && !g_ovlDeclined) ? 1 : 0;
}

extern "C" void mc2_vk_overlay_submit(uint32_t kind, const void* verts, uint32_t vertCount,
                                      const Mc2VkOverlayDraw* draws, uint32_t drawCount, uint64_t staticVersion) {
    if (!verts || vertCount == 0 || !draws || drawCount == 0) return;
    OvlPending p;
    p.kind = (kind != 0) ? 1u : 0u;
    p.vertCount = vertCount;
    p.draws.assign(draws, draws + drawCount);
    if (staticVersion != 0) {
        p.isStatic = true; p.version = staticVersion;
        p.staticPtr = static_cast<const uint8_t*>(verts);
    } else {
        p.verts.assign(static_cast<const uint8_t*>(verts),
                       static_cast<const uint8_t*>(verts) + (size_t)vertCount * kOvlVertStride);
    }
    g_ovlPending.push_back(std::move(p));
}

extern "C" int mc2_vk_overlay_run(int w, int h) {
    ZoneScopedN("VK-terrain-overlay");
    std::vector<OvlPending> pending;
    pending.swap(g_ovlPending);                       // always consume
    if (pending.empty() || !mc2_vk_overlay_enabled() || w <= 0 || h <= 0) return 0;
    TerrainIsland* s = g_island;
    if (!s || !s->inited || s->disabled) return 0;    // terrain island not up: nothing to share state with

    if (g_ovl && g_ovl->owner == s && (g_ovl->w != w || g_ovl->h != h)) ovl_destroy(s);
    if (!g_ovl) {
        g_ovl = new OvlState();
        if (!ovl_init(s, g_ovl, w, h)) {
            vlog("[VK_TERRAIN_OVERLAY] init failed -- overlays/decals will not draw under VK (permanent for this session).");
            g_ovlDeclined = true;
            ovl_destroy(s);
            return 0;
        }
    }
    OvlState* o = g_ovl;
    if (o->disabled) return 0;

    Mc2TerrainTier1Feed feed{};
    if (!mc2_terrain_tier1_get_frame_feed(&feed) || !feed.patches || feed.patchCount == 0) return 0;

    // ---- resolve textures (GL calls) and size the vertex streams BEFORE recording ----
    struct Item { uint32_t first; uint32_t count; uint32_t kind; OvlTex* tex; };   // kind: bit0 decal, bit1 static stream
    std::vector<Item> items;
    VkDeviceSize dynBytes = 0;
    for (const OvlPending& p : pending) if (!p.isStatic) dynBytes += (VkDeviceSize)p.vertCount * kOvlVertStride;

    // Persistent static bake: (re)upload when its version changes (device-idle: rare, mission load).
    for (const OvlPending& p : pending) {
        if (!p.isStatic) continue;
        const VkDeviceSize bytes = (VkDeviceSize)p.vertCount * kOvlVertStride;
        if (o->statVersion == p.version && o->stat != VK_NULL_HANDLE && bytes <= o->statCap) continue;
        vkDeviceWaitIdle(s->device);
        if (o->stat != VK_NULL_HANDLE) { vmaDestroyBuffer(s->allocator, o->stat, o->statAlloc); o->stat = VK_NULL_HANDLE; }
        o->statCap = bytes + 256;
        if (!make_buffer(s, o->statCap, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &o->stat, &o->statAlloc, true)) {
            o->disabled = true; return 0;
        }
        void* dst = nullptr;
        if (vmaMapMemory(s->allocator, o->statAlloc, &dst) != VK_SUCCESS) { o->disabled = true; return 0; }
        std::memcpy(dst, p.staticPtr, (size_t)bytes);
        vmaUnmapMemory(s->allocator, o->statAlloc);
        o->statVersion = p.version;
    }
    for (int i = 0; i < 2; ++i) {
        if (dynBytes > o->vtxCap[i]) {
            vkDeviceWaitIdle(s->device);
            vmaDestroyBuffer(s->allocator, o->vtx[i], o->vtxAlloc[i]);
            o->vtx[i] = VK_NULL_HANDLE;
            o->vtxCap[i] = dynBytes + dynBytes / 2;
            if (!make_buffer(s, o->vtxCap[i], VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &o->vtx[i], &o->vtxAlloc[i], true)) {
                o->disabled = true; return 0;
            }
        }
    }
    // Draw list in submission order; dynamic verts packed back to back.
    VkDeviceSize dynOff = 0;
    std::vector<std::pair<const OvlPending*, uint32_t>> dynRuns;   // (pending, first vertex in the dyn stream)
    for (const OvlPending& p : pending) {
        uint32_t base = 0;
        if (!p.isStatic) { base = (uint32_t)(dynOff / kOvlVertStride); dynRuns.push_back({&p, base}); dynOff += (VkDeviceSize)p.vertCount * kOvlVertStride; }
        for (const Mc2VkOverlayDraw& d : p.draws) {
            if (d.vertCount == 0 || d.firstVert + d.vertCount > p.vertCount) continue;
            OvlTex* t = ovl_get_tex(s, o, d.texHandle);
            if (!t) continue;                               // texture not resident yet: skip this frame
            items.push_back({ base + d.firstVert, d.vertCount, p.kind | (p.isStatic ? 2u : 0u), t });
        }
    }
    if (items.empty()) return 0;

    Mc2VkOverlayFrame fr{};
    mc2_vk_overlay_fill_frame(&fr);

    // ---- sync: same shared parity-slot fence discipline as the other islands ----
    VkImageResource* colorRes = s->colorRes;
    VkFrameSync fsC{};
    { ZoneScopedN("VKIsl.Overlay.FenceWait"); fsC = vkImageResourceBeginFrame(colorRes); }
    const uint32_t par = fsC.parity & 1u;

    if (!ensure_ubo_capacity(s, par, sizeof(TerrainFrameUBO))) { o->disabled = true; return 0; }
    int32_t vkPoolLights = feed.numPoolLights;
    int32_t vkGridDim    = feed.lightGridDim;
    stage_light_pool(s, par, feed, vkPoolLights, vkGridDim);
    {
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, s->uboAlloc[par], &p) != VK_SUCCESS) { o->disabled = true; return 0; }
        TerrainFrameUBO u{};
        fill_frame_ubo(s, feed, feed.patches[0], vkPoolLights, vkGridDim, u);
        std::memcpy(p, &u, sizeof(u));
        vmaUnmapMemory(s->allocator, s->uboAlloc[par]);
    }
    if (!dynRuns.empty()) {
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, o->vtxAlloc[par], &p) != VK_SUCCESS) { o->disabled = true; return 0; }
        for (const auto& r : dynRuns)
            std::memcpy((uint8_t*)p + (size_t)r.second * kOvlVertStride, r.first->verts.data(),
                        (size_t)r.first->vertCount * kOvlVertStride);
        vmaUnmapMemory(s->allocator, o->vtxAlloc[par]);
    }

    VkCommandBuffer cb = o->cbuf[par];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) { o->disabled = true; return 0; }
    VkRenderPassBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = o->rpass; rbi.framebuffer = o->fb;
    rbi.renderArea = {{0, 0}, {(uint32_t)w, (uint32_t)h}};
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {(uint32_t)w, (uint32_t)h}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &scissor);

    OvlPC pc{};
    for (int k = 0; k < 4; ++k) pc.fogColor[k] = fr.fogColor[k];
    pc.artFlatExempt = fr.artFlatExempt; pc.mapHalfExtent = fr.mapHalfExtent;
    pc.biasLegacy = fr.biasLegacy; pc.cementAdopt = fr.cementAdopt; pc.seamFix = fr.seamFix;
    vkCmdPushConstants(cb, o->playout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    const uint32_t dynOffset0 = 0;
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, o->playout, 0, 1, &s->set[par], 1, &dynOffset0);

    int boundKind = -1; int boundStatic = -1; VkDescriptorSet boundTex = VK_NULL_HANDLE;
    unsigned long drawn[2] = {0, 0}, verts = 0;
    for (const Item& it : items) {
        const int kind = (int)(it.kind & 1u);
        const int isStat = (it.kind & 2u) ? 1 : 0;
        if (kind != boundKind) { vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, o->pipe[kind]); boundKind = kind; }
        if (isStat != boundStatic) {
            VkBuffer vb = isStat ? o->stat : o->vtx[par];
            VkDeviceSize zero = 0;
            vkCmdBindVertexBuffers(cb, 0, 1, &vb, &zero);
            boundStatic = isStat;
        }
        if (it.tex->set != boundTex) {
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, o->playout, 1, 1, &it.tex->set, 0, nullptr);
            boundTex = it.tex->set;
        }
        vkCmdDraw(cb, it.count, 1, it.first, 0);
        ++drawn[kind]; verts += it.count;
    }
    vkCmdEndRenderPass(cb);
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) { o->disabled = true; return 0; }
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    { ZoneScopedN("VKIsl.Overlay.Submit");
      if (vkQueueSubmit(s->queue, 1, &si, fsC.fence) != VK_SUCCESS) { o->disabled = true; return 0; } }

    static bool s_firstOk = false;
    static unsigned long s_frames = 0;
    ++s_frames;
    if (!s_firstOk) {
        vlog("[VK_TERRAIN_OVERLAY] first overlay co-write: overlay_draws=%lu decal_draws=%lu verts=%lu textures=%zu.",
             drawn[0], drawn[1], verts, o->texs.size());
        s_firstOk = true;
    }
    return 1;
}

extern "C" void mc2_vk_overlay_reset() {
    g_ovlPending.clear();
    OvlState* o = g_ovl;
    TerrainIsland* s = g_island;
    if (!o || !s || o->owner != s || !o->inited) return;
    // gos texture handles are reused by the next mission: drop the copy-once cache + static bake.
    vkDeviceWaitIdle(s->device);
    for (auto& kv : o->texs) ovl_destroy_tex(s, kv.second);
    o->texs.clear();
    if (o->stat != VK_NULL_HANDLE) { vmaDestroyBuffer(s->allocator, o->stat, o->statAlloc); o->stat = VK_NULL_HANDLE; }
    o->statCap = 0; o->statVersion = UINT64_MAX;
    if (o->texPool) vkResetDescriptorPool(s->device, o->texPool, 0);
    o->dummy.set = VK_NULL_HANDLE;
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
    if (g_island && g_island->gpuTimer.supported && g_island->gpuTimer.samples > 0) {
        vlog("GPU_TIMESTAMPS samples=%llu gpu_pass_us=%.1f gpu_setup_us=%.1f gpu_draw_us=%.1f "
             "(cpu_draw_us=%.1f incl. record+submit+fence-wait)",
             (unsigned long long)g_island->gpuTimer.samples,
             g_island->gpuTimer.avgTotalUs(),
             g_island->gpuTimer.avgGapUs(1),
             g_island->gpuTimer.avgGapUs(2),
             g_health.drawUs);
    }
    if (!g_island) return;
    g_island->gpuTimer.destroy();
    destroy_island(g_island);
    delete g_island; g_island = nullptr;
}

extern "C" void mc2_vk_terrain_set_native_prefix_clear_color(float r, float g, float b, float a)
{
    g_prefixClearColor[0] = r;
    g_prefixClearColor[1] = g;
    g_prefixClearColor[2] = b;
    g_prefixClearColor[3] = a;
}

#endif // MC2_VULKAN_ISLAND
