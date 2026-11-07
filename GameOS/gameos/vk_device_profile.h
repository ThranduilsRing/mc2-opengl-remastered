// GameOS/gameos/vk_device_profile.h -- the Vulkan 1.2 device profile: what the engine requires of a GPU, what it uses when the GPU has it, and
// the verdict for one physical device.
//
// WHAT: one table of facilities (the 1.2 API floor, swapchain, a few limits, timeline semaphores, descriptor indexing, buffer device address,
// dynamic rendering, synchronization2, and the optional memory-budget extension), each with its core-or-extension route; `evaluate()` turns the
// capabilities of a device (`DeviceCaps`, plain data) into a verdict, the set of device extensions and feature bits to enable, and the
// `[VK_PROFILE v1]` line. The context keeps the Evaluation of the device it chose (`VulkanContext::profile()`), so an island asks what is enabled and by
// which route instead of re-deriving it.
// WHY: the shared context asked for Vulkan 1.1, enabled no feature and never looked at what the device offered, so "1.2 minimum" was a sentence and
// not a check. Raising the instance version does not enable a single device feature: a feature is used only if it was queried, found and enabled.
// The table keeps that decision in one place. Owner ruling 2026-10-13: the five facilities (dynamic rendering, synchronization2, timeline semaphores,
// descriptor indexing, buffer device address) are REQUIRED, so a Vulkan island may call any of them without a fallback arm; a device without one is
// refused and the engine falls back to GL with a logged reason, never a crash (older GPUs play the OpenGL build). That one engine-level fallback is
// the only one: an island has no per-facility alternative path.
//
// Header-only and Vulkan-free so the unit tests (tests/unit) and the engine read the same table with no link dependency; the code that queries a
// real device is in vk_device_profile.cpp (MC2_VULKAN only).
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace mc2vk::profile {

constexpr uint32_t makeApi(uint32_t major, uint32_t minor) { return (major << 22) | (minor << 12); }
constexpr uint32_t apiMajor(uint32_t v) { return v >> 22; }
constexpr uint32_t apiMinor(uint32_t v) { return (v >> 12) & 0x3FF; }

inline constexpr uint32_t kFloorApi = makeApi(1, 2);    // the minimum device and instance version
inline constexpr uint32_t kUseApi13 = makeApi(1, 3);    // used when both the loader and the device have it (core dynamic rendering / sync2)
// The instance is requested at min(loader, kUseApi13) because nothing in the engine uses 1.4: a floor above 1.3 could never be met by an instance the
// engine creates, and every device would be refused. The item id "api_1_2" is a literal and must be renamed together with the floor.
static_assert(kFloorApi <= kUseApi13, "the floor must not exceed the version the engine requests");

// Limits the engine relies on. 8192 is the largest shadow map the engine offers; the others are the Vulkan minimums the islands assume.
inline constexpr uint32_t kMinImageDimension2D = 8192;
inline constexpr uint32_t kMinBoundDescriptorSets = 4;
inline constexpr uint32_t kMinPushConstantsSize = 128;
inline constexpr uint64_t kMinStorageBufferRange = 128ull * 1024 * 1024;

enum class Need { Required, Wanted };

// How a facility is reached on a device: core in the usable version, through its extension, a plain limit, or not at all.
enum class Route { None, Core, Ext, Limit };

constexpr std::string_view routeName(Route r) {
    switch (r) {
        case Route::Core: return "core";
        case Route::Ext: return "ext";
        case Route::Limit: return "limit";
        case Route::None: break;
    }
    return "-";
}

// Ray-tracing capability as the device reports it (VK_KHR_acceleration_structure / ray_query / ray_tracing_pipeline). Filled by gather() only when the
// extension is listed; zero otherwise. Selection is `selectRt()` below: an optional tier, never a requirement, so a GPU without it keeps the raster path.
struct RtCaps {
    bool accelerationStructure = false;              // VkPhysicalDeviceAccelerationStructureFeaturesKHR::accelerationStructure
    bool rayQuery = false;                           // VkPhysicalDeviceRayQueryFeaturesKHR::rayQuery
    bool rayTracingPipeline = false;                 // VkPhysicalDeviceRayTracingPipelineFeaturesKHR::rayTracingPipeline
    // VkPhysicalDeviceAccelerationStructurePropertiesKHR
    uint64_t maxGeometryCount = 0;
    uint64_t maxInstanceCount = 0;
    uint64_t maxPrimitiveCount = 0;
    uint32_t maxPerStageDescriptorAccelerationStructures = 0;
    uint32_t maxDescriptorSetAccelerationStructures = 0;
    uint32_t minAccelerationStructureScratchOffsetAlignment = 0;
    // VkPhysicalDeviceRayTracingPipelinePropertiesKHR (shader binding table limits)
    uint32_t shaderGroupHandleSize = 0;
    uint32_t maxRayRecursionDepth = 0;
    uint32_t maxShaderGroupStride = 0;
    uint32_t shaderGroupBaseAlignment = 0;
    uint32_t shaderGroupHandleAlignment = 0;
};

// What a physical device offers. `gather()` fills it from the driver; the tests fill it by hand.
struct DeviceCaps {
    uint32_t deviceApi = 0;                          // VkPhysicalDeviceProperties::apiVersion
    uint32_t instanceApi = 0;                        // the apiVersion the instance was created with (caps what the application may use)
    std::vector<std::string> extensions;             // device extension names
    RtCaps rt;                                       // ray-tracing features and limits (optional tier, see selectRt)
    // Feature bits. dynamicRendering / synchronization2 hold the bit of whichever struct applies (core 1.3 when both versions allow it, else the
    // KHR extension struct); every other bit comes from VkPhysicalDeviceVulkan12Features.
    bool timelineSemaphore = false;
    bool bufferDeviceAddress = false;
    bool descriptorIndexing = false;
    bool runtimeDescriptorArray = false;
    bool shaderSampledImageArrayNonUniformIndexing = false;
    bool descriptorBindingPartiallyBound = false;
    bool dynamicRendering = false;
    bool synchronization2 = false;
    // A core 1.0 feature (VkPhysicalDeviceFeatures): the terrain, water, VFX and legacy-shape pipelines write two colour attachments with a different blend
    // state each (blended colour, untouched normal / object-id target), which is invalid without it.
    bool independentBlend = false;
    bool samplerAnisotropy = false;
    // A core 1.0 feature: the prop island indexes its uPropTex[] sampler array with a per-material value (SampledImageArrayDynamicIndexing).
    bool shaderSampledImageArrayDynamicIndexing = false;
    // GPU-driven props: core 1.0 multiDrawIndirect + drawIndirectFirstInstance and the 1.2 drawIndirectCount feature.
    bool multiDrawIndirect = false;
    bool drawIndirectFirstInstance = false;
    bool drawIndirectCount = false;
    bool shaderDrawParameters = false;   // gl_DrawID: the per-draw packet table index
    // Optimal-tiling support for DEPTH_STENCIL_ATTACHMENT and SAMPLED_IMAGE (with linear-filter-free sampling) on the two depth formats the engine hardcodes:
    // D32_SFLOAT (shadow windows) and D32_SFLOAT_S8_UINT (the shared scene depth). Filled by gather().
    bool depthD32 = false;
    bool depthD32S8 = false;
    // Limits.
    uint32_t maxImageDimension2D = 0;
    uint32_t maxBoundDescriptorSets = 0;
    uint32_t maxPushConstantsSize = 0;
    uint64_t maxStorageBufferRange = 0;
    // VkPhysicalDeviceSubgroupProperties (core 1.1): the wave width and which stages / operation classes may use subgroup ops. Bits are the Vulkan values
    // (stage COMPUTE = 0x20; BASIC = 1, VOTE = 2, ARITHMETIC = 4, BALLOT = 8).
    uint32_t subgroupSize = 0;
    uint32_t subgroupStages = 0;
    uint32_t subgroupOps = 0;
    // The Windows adapter identity of the device (VkPhysicalDeviceIDProperties::deviceLUID, the bytes of a LUID); valid only when the driver says so.
    uint8_t deviceLuid[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    bool deviceLuidValid = false;

    bool hasExtension(std::string_view name) const {
        for (const std::string& e : extensions)
            if (e == name) return true;
        return false;
    }
    // The version the application can actually use device functionality at.
    uint32_t usableApi() const { return deviceApi < instanceApi ? deviceApi : instanceApi; }
};

struct ItemResult {
    std::string_view id;    // one of kItemIds (literal-backed)
    Need need;
    bool ok;
    Route route;            // None when the item is not available
};

struct Evaluation {
    std::vector<ItemResult> items;
    std::vector<std::string> missingRequired;        // item ids; non-empty = the device is refused
    std::vector<std::string> missingWanted;          // item ids that are optional for this context (the swapchain without a present window); never a refusal
    std::vector<std::string> deviceExtensions;       // every device extension to enable, in table order; empty for a refused device
    std::vector<std::string> enabledFeatures;        // feature names to enable, in the spelling of the Vulkan struct member; empty for a refused device
    bool accepted = true;
};

// The facility ids, in the order the log line lists them. One place, so the table, the line and the tests cannot drift.
inline constexpr std::string_view kItemIds[] = {"api_1_2", "swapchain", "limits", "timeline_semaphore", "buffer_device_address", "descriptor_indexing",
                                                "dynamic_rendering", "synchronization2", "independent_blend", "subgroup_compute", "memory_budget", "sampler_anisotropy",
                                                "indirect_draw", "depth_formats"};

inline constexpr std::string_view kExtDynamicRendering = "VK_KHR_dynamic_rendering";
inline constexpr std::string_view kExtSynchronization2 = "VK_KHR_synchronization2";
inline constexpr std::string_view kExtSwapchain = "VK_KHR_swapchain";
inline constexpr std::string_view kExtMemoryBudget = "VK_EXT_memory_budget";   // the OS's per-heap budget and usage; the VRAM report reads it

namespace detail {

inline bool limitsOk(const DeviceCaps& c) {
    return c.maxImageDimension2D >= kMinImageDimension2D && c.maxBoundDescriptorSets >= kMinBoundDescriptorSets &&
           c.maxPushConstantsSize >= kMinPushConstantsSize && c.maxStorageBufferRange >= kMinStorageBufferRange;
}

// Route of a facility that became core in `coreSince` and has an extension for older versions: Core when the usable version has it AND the feature
// bit is on, Ext when the extension is listed AND the bit is on, else None.
inline Route routeFor(const DeviceCaps& c, uint32_t coreSince, std::string_view ext, bool featureBit) {
    if (!featureBit) return Route::None;
    if (c.usableApi() >= coreSince) return Route::Core;
    if (!ext.empty() && c.hasExtension(ext)) return Route::Ext;
    return Route::None;
}

}  // namespace detail

// The verdict for `caps`. `wantPresent` adds VK_KHR_swapchain to the required set (the shared device drives the present window).
inline Evaluation evaluate(const DeviceCaps& c, bool wantPresent) {
    Evaluation e;
    auto add = [&](std::string_view id, Need need, bool ok, Route route) {
        e.items.push_back({id, need, ok, ok ? route : Route::None});
        if (!ok) (need == Need::Required ? e.missingRequired : e.missingWanted).emplace_back(id);
    };
    add("api_1_2", Need::Required, c.deviceApi >= kFloorApi && c.instanceApi >= kFloorApi, Route::Core);
    const bool swapchain = c.hasExtension(kExtSwapchain);
    add("swapchain", wantPresent ? Need::Required : Need::Wanted, swapchain, Route::Ext);
    if (wantPresent && swapchain) e.deviceExtensions.emplace_back(kExtSwapchain);
    add("limits", Need::Required, detail::limitsOk(c), Route::Limit);

    const bool timeline = c.usableApi() >= kFloorApi && c.timelineSemaphore;
    add("timeline_semaphore", Need::Required, timeline, Route::Core);
    if (timeline) e.enabledFeatures.push_back("timelineSemaphore");

    const bool bda = c.usableApi() >= kFloorApi && c.bufferDeviceAddress;
    add("buffer_device_address", Need::Required, bda, Route::Core);
    if (bda) e.enabledFeatures.push_back("bufferDeviceAddress");

    // Descriptor indexing wants the umbrella bit AND each individual bit the descriptor-table design uses.
    const bool di = c.usableApi() >= kFloorApi && c.descriptorIndexing && c.runtimeDescriptorArray &&
                    c.shaderSampledImageArrayNonUniformIndexing && c.descriptorBindingPartiallyBound;
    add("descriptor_indexing", Need::Required, di, Route::Core);
    if (di) {
        e.enabledFeatures.push_back("descriptorIndexing");
        e.enabledFeatures.push_back("runtimeDescriptorArray");
        e.enabledFeatures.push_back("shaderSampledImageArrayNonUniformIndexing");
        e.enabledFeatures.push_back("descriptorBindingPartiallyBound");
        if (c.shaderSampledImageArrayDynamicIndexing) e.enabledFeatures.push_back("shaderSampledImageArrayDynamicIndexing");
    }

    const Route dr = detail::routeFor(c, kUseApi13, kExtDynamicRendering, c.dynamicRendering);
    add("dynamic_rendering", Need::Required, dr != Route::None, dr);
    if (dr != Route::None) e.enabledFeatures.push_back("dynamicRendering");
    if (dr == Route::Ext) e.deviceExtensions.emplace_back(kExtDynamicRendering);

    const Route s2 = detail::routeFor(c, kUseApi13, kExtSynchronization2, c.synchronization2);
    add("synchronization2", Need::Required, s2 != Route::None, s2);
    if (s2 != Route::None) e.enabledFeatures.push_back("synchronization2");
    if (s2 == Route::Ext) e.deviceExtensions.emplace_back(kExtSynchronization2);

    // Required since the first validation run of the Vulkan smoke tier: the islands' two-attachment pipelines blend the colour attachment only.
    const bool independentBlend = c.independentBlend;
    add("independent_blend", Need::Required, independentBlend, Route::Core);
    if (independentBlend) e.enabledFeatures.push_back("independentBlend");

    // Compute shaders use subgroup ballot / arithmetic (vegetation compaction, water append); every target GPU has them. Needs 1.1 SPIR-V: the shaders are built for vulkan1.2.
    constexpr uint32_t kSubgroupComputeStage = 0x20u, kSubgroupNeeded = 1u | 4u | 8u;   // basic + arithmetic + ballot
    const bool subgroup = (c.subgroupStages & kSubgroupComputeStage) != 0 && (c.subgroupOps & kSubgroupNeeded) == kSubgroupNeeded;
    add("subgroup_compute", Need::Required, subgroup, Route::Limit);

    // Optional: the VRAM report is the driver's number when the device has it and VMA's own estimate when it does not.
    const bool budget = c.hasExtension(kExtMemoryBudget);
    add("memory_budget", Need::Wanted, budget, Route::Ext);
    if (budget) e.deviceExtensions.emplace_back(kExtMemoryBudget);

    add("sampler_anisotropy", Need::Wanted, c.samplerAnisotropy, Route::Core);
    if (c.samplerAnisotropy) e.enabledFeatures.push_back("samplerAnisotropy");

    // Required: the static-prop colour pass is GPU-driven (compute compaction into VkDrawIndexedIndirectCommand, vkCmdDrawIndexedIndirectCount).
    const bool indirect = c.multiDrawIndirect && c.drawIndirectFirstInstance && c.drawIndirectCount && c.shaderDrawParameters;
    add("indirect_draw", Need::Required, indirect, Route::Core);
    if (indirect) {
        e.enabledFeatures.push_back("multiDrawIndirect");
        e.enabledFeatures.push_back("drawIndirectFirstInstance");
        e.enabledFeatures.push_back("drawIndirectCount");
        e.enabledFeatures.push_back("shaderDrawParameters");
    }

    // Required: the shared scene depth is D32_SFLOAT_S8_UINT and the shadow windows D32_SFLOAT, both attached and sampled; a device without them is refused here
    // with a named item instead of failing later at image creation.
    add("depth_formats", Need::Required, c.depthD32 && c.depthD32S8, Route::Core);

    e.accepted = e.missingRequired.empty();
    if (!e.accepted) {                               // a refused device gets nothing enabled
        e.enabledFeatures.clear();
        e.deviceExtensions.clear();
    }
    return e;
}

// The route of one item in `e`, or None when the item is absent or not available.
inline Route routeOf(const Evaluation& e, std::string_view id) {
    for (const ItemResult& i : e.items)
        if (id == i.id) return i.route;
    return Route::None;
}

// True when feature `name` (the spelling of the Vulkan struct member) is in the enable set.
inline bool featureEnabled(const Evaluation& e, std::string_view name) {
    for (const std::string& f : e.enabledFeatures)
        if (name == f) return true;
    return false;
}

inline std::string joinOr(const std::vector<std::string>& v, std::string_view none = "-") {
    std::string out;
    for (const std::string& s : v) out += (out.empty() ? "" : ",") + s;
    return out.empty() ? std::string(none) : out;
}

inline std::string versionText(uint32_t v) {
    return std::to_string(apiMajor(v)) + "." + std::to_string(apiMinor(v)) + "." + std::to_string(v & 0xFFF);
}

// The floor as the line states it: major.minor.
inline std::string floorText() { return std::to_string(apiMajor(kFloorApi)) + "." + std::to_string(apiMinor(kFloorApi)); }

// `[VK_PROFILE v1] verdict=ok|refused device="..." api=1.4.3 instance_api=1.3.0 floor=1.2 required_missing=- wanted_missing=- items=id:route,...
//  enabled=feature,... extensions=ext,...` (a "-" is "none"). `floor` is kFloorApi; `extensions` is every device extension the context enables. Pure: no I/O. One snprintf
// carries the whole format so the log-tag census sees the line and its key order.
inline std::string formatLine(std::string_view deviceName, const DeviceCaps& c, const Evaluation& e) {
    std::string items;
    for (const ItemResult& i : e.items)
        items += std::string(items.empty() ? "" : ",") + std::string(i.id) + ":" + std::string(routeName(i.route));
    const std::string device(deviceName), api = versionText(c.deviceApi), instanceApi = versionText(c.instanceApi), floor = floorText(),
                      requiredMissing = joinOr(e.missingRequired), wantedMissing = joinOr(e.missingWanted), enabled = joinOr(e.enabledFeatures),
                      extensions = joinOr(e.deviceExtensions);
    auto print = [&](char* out, size_t capacity) {
        return std::snprintf(out, capacity,
                             "[VK_PROFILE v1] verdict=%s device=\"%s\" api=%s instance_api=%s floor=%s required_missing=%s wanted_missing=%s items=%s enabled=%s extensions=%s",
                             e.accepted ? "ok" : "refused", device.c_str(), api.c_str(), instanceApi.c_str(), floor.c_str(), requiredMissing.c_str(),
                             wantedMissing.c_str(), items.c_str(), enabled.c_str(), extensions.c_str());
    };
    std::string line(static_cast<size_t>(print(nullptr, 0)), ' ');
    print(line.data(), line.size() + 1);
    return line;
}

// ---- Ray-tracing tier (optional capability, never a requirement) ----

inline constexpr std::string_view kExtAccelerationStructure = "VK_KHR_acceleration_structure";
inline constexpr std::string_view kExtDeferredHostOperations = "VK_KHR_deferred_host_operations";
inline constexpr std::string_view kExtRayQuery = "VK_KHR_ray_query";
inline constexpr std::string_view kExtRayTracingPipeline = "VK_KHR_ray_tracing_pipeline";

enum class RtTier { None, Accel, Query, Pipeline, QueryPipeline };

constexpr std::string_view rtTierName(RtTier t) {
    switch (t) {
        case RtTier::Accel: return "accel";
        case RtTier::Query: return "query";
        case RtTier::Pipeline: return "pipeline";
        case RtTier::QueryPipeline: return "query+pipeline";
        case RtTier::None: break;
    }
    return "none";
}

struct RtSelection {
    RtTier tier = RtTier::None;
    bool accel = false;                              // acceleration structures can be built (base of every tier)
    bool rayQuery = false;                           // inline ray queries in any shader stage
    bool rayPipeline = false;                        // trace-rays pipelines and the SBT
    std::vector<std::string> extensions;             // device extensions to enable (empty at tier none)
    std::string reason;                              // why a part is missing; "-" when everything the device lists is usable
};

// Picks the tier from the device caps. Acceleration structures need the two base extensions, the accelerationStructure feature and buffer device address;
// ray query and the RT pipeline are then judged independently (extension + feature bit each). Pure: no I/O.
inline RtSelection selectRt(const DeviceCaps& c) {
    RtSelection r;
    std::vector<std::string> why;
    if (c.usableApi() < kFloorApi) {
        why.push_back("api_below_floor");
    } else if (!c.hasExtension(kExtAccelerationStructure)) {
        why.push_back("no_ext_acceleration_structure");
    } else if (!c.hasExtension(kExtDeferredHostOperations)) {
        why.push_back("no_ext_deferred_host_operations");
    } else if (!c.rt.accelerationStructure) {
        why.push_back("no_feature_acceleration_structure");
    } else if (!c.bufferDeviceAddress) {
        why.push_back("no_buffer_device_address");
    } else if (c.rt.minAccelerationStructureScratchOffsetAlignment == 0) {
        why.push_back("no_scratch_alignment");
    } else {
        r.accel = true;
    }
    if (r.accel) {
        if (!c.hasExtension(kExtRayQuery)) why.push_back("no_ext_ray_query");
        else if (!c.rt.rayQuery) why.push_back("no_feature_ray_query");
        else r.rayQuery = true;
        if (!c.hasExtension(kExtRayTracingPipeline)) why.push_back("no_ext_ray_tracing_pipeline");
        else if (!c.rt.rayTracingPipeline) why.push_back("no_feature_ray_tracing_pipeline");
        else if (c.rt.shaderGroupHandleSize == 0 || c.rt.shaderGroupBaseAlignment == 0) why.push_back("no_sbt_limits");
        else r.rayPipeline = true;
        r.extensions = {std::string(kExtAccelerationStructure), std::string(kExtDeferredHostOperations)};
        if (r.rayQuery) r.extensions.emplace_back(kExtRayQuery);
        if (r.rayPipeline) r.extensions.emplace_back(kExtRayTracingPipeline);
        r.tier = r.rayQuery ? (r.rayPipeline ? RtTier::QueryPipeline : RtTier::Query) : (r.rayPipeline ? RtTier::Pipeline : RtTier::Accel);
    }
    r.reason = joinOr(why);
    return r;
}

// `[VK_RT v1] tier=none|accel|query|pipeline|query+pipeline reason=- accel=0|1 ray_query=0|1 ray_pipeline=0|1 max_instances=N max_primitives=N max_geometries=N
//  scratch_align=N as_per_stage=N sbt_handle=N sbt_base_align=N sbt_handle_align=N sbt_stride=N max_recursion=N`. One snprintf so the log-tag census sees the key order.
inline std::string formatRtLine(const DeviceCaps& c, const RtSelection& r) {
    const std::string tier(rtTierName(r.tier)), reason = r.reason.empty() ? std::string("-") : r.reason;
    auto print = [&](char* out, size_t capacity) {
        return std::snprintf(out, capacity,
                             "[VK_RT v1] tier=%s reason=%s accel=%d ray_query=%d ray_pipeline=%d max_instances=%llu max_primitives=%llu max_geometries=%llu scratch_align=%u "
                             "as_per_stage=%u sbt_handle=%u sbt_base_align=%u sbt_handle_align=%u sbt_stride=%u max_recursion=%u",
                             tier.c_str(), reason.c_str(), r.accel ? 1 : 0, r.rayQuery ? 1 : 0, r.rayPipeline ? 1 : 0,
                             static_cast<unsigned long long>(c.rt.maxInstanceCount), static_cast<unsigned long long>(c.rt.maxPrimitiveCount),
                             static_cast<unsigned long long>(c.rt.maxGeometryCount), c.rt.minAccelerationStructureScratchOffsetAlignment,
                             c.rt.maxPerStageDescriptorAccelerationStructures, c.rt.shaderGroupHandleSize, c.rt.shaderGroupBaseAlignment,
                             c.rt.shaderGroupHandleAlignment, c.rt.maxShaderGroupStride, c.rt.maxRayRecursionDepth);
    };
    std::string line(static_cast<size_t>(print(nullptr, 0)), ' ');
    print(line.data(), line.size() + 1);
    return line;
}

#ifdef MC2_VULKAN
}  // namespace mc2vk::profile

struct VkPhysicalDevice_T;
using VkPhysicalDevice = VkPhysicalDevice_T*;

namespace mc2vk::profile {
// Reads one physical device's version, extensions, feature bits and limits (vk_device_profile.cpp). `instanceApi` is the apiVersion the instance was
// created with. Needs the instance-level dispatch loaded (volkLoadInstance).
DeviceCaps gather(VkPhysicalDevice pd, uint32_t instanceApi);
#endif

}  // namespace mc2vk::profile
