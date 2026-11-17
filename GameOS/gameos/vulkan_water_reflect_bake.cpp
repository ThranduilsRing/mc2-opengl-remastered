#include "vulkan_water_island_internal.h"
#include "vk_water_planar.h"
#include "water_planar_view.h"
#include "vulkan_terrain_island.h"
#include "vk_one_shot.h"
#include "vk_barrier_batch_vk.h"
#include "vk_gpu_profile.h"
#include "gos_static_prop_registry.h"
#include "mc2fmt.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <chrono>
#include <string_view>

int gos_GetWaterReflectionQuality();

namespace vk_water_detail {
namespace {
constexpr float kHeightRange = 8192.0f;
struct ReflectBake : mc2vk::IslandState {
    int size = 0, levels = 0;
    float span = 0, halfMap = 0, water = 0;
    bool ready = false;
    std::array<uint64_t, 6> key{};
    std::array<float, 18> light{};
    std::array<float, 3> bakedRgb{}, scale{1, 1, 1};
    uint64_t content = 0;
    std::chrono::steady_clock::time_point lastBake{};
    mc2vk::OwnedImage color, depth, height;
    mc2vk::OwnedImageView colorView, depthView, heightView;
    std::vector<mc2vk::OwnedImageView> mipViews;
    std::vector<mc2vk::OwnedBuffer> params;
    mc2vk::OwnedDescriptorSetLayout dsl;
    mc2vk::OwnedDescriptorPool pool;
    mc2vk::OwnedPipelineLayout layout;
    mc2vk::OwnedPipeline pipe;
    mc2vk::OwnedQueryPool queries;
    std::vector<VkDescriptorSet> sets;
    std::unique_ptr<mc2vk::IslandState> terrain, props;
};

bool initBake(WaterIsland* s, ReflectBake& b) {
    // Include the optional diagnostic colour readback in the 64 MiB ceiling.
    // 8-byte colour + 4-byte depth + 8/3-byte height mips + 8-byte readback.
    const int cap = static_cast<int>(std::sqrt((64.0 * 1024 * 1024) / (68.0 / 3.0)));
    b.size = std::clamp(static_cast<int>(std::ceil(b.span / 8)), 1, (std::min)(4096, cap));
    for (int n = b.size; n; n >>= 1) ++b.levels;
    auto image = [&](mc2vk::OwnedImage& out, mc2vk::OwnedImageView& view, VkFormat format,
                     VkImageUsageFlags usage, VkImageAspectFlags aspect, int levels) {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D; ci.format = format;
        ci.extent = {static_cast<uint32_t>(b.size), static_cast<uint32_t>(b.size), 1};
        ci.mipLevels = levels; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = usage;
        VmaAllocationCreateInfo ai{}; ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (!out.create(s->allocator, s->device, ci, ai, "water.bake")) return false;
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = out.get(); vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = format;
        vi.subresourceRange = {aspect, 0, static_cast<uint32_t>(levels), 0, 1};
        return mc2vk::create(view, s->device, [&](auto* h) { return vkCreateImageView(s->device, &vi, nullptr, h); });
    };
    if (!image(b.color, b.colorView, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 1) ||
        !image(b.depth, b.depthView, VK_FORMAT_D32_SFLOAT,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, 1) ||
        !image(b.height, b.heightView, VK_FORMAT_R16_SFLOAT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT, b.levels)) return false;
    for (int i = 0; i < b.levels; ++i) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = b.height.get(); vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = VK_FORMAT_R16_SFLOAT;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, static_cast<uint32_t>(i), 1, 0, 1};
        mc2vk::OwnedImageView view;
        if (!mc2vk::create(view, s->device, [&](auto* h) { return vkCreateImageView(s->device, &vi, nullptr, h); })) return false;
        b.mipViews.push_back(std::move(view));
    }
    const VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo di{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    di.bindingCount = 3; di.pBindings = bindings;
    if (!mc2vk::create(b.dsl, s->device, [&](auto* h) { return vkCreateDescriptorSetLayout(s->device, &di, nullptr, h); })) return false;
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, static_cast<uint32_t>(b.levels)},
                                        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, static_cast<uint32_t>(b.levels)},
                                        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, static_cast<uint32_t>(b.levels)}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = b.levels; pi.poolSizeCount = 3; pi.pPoolSizes = sizes;
    if (!mc2vk::create(b.pool, s->device, [&](auto* h) { return vkCreateDescriptorPool(s->device, &pi, nullptr, h); })) return false;
    const VkDescriptorSetLayout dsl = b.dsl.get();
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 1; li.pSetLayouts = &dsl;
    if (!mc2vk::create(b.layout, s->device, [&](auto* h) { return vkCreatePipelineLayout(s->device, &li, nullptr, h); })) return false;
    mc2vk::OwnedShaderModule shader(s->device, load_spv(s->device, spv_dir() + "water_reflect_bake_mips.comp.spv"));
    if (!shader) return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, shader.get(), "main", nullptr};
    ci.layout = b.layout.get();
    if (!mc2vk::create(b.pipe, s->device, [&](auto* h) { return mc2vk::infra::compute(s->device, VK_NULL_HANDLE, 1, &ci, nullptr, h); })) return false;
    b.sets.resize(b.levels);
    b.params.resize(b.levels);
    for (int i = 0; i < b.levels; ++i) {
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = b.pool.get(); alloc.descriptorSetCount = 1; alloc.pSetLayouts = &dsl;
        if (MC2_VK_ALLOC_SETS(s->device, &alloc, &b.sets[i]) != VK_SUCCESS) return false;
        const VkDescriptorImageInfo images[] = {
            {VK_NULL_HANDLE, i ? b.mipViews[i-1].get() : b.depthView.get(), VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE, b.mipViews[i].get(), VK_IMAGE_LAYOUT_GENERAL}};
        const int size = (std::max)(1, b.size >> i);
        const struct { int w, h, fromDepth; float range; } params{size, size, i == 0, kHeightRange};
        static_assert(sizeof(params) == 16);
        if (!b.params[i].createFilled(s->allocator, s->device, &params, sizeof(params), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, "water.bake.params")) return false;
        const VkDescriptorBufferInfo buffer{b.params[i].get(), 0, sizeof(params)};
        VkWriteDescriptorSet writes[3]{};
        for (int j = 0; j < 3; ++j) {
            writes[j].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[j].dstSet = b.sets[i]; writes[j].dstBinding = j;
            writes[j].descriptorCount = 1; writes[j].descriptorType = bindings[j].descriptorType;
            if (j == 2) writes[j].pBufferInfo = &buffer; else writes[j].pImageInfo = &images[j];
        }
        vkUpdateDescriptorSets(s->device, 3, writes, 0, nullptr);
    }
    VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = 2;
    return mc2vk::create(b.queries, s->device, [&](auto* h) { return vkCreateQueryPool(s->device, &qi, nullptr, h); });
}

float halfFloat(uint16_t v) {
    const int exponent = (v >> 10) & 31;
    const float magnitude = exponent == 0 ? std::ldexp(static_cast<float>(v & 1023), -24) :
        std::ldexp(1.0f + static_cast<float>(v & 1023) / 1024, exponent - 15);
    return v & 32768 ? -magnitude : magnitude;
}
}

bool prepare_reflect_bake(WaterIsland* s, float water) {
    auto fail = [](std::string_view phase) {
        static bool logged = false;
        if (!logged && mc2_diag::tagRequested("WATER_BAKE")) mc2::fmt::print_err("[WATER_BAKE] FAILED phase={}\n", phase);
        logged = true;
        return false;
    };
    if (!s->mapHasWater || gos_GetWaterReflectionQuality() != 2) return true;
    Mc2TerrainTier1Feed feed{};
    gos_terrain_lighting::VkProducerSnapshot lighting{};
    if (!mc2_terrain_tier1_get_load_feed(&feed) || !gos_terrain_lighting::GetVkProducerSnapshot(lighting)) return true;
    const std::array<uint64_t, 6> key{GpuStaticPropRegistry::getRegistryGeneration(), feed.heightVersion,
        feed.visualHeightVersion, feed.colormapVersion, feed.cementAtlasVersion, feed.cementVersion};
    std::array<float, 18> light{};
    for (int i = 0; i < 3; ++i) {
        light[i] = lighting.sunDir[i]; light[3+i] = static_cast<float>(lighting.lightRgb[i]);
        light[6+i] = static_cast<float>(lighting.ambientRgb[i]); light[9+i] = static_cast<float>(lighting.baseRgb[i]);
        light[12+i] = feed.missionFlatLight[i];
    }
    light[15] = lighting.rain; light[16] = feed.ambientFloor; light[17] = feed.ambientDayGate;
    std::array<float, 3> rgb{};
    for (int i = 0; i < 3; ++i) {
        rgb[i] = mc2::waterplanar::weatherLight(light[3+i], light[6+i], lighting.rain);
        light[3+i] = light[6+i] = 0;
    }
    light[15] = 0; // Weather is applied to the baked tap, with bounded lamp drift.
    const auto content = GpuStaticPropRegistry::getContentRevision();
    const auto now = std::chrono::steady_clock::now();
    auto& owner = s->reflectionBake;
    std::string why = "load";
    std::string source = "mission";
    if (owner) {
        auto& b = *static_cast<ReflectBake*>(owner.get());
        if (b.ready) {
            bool drift = false;
            for (int i = 0; i < 3; ++i) {
                b.scale[i] = b.bakedRgb[i] > 0 ? rgb[i] / b.bakedRgb[i] : 1;
                drift |= b.scale[i] < 0.9f || b.scale[i] > 1.1f || (b.bakedRgb[i] == 0 && rgb[i] != 0);
            }
            source.clear();
            if (b.content != content) source += "registry;";
            // Weather also quantizes the flat-pad light in gamecam.cpp. Treat
            // those steps as bounded lighting refreshes, not geometry edits.
            bool flatLightChanged = false;
            for (size_t i = 0; i < light.size(); ++i) {
                if (i >= 12 && i <= 14) flatLightChanged |= b.light[i] != light[i];
                else if (b.light[i] != light[i]) source += "light[" + std::to_string(i) + "];";
            }
            if (b.water != water) source += "water;";
            if (b.halfMap != feed.halfMap) source += "halfMap;";
            for (size_t i = 1; i < key.size(); ++i)
                if (b.key[i] != key[i]) source += "key[" + std::to_string(i) + "];";
            if (!source.empty()) why = "content";
            else if (drift) why = "drift";
            else if ((b.key[0] != key[0] || flatLightChanged) && now - b.lastBake >= std::chrono::seconds(1)) {
                why = "reveal";
                source = flatLightChanged ? "flatLight" : "visibility";
            }
            else return true;
        }
        // Mission dimensions may change without a window resize.
        if (b.span != 128.0f * (feed.mapSide - 1)) mc2vk::getOrCreateEngineVulkanContext().retire(std::move(owner));
    }
    if (!owner) {
        auto b = std::make_unique<ReflectBake>();
        b->span = 128.0f * (feed.mapSide - 1); b->halfMap = feed.halfMap;
        if (!initBake(s, *b)) return fail("init");
        owner = std::move(b);
    }
    auto& b = *static_cast<ReflectBake*>(owner.get());
    std::array<float, 16> matrix{};
    matrix[0] = 2 / b.span; matrix[5] = -2 / b.span;
    matrix[10] = -1 / kHeightRange; matrix[14] = water / kHeightRange; matrix[15] = 1;
    matrix[12] = matrix[13] = 2 * b.halfMap / b.span - 1;
    const float eye[]{0, water + kHeightRange, 0};
    mc2::waterplanar::Target target;
    target.w = target.h = b.size; target.water = water; target.mainWorldToClip = matrix.data();
    target.useTargetView = true; target.cameraPos = eye;
    target.color = b.colorView.get(); target.depth = b.depthView.get();
    mc2vk::VulkanContext::RecordFn terrain, props;
    if (!mc2::waterplanar::prepareTerrain(b.terrain, target, terrain)) return fail("terrain");
    if (!mc2::waterplanar::prepareProps(b.props, target, props)) return fail("props");
    mc2vk::OwnedBuffer readback;
    const bool dump = mc2::gate::get<"MC2_DEBUG_STATE_DUMP">();
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(b.size) * b.size * 8;
    if (dump && !readback.createHostWritable(s->allocator, s->device, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, "water.bake.readback")) return false;
    auto& ctx = mc2vk::getOrCreateEngineVulkanContext();
    bool recorded = true;
    if (!mc2vk::submitOneShot(ctx, [&](VkCommandBuffer cb) {
        MC2_VK_GPU_SCOPE(scope, cb, "VKIsl.Water.Bake");
        vkCmdResetQueryPool(cb, b.queries.get(), 0, 2);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, b.queries.get(), 0);
        auto transition = [&](VkImage image, VkImageAspectFlags aspect, int mip, int levels, VkImageLayout from, VkImageLayout to) {
            mc2vk::sync::cmdImageBarrier(ctx, cb, image, {aspect, static_cast<uint32_t>(mip), static_cast<uint32_t>(levels), 0, 1},
                from, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                to, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        };
        transition(b.color.get(), VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, b.ready ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        transition(b.depth.get(), VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, b.ready ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const VkClearDepthStencilValue zero{0, 0};
        const VkImageSubresourceRange depthRange{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        vkCmdClearDepthStencilImage(cb, b.depth.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &depthRange);
        transition(b.depth.get(), VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        recorded = terrain && terrain(cb) && (!props || props(cb));
        transition(b.depth.get(), VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        transition(b.height.get(), VK_IMAGE_ASPECT_COLOR_BIT, 0, b.levels, b.ready ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        mc2vk::infra::bind(cb, VK_PIPELINE_BIND_POINT_COMPUTE, b.pipe.get());
        for (int i = 0; i < b.levels; ++i) {
            const int size = (std::max)(1, b.size >> i);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, b.layout.get(), 0, 1, &b.sets[i], 0, nullptr);
            vkCmdDispatch(cb, (size+7)/8, (size+7)/8, 1);
            transition(b.height.get(), VK_IMAGE_ASPECT_COLOR_BIT, i, 1, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        }
        transition(b.color.get(), VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, dump ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, b.queries.get(), 1);
        if (dump) {
            VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {static_cast<uint32_t>(b.size), static_cast<uint32_t>(b.size), 1};
            vkCmdCopyImageToBuffer(cb, b.color.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.get(), 1, &copy);
            transition(b.color.get(), VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        }
    }, "water reflection bake") || !recorded) return false;
    uint64_t timestamps[2]{};
    if (vkGetQueryPoolResults(s->device, b.queries.get(), 0, 2, sizeof(timestamps), timestamps, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) return false;
    VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(s->phys, &properties);
    const double ms = (timestamps[1] - timestamps[0]) * properties.limits.timestampPeriod / 1.0e6;
    const double mb = static_cast<double>(b.size) * b.size * (12 + 8.0/3) / (1024*1024);
    if (mc2_diag::tagRequested("WATER_BAKE")) mc2::fmt::print_err("[WATER_BAKE] size={}x{} ms={:.3f} MB={:.3f} registry={} why={} source={}\n", b.size, b.size, ms, mb, key[0], why, source);
    if (dump) {
        std::vector<uint16_t> pixels(static_cast<size_t>(bytes / 2));
        if (!readback.read(pixels.data(), bytes)) return false;
        std::filesystem::path dir = mc2::gate::str<"MC2_DEBUG_STATE_DUMP_DIR">();
        if (dir.empty()) dir = "debug_state";
        std::error_code ec; std::filesystem::create_directories(dir, ec);
        if (FILE* file = std::fopen((dir / "water_bake.ppm").string().c_str(), "wb")) {
            std::fprintf(file, "P6\n%d %d\n255\n", b.size, b.size);
            for (size_t i = 0; i < pixels.size(); i += 4) for (int c = 0; c < 3; ++c) {
                const float v = (std::max)(0.0f, halfFloat(pixels[i+c]));
                const auto encoded = static_cast<unsigned char>(255 * std::pow(v / (1 + v), 1.0f / 2.2f));
                std::fputc(encoded, file);
            }
            std::fclose(file);
        } else return false;
    }
    b.key = key; b.light = light; b.water = water; b.ready = true;
    b.content = content; b.bakedRgb = rgb; b.scale = {1, 1, 1}; b.lastBake = now;
    return true;
}

void bind_reflect_bake(WaterIsland* s, uint32_t par, WaterFsParams* params) {
    params->DEEP_COLOR[3] = 0;
    if (!s->reflectionBake || gos_GetWaterReflectionQuality() != 2) return;
    const auto& b = *static_cast<ReflectBake*>(s->reflectionBake.get());
    if (!b.ready) return;
    params->DEEP_COLOR[3] = -1; // quality 2: camera-independent reflection
    params->bake[0] = b.span; params->bake[1] = b.halfMap;
    params->bake[2] = b.scale[0]; params->bake[3] = b.scale[1]; params->SHALLOW_COLOR[3] = b.scale[2];
    const VkDescriptorImageInfo images[] = {
        {s->hdriSampler.get(), b.colorView.get(), VK_IMAGE_LAYOUT_GENERAL},
        {s->hdriSampler.get(), b.heightView.get(), VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[2]{};
    for (int i = 0; i < 2; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet = s->set[par];
        writes[i].dstBinding = 8 + i; writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(s->device, 2, writes, 0, nullptr);
}
} // namespace vk_water_detail
