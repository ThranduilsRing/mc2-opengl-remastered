// vk_native_producers.cpp -- see vk_native_producers.h.

#ifdef MC2_VULKAN_ISLAND

#include "vk_native_producers.h"

#include "vk_island_util.h"
#include "vulkan_context.h"

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include <vk_mem_alloc.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace mc2vk {

namespace {
void plog(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vk_island_util::vlogv("[VK_PRODUCERS] ", fmt, ap);
    va_end(ap);
}
constexpr VkBufferUsageFlags kStorage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
constexpr VkBufferUsageFlags kUniform = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
struct ThinRec { uint32_t w[12]; };
}  // namespace

bool NativeProducers::makeDevice(OwnedBuffer& buf, VkDeviceSize size, VkBufferUsageFlags usage, const char* name, int i0) {
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size ? size : 16;
    bci.usage = usage;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;   // no host-access flag: device-local
    if (!buf.create(allocator_, device_, bci, aci, name, i0)) {
        plog("device buffer '%s' failed (%llu bytes).", name, static_cast<unsigned long long>(size));
        return false;
    }
    return true;
}

bool NativeProducers::growHost(OwnedBuffer& buf, VkDeviceSize& cap, VkDeviceSize need, VkBufferUsageFlags usage, const char* name, uint32_t par) {
    if (buf && need <= cap) return false;
    if (buf) ctx_->waitIdle("producers_host_grow");
    const VkDeviceSize newCap = std::max<VkDeviceSize>(need + need / 4 + 256, 256);
    buf.reset();
    cap = 0;
    if (!buf.createHostWritable(allocator_, device_, newCap, usage, name, static_cast<int>(par))) {
        plog("host buffer '%s' failed (%llu bytes).", name, static_cast<unsigned long long>(newCap));
        return false;
    }
    cap = newCap;
    return true;
}

bool NativeProducers::init(const VulkanContext& ctx, uint32_t numVertices, uint32_t recipeCount, VkBuffer recipes) {
    ready_ = false;
    ctx_ = &ctx;
    device_ = ctx.device();
    allocator_ = ctx.allocator();
    numVertices_ = numVertices;
    recipeCount_ = recipeCount;
    recipes_ = recipes;
    maxThin_ = std::min<uint32_t>(recipeCount, kMaxThin);
    if (device_ == VK_NULL_HANDLE || !allocator_ || numVertices == 0 || recipeCount == 0 || recipes == VK_NULL_HANDLE) return false;

    const std::string dir = vk_island_util::spvDir();
    const VkDescriptorType S = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, U = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    if (!lightP_.create(device_, dir + "terrain_lighting.comp.spv", {{0, S}, {1, S}, {2, S}, {3, U}}, "producers.terrainLighting")) return false;
    if (!cullP_.create(device_, dir + "water_cull_pack.comp.spv", {{0, S}, {1, S}, {2, S}, {3, U}, {4, S}}, "producers.waterCull")) return false;
    if (!argsP_.create(device_, dir + "water_draw_args.comp.spv", {{0, S}, {1, S}, {2, U}}, "producers.waterArgs")) return false;
    if (!lightSets_.create(device_, lightP_, kSlots, "producers.lightSets") || !cullSets_.create(device_, cullP_, kSlots, "producers.cullSets") ||
        !argsSets_.create(device_, argsP_, kSlots, "producers.argsSets")) return false;

    const VkBufferUsageFlags dst = VK_BUFFER_USAGE_TRANSFER_DST_BIT, src = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (!makeDevice(lightOut_, static_cast<VkDeviceSize>(numVertices) * 8u, kStorage | dst | src, "producers.lightOut")) return false;
    for (uint32_t p = 0; p < kSlots; ++p) {
        if (!lightUbo_[p].createHostWritable(allocator_, device_, sizeof(VkTerrainLightingParams), kUniform, "producers.lightUbo", static_cast<int>(p)) ||
            !cullUbo_[p].createHostWritable(allocator_, device_, sizeof(VkWaterCullParams), kUniform, "producers.cullUbo", static_cast<int>(p)) ||
            !argsUbo_[p].createHostWritable(allocator_, device_, sizeof(VkDrawArgsParams), kUniform, "producers.argsUbo", static_cast<int>(p))) return false;
        VkDrawArgsParams ap{{static_cast<int32_t>(maxThin_), 6, 0, 0}};
        if (!argsUbo_[p].upload(&ap, sizeof(ap))) return false;
        growHost(lightIn_[p], lightInCap_[p], 4096, kStorage, "producers.lightIn", p);
        growHost(lights_[p], lightsCap_[p], 48 * 64, kStorage, "producers.lights", p);
        if (!lightIn_[p] || !lights_[p]) return false;
        if (!makeDevice(header_[p], 16, kStorage | dst | src, "producers.header", static_cast<int>(p)) ||
            !makeDevice(drawArgs_[p], 16, kStorage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, "producers.drawArgs", static_cast<int>(p)) ||
            !makeDevice(thin_[p], static_cast<VkDeviceSize>(maxThin_) * 48u, kStorage | src, "producers.thin", static_cast<int>(p))) return false;
    }
    writeAll();
    ready_ = true;
    plog("native producers ready: vertices=%u recipes=%u maxThin=%u (Slang compute: terrain_lighting, water_cull_pack, water_draw_args).",
         numVertices, recipeCount, maxThin_);
    return true;
}

void NativeProducers::writeAll() {
    const VkDescriptorType S = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, U = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    for (uint32_t p = 0; p < kSlots; ++p) {
        lightSets_.write(device_, p, 0, S, lightIn_[p].get());
        lightSets_.write(device_, p, 1, S, lights_[p].get());
        lightSets_.write(device_, p, 2, S, lightOut_.get());
        lightSets_.write(device_, p, 3, U, lightUbo_[p].get());
        cullSets_.write(device_, p, 0, S, recipes_);
        cullSets_.write(device_, p, 1, S, lightOut_.get());
        cullSets_.write(device_, p, 2, S, thin_[p].get());
        cullSets_.write(device_, p, 3, U, cullUbo_[p].get());
        cullSets_.write(device_, p, 4, S, header_[p].get());
        argsSets_.write(device_, p, 0, S, header_[p].get());
        argsSets_.write(device_, p, 1, S, drawArgs_[p].get());
        argsSets_.write(device_, p, 2, U, argsUbo_[p].get());
    }
}

bool NativeProducers::record(VkCommandBuffer cb, uint32_t par, const gos_terrain_lighting::VkProducerSnapshot& L, const WaterCullFrame& W, bool parity) {
    if (!ready_) return false;
    par &= 1u;

    // ---- stage the slot's inputs (the slot is retired by the island's frame-slot fence) ----
    bool regrow = false;
    regrow |= growHost(lightIn_[par], lightInCap_[par], static_cast<VkDeviceSize>(L.windowCount) * sizeof(gos_terrain_lighting::GpuTerrainVertexInput), kStorage, "producers.lightIn", par);
    regrow |= growHost(lights_[par], lightsCap_[par], static_cast<VkDeviceSize>(std::max(L.numLights, 1u)) * sizeof(gos_terrain_lighting::GpuTerrainLight), kStorage, "producers.lights", par);
    if (!lightIn_[par] || !lights_[par]) return false;
    if (regrow) writeAll();   // growHost idled the device
    if (L.windowCount > 0 && !lightIn_[par].upload(L.window, static_cast<VkDeviceSize>(L.windowCount) * sizeof(gos_terrain_lighting::GpuTerrainVertexInput))) return false;
    if (L.numLights > 0 && !lights_[par].upload(L.lights, static_cast<VkDeviceSize>(L.numLights) * sizeof(gos_terrain_lighting::GpuTerrainLight))) return false;

    VkTerrainLightingParams lp{};
    lp.counts[0] = static_cast<int32_t>(L.numVertices); lp.counts[1] = static_cast<int32_t>(L.numLights);
    lp.counts[2] = static_cast<int32_t>(L.vertexOffset); lp.counts[3] = static_cast<int32_t>(L.windowCount);
    for (int i = 0; i < 3; ++i) { lp.sunRgb[i] = L.lightRgb[i]; lp.ambRgb[i] = L.ambientRgb[i]; lp.baseRgb[i] = L.baseRgb[i]; lp.sunDirRain[i] = L.sunDir[i]; lp.camDist[i] = L.camPos[i]; }
    lp.sunRgb[3] = L.rendererSW; lp.ambRgb[3] = L.lightening; lp.baseRgb[3] = L.useFog; lp.sunDirRain[3] = L.rain;
    lp.fogHaze[0] = L.fogStart; lp.fogHaze[1] = L.fogFull; lp.fogHaze[2] = L.minHaze; lp.fogHaze[3] = L.maxClip;
    for (int i = 0; i < 4; ++i) lp.playArea[i] = L.playArea[i];
    lp.camDist[3] = L.distFactor;
    if (!lightUbo_[par].upload(&lp, sizeof(lp))) return false;

    VkWaterCullParams wp{};
    std::memcpy(wp.clip, W.mvp, sizeof(wp.clip));
    wp.misc0[0] = W.waterElevation; wp.misc0[1] = W.frameCos; wp.misc0[2] = W.shoreExt;
    wp.misc1[0] = static_cast<int32_t>(recipeCount_); wp.misc1[1] = static_cast<int32_t>(maxThin_);
    wp.misc1[2] = static_cast<int32_t>(W.mapSide); wp.misc1[3] = W.biasLegacy;
    if (!cullUbo_[par].upload(&wp, sizeof(wp))) return false;

    // ---- record ----
    // Hand-off from earlier frames' work on this queue (their cull read lightOut; the vertex stage read thin / recipes).
    cmdBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdFillBuffer(cb, header_[par].get(), 0, 16, 0);
    if (!lightOutCleared_) {
        vkCmdFillBuffer(cb, lightOut_.get(), 0, VK_WHOLE_SIZE, 0);
        lightOutCleared_ = true;
    }
    cmdBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

    if (L.windowCount > 0) {
        lightP_.bind(cb, lightSets_.set(par));
        lightP_.dispatch(cb, (L.windowCount + 63u) / 64u);
        cmdBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
    cullP_.bind(cb, cullSets_.set(par));
    cullP_.dispatch(cb, (recipeCount_ + 63u) / 64u);
    cmdBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    argsP_.bind(cb, argsSets_.set(par));
    argsP_.dispatch(cb, 1);

    VkPipelineStageFlags toStages = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
    VkAccessFlags toAccess = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
    if (parity) { toStages |= VK_PIPELINE_STAGE_TRANSFER_BIT; toAccess |= VK_ACCESS_TRANSFER_READ_BIT; }
    cmdBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, toStages, toAccess);

    if (parity) {
        const VkDeviceSize thinBytes = static_cast<VkDeviceSize>(std::min(maxThin_, kParityCap)) * 48u;
        if (!parityHdr_) parityHdr_.createHostWritable(allocator_, device_, 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, "producers.parityHdr");
        if (!parityThin_) parityThin_.createHostWritable(allocator_, device_, thinBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, "producers.parityThin");
        parityLightFirst_ = L.vertexOffset;
        parityLightCount_ = L.windowCount;
        if (L.windowCount > 0) {
            parityLight_.reset();
            parityLight_.createHostWritable(allocator_, device_, static_cast<VkDeviceSize>(L.windowCount) * 8u, VK_BUFFER_USAGE_TRANSFER_DST_BIT, "producers.parityLight");
        }
        if (parityHdr_ && parityThin_) {
            VkBufferCopy c{0, 0, 16};
            vkCmdCopyBuffer(cb, header_[par].get(), parityHdr_.get(), 1, &c);
            VkBufferCopy t{0, 0, thinBytes};
            vkCmdCopyBuffer(cb, thin_[par].get(), parityThin_.get(), 1, &t);
        }
        if (parityLight_ && L.windowCount > 0) {
            VkBufferCopy l{static_cast<VkDeviceSize>(L.vertexOffset) * 8u, 0, static_cast<VkDeviceSize>(L.windowCount) * 8u};
            vkCmdCopyBuffer(cb, lightOut_.get(), parityLight_.get(), 1, &l);
        }
        cmdBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    }
    return true;
}

ProducerParity NativeProducers::compare(uint32_t, const void* glThin, uint32_t glThinCount, const gos_terrain_lighting::GpuTerrainLightingOutput* glLight,
                                        uint32_t lightFirst, uint32_t lightCount) {
    ProducerParity r;
    r.glCount = glThinCount;
    if (!parityHdr_ || !parityThin_) return r;

    uint32_t hdr[4] = {0, 0, 0, 0};
    if (void* p = parityHdr_.map()) { std::memcpy(hdr, p, 16); parityHdr_.unmap(); }
    r.vkVisible = hdr[0];
    r.vkCompared = std::min({hdr[0], maxThin_, kParityCap});
    r.truncated = hdr[0] > r.vkCompared || glThinCount > kParityCap;

    std::vector<ThinRec> vk(r.vkCompared);
    if (r.vkCompared) {
        if (void* p = parityThin_.map()) { std::memcpy(vk.data(), p, vk.size() * sizeof(ThinRec)); parityThin_.unmap(); }
    }
    std::vector<ThinRec> gl(glThinCount);
    if (glThinCount && glThin) std::memcpy(gl.data(), glThin, gl.size() * sizeof(ThinRec));
    auto byRecipe = [](const ThinRec& a, const ThinRec& b) { return a.w[0] < b.w[0]; };
    std::sort(vk.begin(), vk.end(), byRecipe);
    std::sort(gl.begin(), gl.end(), byRecipe);
    size_t i = 0, j = 0;
    while (i < gl.size() || j < vk.size()) {
        if (j >= vk.size() || (i < gl.size() && gl[i].w[0] < vk[j].w[0])) { ++r.onlyGl; ++i; }
        else if (i >= gl.size() || vk[j].w[0] < gl[i].w[0]) { ++r.onlyVk; ++j; }
        else {
            if (std::memcmp(&gl[i], &vk[j], sizeof(ThinRec)) == 0) ++r.same; else ++r.differ;
            ++i; ++j;
        }
    }

    if (glLight && parityLight_ && lightFirst == parityLightFirst_ && lightCount == parityLightCount_ && lightCount > 0) {
        std::vector<gos_terrain_lighting::GpuTerrainLightingOutput> vl(lightCount);
        if (void* p = parityLight_.map()) { std::memcpy(vl.data(), p, vl.size() * sizeof(vl[0])); parityLight_.unmap(); }
        r.lightCompared = lightCount;
        for (uint32_t k = 0; k < lightCount; ++k) {
            if (vl[k].lightRGB != glLight[k].lightRGB) ++r.lightMismatch;
            if (vl[k].fogRGB != glLight[k].fogRGB) ++r.fogMismatch;
            for (int sh = 0; sh < 32; sh += 8) {
                const int a = static_cast<int>((vl[k].lightRGB >> sh) & 0xFFu), b = static_cast<int>((glLight[k].lightRGB >> sh) & 0xFFu);
                const int c = static_cast<int>((vl[k].fogRGB >> sh) & 0xFFu), d = static_cast<int>((glLight[k].fogRGB >> sh) & 0xFFu);
                r.lightMaxDelta = std::max<uint32_t>(r.lightMaxDelta, static_cast<uint32_t>(std::max(std::abs(a - b), std::abs(c - d))));
            }
        }
    }
    return r;
}

}  // namespace mc2vk

#endif  // MC2_VULKAN_ISLAND
