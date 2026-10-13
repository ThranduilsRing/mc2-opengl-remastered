// GameOS/gameos/vk_device_profile.h -- the Vulkan 1.2 device profile: what the engine requires of a GPU, what it uses when the GPU has it, and
// the verdict for one physical device.
//
// WHAT: one table of facilities (the 1.2 API floor, swapchain, a few limits, timeline semaphores, descriptor indexing, buffer device address,
// dynamic rendering, synchronization2), each with its core-or-extension route and what the engine does without it; `evaluate()` turns the
// capabilities of a device (`DeviceCaps`, plain data) into a verdict, the set of device extensions and feature bits to enable, and the
// `[VK_PROFILE v1]` line.
// WHY: the shared context asked for Vulkan 1.1, enabled no feature and never looked at what the device offered, so "1.2 minimum" was a sentence and
// not a check. Raising the instance version does not enable a single device feature: a feature is used only if it was queried, found and enabled.
// The table keeps that decision in one place: REQUIRED items refuse the device (the engine falls back to GL with a logged reason, never a crash);
// WANTED items are enabled when present and the engine keeps the path it has today when they are not.
// Owner question still open (best-practices review, question 1): whether the 1.2 floor should REQUIRE dynamic rendering and the other
// selected extensions. Flipping a WANTED row to REQUIRED below is the whole change.
//
// Header-only and Vulkan-free so the unit tests (tests/unit) and the engine read the same table with no link dependency; the code that queries a
// real device is in vk_device_profile.cpp (MC2_VULKAN only).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mc2vk::profile {

constexpr uint32_t makeApi(uint32_t major, uint32_t minor) { return (major << 22) | (minor << 12); }
constexpr uint32_t apiMajor(uint32_t v) { return v >> 22; }
constexpr uint32_t apiMinor(uint32_t v) { return (v >> 12) & 0x3FF; }

inline constexpr uint32_t kFloorApi = makeApi(1, 2);    // the minimum device and instance version
inline constexpr uint32_t kUseApi13 = makeApi(1, 3);    // used when both the loader and the device have it (core dynamic rendering / sync2)

// Limits the engine relies on. 8192 is the largest shadow map the engine offers; the others are the Vulkan minimums the islands assume.
inline constexpr uint32_t kMinImageDimension2D = 8192;
inline constexpr uint32_t kMinBoundDescriptorSets = 4;
inline constexpr uint32_t kMinPushConstantsSize = 128;
inline constexpr uint64_t kMinStorageBufferRange = 128ull * 1024 * 1024;

enum class Need { Required, Wanted };

// What a physical device offers. `gather()` fills it from the driver; the tests fill it by hand.
struct DeviceCaps {
    uint32_t deviceApi = 0;                          // VkPhysicalDeviceProperties::apiVersion
    uint32_t instanceApi = 0;                        // the apiVersion the instance was created with (caps what the application may use)
    std::vector<std::string> extensions;             // device extension names
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
    // Limits.
    uint32_t maxImageDimension2D = 0;
    uint32_t maxBoundDescriptorSets = 0;
    uint32_t maxPushConstantsSize = 0;
    uint64_t maxStorageBufferRange = 0;

    bool hasExtension(std::string_view name) const {
        for (const std::string& e : extensions)
            if (e == name) return true;
        return false;
    }
    // The version the application can actually use device functionality at.
    uint32_t usableApi() const { return deviceApi < instanceApi ? deviceApi : instanceApi; }
};

struct ItemResult {
    const char* id;
    Need need;
    bool ok;
    const char* route;      // "core", "ext", "feature", "limit" or "-" (not available)
};

struct Evaluation {
    std::vector<ItemResult> items;
    std::vector<std::string> missingRequired;        // item ids; non-empty = the device is refused
    std::vector<std::string> missingWanted;          // item ids; the engine keeps its current path for each
    std::vector<std::string> deviceExtensions;       // device extensions to enable (swapchain excluded: the context adds it with the surface)
    std::vector<std::string> enabledFeatures;        // feature names to enable, in the spelling of the Vulkan struct member
    bool accepted = true;
};

// The facility ids, in the order the log line lists them. One place, so the table, the line and the tests cannot drift.
inline constexpr const char* kItemIds[] = {"api_1_2", "swapchain", "limits", "timeline_semaphore", "buffer_device_address", "descriptor_indexing",
                                           "dynamic_rendering", "synchronization2"};

inline constexpr const char* kExtDynamicRendering = "VK_KHR_dynamic_rendering";
inline constexpr const char* kExtSynchronization2 = "VK_KHR_synchronization2";
inline constexpr const char* kExtSwapchain = "VK_KHR_swapchain";

namespace detail {

inline bool limitsOk(const DeviceCaps& c) {
    return c.maxImageDimension2D >= kMinImageDimension2D && c.maxBoundDescriptorSets >= kMinBoundDescriptorSets &&
           c.maxPushConstantsSize >= kMinPushConstantsSize && c.maxStorageBufferRange >= kMinStorageBufferRange;
}

// Route of a facility that became core in `coreSince` and has an extension for older versions: "core" when the usable version has it AND the feature
// bit is on, "ext" when the extension is listed AND the bit is on, else "-".
inline const char* routeFor(const DeviceCaps& c, uint32_t coreSince, const char* ext, bool featureBit) {
    if (!featureBit) return "-";
    if (c.usableApi() >= coreSince) return "core";
    if (ext && c.hasExtension(ext)) return "ext";
    return "-";
}

}  // namespace detail

// The verdict for `caps`. `wantPresent` adds VK_KHR_swapchain to the required set (the shared device drives the present window).
inline Evaluation evaluate(const DeviceCaps& c, bool wantPresent) {
    Evaluation e;
    auto add = [&](const char* id, Need need, bool ok, const char* route) {
        e.items.push_back({id, need, ok, ok ? route : "-"});
        if (!ok) (need == Need::Required ? e.missingRequired : e.missingWanted).push_back(id);
    };
    add("api_1_2", Need::Required, c.deviceApi >= kFloorApi && c.instanceApi >= kFloorApi, "core");
    add("swapchain", wantPresent ? Need::Required : Need::Wanted, c.hasExtension(kExtSwapchain), "ext");
    add("limits", Need::Required, detail::limitsOk(c), "limit");

    const char* timeline = (c.usableApi() >= kFloorApi && c.timelineSemaphore) ? "core" : "-";
    add("timeline_semaphore", Need::Wanted, timeline[0] == 'c', timeline);
    if (timeline[0] == 'c') e.enabledFeatures.push_back("timelineSemaphore");

    const bool bda = c.usableApi() >= kFloorApi && c.bufferDeviceAddress;
    add("buffer_device_address", Need::Wanted, bda, "core");
    if (bda) e.enabledFeatures.push_back("bufferDeviceAddress");

    // Descriptor indexing wants the umbrella bit AND each individual bit the descriptor-table design uses.
    const bool di = c.usableApi() >= kFloorApi && c.descriptorIndexing && c.runtimeDescriptorArray &&
                    c.shaderSampledImageArrayNonUniformIndexing && c.descriptorBindingPartiallyBound;
    add("descriptor_indexing", Need::Wanted, di, "core");
    if (di) {
        e.enabledFeatures.push_back("descriptorIndexing");
        e.enabledFeatures.push_back("runtimeDescriptorArray");
        e.enabledFeatures.push_back("shaderSampledImageArrayNonUniformIndexing");
        e.enabledFeatures.push_back("descriptorBindingPartiallyBound");
    }

    const char* dr = detail::routeFor(c, kUseApi13, kExtDynamicRendering, c.dynamicRendering);
    add("dynamic_rendering", Need::Wanted, dr[0] != '-', dr);
    if (dr[0] != '-') e.enabledFeatures.push_back("dynamicRendering");
    if (dr[0] == 'e') e.deviceExtensions.push_back(kExtDynamicRendering);

    const char* s2 = detail::routeFor(c, kUseApi13, kExtSynchronization2, c.synchronization2);
    add("synchronization2", Need::Wanted, s2[0] != '-', s2);
    if (s2[0] != '-') e.enabledFeatures.push_back("synchronization2");
    if (s2[0] == 'e') e.deviceExtensions.push_back(kExtSynchronization2);

    e.accepted = e.missingRequired.empty();
    return e;
}

// The route of one item in `e` ("core", "ext", "feature", "limit"), or "-" when the item is absent or not available.
inline const char* routeOf(const Evaluation& e, std::string_view id) {
    for (const ItemResult& i : e.items)
        if (id == i.id) return i.route;
    return "-";
}

// True when feature `name` (the spelling of the Vulkan struct member) is in the enable set.
inline bool featureEnabled(const Evaluation& e, std::string_view name) {
    for (const std::string& f : e.enabledFeatures)
        if (name == f) return true;
    return false;
}

inline std::string joinOr(const std::vector<std::string>& v, const char* none = "-") {
    std::string out;
    for (const std::string& s : v) out += (out.empty() ? "" : ",") + s;
    return out.empty() ? none : out;
}

inline std::string versionText(uint32_t v) {
    return std::to_string(apiMajor(v)) + "." + std::to_string(apiMinor(v)) + "." + std::to_string(v & 0xFFF);
}

// `[VK_PROFILE v1] verdict=ok|refused device="..." api=1.4.3 instance_api=1.3.0 floor=1.2 required_missing=- wanted_missing=- items=id:route,...
//  enabled=feature,... extensions=ext,...` (a "-" is "none"). Pure: no I/O.
inline std::string formatLine(const std::string& deviceName, const DeviceCaps& c, const Evaluation& e) {
    std::string items;
    for (const ItemResult& i : e.items)
        items += std::string(items.empty() ? "" : ",") + i.id + ":" + i.route;
    return std::string("[VK_PROFILE v1] verdict=") + (e.accepted ? "ok" : "refused") + " device=\"" + deviceName + "\" api=" + versionText(c.deviceApi) +
           " instance_api=" + versionText(c.instanceApi) + " floor=1.2 required_missing=" + joinOr(e.missingRequired) +
           " wanted_missing=" + joinOr(e.missingWanted) + " items=" + items + " enabled=" + joinOr(e.enabledFeatures) +
           " extensions=" + joinOr(e.deviceExtensions);
}

#ifdef MC2_VULKAN
}  // namespace mc2vk::profile

struct VkPhysicalDevice_T;
typedef struct VkPhysicalDevice_T* VkPhysicalDevice;

namespace mc2vk::profile {
// Reads one physical device's version, extensions, feature bits and limits (vk_device_profile.cpp). `instanceApi` is the apiVersion the instance was
// created with. Needs the instance-level dispatch loaded (volkLoadInstance).
DeviceCaps gather(VkPhysicalDevice pd, uint32_t instanceApi);
#endif

}  // namespace mc2vk::profile
