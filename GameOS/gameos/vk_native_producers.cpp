// vk_native_producers.cpp -- see vk_native_producers.h.

#ifdef MC2_VULKAN_ISLAND

#include "vk_native_producers.h"

#include "vk_island_util.h"
#include "vk_one_shot.h"
#include "vk_gpu_profile.h"   // VK-GPU-ZONES: MC2_VK_GPU_SCOPE
#include "vulkan_context.h"
#include "mc2_diag_verbose.h"

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include <vk_mem_alloc.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
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

bool NativeProducers::prewarmPipelines(const VulkanContext& ctx) {
    ctx_ = &ctx;
    device_ = ctx.device();
    allocator_ = ctx.allocator();
    if (!device_ || !allocator_) return false;
    if (lightP_ && cullP_ && argsP_ && lightSets_.set(kSlots-1) && cullSets_.set(kSlots-1) && argsSets_.set(kSlots-1)) return true;
    const std::string dir = vk_island_util::spvDir();
    const VkDescriptorType S = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, U = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    const VkDescriptorType SD = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, UD = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;   // frame-ring bindings
    if (!lightP_.create(device_, dir + "terrain_lighting.comp.spv", {{0, SD}, {1, SD}, {2, S}, {3, UD}}, "producers.terrainLighting")) return false;
    if (!cullP_.create(device_, dir + "water_cull_pack.comp.spv", {{0, S}, {1, S}, {2, S}, {3, UD}, {4, S}}, "producers.waterCull")) return false;
    if (!argsP_.create(device_, dir + "water_draw_args.comp.spv", {{0, S}, {1, S}, {2, U}}, "producers.waterArgs")) return false;
    if (!lightSets_.create(device_, lightP_, kSlots, "producers.lightSets") || !cullSets_.create(device_, cullP_, kSlots, "producers.cullSets") ||
        !argsSets_.create(device_, argsP_, kSlots, "producers.argsSets")) return false;
    return true;
}

bool NativeProducers::init(const VulkanContext& ctx, uint32_t numVertices, uint32_t recipeCount, VkBuffer recipes, uint32_t waterEligibleBound) {
    ready_ = false;
    numVertices_ = numVertices;
    recipeCount_ = recipeCount;
    recipes_ = recipes;
    // Capacity follows the map: the cull emits only water-eligible quads, so the bound (not the recipe total) is what must fit.
    maxThin_ = std::min<uint32_t>(recipeCount, std::clamp<uint32_t>(waterEligibleBound, kMaxThin, kMaxThinCeiling));
    if (numVertices == 0 || recipeCount == 0 || recipes == VK_NULL_HANDLE || !prewarmPipelines(ctx)) return false;

    const VkBufferUsageFlags dst = VK_BUFFER_USAGE_TRANSFER_DST_BIT, src = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (!makeDevice(lightOut_, static_cast<VkDeviceSize>(numVertices) * 8u, kStorage | dst | src, "producers.lightOut")) return false;
    for (uint32_t p = 0; p < kSlots; ++p) {
        if (!argsUbo_[p].createHostWritable(allocator_, device_, sizeof(VkDrawArgsParams), kUniform, "producers.argsUbo", static_cast<int>(p))) return false;
        VkDrawArgsParams ap{{static_cast<int32_t>(maxThin_), 6, 0, 0}};
        if (!argsUbo_[p].upload(&ap, sizeof(ap))) return false;
        if (!makeDevice(header_[p], 16, kStorage | dst | src, "producers.header", static_cast<int>(p)) ||
            !makeDevice(drawArgs_[p], 16, kStorage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, "producers.drawArgs", static_cast<int>(p)) ||
            !makeDevice(thin_[p], static_cast<VkDeviceSize>(maxThin_) * 48u, kStorage | src, "producers.thin", static_cast<int>(p))) return false;
    }
    writeAll();
    {   // the ring bindings need a valid buffer from the start; record() re-points them at the span of its frame
        VulkanContext& rctx = getOrCreateEngineVulkanContext();
        if (const VkBuffer ring = rctx.frameRingBuffer()) {
            for (uint32_t p = 0; p < kSlots; ++p) { ringBound_[p] = VK_NULL_HANDLE; syncInputs(p, ring, rctx.frameRingRange()); }
        } else {
            return false;
        }
        for (uint32_t p = 0; p < kSlots; ++p) ringBound_[p] = VK_NULL_HANDLE;
    }
    ready_ = true;
    // Warn only when quads the cull can really emit may not fit (a bound above the capacity), not on the raw recipe total.
    const uint32_t needed = std::min(waterEligibleBound, recipeCount);   // 0 = bound unknown: nothing to warn about
    if (needed > maxThin_)
        plog("WARNING: up to %u water quads can be emitted but the native thin capacity is %u (VRAM ceiling %u): the surplus quads are dropped.", needed, maxThin_, kMaxThinCeiling);
    plog("thin capacity %u records/slot (%.1f MB x %u slots): recipes=%u waterEligibleBound=%u", maxThin_,
         static_cast<double>(maxThin_) * 48.0 / (1024.0 * 1024.0), kSlots, recipeCount, waterEligibleBound);
    if (const uint32_t bad = selfTestLighting(); bad != 0) {
        // A lighting mismatch against the reference transcription means this driver/compile differs from GL: do not ship it.
        plog("native lighting self-test FAILED (%u mismatches): native producers refused, the GL cull/pack feed stays in use.", bad);
        ready_ = false;
        return false;
    }
    plog("native producers ready: vertices=%u recipes=%u maxThin=%u (Slang compute: terrain_lighting, water_cull_pack, water_draw_args).",
         numVertices, recipeCount, maxThin_);
    return true;
}

namespace {
// Transcription of quad.cpp:1266-1426 as ported in shaders/gos_terrain_lighting.comp (same operation order, float math).
uint32_t refChannel(float intensity, uint32_t lightChan, uint32_t amb) {
    if (intensity > 0.0f) {
        int r = static_cast<int>(static_cast<float>(lightChan) * intensity + static_cast<float>(amb));
        if (r >= 255) return 255u;
        if (r < 0) return 0u;
        return static_cast<uint32_t>(r);
    }
    return amb;
}

void refLight(const gos_terrain_lighting::GpuTerrainVertexInput& v, const gos_terrain_lighting::GpuTerrainLight* lights, const VkTerrainLightingParams& P,
              uint32_t& outLight, uint32_t& outFog) {
    uint32_t lr = 255, lg = 255, lb = 255, sr = 0, sg = 0, sb = 0;
    if (P.sunRgb[3] == 0) {
        float li = v.normal[0] * P.sunDirRain[0] + v.normal[1] * P.sunDirRain[1] + v.normal[2] * P.sunDirRain[2];
        if ((v.flags & 1u) && li > 0.2f) li = 0.2f;
        lr = refChannel(li, static_cast<uint32_t>(P.sunRgb[0]), static_cast<uint32_t>(P.ambRgb[0]));
        lg = refChannel(li, static_cast<uint32_t>(P.sunRgb[1]), static_cast<uint32_t>(P.ambRgb[1]));
        lb = refChannel(li, static_cast<uint32_t>(P.sunRgb[2]), static_cast<uint32_t>(P.ambRgb[2]));
        if (P.baseRgb[0] != 0 || P.baseRgb[1] != 0 || P.baseRgb[2] != 0) {
            lr = std::min(lr + static_cast<uint32_t>(P.baseRgb[0]), 255u);
            lg = std::min(lg + static_cast<uint32_t>(P.baseRgb[1]), 255u);
            lb = std::min(lb + static_cast<uint32_t>(P.baseRgb[2]), 255u);
        }
        const float rain = P.sunDirRain[3];
        if (rain < 1.0f) {
            lr = static_cast<uint32_t>(static_cast<float>(lr) * rain);
            lb = static_cast<uint32_t>(static_cast<float>(lb) * rain);
            lg = static_cast<uint32_t>(static_cast<float>(lg) * rain);
        }
        if (P.ambRgb[3] > 0) sr = sg = sb = static_cast<uint32_t>(P.ambRgb[3]);
        for (int i = 0; i < P.counts[1]; ++i) {
            const auto& L = lights[i];
            if (L.lightType != 1u && L.lightType != 2u && L.lightType != 3u) continue;
            const float dx = v.xy[0] - L.position[0], dy = v.xy[1] - L.position[1], dz = v.elevation - L.position[2];
            const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
            float fo = 1.0f;
            if (len <= L.closeDistance) fo = 1.0f;
            else if (len >= L.farDistance) continue;
            else fo = (L.farDistance - len) * L.oneOverDistance;
            sr = std::min(sr + static_cast<uint32_t>(L.color[0] * 255.0f * fo), 255u);
            sg = std::min(sg + static_cast<uint32_t>(L.color[1] * 255.0f * fo), 255u);
            sb = std::min(sb + static_cast<uint32_t>(L.color[2] * 255.0f * fo), 255u);
        }
    }
    outLight = (0xFFu << 24) | (lr << 16) | (lg << 8) | lb;
    outFog = (0xFFu << 24) | (sr << 16) | (sg << 8) | sb;
    uint32_t fogResult = 0xFFu;
    const float fs = P.fogHaze[0], ff = P.fogHaze[1];
    if (P.sunRgb[3] == 0 && P.baseRgb[3] != 0) {
        if (v.elevation < fs) {
            float fogFactor = fs - v.elevation;
            if (fogFactor < 0.0f || fs == ff) fogResult = 0xFFu;
            else {
                fogFactor /= (fs - ff);
                if (fogFactor <= 1.0f) { fogFactor = fogFactor * fogFactor; fogFactor = 1.0f - fogFactor; fogFactor *= 256.0f; }
                else fogFactor = 256.0f;
                fogResult = static_cast<uint32_t>(static_cast<int>(fogFactor));
            }
        }
        outFog = (fogResult << 24) | (sr << 16) | (sg << 8) | sb;
    }
    float haze;
    const bool outside = v.xy[0] <= P.playArea[0] || v.xy[0] >= P.playArea[2] || v.xy[1] <= P.playArea[1] || v.xy[1] >= P.playArea[3];
    if (outside) haze = 1.0f;
    else {
        const float dx = v.xy[0] - P.camDist[0], dy = v.xy[1] - P.camDist[1], dz = v.elevation - P.camDist[2];
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d > P.fogHaze[3]) haze = 1.0f;
        else if (d > P.fogHaze[2]) haze = (d - P.fogHaze[2]) * P.camDist[3];
        else haze = 0.0f;
    }
    if (haze != 0.0f) {
        const float fogFactor = 1.0f - haze;
        const uint32_t distFog = static_cast<uint32_t>(static_cast<int>(fogFactor * 255.0f));
        if (distFog < fogResult) fogResult = distFog;
        outFog = (fogResult << 24) | (sr << 16) | (sg << 8) | sb;
    }
}
}  // namespace

uint32_t NativeProducers::selfTestLighting() {
    using namespace gos_terrain_lighting;
    constexpr uint32_t N = 4096, NL = 8, kFirst = 100;
    if (numVertices_ < kFirst + N) { plog("[VK_PRODUCER_SELFTEST v1] lighting skipped (map too small: %u vertices).", numVertices_); return 0; }
    uint32_t seed = 12345u;
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return static_cast<float>((seed >> 8) & 0xFFFFu) / 65535.0f; };
    std::vector<GpuTerrainVertexInput> vin(N);
    for (auto& v : vin) {
        v.xy[0] = (rnd() - 0.5f) * 2400.0f; v.xy[1] = (rnd() - 0.5f) * 2400.0f; v.elevation = rnd() * 300.0f; v.hazeFactor = 0.0f;
        float nx = rnd() - 0.5f, ny = rnd() - 0.5f, nz = rnd() * 0.8f + 0.2f;
        const float il = 1.0f / std::sqrt(nx * nx + ny * ny + nz * nz);
        v.normal[0] = nx * il; v.normal[1] = ny * il; v.normal[2] = nz * il;
        v.flags = rnd() < 0.3f ? 1u : 0u;
    }
    std::vector<GpuTerrainLight> lights(NL);
    for (uint32_t i = 0; i < NL; ++i) {
        auto& L = lights[i];
        L.position[0] = (rnd() - 0.5f) * 2000.0f; L.position[1] = (rnd() - 0.5f) * 2000.0f; L.position[2] = rnd() * 200.0f;
        L.lightType = (i % 4) + 0u;   // includes type 0 (ignored)
        L.color[0] = rnd(); L.color[1] = rnd(); L.color[2] = rnd();
        L.closeDistance = rnd() * 30.0f; L.farDistance = L.closeDistance + 50.0f + rnd() * 500.0f; L.oneOverDistance = 1.0f / (L.farDistance - L.closeDistance);
        L._pad1 = L._pad2 = 0.0f;
    }
    VkTerrainLightingParams P{};
    P.counts[0] = static_cast<int32_t>(numVertices_); P.counts[1] = NL; P.counts[2] = kFirst; P.counts[3] = N;
    P.sunRgb[0] = 250; P.sunRgb[1] = 240; P.sunRgb[2] = 220; P.sunRgb[3] = 0;
    P.ambRgb[0] = 30; P.ambRgb[1] = 32; P.ambRgb[2] = 40; P.ambRgb[3] = 7;
    P.baseRgb[0] = 3; P.baseRgb[1] = 0; P.baseRgb[2] = 5; P.baseRgb[3] = 1;
    P.sunDirRain[0] = 0.3f; P.sunDirRain[1] = -0.4f; P.sunDirRain[2] = std::sqrt(1.0f - 0.09f - 0.16f); P.sunDirRain[3] = 0.85f;
    P.fogHaze[0] = 200.0f; P.fogHaze[1] = 20.0f; P.fogHaze[2] = 400.0f; P.fogHaze[3] = 1500.0f;
    P.playArea[0] = -1100.0f; P.playArea[1] = -1100.0f; P.playArea[2] = 1100.0f; P.playArea[3] = 1100.0f;
    P.camDist[0] = 10.0f; P.camDist[1] = -20.0f; P.camDist[2] = 150.0f; P.camDist[3] = 1.0f / 1100.0f;

    const uint32_t par = 0;
    VulkanContext& rctx = getOrCreateEngineVulkanContext();
    const VkDeviceSize al = rctx.frameAlignment();
    const VkDeviceSize offIn = alignUp(sizeof(P), al);
    const VkDeviceSize offLights = alignUp(offIn + sizeof(GpuTerrainVertexInput) * N, al);
    const FrameAlloc span = rctx.frameAlloc(offLights + sizeof(GpuTerrainLight) * NL);
    OwnedBuffer rb;
    if (!span || !rb.createHostWritable(allocator_, device_, static_cast<VkDeviceSize>(N) * 8u, VK_BUFFER_USAGE_TRANSFER_DST_BIT, "producers.selfTest")) {
        plog("[VK_PRODUCER_SELFTEST v1] lighting skipped (buffers).");
        return 0;
    }
    std::memcpy(span.ptr, &P, sizeof(P));
    std::memcpy(static_cast<uint8_t*>(span.ptr) + offIn, vin.data(), sizeof(GpuTerrainVertexInput) * N);
    std::memcpy(static_cast<uint8_t*>(span.ptr) + offLights, lights.data(), sizeof(GpuTerrainLight) * NL);
    ringBound_[par] = VK_NULL_HANDLE;
    syncInputs(par, span.buffer, span.range);
    const uint32_t offsets[3] = {static_cast<uint32_t>(span.offset + offIn), static_cast<uint32_t>(span.offset + offLights), static_cast<uint32_t>(span.offset)};
    const bool ok = submitOneShot(*ctx_, [&](VkCommandBuffer cb) {
        lightP_.bind(cb, lightSets_.set(par), offsets);
        lightP_.dispatch(cb, (N + 63u) / 64u);
        cmdBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferCopy c{static_cast<VkDeviceSize>(kFirst) * 8u, 0, static_cast<VkDeviceSize>(N) * 8u};
        vkCmdCopyBuffer(cb, lightOut_.get(), rb.get(), 1, &c);
        cmdBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    }, "producers_selftest");
    lightOutCleared_ = false;   // the self-test wrote real values: the first frame refills the white sentinel
    if (!ok) { plog("[VK_PRODUCER_SELFTEST v1] lighting skipped (submit)."); return 0; }
    std::vector<GpuTerrainLightingOutput> got(N);
    if (void* p = rb.map()) { std::memcpy(got.data(), p, got.size() * sizeof(got[0])); rb.unmap(); }
    uint32_t bad = 0, maxDelta = 0;
    for (uint32_t i = 0; i < N; ++i) {
        uint32_t el, ef;
        refLight(vin[i], lights.data(), P, el, ef);
        if (el != got[i].lightRGB || ef != got[i].fogRGB) ++bad;
        for (int sh = 0; sh < 32; sh += 8) {
            maxDelta = std::max<uint32_t>(maxDelta, static_cast<uint32_t>(std::abs(static_cast<int>((el >> sh) & 255u) - static_cast<int>((got[i].lightRGB >> sh) & 255u))));
            maxDelta = std::max<uint32_t>(maxDelta, static_cast<uint32_t>(std::abs(static_cast<int>((ef >> sh) & 255u) - static_cast<int>((got[i].fogRGB >> sh) & 255u))));
        }
    }
    if (mc2DiagVerbose()) plog("[VK_PRODUCER_SELFTEST v1] lighting n=%u lights=%u mismatches=%u max_channel_delta=%u (Slang compute vs C++ transcription of quad.cpp)", N, NL, bad, maxDelta);
    return bad;
}

void NativeProducers::writeAll() {
    const VkDescriptorType S = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, U = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    for (uint32_t p = 0; p < kSlots; ++p) {
        lightSets_.write(device_, p, 2, S, lightOut_.get());
        cullSets_.write(device_, p, 0, S, recipes_);
        cullSets_.write(device_, p, 1, S, lightOut_.get());
        cullSets_.write(device_, p, 2, S, thin_[p].get());
        cullSets_.write(device_, p, 4, S, header_[p].get());
        argsSets_.write(device_, p, 0, S, header_[p].get());
        argsSets_.write(device_, p, 1, S, drawArgs_[p].get());
        argsSets_.write(device_, p, 2, U, argsUbo_[p].get());
    }
}

void NativeProducers::syncInputs(uint32_t p, VkBuffer buffer, VkDeviceSize range) {
    if (ringBound_[p] == buffer) return;
    lightSets_.write(device_, p, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, buffer, range);
    lightSets_.write(device_, p, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, buffer, range);
    lightSets_.write(device_, p, 3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, buffer, sizeof(VkTerrainLightingParams));
    cullSets_.write(device_, p, 3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, buffer, sizeof(VkWaterCullParams));
    ringBound_[p] = buffer;
}

bool NativeProducers::record(VkCommandBuffer cb, uint32_t par, const gos_terrain_lighting::VkProducerSnapshot& L, const WaterCullFrame& W, bool parity) {
    if (!ready_) return false;
    par &= 1u;

    // ---- stage the frame's inputs: one frame-ring span holding both parameter blocks, the lighting window and the lights ----
    VkTerrainLightingParams lp{};
    lp.counts[0] = static_cast<int32_t>(L.numVertices); lp.counts[1] = static_cast<int32_t>(L.numLights);
    lp.counts[2] = static_cast<int32_t>(L.vertexOffset); lp.counts[3] = static_cast<int32_t>(L.windowCount);
    for (int i = 0; i < 3; ++i) { lp.sunRgb[i] = L.lightRgb[i]; lp.ambRgb[i] = L.ambientRgb[i]; lp.baseRgb[i] = L.baseRgb[i]; lp.sunDirRain[i] = L.sunDir[i]; lp.camDist[i] = L.camPos[i]; }
    lp.sunRgb[3] = L.rendererSW; lp.ambRgb[3] = L.lightening; lp.baseRgb[3] = L.useFog; lp.sunDirRain[3] = L.rain;
    lp.fogHaze[0] = L.fogStart; lp.fogHaze[1] = L.fogFull; lp.fogHaze[2] = L.minHaze; lp.fogHaze[3] = L.maxClip;
    for (int i = 0; i < 4; ++i) lp.playArea[i] = L.playArea[i];
    lp.camDist[3] = L.distFactor;

    VkWaterCullParams wp{};
    std::memcpy(wp.clip, W.mvp, sizeof(wp.clip));
    wp.misc0[0] = W.waterElevation; wp.misc0[1] = W.frameCos; wp.misc0[2] = W.shoreExt;
    wp.misc1[0] = static_cast<int32_t>(recipeCount_); wp.misc1[1] = static_cast<int32_t>(maxThin_);
    wp.misc1[2] = static_cast<int32_t>(W.mapSide); wp.misc1[3] = W.biasLegacy;

    VulkanContext& rctx = getOrCreateEngineVulkanContext();
    const VkDeviceSize al = rctx.frameAlignment();
    const VkDeviceSize inBytes = static_cast<VkDeviceSize>(L.windowCount) * sizeof(gos_terrain_lighting::GpuTerrainVertexInput);
    const VkDeviceSize lightBytes = static_cast<VkDeviceSize>(std::max(L.numLights, 1u)) * sizeof(gos_terrain_lighting::GpuTerrainLight);
    const VkDeviceSize offCull = alignUp(sizeof(VkTerrainLightingParams), al);
    const VkDeviceSize offIn = alignUp(offCull + sizeof(VkWaterCullParams), al);
    const VkDeviceSize offLights = alignUp(offIn + (inBytes ? inBytes : sizeof(gos_terrain_lighting::GpuTerrainVertexInput)), al);
    const FrameAlloc span = rctx.frameAlloc(offLights + lightBytes);
    if (!span) return false;
    uint8_t* const base = static_cast<uint8_t*>(span.ptr);
    std::memcpy(base, &lp, sizeof(lp));
    std::memcpy(base + offCull, &wp, sizeof(wp));
    if (L.windowCount > 0) std::memcpy(base + offIn, L.window, static_cast<size_t>(inBytes));
    if (L.numLights > 0) std::memcpy(base + offLights, L.lights, static_cast<size_t>(L.numLights) * sizeof(gos_terrain_lighting::GpuTerrainLight));
    syncInputs(par, span.buffer, span.range);   // the island's slot fence retired this slot's last use; the other slot may be in flight and is not touched
    const uint32_t lightOffsets[3] = {static_cast<uint32_t>(span.offset + offIn), static_cast<uint32_t>(span.offset + offLights), static_cast<uint32_t>(span.offset)};
    const uint32_t cullOffset = static_cast<uint32_t>(span.offset + offCull);

    // ---- record ----
    // Hand-off from earlier frames' work on this queue (their cull read lightOut; the vertex stage read thin / recipes).
    cmdBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    MC2_VK_GPU_SCOPE(gpuCompute, cb, "VKIsl.Terrain.CullCompute");
    vkCmdFillBuffer(cb, header_[par].get(), 0, 16, 0);
    if (!lightOutCleared_) {
        vkCmdFillBuffer(cb, lightOut_.get(), 0, VK_WHOLE_SIZE, 0xFFFFFFFFu);   // the engine no-tint white sentinel, like the GL output SSBO's clear
        lightOutCleared_ = true;
    }
    cmdBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

    if (L.windowCount > 0) {
        lightP_.bind(cb, lightSets_.set(par), lightOffsets);
        lightP_.dispatch(cb, (L.windowCount + 63u) / 64u);
        cmdBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
    cullP_.bind(cb, cullSets_.set(par), std::span<const uint32_t>(&cullOffset, 1));
    cullP_.dispatch(cb, (recipeCount_ + 63u) / 64u);
    cmdBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    argsP_.bind(cb, argsSets_.set(par));
    argsP_.dispatch(cb, 1);
    gpuCompute.end();

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
    // A truncated side makes the set difference meaningless (the VK list is cut in atomic order): leave only_gl/only_vk at 0 then.
    const bool countOnly = !r.truncated;
    auto byRecipe = [](const ThinRec& a, const ThinRec& b) { return a.w[0] < b.w[0]; };
    std::sort(vk.begin(), vk.end(), byRecipe);
    std::sort(gl.begin(), gl.end(), byRecipe);
    size_t i = 0, j = 0;
    while (i < gl.size() || j < vk.size()) {
        if (j >= vk.size() || (i < gl.size() && gl[i].w[0] < vk[j].w[0])) { if (countOnly) ++r.onlyGl; ++i; }
        else if (i >= gl.size() || vk[j].w[0] < gl[i].w[0]) { if (countOnly) ++r.onlyVk; ++j; }
        else {
            if (std::memcmp(&gl[i], &vk[j], sizeof(ThinRec)) == 0) ++r.same;
            else {
                ++r.differ;
                if (!r.haveDiff) { r.haveDiff = true; std::memcpy(r.firstDiffGl, &gl[i], sizeof(ThinRec)); std::memcpy(r.firstDiffVk, &vk[j], sizeof(ThinRec)); }
            }
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
