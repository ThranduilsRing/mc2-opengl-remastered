// VK terrain island: island buffers, patch / skirt mesh caches, descriptor writes, capacity growth and init / destroy.
// Declared in the class headers; shared names in vulkan_terrain_island_internal.h.
#include "vulkan_terrain_island_internal.h"

#ifdef MC2_VULKAN_ISLAND


namespace vk_terrain_detail {

bool make_buffer(TerrainIsland* s, VkDeviceSize size, VkBufferUsageFlags usage,
                 VkBuffer* buf, VmaAllocation* alloc, bool hostVisible,
                 std::string_view name, int i0, int i1) {
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size ? size : 16; bci.usage = usage;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = hostVisible ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT : 0;
    if (vmaCreateBuffer(s->allocator, &bci, &aci, buf, alloc, nullptr) != VK_SUCCESS) {
        vlog("vmaCreateBuffer failed."); return false;
    }
    mc2vk::nameObjectAt(s->device, *buf, name, i0, i1);
    return true;
}

// Build (or fetch cached) a patch grid for (qcX,qcY,lodStep). qcX/qcY are the
// max-offset SPAN in map-cells (feed.quadCountX/Y, already rounded down to a multiple
// of lodStep by the GL capture site); lodStep strides the local-offset grid the same
// way GL's makeSamplePositions() does (`p += lodStep`, p in [0,qc]) so the vert
// shader's un-scaled `blockOriginX + localOffset.x` map-space math is IDENTICAL
// whether lodStep==1 (LOD0) or >1 (coarse band, VK-TERRAIN-ALL-LODS rung). The CCW
// two-triangles-per-quad split is the FIXED (TL-BR) diagonal (checkerDiag is a
// separate default-off feature not ported to the probe), matching getOrBuildPatch()'s
// bottomRight default.
const PatchMesh* getOrBuildPatch(TerrainIsland* s, int qcX, int qcY, int lodStep) {
    if (lodStep <= 0) lodStep = 1;
    const uint32_t key = ((uint32_t)(qcX & 0xFFFF)) | (((uint32_t)(qcY & 0xFFFF)) << 16)
                        ^ ((uint32_t)(lodStep & 0xFF) << 24);
    auto it = s->patchCache.find(key);
    if (it != s->patchCache.end()) return &it->second;
    if (qcX <= 0 || qcY <= 0) return nullptr;

    // Sample positions: 0, lodStep, 2*lodStep, ... up to qc (mirrors GL
    // makeSamplePositions(quadCount, lodStep)). nx/ny = number of grid POINTS.
    std::vector<int> xs, ys;
    for (int p = 0; p <= qcX; p += lodStep) xs.push_back(p);
    if (xs.empty() || xs.back() != qcX) xs.push_back(qcX);
    for (int p = 0; p <= qcY; p += lodStep) ys.push_back(p);
    if (ys.empty() || ys.back() != qcY) ys.push_back(qcY);
    const int nx = (int)xs.size(), ny = (int)ys.size();

    // Vertices: nx x ny grid of int16 local offsets (map-cell units, lodStep-strided).
    std::vector<LocalVertex> verts;
    verts.reserve((size_t)nx * ny);
    for (int yy = 0; yy < ny; ++yy)
        for (int xx = 0; xx < nx; ++xx)
            verts.push_back({(int16_t)xs[xx], (int16_t)ys[yy]});

    // Indices: two CCW triangles per quad, FIXED TL-BR diagonal (== getOrBuildPatch
    // bottomRight default): {TL,BL,BR} + {TL,BR,TR}.
    std::vector<uint16_t> indices;
    const int W = nx;
    for (int j = 0; j < ny - 1; ++j) {
        for (int i = 0; i < nx - 1; ++i) {
            uint16_t tl = (uint16_t)(j * W + i);
            uint16_t tr = (uint16_t)(j * W + i + 1);
            uint16_t bl = (uint16_t)((j + 1) * W + i);
            uint16_t br = (uint16_t)((j + 1) * W + i + 1);
            indices.insert(indices.end(), {tl, bl, br, tl, br, tr});
        }
    }

    PatchMesh pm;
    pm.indexCount = (uint32_t)indices.size();
    const VkDeviceSize vboBytes = verts.size() * sizeof(LocalVertex);
    const VkDeviceSize iboBytes = indices.size() * sizeof(uint16_t);
    if (!make_buffer(s, vboBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &pm.vbo, &pm.vboAlloc, true, "terrain.patchMesh.vbo")) return nullptr;
    if (!make_buffer(s, iboBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,  &pm.ibo, &pm.iboAlloc, true, "terrain.patchMesh.ibo")) return nullptr;
    { void* p = nullptr;
      if (vmaMapMemory(s->allocator, pm.vboAlloc, &p) == VK_SUCCESS) {
          std::memcpy(p, verts.data(), (size_t)vboBytes); vmaUnmapMemory(s->allocator, pm.vboAlloc); } }
    { void* p = nullptr;
      if (vmaMapMemory(s->allocator, pm.iboAlloc, &p) == VK_SUCCESS) {
          std::memcpy(p, indices.data(), (size_t)iboBytes); vmaUnmapMemory(s->allocator, pm.iboAlloc); } }

    auto res = s->patchCache.emplace(key, pm);
    return &res.first->second;
}

// VK-TERRAIN-SKIRT (rung 4): build (or fetch cached) a skirt-strip mesh for
// (qcX,qcY,lodStep), keyed the SAME way as getOrBuildPatch. Reproduces GL's skirt
// builder BIT-FOR-BIT (gos_terrain_lod_chunk.cpp:459-500 buildEdge lambda + the
// N,S,W,E per-edge offset/count bookkeeping) using the identical xs/ys sample-position
// construction getOrBuildPatch uses (mirrors GL's makeSamplePositions(quadCount,lodStep)
// call sharing the same coarse-line sample points as the main patch, so skirt verts
// land exactly on the main patch's edge verts -- no seam between skirt-top and
// main-patch-edge).
const SkirtMesh* getOrBuildSkirtMesh(TerrainIsland* s, int qcX, int qcY, int lodStep) {
    if (lodStep <= 0) lodStep = 1;
    const uint32_t key = ((uint32_t)(qcX & 0xFFFF)) | (((uint32_t)(qcY & 0xFFFF)) << 16)
                        ^ ((uint32_t)(lodStep & 0xFF) << 24);
    auto it = s->skirtCache.find(key);
    if (it != s->skirtCache.end()) return &it->second;
    if (qcX <= 0 || qcY <= 0) return nullptr;

    std::vector<int> xs, ys;
    for (int p = 0; p <= qcX; p += lodStep) xs.push_back(p);
    if (xs.empty() || xs.back() != qcX) xs.push_back(qcX);
    for (int p = 0; p <= qcY; p += lodStep) ys.push_back(p);
    if (ys.empty() || ys.back() != qcY) ys.push_back(qcY);

    std::vector<SkirtVertex> skirtVerts;
    std::vector<uint16_t>    skirtIdx;

    // Mirrors GL's buildEdge lambda exactly (gos_terrain_lod_chunk.cpp:477-494).
    auto buildEdge = [&](const std::vector<int>& uPos, int fixedCoord, bool fixedIsY) {
        int base = (int)skirtVerts.size();
        int n    = (int)uPos.size();
        for (int i = 0; i < n; ++i) {
            int16_t lx = fixedIsY ? (int16_t)uPos[i] : (int16_t)fixedCoord;
            int16_t ly = fixedIsY ? (int16_t)fixedCoord : (int16_t)uPos[i];
            skirtVerts.push_back({lx, ly, 0, 0});  // surface
            skirtVerts.push_back({lx, ly, 1, 0});  // below
        }
        for (int i = 0; i < n - 1; ++i) {
            uint16_t t0 = (uint16_t)(base + 2*i);
            uint16_t b0 = (uint16_t)(base + 2*i + 1);
            uint16_t t1 = (uint16_t)(base + 2*(i+1));
            uint16_t b1 = (uint16_t)(base + 2*(i+1) + 1);
            skirtIdx.insert(skirtIdx.end(), {t0, b0, b1, t0, b1, t1});
        }
    };

    SkirtMesh sm;
    sm.edgeOffset[0] = (uint32_t)skirtIdx.size(); buildEdge(xs, (int)ys.front(), false); sm.edgeCount[0] = (uint32_t)skirtIdx.size() - sm.edgeOffset[0]; // North
    sm.edgeOffset[1] = (uint32_t)skirtIdx.size(); buildEdge(xs, (int)ys.back(),  false); sm.edgeCount[1] = (uint32_t)skirtIdx.size() - sm.edgeOffset[1]; // South
    sm.edgeOffset[2] = (uint32_t)skirtIdx.size(); buildEdge(ys, (int)xs.front(), true ); sm.edgeCount[2] = (uint32_t)skirtIdx.size() - sm.edgeOffset[2]; // West
    sm.edgeOffset[3] = (uint32_t)skirtIdx.size(); buildEdge(ys, (int)xs.back(),  true ); sm.edgeCount[3] = (uint32_t)skirtIdx.size() - sm.edgeOffset[3]; // East

    if (skirtVerts.empty() || skirtIdx.empty()) return nullptr;

    const VkDeviceSize vboBytes = skirtVerts.size() * sizeof(SkirtVertex);
    const VkDeviceSize iboBytes = skirtIdx.size() * sizeof(uint16_t);
    if (!make_buffer(s, vboBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &sm.vbo, &sm.vboAlloc, true, "terrain.skirtMesh.vbo")) return nullptr;
    if (!make_buffer(s, iboBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,  &sm.ibo, &sm.iboAlloc, true, "terrain.skirtMesh.ibo")) return nullptr;
    { void* p = nullptr;
      if (vmaMapMemory(s->allocator, sm.vboAlloc, &p) == VK_SUCCESS) {
          std::memcpy(p, skirtVerts.data(), (size_t)vboBytes); vmaUnmapMemory(s->allocator, sm.vboAlloc); } }
    { void* p = nullptr;
      if (vmaMapMemory(s->allocator, sm.iboAlloc, &p) == VK_SUCCESS) {
          std::memcpy(p, skirtIdx.data(), (size_t)iboBytes); vmaUnmapMemory(s->allocator, sm.iboAlloc); } }

    auto res = s->skirtCache.emplace(key, sm);
    return &res.first->second;
}

void write_descriptors(TerrainIsland* s) {
    // VK-TERRAIN-FENCE-DEFER-1: write BOTH parity sets (set[i] binds ubo[i]; every
    // other binding is identical single-instance state). Callers must guarantee no
    // in-flight command buffer can be using either set during the update: buffer-grow
    // callers vkDeviceWaitIdle first, copy-once importers run right after an inline
    // one-shot fence wait (which retires ALL earlier submission-order work on the
    // queue), and the per-frame bound-view switch in run() device-idles (rare).
    for (int i = 0; i < 2; ++i) {
    if (s->set[i] == VK_NULL_HANDLE) continue;
    // binding 0 = dynamic UBO (per-patch frame block); binding 1 = height SSBO;
    // binding 2 = colormap combined-image-sampler (COLORMAP-BASE, only when live).
    // binding 3 = cement combined-image-sampler (dummy/real); binding 4 = cement-word SSBO.
    VkDescriptorBufferInfo uboInfo{ s->ubo[i], 0, sizeof(TerrainFrameUBO) }; // range = one slot
    VkDescriptorBufferInfo hInfo{ s->heightBuf, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w[14]{};
    uint32_t n = 0;
    w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = s->set[i];
    w[n].dstBinding = 0; w[n].descriptorCount = 1;
    w[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; w[n].pBufferInfo = &uboInfo; ++n;
    w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = s->set[i];
    w[n].dstBinding = 1; w[n].descriptorCount = 1;
    w[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[n].pBufferInfo = &hInfo; ++n;
    VkDescriptorImageInfo cmInfo{};
    if (s->colormapReady && s->colormapView != VK_NULL_HANDLE && s->colormapSamp != VK_NULL_HANDLE) {
        cmInfo.sampler = s->colormapSamp; cmInfo.imageView = s->colormapView;
        cmInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = s->set[i];
        w[n].dstBinding = 2; w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[n].pImageInfo = &cmInfo; ++n;
    }
    // CEMENT: binding 3 sampler — always written (dummy 1x1 until the real atlas imports).
    VkDescriptorImageInfo ceInfo{};
    if (s->cementView != VK_NULL_HANDLE && s->cementSamp != VK_NULL_HANDLE) {
        ceInfo.sampler = s->cementSamp; ceInfo.imageView = s->cementView;
        ceInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = s->set[i];
        w[n].dstBinding = 3; w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[n].pImageInfo = &ceInfo; ++n;
    }
    // CEMENT: binding 4 cement-word SSBO — always written (dummy 16B until first snapshot).
    VkDescriptorBufferInfo ceBuf{};
    if (s->cementBuf != VK_NULL_HANDLE) {
        ceBuf.buffer = s->cementBuf; ceBuf.offset = 0; ceBuf.range = VK_WHOLE_SIZE;
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = s->set[i];
        w[n].dstBinding = 4; w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[n].pBufferInfo = &ceBuf; ++n;
    }
    // VK-TERRAIN-LIGHTING: binding 5 static shadow sampler2DShadow — always written once
    // the (dummy or real) depth-compare image + sampler exist (uT.enableShadows gates use).
    VkDescriptorImageInfo shInfo{};
    if (s->shadowView != VK_NULL_HANDLE && s->shadowSamp != VK_NULL_HANDLE) {
        shInfo.sampler = s->shadowSamp; shInfo.imageView = s->shadowView;
        shInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = s->set[i];
        w[n].dstBinding = 5; w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[n].pImageInfo = &shInfo; ++n;
    }
    // VK-TERRAIN-DETAIL: binding 6 material detail-normal sampler2DArray — always written
    // once the (dummy 1x1x1 or real) array image + sampler exist (uT.detailReady gates use).
    VkDescriptorImageInfo mnInfo{};
    if (s->matNormalView != VK_NULL_HANDLE && s->matNormalSamp != VK_NULL_HANDLE) {
        mnInfo.sampler = s->matNormalSamp; mnInfo.imageView = s->matNormalView;
        mnInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = s->set[i];
        w[n].dstBinding = 6; w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[n].pImageInfo = &mnInfo; ++n;
    }
    // VK-TERRAIN-DYNSHADOW: binding 7 dynamic shadow sampler2DShadow — always written once
    // the (dummy or real) depth-compare image exists (uT.enableDynShadows gates use).
    // VK-SHADOW-DIRECT-CONSUME-1: prefer the shadow island's own view (zero-copy) when
    // it produced+fenced this frame; falls back to this island's own copy-updated image
    // (dynShadowView) otherwise. Layout is DEPTH_STENCIL_READ_ONLY_OPTIMAL on the direct
    // path (see vulkan_shadow_island_live.cpp render-pass finalLayout) which is a valid
    // sampled-read layout for a combined-image-sampler descriptor.
    VkDescriptorImageInfo dshInfo{};
    const VkImageView dshView = (s->dynShadowExternalView != VK_NULL_HANDLE)
        ? s->dynShadowExternalView : s->dynShadowView;
    if (dshView != VK_NULL_HANDLE && s->shadowSamp != VK_NULL_HANDLE) {
        dshInfo.sampler = s->shadowSamp; dshInfo.imageView = dshView;
        dshInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = s->set[i];
        w[n].dstBinding = 7; w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[n].pImageInfo = &dshInfo; ++n;
    }
    // VK-TERRAIN-CSM-NEAR-1: binding 8 near-cascade array, binding 9 full-map cascade —
    // always written once the (dummy or real) images + shared shadowSamp exist.
    VkDescriptorImageInfo csmArrInfo{};
    const VkImageView csmArrView = (s->csmArrayExternalView != VK_NULL_HANDLE)
        ? s->csmArrayExternalView : s->csmArrayView;
    if (csmArrView != VK_NULL_HANDLE && s->shadowSamp != VK_NULL_HANDLE) {
        csmArrInfo.sampler = s->shadowSamp; csmArrInfo.imageView = csmArrView;
        csmArrInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = s->set[i];
        w[n].dstBinding = 8; w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[n].pImageInfo = &csmArrInfo; ++n;
    }
    VkDescriptorImageInfo csmFmInfo{};
    const VkImageView csmFmView = (s->csmFullMapExternalView != VK_NULL_HANDLE)
        ? s->csmFullMapExternalView : s->csmFullMapView;
    if (csmFmView != VK_NULL_HANDLE && s->shadowSamp != VK_NULL_HANDLE) {
        csmFmInfo.sampler = s->shadowSamp; csmFmInfo.imageView = csmFmView;
        csmFmInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = s->set[i];
        w[n].dstBinding = 9; w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[n].pImageInfo = &csmFmInfo; ++n;
    }
    // VK-TERRAIN-VISUALDISPLACE-FAR-1: binding 10 visual-height (fine bake) SSBO —
    // always written (dummy 16B until the first bake snapshot; uT.visualMode==0 gates use).
    VkDescriptorBufferInfo vhBuf{};
    if (s->visualHeightBuf != VK_NULL_HANDLE) {
        vhBuf.buffer = s->visualHeightBuf; vhBuf.offset = 0; vhBuf.range = VK_WHOLE_SIZE;
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = s->set[i];
        w[n].dstBinding = 10; w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[n].pBufferInfo = &vhBuf; ++n;
    }
    // VK-PARITY-TERRAIN-2: bindings 11/12/13 light pool / grid header / grid indices
    // (per-parity buffers; always valid once created, uT.numPoolLights gates use).
    VkDescriptorBufferInfo lpInfo[3]{};
    for (int k = 0; k < 3; ++k) {
        if (s->lpBuf[i][k] == VK_NULL_HANDLE) continue;
        lpInfo[k].buffer = s->lpBuf[i][k]; lpInfo[k].offset = 0; lpInfo[k].range = VK_WHOLE_SIZE;
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = s->set[i];
        w[n].dstBinding = 11 + k; w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[n].pBufferInfo = &lpInfo[k]; ++n;
    }
    vkUpdateDescriptorSets(s->device, n, w, 0, nullptr);
    }   // per-parity set loop
}

bool ensure_height_capacity(TerrainIsland* s, VkDeviceSize bytes) {
    if (s->heightBuf != VK_NULL_HANDLE && bytes <= s->heightCapBytes) return true;
    if (s->heightBuf != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(s->device);
        vmaDestroyBuffer(s->allocator, s->heightBuf, s->heightAlloc);
        s->heightBuf = VK_NULL_HANDLE;
    }
    VkDeviceSize cap = bytes + bytes / 4 + 4096;
    if (!make_buffer(s, cap, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &s->heightBuf, &s->heightAlloc, true, "terrain.heightBuf"))
        return false;
    s->heightCapBytes = cap;
    s->heightVersionCopied = UINT64_MAX; // force re-snapshot into the new buffer
    write_descriptors(s);
    return true;
}

// Grow slot `par`'s per-patch dynamic UBO (VK-TERRAIN-FENCE-DEFER-1: per parity slot).
bool ensure_ubo_capacity(TerrainIsland* s, uint32_t par, VkDeviceSize bytes) {
    if (s->ubo[par] != VK_NULL_HANDLE && bytes <= s->uboCapBytes[par]) return true;
    if (s->ubo[par] != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(s->device);
        vmaDestroyBuffer(s->allocator, s->ubo[par], s->uboAlloc[par]);
        s->ubo[par] = VK_NULL_HANDLE;
    }
    VkDeviceSize cap = bytes + bytes / 2 + sizeof(TerrainFrameUBO);
    if (!make_buffer(s, cap, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &s->ubo[par], &s->uboAlloc[par], true, "terrain.ubo", static_cast<int>(par)))
        return false;
    s->uboCapBytes[par] = cap;
    write_descriptors(s);
    return true;
}

// VK-PARITY-TERRAIN-2: grow slot `par`'s light buffer k (0 pool, 1 grid header,
// 2 grid indices). Same grow discipline as ensure_ubo_capacity (device idle, rewrite
// both sets). Initial sizes cover the defaults (256-light cap, 64x64 grid) so the
// regrow path only runs for MC2_LIGHT_GRID_DIM overrides / unusually dense stamps.
bool ensure_lightpool_capacity(TerrainIsland* s, uint32_t par, int k, VkDeviceSize bytes) {
    if (s->lpBuf[par][k] != VK_NULL_HANDLE && bytes <= s->lpCap[par][k]) return true;
    if (s->lpBuf[par][k] != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(s->device);
        vmaDestroyBuffer(s->allocator, s->lpBuf[par][k], s->lpAlloc[par][k]);
        s->lpBuf[par][k] = VK_NULL_HANDLE;
    }
    VkDeviceSize cap = bytes + bytes / 2 + 256;
    if (!make_buffer(s, cap, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &s->lpBuf[par][k], &s->lpAlloc[par][k], true, "terrain.lpBuf", static_cast<int>(par), static_cast<int>(k)))
        return false;
    s->lpCap[par][k] = cap;
    write_descriptors(s);
    return true;
}

void ovl_destroy(TerrainIsland* s);   // VK-PARITY-OVERLAY-1 (defined with the overlay block below)

void destroy_island(TerrainIsland* s) {
    if (!s) return;
    VkDevice d = s->device;
    if (d != VK_NULL_HANDLE) vkDeviceWaitIdle(d);
    ovl_destroy(s);   // VK-PARITY-OVERLAY-1: overlay objects depend on s->dsl / shared views
    for (auto& kv : s->patchCache) {
        if (kv.second.vbo) vmaDestroyBuffer(s->allocator, kv.second.vbo, kv.second.vboAlloc);
        if (kv.second.ibo) vmaDestroyBuffer(s->allocator, kv.second.ibo, kv.second.iboAlloc);
    }
    s->patchCache.clear();
    // VK-TERRAIN-SKIRT (rung 4): mirror the main-patch teardown for skirt resources.
    for (auto& kv : s->skirtCache) {
        if (kv.second.vbo) vmaDestroyBuffer(s->allocator, kv.second.vbo, kv.second.vboAlloc);
        if (kv.second.ibo) vmaDestroyBuffer(s->allocator, kv.second.ibo, kv.second.iboAlloc);
    }
    s->skirtCache.clear();
    if (s->skirtPipe) vkDestroyPipeline(d, s->skirtPipe, nullptr);
    if (s->skirtVert) vkDestroyShaderModule(d, s->skirtVert, nullptr);
    if (s->skirtFrag) vkDestroyShaderModule(d, s->skirtFrag, nullptr);
    if (s->pipe)    vkDestroyPipeline(d, s->pipe, nullptr);
    if (s->playout) vkDestroyPipelineLayout(d, s->playout, nullptr);
    if (s->vert)    vkDestroyShaderModule(d, s->vert, nullptr);
    if (s->frag)    vkDestroyShaderModule(d, s->frag, nullptr);
    if (s->dsl)     vkDestroyDescriptorSetLayout(d, s->dsl, nullptr);
    if (s->pool)    vkDestroyDescriptorPool(d, s->pool, nullptr);
    if (s->fb)      vkDestroyFramebuffer(d, s->fb, nullptr);
    if (s->rpass)   vkDestroyRenderPass(d, s->rpass, nullptr);
    if (s->heightBuf) vmaDestroyBuffer(s->allocator, s->heightBuf, s->heightAlloc);
    if (s->visualHeightBuf) vmaDestroyBuffer(s->allocator, s->visualHeightBuf, s->visualHeightAlloc);
    for (int i = 0; i < 2; ++i)   // VK-TERRAIN-FENCE-DEFER-1: per-parity UBO pair
        if (s->ubo[i]) vmaDestroyBuffer(s->allocator, s->ubo[i], s->uboAlloc[i]);
    for (int i = 0; i < 2; ++i)   // VK-PARITY-TERRAIN-2: per-parity light pool/grid buffers
        for (int k = 0; k < 3; ++k)
            if (s->lpBuf[i][k]) vmaDestroyBuffer(s->allocator, s->lpBuf[i][k], s->lpAlloc[i][k]);
    if (s->colormapSamp) vkDestroySampler(d, s->colormapSamp, nullptr);
    if (s->colormapView) vkDestroyImageView(d, s->colormapView, nullptr);
    if (s->colormapImg)  vmaDestroyImage(s->allocator, s->colormapImg, s->colormapAlloc);
    if (s->cementSamp) vkDestroySampler(d, s->cementSamp, nullptr);
    if (s->cementView) vkDestroyImageView(d, s->cementView, nullptr);
    if (s->cementImg)  vmaDestroyImage(s->allocator, s->cementImg, s->cementAlloc);
    if (s->cementBuf)  vmaDestroyBuffer(s->allocator, s->cementBuf, s->cementBufAlloc);
    if (s->shadowSamp) vkDestroySampler(d, s->shadowSamp, nullptr);
    if (s->shadowView) vkDestroyImageView(d, s->shadowView, nullptr);
    if (s->shadowImg)  vmaDestroyImage(s->allocator, s->shadowImg, s->shadowAlloc);
    if (s->shadowStage) vmaDestroyBuffer(s->allocator, s->shadowStage, s->shadowStageAlloc);
    if (s->dynShadowView) vkDestroyImageView(d, s->dynShadowView, nullptr);
    if (s->dynShadowImg)  vmaDestroyImage(s->allocator, s->dynShadowImg, s->dynShadowAlloc);
    if (s->dynShadowStage) vmaDestroyBuffer(s->allocator, s->dynShadowStage, s->dynShadowStageAlloc);
    if (s->csmArrayView) vkDestroyImageView(d, s->csmArrayView, nullptr);
    if (s->csmArrayImg)  vmaDestroyImage(s->allocator, s->csmArrayImg, s->csmArrayAlloc);
    if (s->csmFullMapView) vkDestroyImageView(d, s->csmFullMapView, nullptr);
    if (s->csmFullMapImg)  vmaDestroyImage(s->allocator, s->csmFullMapImg, s->csmFullMapAlloc);
    if (s->matNormalSamp) vkDestroySampler(d, s->matNormalSamp, nullptr);
    if (s->matNormalView) vkDestroyImageView(d, s->matNormalView, nullptr);
    if (s->matNormalImg)  vmaDestroyImage(s->allocator, s->matNormalImg, s->matNormalAlloc);
    if (s->cpool)   vkDestroyCommandPool(d, s->cpool, nullptr);
    // instance/phys/device/queue/allocator + the shared GBuffer images are BORROWED.
}

// One-time init: fetch shared GBuffer, build LOAD renderpass/framebuffer/pipeline.
bool init_island(TerrainIsland* s, int w, int h) {
    s->w = w; s->h = h;
    ++g_health.attempted;
    g_terrainInitReachedBuild = 0;  // reset; flips to 1 after readiness gates pass

    if (!mc2vk::ensureEngineVulkanContext()) { setFallback("engine_vk_context_birth_failed"); return false; }
    mc2vk::VulkanContext& ctx = mc2vk::getOrCreateEngineVulkanContext();
    if (!ctx.valid()) { setFallback("engine_vk_context_not_valid"); return false; }
    s->instance  = ctx.instance();
    s->phys      = ctx.physicalDevice();
    s->device    = ctx.device();
    s->queue     = ctx.graphicsQueue();
    s->gfxFamily = ctx.graphicsFamily();
    s->allocator = ctx.allocator();
    if (s->device == VK_NULL_HANDLE || s->allocator == VK_NULL_HANDLE) { setFallback("engine_vk_device_or_allocator_null"); return false; }
    g_health.vulkanAvailable = 1;
    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(s->phys, &props);
        std::strncpy(g_health.deviceName, props.deviceName, sizeof(g_health.deviceName) - 1);
        g_health.deviceName[sizeof(g_health.deviceName) - 1] = '\0';
    }

    if (!terrainTier1Requested()) { setFallback("gate_off"); return false; }

    // ---- Fetch the SHARED scene color(MRT0)+normal(MRT1)+depth exportable images the
    // GL scene renders terrain into. Fail-soft: any missing => GL fallback. Same
    // accessors the prop Tier-1 island uses (built by the postprocess createFBOs seam,
    // now triggered for the terrain gate too). ----
    s->colorRes  = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_COLOR0);
    s->normalRes = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_NORMAL);
    s->depthRes  = mc2SharedSceneDepthGetResource();
    // VK-RUNG8-ITEM5: the shared scene is VK-only (no GL interop view). Terrain is
    // the direct-chain first-owner; only the VkImageResources are required.
    if (!s->colorRes || !s->normalRes || !s->depthRes) {
        setFallback("shared_gbuffer_not_ready");
        return false;
    }
    g_health.interopActive = 0;   // item-4: no GL<->VK semaphore handoff arm exists
    g_health.vk2glSemActive = 0;
    // Readiness gates (context + shared GBuffer) passed: any failure past this point is
    // a PERMANENT build failure -> the island declines for the session (item-2).
    g_terrainInitReachedBuild = 1;

    // ---- Shared-GBuffer renderpass. Item-4: terrain is unconditionally the native
    // first owner of color0/normal/depth for the frame — CLEAR loadOp on all three
    // (color0 = gameplay/menu clear color via mc2_vk_terrain_set_native_prefix_clear_color,
    // normal = clearGBuffer1 sentinel, depth = reverse-Z far). The GL frame-entry clear
    // + clearGBuffer1 are skipped in gameosmain.cpp when this island is armed
    // (mc2vkgate::umbrellaOn()). The old LOAD-preserve hybrid renderpass
    // (GL clears + GL->VK semaphore handoff) was deleted.
    {
        VkAttachmentDescription att[3]{};
        att[0].format = s->colorRes->format; att[0].samples = VK_SAMPLE_COUNT_1_BIT;
        att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[0].finalLayout   = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[1].format = s->normalRes->format; att[1].samples = VK_SAMPLE_COUNT_1_BIT;
        att[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[1].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[1].finalLayout   = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        // FAR-BAND-BLACK-1 FIX: depth uses CLEAR, not LOAD. The shared depth image is
        // exported as VK_FORMAT_D32_SFLOAT_S8_UINT (D24_UNORM_S8_UINT export is
        // unsupported on this device -- see vulkan_shared_scene_depth.cpp), while GL's
        // per-frame glClearDepth(0.0f)/glClear(GL_DEPTH_BUFFER_BIT) writes through a
        // GL_DEPTH24_STENCIL8 *reinterpretation* of the SAME bytes. 0.0f happens to be
        // bit-identical (0x00000000) in both encodings, so the GL clear alone would be
        // safe in isolation -- but terrain (this island) is the FIRST GL/VK writer each
        // frame and LOAD_OP_LOAD makes its GEQUAL depth test trust whatever is already
        // in the image at that instant, which depends on GL/VK submission-order
        // interleaving with the shared image's cross-API layout transitions; any
        // non-zero residue read through the wrong format's bit-layout is garbage, and
        // reverse-Z GEQUAL against garbage fails almost everywhere on a hard camera cut
        // (root cause of FAR-BAND-BLACK: overview_center/oblique_horizon divergence).
        // A native VK CLEAR here is unambiguous -- vkCmdClearDepthStencilImage/
        // VK_ATTACHMENT_LOAD_OP_CLEAR writes through the image's OWN declared format
        // (D32_SFLOAT_S8_UINT), so terrain always starts its depth test from a
        // guaranteed-correct reverse-Z-far value, independent of what GL's differently-
        // interpreted clear wrote to the same memory. Terrain runs first in the frame
        // (Terrain::flushDrawCommands() before mcTextureManager->renderLists() --
        // code/gamecam.cpp:530,535), so clearing here does not discard anything real:
        // nothing else has written scene depth yet this frame. STORE_OP_STORE (below)
        // still hands the correct depth downstream to props/mech Tier-1 (LOAD-preserve,
        // unaffected by this change) and to GL's own post-process reads.
        att[2].format = s->depthRes->format; att[2].samples = VK_SAMPLE_COUNT_1_BIT;
        att[2].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[2].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        att[2].finalLayout   = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference colorRefs[2] = {
            {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
            {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        };
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
        deps[1].dstStageMask  = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        deps[1].dstAccessMask = 0;

        VkRenderPassCreateInfo rpci{};
        rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpci.attachmentCount = 3; rpci.pAttachments = att;
        rpci.subpassCount = 1; rpci.pSubpasses = &sub;
        rpci.dependencyCount = 2; rpci.pDependencies = deps;
        if (vkCreateRenderPass(s->device, &rpci, nullptr, &s->rpass) != VK_SUCCESS) {
            vlog("vkCreateRenderPass failed."); setFallback("renderpass_failed"); return false;
        }
        VkImageView fbViews[3] = { s->colorRes->view, s->normalRes->view, s->depthRes->view };
        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = s->rpass; fci.attachmentCount = 3; fci.pAttachments = fbViews;
        fci.width = (uint32_t)w; fci.height = (uint32_t)h; fci.layers = 1;
        if (vkCreateFramebuffer(s->device, &fci, nullptr, &s->fb) != VK_SUCCESS) {
            vlog("vkCreateFramebuffer failed."); setFallback("framebuffer_failed"); return false;
        }
    }

    // ---- Buffers (height snapshot SSBO + per-parity per-patch dynamic UBOs). ----
    if (!ensure_height_capacity(s, 256 * 1024)) return false;
    if (!ensure_ubo_capacity(s, 0, 64 * sizeof(TerrainFrameUBO))) return false;
    if (!ensure_ubo_capacity(s, 1, 64 * sizeof(TerrainFrameUBO))) return false;

    // ---- COLORMAP-BASE: command pool must exist before the copy-once import can
    // run its one-shot cmd buffer; create it here (also allocs the 2 draw cbufs). ----
    {
        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = s->gfxFamily;
        if (vkCreateCommandPool(s->device, &pci, nullptr, &s->cpool) != VK_SUCCESS) {
            vlog("vkCreateCommandPool failed."); return false;
        }
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = s->cpool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 2;
        if (vkAllocateCommandBuffers(s->device, &cai, s->cbuf) != VK_SUCCESS) {
            vlog("vkAllocateCommandBuffers failed."); return false;
        }
    }

    // VK-DRAW-PERF-1: 3-stamp GPU timer (pass-start / draws-start / pass-end).
    // No-op unless MC2_VK_GPU_TIMESTAMPS is set; fail-soft if timestamps unsupported.
    if (mc2vkts::timestampsEnabled()) {
        if (s->gpuTimer.init(s->device, s->phys, s->gfxFamily, 3))
            vlog("GPU timestamps ON (period=%.2f ns): pass segments = setup+clear / draws.",
                 s->gpuTimer.periodNs);
        else
            vlog("GPU timestamps requested but unsupported on this queue (validBits=0 or period<=0).");
    }
    // Bind a 1x1 dummy colormap so binding 2 is a valid descriptor from frame 1
    // (colormapReady stays false until the real atlas imports -> FS uses olive).
    if (!ensure_dummy_colormap(s)) { setFallback("colormap_dummy_failed"); return false; }
    // CEMENT: dummy cement atlas (binding 3) + dummy cement-word SSBO (binding 4) so both
    // are valid descriptors from frame 1 (useCement stays 0 until the real atlas+words
    // are ready -> FS ignores the cement branch -> colormap base, no hole).
    if (!ensure_dummy_cement(s)) { setFallback("cement_dummy_failed"); return false; }
    if (s->cementBuf == VK_NULL_HANDLE) {
        if (!make_buffer(s, 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &s->cementBuf, &s->cementBufAlloc, true, "terrain.cementBuf")) {
            setFallback("cement_dummy_ssbo_failed"); return false;
        }
        s->cementCapBytes = 16;
    }
    // VK-TERRAIN-VISUALDISPLACE-FAR-1: dummy 16B visual-height SSBO so binding 10 is a
    // valid descriptor from frame 1 (visualHeightReady stays false until the sub-gate is
    // on AND the fine bake snapshots -> UBO visualMode stays 0 -> VS never reads it).
    if (s->visualHeightBuf == VK_NULL_HANDLE) {
        if (!make_buffer(s, 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &s->visualHeightBuf, &s->visualHeightAlloc, true, "terrain.visualHeightBuf")) {
            setFallback("visual_height_dummy_ssbo_failed"); return false;
        }
        s->visualHeightCapBytes = 16;
    }
    // VK-TERRAIN-LIGHTING: dummy 1x1 depth-compare shadow (binding 5) so binding 5 is a
    // valid descriptor from frame 1 (shadowReady stays false until MC2_VK_TERRAIN_LIGHTING
    // is on AND the GL static shadow map is live -> FS enableShadows=0 -> lit-only, no hole).
    if (!ensure_dummy_shadow(s)) { setFallback("shadow_dummy_failed"); return false; }
    // VK-TERRAIN-DETAIL: 1x1x1 flat-normal matNormalArray dummy so binding 6 is a valid
    // descriptor from frame 1 (matNormalReady stays false until MC2_VK_TERRAIN_DETAIL is on
    // AND the GL array is live -> FS uses macro-slope-only normal, no hole).
    if (!ensure_dummy_matnormal(s)) { setFallback("matnormal_dummy_failed"); return false; }
    // VK-TERRAIN-CSM-NEAR-1: dummy 1x1(x1) near-cascade array + full-map depth-compare
    // (bindings 8/9) so both are valid descriptors from frame 1 (csmArrayReady/csmFullMapReady
    // stay false until MC2_VK_TERRAIN_CSM is armed AND GL CSM is actually built -> FS
    // csmEnabled=0 -> falls back to the legacy single-map path, no hole).
    if (!ensure_dummy_csm(s)) { setFallback("csm_dummy_failed"); return false; }

    // VK-PARITY-TERRAIN-2: initial light pool / grid buffers (both parities) so bindings
    // 11-13 are valid descriptors from frame 1. Sizes: 256 x 48B pool, 64x64 x uvec2 header,
    // 16K index words; grown on demand by ensure_lightpool_capacity.
    {
        const VkDeviceSize initBytes[3] = { 256u * 48u, 64u * 64u * 8u, 16384u * 4u };
        for (int i = 0; i < 2; ++i)
            for (int k = 0; k < 3; ++k)
                if (s->lpBuf[i][k] == VK_NULL_HANDLE) {
                    if (!make_buffer(s, initBytes[k], VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                     &s->lpBuf[i][k], &s->lpAlloc[i][k], true, "terrain.lpBuf", i, k)) {
                        setFallback("lightpool_ssbo_failed"); return false;
                    }
                    s->lpCap[i][k] = initBytes[k];
                }
    }

    // ---- Descriptor layout: binding0 dynamic UBO (VS+FS), binding1 height SSBO (VS). ----
    {
        VkDescriptorSetLayoutBinding b[14]{};
        b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        // VK-TERRAIN-DETAIL: binding 1 (height SSBO) is now ALSO read by the FS
        // (mv_smoothTerrainNormal reproduces GL's height-derived macro slope).
        b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        // COLORMAP-BASE: binding 2 = colormap sampler (FS). Always in the layout; the
        // descriptor is always written (dummy 1x1 until the real atlas is imported).
        b[2].binding = 2; b[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[2].descriptorCount = 1; b[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        // CEMENT: binding 3 = cement atlas sampler (FS); binding 4 = cement-word SSBO (FS).
        b[3].binding = 3; b[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[3].descriptorCount = 1; b[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        b[4].binding = 4; b[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[4].descriptorCount = 1; b[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        // VK-TERRAIN-LIGHTING: binding 5 = static shadow sampler2DShadow (FS).
        b[5].binding = 5; b[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[5].descriptorCount = 1; b[5].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        // VK-TERRAIN-DETAIL: binding 6 = material detail-normal sampler2DArray (FS).
        b[6].binding = 6; b[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[6].descriptorCount = 1; b[6].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        // VK-TERRAIN-DYNSHADOW: binding 7 = dynamic shadow sampler2DShadow (FS).
        b[7].binding = 7; b[7].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[7].descriptorCount = 1; b[7].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        // VK-TERRAIN-CSM-NEAR-1: binding 8 = near-cascade array sampler2DArrayShadow (FS);
        // binding 9 = full-map cascade sampler2DShadow (FS).
        b[8].binding = 8; b[8].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[8].descriptorCount = 1; b[8].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        b[9].binding = 9; b[9].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[9].descriptorCount = 1; b[9].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        // VK-TERRAIN-VISUALDISPLACE-FAR-1: binding 10 = visual-height fine-bake SSBO (VS,
        // both main + skirt verts read it for the mode-2 Z-swap).
        b[10].binding = 10; b[10].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[10].descriptorCount = 1; b[10].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        // VK-PARITY-TERRAIN-2: bindings 11/12/13 = light pool / grid header / grid index SSBOs (FS).
        for (int k = 11; k <= 13; ++k) {
            b[k].binding = (uint32_t)k; b[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b[k].descriptorCount = 1; b[k].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = 14; lci.pBindings = b;
        if (vkCreateDescriptorSetLayout(s->device, &lci, nullptr, &s->dsl) != VK_SUCCESS) {
            vlog("vkCreateDescriptorSetLayout failed."); return false;
        }
        // VK-TERRAIN-FENCE-DEFER-1: TWO sets (one per parity slot), so pool sizes double.
        VkDescriptorPoolSize sizes[4]{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;  sizes[0].descriptorCount = 2;
        sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;          sizes[1].descriptorCount = 12; // (height + cement + visual-height + pool + grid hdr + grid idx) x2
        sizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;  sizes[2].descriptorCount = 14; // (colormap + cement + static shadow + matNormalArray + dyn shadow + csm array + csm fullmap) x2
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets = 2; pci.poolSizeCount = 3; pci.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(s->device, &pci, nullptr, &s->pool) != VK_SUCCESS) {
            vlog("vkCreateDescriptorPool failed."); return false;
        }
        VkDescriptorSetLayout layouts[2] = { s->dsl, s->dsl };
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = s->pool; dsai.descriptorSetCount = 2; dsai.pSetLayouts = layouts;
        if (vkAllocateDescriptorSets(s->device, &dsai, s->set) != VK_SUCCESS) {
            vlog("vkAllocateDescriptorSets failed."); return false;
        }
        write_descriptors(s);
    }

    // ---- Pipeline: int16x2 SINT vertex, reverse-Z GEQUAL depth-test+WRITE, cull
    // NONE, frontFace CCW, 2 GBuffer outputs, NO blend (opaque overwrite). State COPIED
    // from the GL terrain contract (== prop tier1 pipeline). ----
    {
        s->vert = load_spv(s->device, spv_dir() + "terrain_tier1.vert.spv");
        s->frag = load_spv(s->device, spv_dir() + "terrain_tier1.frag.spv");
        if (s->vert == VK_NULL_HANDLE || s->frag == VK_NULL_HANDLE) { setFallback("spv_load_failed"); return false; }

        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1; plci.pSetLayouts = &s->dsl;
        if (vkCreatePipelineLayout(s->device, &plci, nullptr, &s->playout) != VK_SUCCESS) {
            vlog("vkCreatePipelineLayout failed."); return false;
        }

        // Mirror the GL terrain VAO: loc0 ivec2 (int16 lx,ly), stride 4, integer.
        VkVertexInputBindingDescription vib{};
        vib.binding = 0; vib.stride = sizeof(LocalVertex); vib.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        VkVertexInputAttributeDescription via{};
        via.location = 0; via.binding = 0; via.format = VK_FORMAT_R16G16_SINT; via.offset = 0;
        VkPipelineVertexInputStateCreateInfo vin{};
        vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vin.vertexBindingDescriptionCount = 1; vin.pVertexBindingDescriptions = &vib;
        vin.vertexAttributeDescriptionCount = 1; vin.pVertexAttributeDescriptions = &via;

        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vps{};
        vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vps.viewportCount = 1; vps.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;                        // double-sided (terrainMVP X-flip)
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;         // authored CCW
        rs.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo ds{};
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;    // reverse-Z GEQUAL

        VkPipelineColorBlendAttachmentState cba[2]{};
        for (int k = 0; k < 2; ++k) {
            cba[k].blendEnable = VK_FALSE;                      // opaque in-place overwrite
            cba[k].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        }
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 2; cb.pAttachments = cba;

        VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dsci{};
        dsci.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dsci.dynamicStateCount = 2; dsci.pDynamicStates = dyn;

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = s->vert; stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = s->frag; stages[1].pName = "main";
        VkGraphicsPipelineCreateInfo gp{};
        gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gp.stageCount = 2; gp.pStages = stages;
        gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vps; gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms; gp.pDepthStencilState = &ds; gp.pColorBlendState = &cb;
        gp.pDynamicState = &dsci;
        gp.layout = s->playout; gp.renderPass = s->rpass; gp.subpass = 0;
        if (vkCreateGraphicsPipelines(s->device, VK_NULL_HANDLE, 1, &gp, nullptr, &s->pipe) != VK_SUCCESS) {
            vlog("vkCreateGraphicsPipelines failed."); setFallback("pipeline_failed"); return false;
        }
        mc2vk::nameObject(s->device, s->pipe, "terrain.pipe");

        // VK-TERRAIN-SKIRT (rung 4): second pipeline, same layout/renderpass/raster/
        // depth/blend state as above -- only the shader modules + vertex-input state
        // differ (2 attributes: ivec2 localOffset + int isSkirtFlag, mirrors GL's
        // skirt VAO glVertexAttribIPointer calls, gos_terrain_lod_chunk.cpp:1744-1748).
        s->skirtVert = load_spv(s->device, spv_dir() + "terrain_tier1_skirt.vert.spv");
        s->skirtFrag = load_spv(s->device, spv_dir() + "terrain_tier1_skirt.frag.spv");
        if (s->skirtVert == VK_NULL_HANDLE || s->skirtFrag == VK_NULL_HANDLE) {
            setFallback("spv_load_failed"); return false;
        }

        VkVertexInputBindingDescription svib{};
        svib.binding = 0; svib.stride = sizeof(SkirtVertex); svib.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        VkVertexInputAttributeDescription svia[2]{};
        svia[0].location = 0; svia[0].binding = 0; svia[0].format = VK_FORMAT_R16G16_SINT;  svia[0].offset = 0;
        svia[1].location = 1; svia[1].binding = 0; svia[1].format = VK_FORMAT_R16_SINT;      svia[1].offset = 4;
        VkPipelineVertexInputStateCreateInfo svin{};
        svin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        svin.vertexBindingDescriptionCount = 1; svin.pVertexBindingDescriptions = &svib;
        svin.vertexAttributeDescriptionCount = 2; svin.pVertexAttributeDescriptions = svia;

        VkPipelineShaderStageCreateInfo sstages[2]{};
        sstages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        sstages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; sstages[0].module = s->skirtVert; sstages[0].pName = "main";
        sstages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        sstages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; sstages[1].module = s->skirtFrag; sstages[1].pName = "main";

        VkGraphicsPipelineCreateInfo sgp{};
        sgp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        sgp.stageCount = 2; sgp.pStages = sstages;
        sgp.pVertexInputState = &svin; sgp.pInputAssemblyState = &ia;
        sgp.pViewportState = &vps; sgp.pRasterizationState = &rs;
        sgp.pMultisampleState = &ms; sgp.pDepthStencilState = &ds; sgp.pColorBlendState = &cb;
        sgp.pDynamicState = &dsci;
        sgp.layout = s->playout; sgp.renderPass = s->rpass; sgp.subpass = 0;
        if (vkCreateGraphicsPipelines(s->device, VK_NULL_HANDLE, 1, &sgp, nullptr, &s->skirtPipe) != VK_SUCCESS) {
            vlog("vkCreateGraphicsPipelines (skirt) failed."); setFallback("pipeline_failed"); return false;
        }
        mc2vk::nameObject(s->device, s->skirtPipe, "terrain.skirtPipe");
    }

    s->inited = true;
    setFallback("");
    vlog("island initialized %dx%d (shared GBuffer co-write; LOD0 main-triangle terrain "
         "prefix; reverse-Z GEQUAL double-sided; gbuffer1.a=1.0).", w, h);
    return true;
}

} // namespace

#endif // MC2_VULKAN_ISLAND
