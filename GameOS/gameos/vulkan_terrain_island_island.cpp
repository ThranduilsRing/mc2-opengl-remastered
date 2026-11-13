// VK terrain island: island buffers, patch / skirt mesh caches, descriptor writes, capacity growth and init / destroy.
// Declared in the class headers; shared names in vulkan_terrain_island_internal.h.
#include "vk_owned.h"
#include "vulkan_terrain_island_internal.h"
#include "Gates/gate_cold.h"
#include "terrain_diagonal.h"
#include "vk_dynamic_rendering.h"
#include <cstdlib>


#include <bit>

namespace vk_terrain_detail {

bool make_buffer(TerrainIsland* s, mc2vk::OwnedBuffer& out, VkDeviceSize size, VkBufferUsageFlags usage,
                 bool hostVisible, std::string_view name, int i0, int i1) {
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size ? size : 16; bci.usage = usage;
    VmaAllocationCreateInfo aci{};
    aci.usage = hostVisible ? VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE : VMA_MEMORY_USAGE_AUTO;   // host-visible here = CPU-written, GPU-read: ReBAR device-local where available
    aci.flags = hostVisible ? VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT : 0;
    if (hostVisible) aci.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;   // callers write with map+memcpy and no flush
    if (!out.create(s->allocator, s->device, bci, aci, name, i0, i1)) {
        vlog("vmaCreateBuffer failed."); return false;
    }
    return true;
}

bool replace_host_buffer(TerrainIsland* s, mc2vk::OwnedBuffer& buf, VkDeviceSize size, VkBufferUsageFlags usage, std::string_view name) {
    mc2vk::OwnedBuffer fresh;
    if (!make_buffer(s, fresh, size, usage, true, name)) return false;
    if (buf) mc2vk::getOrCreateEngineVulkanContext().retire(std::move(buf), size);
    buf = std::move(fresh);
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
// TERRAIN-HIRES-NEAR-DIAG: with `fine` (the 4x bake, fineSide wide; origin = block origin in coarse cells, mesh offsets in
// fine units) each quad splits along the diagonal whose corner heights differ less, so cliff edges that run against TL-BR no
// longer saw-tooth. Only the interior diagonal moves: every quad edge, hence every stitched or skirted chunk border, is unchanged.
const PatchMesh* getOrBuildPatch(TerrainIsland* s, int qcX, int qcY, int lodStep,
                                 const float* fine, int fineSide, int originX, int originY) {
    if (lodStep <= 0) lodStep = 1;
    const uint32_t key = (static_cast<uint32_t>((qcX & 0xFFFF))) | ((static_cast<uint32_t>((qcY & 0xFFFF))) << 16)
                        ^ (static_cast<uint32_t>((lodStep & 0xFF)) << 24);
    const uint64_t dkey = (static_cast<uint64_t>(key)) ^ (static_cast<uint64_t>(originX & 0xFFFF) << 32) ^ (static_cast<uint64_t>(originY & 0xFFFF) << 48);
    auto& cache = s->patchCache;
    if (fine) {
        auto dit = s->diagPatchCache.find(dkey);
        if (dit != s->diagPatchCache.end()) {
            const auto& [cacheKey, mesh] = *dit;
            return &mesh;
        }
    } else {
        auto it = cache.find(key);
        if (it != cache.end()) {
            const auto& [cacheKey, mesh] = *it;
            return &mesh;
        }
    }
    if (qcX <= 0 || qcY <= 0) return nullptr;
    auto fineH = [&](int lx, int ly) {
        const int fx = std::clamp(originX * 4 + lx, 0, fineSide - 1), fy = std::clamp(originY * 4 + ly, 0, fineSide - 1);
        return fine[static_cast<size_t>(fx) + static_cast<size_t>(fy) * static_cast<size_t>(fineSide)];
    };

    // Sample positions: 0, lodStep, 2*lodStep, ... up to qc (mirrors GL
    // makeSamplePositions(quadCount, lodStep)). nx/ny = number of grid POINTS.
    std::vector<int> xs, ys;
    for (int p = 0; p <= qcX; p += lodStep) xs.push_back(p);
    if (xs.empty() || xs.back() != qcX) xs.push_back(qcX);
    for (int p = 0; p <= qcY; p += lodStep) ys.push_back(p);
    if (ys.empty() || ys.back() != qcY) ys.push_back(qcY);
    const int nx = static_cast<int>(xs.size()), ny = static_cast<int>(ys.size());

    // Vertices: nx x ny grid of int16 local offsets (map-cell units, lodStep-strided).
    std::vector<LocalVertex> verts;
    verts.reserve(static_cast<size_t>(nx) * ny);
    for (int yy = 0; yy < ny; ++yy)
        for (int xx = 0; xx < nx; ++xx)
            verts.push_back({static_cast<int16_t>(xs[xx]), static_cast<int16_t>(ys[yy])});

    // Indices: two CCW triangles per quad, TL-BR diagonal (== getOrBuildPatch
    // bottomRight default): {TL,BL,BR} + {TL,BR,TR}; with `fine`, per quad the flatter diagonal.
    std::vector<uint16_t> indices;
    const int W = nx;
    for (int j = 0; j < ny - 1; ++j) {
        for (int i = 0; i < nx - 1; ++i) {
            uint16_t tl = static_cast<uint16_t>((j * W + i));
            uint16_t tr = static_cast<uint16_t>((j * W + i + 1));
            uint16_t bl = static_cast<uint16_t>(((j + 1) * W + i));
            uint16_t br = static_cast<uint16_t>(((j + 1) * W + i + 1));
            if (fine && mc2terrain::quadSplitsTrBl(fineH(xs[i], ys[j]), fineH(xs[i + 1], ys[j]), fineH(xs[i], ys[j + 1]), fineH(xs[i + 1], ys[j + 1])))
                indices.insert(indices.end(), {tl, bl, tr, bl, br, tr});   // TR-BL diagonal, same CCW winding
            else
                indices.insert(indices.end(), {tl, bl, br, tl, br, tr});
        }
    }

    PatchMesh pm;
    pm.indexCount = static_cast<uint32_t>(indices.size());
    const VkDeviceSize vboBytes = verts.size() * sizeof(LocalVertex);
    const VkDeviceSize iboBytes = indices.size() * sizeof(uint16_t);
    if (!make_buffer(s, pm.vbo, vboBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true, "terrain.patchMesh.vbo")) return nullptr;
    if (!make_buffer(s, pm.ibo, iboBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true, "terrain.patchMesh.ibo")) return nullptr;
    { void* p = nullptr;
      if (vmaMapMemory(s->allocator, pm.vbo.allocation(), &p) == VK_SUCCESS) {
          std::memcpy(p, verts.data(), static_cast<size_t>(vboBytes)); vmaUnmapMemory(s->allocator, pm.vbo.allocation()); } }
    { void* p = nullptr;
      if (vmaMapMemory(s->allocator, pm.ibo.allocation(), &p) == VK_SUCCESS) {
          std::memcpy(p, indices.data(), static_cast<size_t>(iboBytes)); vmaUnmapMemory(s->allocator, pm.ibo.allocation()); } }

    if (fine) return &(s->diagPatchCache[dkey] = std::move(pm));
    return &(cache[key] = std::move(pm));
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
    const uint32_t key = (static_cast<uint32_t>((qcX & 0xFFFF))) | ((static_cast<uint32_t>((qcY & 0xFFFF))) << 16)
                        ^ (static_cast<uint32_t>((lodStep & 0xFF)) << 24);
    auto it = s->skirtCache.find(key);
    if (it != s->skirtCache.end()) {
        const auto& [cacheKey, mesh] = *it;
        return &mesh;
    }
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
        int base = static_cast<int>(skirtVerts.size());
        int n    = static_cast<int>(uPos.size());
        for (int i = 0; i < n; ++i) {
            int16_t lx = fixedIsY ? static_cast<int16_t>(uPos[i]) : static_cast<int16_t>(fixedCoord);
            int16_t ly = fixedIsY ? static_cast<int16_t>(fixedCoord) : static_cast<int16_t>(uPos[i]);
            skirtVerts.push_back({lx, ly, 0, 0});  // surface
            skirtVerts.push_back({lx, ly, 1, 0});  // below
        }
        for (int i = 0; i < n - 1; ++i) {
            uint16_t t0 = static_cast<uint16_t>((base + 2*i));
            uint16_t b0 = static_cast<uint16_t>((base + 2*i + 1));
            uint16_t t1 = static_cast<uint16_t>((base + 2*(i+1)));
            uint16_t b1 = static_cast<uint16_t>((base + 2*(i+1) + 1));
            skirtIdx.insert(skirtIdx.end(), {t0, b0, b1, t0, b1, t1});
        }
    };

    SkirtMesh sm;
    sm.edgeOffset[0] = static_cast<uint32_t>(skirtIdx.size()); buildEdge(xs, static_cast<int>(ys.front()), false); sm.edgeCount[0] = static_cast<uint32_t>(skirtIdx.size()) - sm.edgeOffset[0]; // North
    sm.edgeOffset[1] = static_cast<uint32_t>(skirtIdx.size()); buildEdge(xs, static_cast<int>(ys.back()),  false); sm.edgeCount[1] = static_cast<uint32_t>(skirtIdx.size()) - sm.edgeOffset[1]; // South
    sm.edgeOffset[2] = static_cast<uint32_t>(skirtIdx.size()); buildEdge(ys, static_cast<int>(xs.front()), true ); sm.edgeCount[2] = static_cast<uint32_t>(skirtIdx.size()) - sm.edgeOffset[2]; // West
    sm.edgeOffset[3] = static_cast<uint32_t>(skirtIdx.size()); buildEdge(ys, static_cast<int>(xs.back()),  true ); sm.edgeCount[3] = static_cast<uint32_t>(skirtIdx.size()) - sm.edgeOffset[3]; // East

    if (skirtVerts.empty() || skirtIdx.empty()) return nullptr;

    const VkDeviceSize vboBytes = skirtVerts.size() * sizeof(SkirtVertex);
    const VkDeviceSize iboBytes = skirtIdx.size() * sizeof(uint16_t);
    if (!make_buffer(s, sm.vbo, vboBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true, "terrain.skirtMesh.vbo")) return nullptr;
    if (!make_buffer(s, sm.ibo, iboBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true, "terrain.skirtMesh.ibo")) return nullptr;
    { void* p = nullptr;
      if (vmaMapMemory(s->allocator, sm.vbo.allocation(), &p) == VK_SUCCESS) {
          std::memcpy(p, skirtVerts.data(), static_cast<size_t>(vboBytes)); vmaUnmapMemory(s->allocator, sm.vbo.allocation()); } }
    { void* p = nullptr;
      if (vmaMapMemory(s->allocator, sm.ibo.allocation(), &p) == VK_SUCCESS) {
          std::memcpy(p, skirtIdx.data(), static_cast<size_t>(iboBytes)); vmaUnmapMemory(s->allocator, sm.ibo.allocation()); } }

    const auto [cachedIt, inserted] = s->skirtCache.emplace(key, std::move(sm));
    const auto& [cacheKey, mesh] = *cachedIt;
    return &mesh;
}

// The shadow island hands its depth images over in DEPTH_STENCIL_READ_ONLY (its render pass final layout); the descriptor must name the layout the image is in at
// draw time, while the island's own copies are transitioned to SHADER_READ_ONLY.
constexpr VkImageLayout kDirectShadowLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

void write_descriptors(TerrainIsland* s, int parity) {
    if (parity<0) { for (auto& dirty : s->descriptorsDirty) dirty = true; return; }
    // Only the retired parity is written; upload requests mark both parities dirty.
    for (int i = parity; i <= parity; ++i) {
    s->descriptorsDirty[i]=false;
    if (s->set[i] == VK_NULL_HANDLE) continue;
    // binding 0 = dynamic UBO (per-patch frame block); binding 1 = height SSBO;
    // binding 2 = colormap combined-image-sampler (COLORMAP-BASE, only when live).
    // binding 3 = cement combined-image-sampler (dummy/real); binding 4 = cement-word SSBO.
    // Bindings 0 (UBO) and 11-13 (light pool) are frame-ring dynamic bindings: written at the ring's current buffer, re-pointed at the next frame of this
    // parity (ringBound cleared).
    if (const VkBuffer ring = mc2vk::getOrCreateEngineVulkanContext().frameRingBuffer()) {
        const VkDeviceSize range = mc2vk::getOrCreateEngineVulkanContext().frameRingRange();
        mc2vk::writeDynamicBinding(s->device, s->set[i], 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, ring, sizeof(TerrainFrameUBO));
        for (uint32_t k = 0; k < 3; ++k)
            mc2vk::writeDynamicBinding(s->device, s->set[i], 11 + k, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, ring, range);
    }
    for (VkBuffer& b : s->ringBound[i]) b = VK_NULL_HANDLE;
    VkDescriptorBufferInfo hInfo{ s->heightBuf.get(), 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w[20]{};
    uint32_t n = 0;
    w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = s->set[i];
    w[n].dstBinding = 1; w[n].descriptorCount = 1;
    w[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[n].pBufferInfo = &hInfo; ++n;
    VkDescriptorImageInfo cmInfo{};
    // The dummy is a valid binding even when the real atlas is absent on editor reopen.
    if (s->colormapTexture && s->colormapSamp) {
        cmInfo.sampler = s->colormapSamp.get(); cmInfo.imageView = s->colormapTexture.view();
        cmInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = s->set[i];
        w[n].dstBinding = 2; w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[n].pImageInfo = &cmInfo; ++n;
    }
    // CEMENT: binding 3 sampler — always written (dummy 1x1 until the real atlas imports).
    VkDescriptorImageInfo ceInfo{};
    if (s->cementTexture && s->cementSamp) {
        ceInfo.sampler = s->cementSamp.get(); ceInfo.imageView = s->cementTexture.view();
        ceInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = s->set[i];
        w[n].dstBinding = 3; w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[n].pImageInfo = &ceInfo; ++n;
    }
    // CEMENT: binding 4 cement-word SSBO — always written (dummy 16B until first snapshot).
    VkDescriptorBufferInfo ceBuf{};
    if (s->cementBuf) {
        ceBuf.buffer = s->cementBuf.get(); ceBuf.offset = 0; ceBuf.range = VK_WHOLE_SIZE;
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = s->set[i];
        w[n].dstBinding = 4; w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[n].pBufferInfo = &ceBuf; ++n;
    }
    // VK-TERRAIN-DETAIL: binding 6 material detail-normal sampler2DArray — always written
    // once the (dummy 1x1x1 or real) array image + sampler exist (uT.detailReady gates use).
    VkDescriptorImageInfo mnInfo{};
    if (s->matNormalTexture && s->matNormalSamp) {
        mnInfo.sampler = s->matNormalSamp.get(); mnInfo.imageView = s->matNormalTexture.view();
        mnInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = s->set[i];
        w[n].dstBinding = 6; w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[n].pImageInfo = &mnInfo; ++n;
    }
    // VK-TERRAIN-VISUALDISPLACE-FAR-1: binding 10 visual-height (fine bake) SSBO —
    // always written (dummy 16B until the first bake snapshot; uT.visualMode==0 gates use).
    VkDescriptorBufferInfo vhBuf{};
    if (s->visualHeightBuf) {
        vhBuf.buffer = s->visualHeightBuf.get(); vhBuf.offset = 0; vhBuf.range = VK_WHOLE_SIZE;
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = s->set[i];
        w[n].dstBinding = 10; w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[n].pBufferInfo = &vhBuf; ++n;
    }
    // VK-PARITY-TERRAIN-2: bindings 11/12/13 light pool / grid header / grid indices are the ring bindings written above.
    // TERRAIN-SHORELINE-V3: binding 14 shoreline mask (dummy until a mission ships one; uT.shorelineHasMask gates use).
    VkDescriptorImageInfo smInfo{};
    if (s->shoreMaskTexture && s->colormapSamp) {
        smInfo.sampler = s->colormapSamp.get(); smInfo.imageView = s->shoreMaskTexture.view();
        smInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = s->set[i];
        w[n].dstBinding = 14; w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[n].pImageInfo = &smInfo; ++n;
    }
    // Baked semantic material weights; the old shore-image slot had no shader consumer.
    VkDescriptorImageInfo sfInfo{};
    if (s->materialTexture && s->colormapSamp) {
        sfInfo.sampler = s->colormapSamp.get(); sfInfo.imageView = s->materialTexture.view();
        sfInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = s->set[i];
        w[n].dstBinding = 15; w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[n].pImageInfo = &sfInfo; ++n;
    }
    const VkDescriptorImageInfo gradientInfo{s->colormapSamp.get(), s->macroGradientTexture.view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = s->set[i];
    w[n].dstBinding = 16; w[n].descriptorCount = 1;
    w[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[n].pImageInfo = &gradientInfo; ++n;
    const VkDescriptorImageInfo visibilityInfo{s->staticVisibilitySampler.get(),
        s->staticVisibilityExternalView ? s->staticVisibilityExternalView : s->staticVisibilityDummy.view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = s->set[i];
    w[n].dstBinding = 17; w[n].descriptorCount = 1;
    w[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[n].pImageInfo = &visibilityInfo; ++n;
    // LOCAL-LIGHTS-2: binding 19 local-light shadow atlas; the visibility dummy until the shadow island has one
    // (no pool record carries a shadow tile before then, so the dummy is never sampled).
    const VkDescriptorImageInfo localShadowInfo = s->localShadowBoundView
        ? VkDescriptorImageInfo{s->colormapSamp.get(), s->localShadowBoundView, kDirectShadowLayout}
        : VkDescriptorImageInfo{s->colormapSamp.get(), s->staticVisibilityDummy.view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = s->set[i];
    w[n].dstBinding = 19; w[n].descriptorCount = 1;
    w[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[n].pImageInfo = &localShadowInfo; ++n;
    VkDescriptorImageInfo cdInfo{};
    if (s->concreteDetailTexture && s->concreteDetailSamp) {
        cdInfo.sampler = s->concreteDetailSamp.get(); cdInfo.imageView = s->concreteDetailTexture.view();
        cdInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = s->set[i];
        w[n].dstBinding = 18; w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[n].pImageInfo = &cdInfo; ++n;
    }
    vkUpdateDescriptorSets(s->device, n, w, 0, nullptr);
    s->setRev[i] = mc2vk::nextDescRevision();   // ground-lighting copies of this set are stale
    }   // per-parity set loop
}

bool ensure_height_capacity(TerrainIsland* s, VkDeviceSize bytes) {
    if (s->heightBuf && bytes <= s->heightCapBytes) return true;
    // Power-of-two capacity so growth is rare; the old buffer retires on the open-frame ticket (in-flight submits may still read it).
    const VkDeviceSize cap = std::bit_ceil(static_cast<uint64_t>(bytes + bytes / 4 + 4096));
    if (!replace_host_buffer(s, s->heightBuf, cap, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "terrain.heightBuf"))
        return false;
    s->heightCapBytes = cap;
    s->heightVersionCopied = UINT64_MAX; // force re-snapshot into the new buffer
    write_descriptors(s);
    return true;
}

void sync_ring_bindings(TerrainIsland* s, uint32_t par, const mc2vk::FrameAlloc& ubo, const mc2vk::FrameAlloc (&lp)[3]) {
    if (s->ringBound[par][0] != ubo.buffer) {
        mc2vk::writeDynamicBinding(s->device, s->set[par], 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, ubo.buffer, sizeof(TerrainFrameUBO));
        s->ringBound[par][0] = ubo.buffer;
        s->setRev[par] = mc2vk::nextDescRevision();
    }
    for (uint32_t k = 0; k < 3; ++k) {
        if (s->ringBound[par][1 + k] == lp[k].buffer) continue;
        mc2vk::writeDynamicBinding(s->device, s->set[par], 11 + k, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, lp[k].buffer, lp[k].range);
        s->ringBound[par][1 + k] = lp[k].buffer;
        s->setRev[par] = mc2vk::nextDescRevision();
    }
    // Published after the ring writes so the revision covers them (consumers copy this set's bindings by revision).
    s->groundFrame = {s->set[par], {static_cast<uint32_t>(ubo.offset), static_cast<uint32_t>(lp[0].offset), static_cast<uint32_t>(lp[1].offset), static_cast<uint32_t>(lp[2].offset)}, s->setRev[par]};
    s->groundFrameNo = mc2vk::getOrCreateEngineVulkanContext().tracker().frameNo();
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

    // ---- Shared-GBuffer scene pass (VK_KHR_dynamic_rendering, no VkRenderPass/VkFramebuffer). Terrain is the native first owner of
    // color0/normal/depth for the frame: loadOp CLEAR on all three (color0 = gameplay/menu clear color via
    // mc2_vk_terrain_set_native_prefix_clear_color, normal = clearGBuffer1 sentinel, depth = reverse-Z far; depth is cleared, never LOADed,
    // because nothing has written scene depth yet this frame: FAR-BAND-BLACK-1). storeOp STORE on all three (props/mech/water/vfx and the
    // post chain read them). The pass stays in COLOR/DEPTH_STENCIL_ATTACHMENT_OPTIMAL; the subpass dependency of the old render pass is the
    // memory barrier recorded by terrain_begin_scene_pass (vulkan_terrain_island_run.cpp). The pipelines below carry
    // VkPipelineRenderingCreateInfo with the same formats.

    // ---- Buffers (height snapshot SSBO; the per-frame UBO and light pool are frame-ring spans). ----
    if (!ensure_height_capacity(s, 256 * 1024)) return false;

    // ---- COLORMAP-BASE: command pool must exist before the copy-once import can
    // run its one-shot cmd buffer; create it here (also allocs the 2 draw cbufs). ----
    {
        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = 0;
        pci.queueFamilyIndex = s->gfxFamily;
        if (!mc2vk::create(s->cpool, s->device, [&](auto* h) { return vkCreateCommandPool(s->device, &pci, nullptr, h); })) {
            vlog("vkCreateCommandPool failed."); return false;
        }
        for (uint32_t i = 0; i < 2 * mc2vk::kMaxFramesInFlight; ++i) {
            if (!mc2vk::create(s->slotPool[i], s->device, [&](auto* h) { return vkCreateCommandPool(s->device, &pci, nullptr, h); })) {
                vlog("vkCreateCommandPool failed."); return false;
            }
            VkCommandBufferAllocateInfo cai{};
            cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            cai.commandPool = s->slotPool[i].get(); cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
            if (vkAllocateCommandBuffers(s->device, &cai, &s->cbuf[i]) != VK_SUCCESS) {
                vlog("vkAllocateCommandBuffers failed."); return false;
            }
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
    const uint8_t unknownVisibility[4] = {0,255,0,0};
    if (!upload_sampled_image(s,s->staticVisibilityDummy,VK_FORMAT_R8G8B8A8_UNORM,1,1,unknownVisibility,sizeof(unknownVisibility))) {
        setFallback("static_visibility_dummy_failed"); return false;
    }
    VkSamplerCreateInfo visibilitySampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    // The bake mask is pre-filtered: bilinear within a level, linear between its mips.
    visibilitySampler.magFilter = visibilitySampler.minFilter = VK_FILTER_LINEAR;
    visibilitySampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    visibilitySampler.maxLod = VK_LOD_CLAMP_NONE;
    visibilitySampler.addressModeU = visibilitySampler.addressModeV = visibilitySampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (!mc2vk::create(s->staticVisibilitySampler,s->device,[&](auto* h){return vkCreateSampler(s->device,&visibilitySampler,nullptr,h);})) return false;
    const float flatGradient[2] = {0, 0};
    if (!upload_sampled_image(s, s->macroGradientTexture, VK_FORMAT_R32G32_SFLOAT, 1, 1, flatGradient, sizeof(flatGradient))) {
        setFallback("macro_gradient_dummy_failed"); return false;
    }
    // CEMENT: dummy cement atlas (binding 3) + dummy cement-word SSBO (binding 4) so both
    // are valid descriptors from frame 1 (useCement stays 0 until the real atlas+words
    // are ready -> FS ignores the cement branch -> colormap base, no hole).
    if (!ensure_dummy_cement(s)) { setFallback("cement_dummy_failed"); return false; }
    if (!ensure_dummy_shore_mask(s)) { setFallback("shore_mask_dummy_failed"); return false; }
    if (!ensure_dummy_materials(s)) { setFallback("material_dummy_failed"); return false; }
    if (!s->cementBuf) {
        if (!make_buffer(s, s->cementBuf, 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, "terrain.cementBuf")) {
            setFallback("cement_dummy_ssbo_failed"); return false;
        }
        s->cementCapBytes = 16;
    }
    // VK-TERRAIN-VISUALDISPLACE-FAR-1: dummy 16B visual-height SSBO so binding 10 is a
    // valid descriptor from frame 1 (visualHeightReady stays false until the sub-gate is
    // on AND the fine bake snapshots -> UBO visualMode stays 0 -> VS never reads it).
    if (!s->visualHeightBuf) {
        if (!make_buffer(s, s->visualHeightBuf, 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, "terrain.visualHeightBuf")) {
            setFallback("visual_height_dummy_ssbo_failed"); return false;
        }
        s->visualHeightCapBytes = 16;
    }
    // VK-TERRAIN-LIGHTING: dummy 1x1 depth-compare shadow (binding 5) so binding 5 is a
    // valid descriptor from frame 1 (shadowReady stays false until MC2_VK_TERRAIN_LIGHTING
    // is on AND the GL static shadow map is live -> FS enableShadows=0 -> lit-only, no hole).
    // VK-TERRAIN-DETAIL: 1x1x1 flat-normal matNormalArray dummy so binding 6 is a valid
    // descriptor from frame 1 (matNormalReady stays false until MC2_VK_TERRAIN_DETAIL is on
    // AND the GL array is live -> FS uses macro-slope-only normal, no hole).
    if (!ensure_dummy_matnormal(s)) { setFallback("matnormal_dummy_failed"); return false; }
    if (!ensure_dummy_concrete_detail(s)) { setFallback("concrete_detail_dummy_failed"); return false; }
    // VK-TERRAIN-CSM-NEAR-1: dummy 1x1(x1) near-cascade array + full-map depth-compare
    // (bindings 8/9) so both are valid descriptors from frame 1 (csmArrayReady/csmFullMapReady
    // stay false until MC2_VK_TERRAIN_CSM is armed AND GL CSM is actually built -> FS
    // csmEnabled=0 -> falls back to the legacy single-map path, no hole).

    // ---- Descriptor layout: binding0 dynamic UBO (VS+FS), binding1 height SSBO (VS). ----
    {
        VkDescriptorSetLayoutBinding b[19]{};
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
        // VK-TERRAIN-DETAIL: binding 6 = material detail-normal sampler2DArray (FS).
        b[5].binding = 6; b[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[5].descriptorCount = 1; b[5].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        // VK-TERRAIN-DYNSHADOW: binding 7 = dynamic shadow sampler2DShadow (FS).
        // VK-TERRAIN-CSM-NEAR-1: binding 8 = near-cascade array sampler2DArrayShadow (FS);
        // binding 9 = full-map cascade sampler2DShadow (FS).
        // VK-TERRAIN-VISUALDISPLACE-FAR-1: binding 10 = visual-height fine-bake SSBO (VS,
        // both main + skirt verts read it for the mode-1/2 displacement; TERRAIN-HIRES-NEAR-1:
        // the frag reads it too, for the fine shading normal).
        b[6].binding = 10; b[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[6].descriptorCount = 1; b[6].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        // VK-PARITY-TERRAIN-2: bindings 11/12/13 = light pool / grid header / grid index SSBOs (FS).
        for (int k = 11; k <= 13; ++k) {
            auto& binding = b[k - 4];
            binding.binding = static_cast<uint32_t>(k); binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
            binding.descriptorCount = 1; binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        // TERRAIN-SHORELINE-V3: binding 14 = shoreline mask sidecar sampler2D (FS).
        b[10].binding = 14; b[10].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[10].descriptorCount = 1; b[10].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        // Material weights, sampled with the macro atlas coordinates (FS).
        b[11].binding = 15; b[11].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[11].descriptorCount = 1; b[11].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        b[12].binding = 16; b[12].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[12].descriptorCount = 1; b[12].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        b[13].binding = 17; b[13].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[13].descriptorCount = 1; b[13].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        b[14].binding=24; b[14].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[14].descriptorCount=1; b[14].stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;
        b[15].binding=25; b[15].descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b[15].descriptorCount=1; b[15].stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;
        b[16].binding = 18; b[16].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[16].descriptorCount = 1; b[16].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        // LOCAL-LIGHTS-2: binding 19 = local-light shadow atlas (D16, plain sampler2D + gather; FS).
        b[17].binding = 19; b[17].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[17].descriptorCount = 1; b[17].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        // U1: binding 26 = the per-frame mission environment (EnvironmentUBO), read by vertex and fragment stages.
        b[18].binding = 26; b[18].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b[18].descriptorCount = 1; b[18].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = 19; lci.pBindings = b;
        if (!mc2vk::create(s->dsl, s->device, [&](auto* h) { return vkCreateDescriptorSetLayout(s->device, &lci, nullptr, h); })) {
            vlog("vkCreateDescriptorSetLayout failed."); return false;
        }
        // VK-TERRAIN-FENCE-DEFER-1: TWO sets (one per parity slot), so pool sizes double.
        VkDescriptorPoolSize sizes[5]{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;  sizes[0].descriptorCount = 1 * mc2vk::kMaxFramesInFlight;
        sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;          sizes[1].descriptorCount = 3 * mc2vk::kMaxFramesInFlight;  // (height + cement + visual-height) x2
        // Image samplers per set: colormap, cement, static shadow, matNormalArray, dyn shadow, csm array, csm full map,
        // shoreline mask, shore field; two sets.
        sizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;  sizes[2].descriptorCount = 13 * mc2vk::kMaxFramesInFlight;   // + local shadow atlas x2
        sizes[3].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;  sizes[3].descriptorCount = 3 * mc2vk::kMaxFramesInFlight;  // light pool + grid header + grid indices, x2
        sizes[4].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; sizes[4].descriptorCount = 2 * mc2vk::kMaxFramesInFlight;   // mover shadow frame (25) + environment (26)
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets = mc2vk::kMaxFramesInFlight; pci.poolSizeCount = 5; pci.pPoolSizes = sizes;
        if (!mc2vk::create(s->pool, s->device, [&](auto* h) { return vkCreateDescriptorPool(s->device, &pci, nullptr, h); })) {
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

    // ---- Pipeline: int16x2 SINT vertex, reverse-Z GEQUAL depth-test+WRITE, cull
    // NONE, frontFace CCW, 2 GBuffer outputs, NO blend (opaque overwrite). State COPIED
    // from the GL terrain contract (== prop tier1 pipeline). ----
    {
        s->vert = load_spv(s->device, spv_dir() + "terrain_tier1.vert.spv");
        s->frag = load_spv(s->device, spv_dir() + "terrain_tier1.frag.spv");
        if (!s->vert || !s->frag) { setFallback("spv_load_failed"); return false; }

        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        const VkDescriptorSetLayout setLayout = s->dsl.get();
        plci.setLayoutCount = 1; plci.pSetLayouts = &setLayout;
        const VkPushConstantRange patchRange{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(uint64_t)};   // device address of the run's first TerrainPatchConstants record
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &patchRange;
        if (!mc2vk::create(s->playout, s->device, [&](auto* h) { return vkCreatePipelineLayout(s->device, &plci, nullptr, h); })) {
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
        for (auto & cbaElem : cba) {
            cbaElem.blendEnable = VK_FALSE;                      // opaque in-place overwrite
            cbaElem.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
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
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = s->vert.get(); stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = s->frag.get(); stages[1].pName = "main";
        // vk-present-instance 8: constant_id 0 = csm debug viz mode (was a per-draw UBO branch).
        const int32_t csmDebugSpec = std::atoi(mc2::gate::str<"MC2_SHADOW_DEBUG_MODE">().c_str());
        const VkSpecializationMapEntry csmDebugEntry{0, 0, sizeof(int32_t)};
        VkSpecializationInfo csmDebugInfo{1, &csmDebugEntry, sizeof(int32_t), &csmDebugSpec};
        stages[1].pSpecializationInfo = &csmDebugInfo;
        VkGraphicsPipelineCreateInfo gp{};
        gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gp.stageCount = 2; gp.pStages = stages;
        gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vps; gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms; gp.pDepthStencilState = &ds; gp.pColorBlendState = &cb;
        gp.pDynamicState = &dsci;
        const VkFormat sceneFmts[2] = { s->colorRes->format, s->normalRes->format };
        const VkPipelineRenderingCreateInfo prci = mc2vk::dynrender::pipelineInfo(2, sceneFmts, s->depthRes->format);
        gp.pNext = &prci;
        gp.layout = s->playout.get(); gp.renderPass = VK_NULL_HANDLE; gp.subpass = 0;
        if (!mc2vk::create(s->pipe, s->device, [&](auto* h) { return mc2vk::infra::graphics(s->device, VK_NULL_HANDLE, 1, &gp, nullptr, h); })) {
            vlog("vkCreateGraphicsPipelines failed."); setFallback("pipeline_failed"); return false;
        }
        mc2vk::nameObject(s->device, s->pipe.get(), "terrain.pipe");

        // Same materials and descriptors; absent MRT1 is not a reflection allocation.
        const int32_t planarSpec[2] = {csmDebugSpec, 1};
        const VkSpecializationMapEntry planarEntries[2] = {{0, 0, sizeof(int32_t)}, {10, sizeof(int32_t), sizeof(int32_t)}};
        const VkSpecializationInfo planarInfo{2, planarEntries, sizeof(planarSpec), planarSpec};
        stages[1].pSpecializationInfo = &planarInfo;
        const VkFormat planarFormats[2] = {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_UNDEFINED};
        const auto planarRendering = mc2vk::dynrender::pipelineInfo(2, planarFormats, VK_FORMAT_D32_SFLOAT);
        gp.pNext = &planarRendering;
        cba[1].colorWriteMask = 0;
        rs.frontFace = VK_FRONT_FACE_CLOCKWISE; // mirror determinant -1 (terrain stays double-sided)
        ds.depthCompareOp = VK_COMPARE_OP_GREATER; // 1.0 is the mask's unconditionally blocked depth
        if (!mc2vk::create(s->planarPipe, s->device, [&](auto* h) { return mc2vk::infra::graphics(s->device, VK_NULL_HANDLE, 1, &gp, nullptr, h); })) return false;
        gp.pNext = &prci;
        stages[1].pSpecializationInfo = &csmDebugInfo;
        cba[1].colorWriteMask = cba[0].colorWriteMask;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;

        // VK-TERRAIN-SKIRT (rung 4): second pipeline, same layout/renderpass/raster/
        // depth/blend state as above -- only the shader modules + vertex-input state
        // differ (2 attributes: ivec2 localOffset + int isSkirtFlag, mirrors GL's
        // skirt VAO glVertexAttribIPointer calls, gos_terrain_lod_chunk.cpp:1744-1748).
        s->skirtVert = load_spv(s->device, spv_dir() + "terrain_tier1_skirt.vert.spv");
        s->skirtFrag = load_spv(s->device, spv_dir() + "terrain_tier1_skirt.frag.spv");
        if (!s->skirtVert || !s->skirtFrag) {
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
        sstages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; sstages[0].module = s->skirtVert.get(); sstages[0].pName = "main";
        sstages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        sstages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; sstages[1].module = s->skirtFrag.get(); sstages[1].pName = "main";

        VkGraphicsPipelineCreateInfo sgp{};
        sgp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        sgp.stageCount = 2; sgp.pStages = sstages;
        sgp.pVertexInputState = &svin; sgp.pInputAssemblyState = &ia;
        sgp.pViewportState = &vps; sgp.pRasterizationState = &rs;
        sgp.pMultisampleState = &ms; sgp.pDepthStencilState = &ds; sgp.pColorBlendState = &cb;
        sgp.pDynamicState = &dsci;
        sgp.pNext = &prci;
        sgp.layout = s->playout.get(); sgp.renderPass = VK_NULL_HANDLE; sgp.subpass = 0;
        if (!mc2vk::create(s->skirtPipe, s->device, [&](auto* h) { return mc2vk::infra::graphics(s->device, VK_NULL_HANDLE, 1, &sgp, nullptr, h); })) {
            vlog("vkCreateGraphicsPipelines (skirt) failed."); setFallback("pipeline_failed"); return false;
        }
        mc2vk::nameObject(s->device, s->skirtPipe.get(), "terrain.skirtPipe");
    }

    s->inited = true;
    setFallback("");
    vlog("island initialized %dx%d (shared GBuffer co-write; LOD0 main-triangle terrain "
         "prefix; reverse-Z GEQUAL double-sided; gbuffer1.a=1.0).", w, h);
    return true;
}

} // namespace

std::optional<Mc2GroundLightingFrame> mc2_vk_ground_lighting_frame() {
    if (!mc2vk::engineVulkanContextValid()) return std::nullopt;
    const auto* s = vk_terrain_detail::parkedIsland();
    if (!s || !s->inited || !s->groundFrame.set ||
        s->groundFrameNo != mc2vk::getOrCreateEngineVulkanContext().tracker().frameNo()) return std::nullopt;
    return s->groundFrame;
}

