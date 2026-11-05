// Oracle for mc2vk::profile::evaluate() (GameOS/gameos/vk_device_profile.h): the verdict for real GPUs, not hand-made DeviceCaps.
//
// The fixtures in tests/fixtures/vk_profiles/*.caps are extracts of per-GPU gpuinfo JSONs from KhronosGroup/Vulkan-Profiles (pinned commit and file
// name in each header; Apache-2.0). `capsFromFixture()` mirrors gather() in vk_device_profile.cpp field by field (same structs, same version
// early-out, same core-1.3-or-KHR choice for dynamic rendering / sync2), so the verdict here is the verdict the engine would reach on that device.
// A change to the profile table that would refuse one of these shipping GPUs (a new Required item, a higher limit) turns a case below red.
//
// Expectations are derived from the code in evaluate(), not copied from its output; the reasoning is in the comment above each table.
// Out of scope (noted for later): a profiles-layer box row (VK_LAYER_KHRONOS_profiles + profile_file) that clamps a real driver to one of these.
#include "doctest.h"

#include "vk_device_profile.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace mc2vk::profile;

namespace {

struct Fixture {
    std::map<std::string, std::string> kv;
    std::vector<std::string> extensions;
    std::string get(const std::string& k) const {
        auto it = kv.find(k);
        return it == kv.end() ? std::string() : it->second;
    }
    bool flag(const std::string& k) const { return get(k) == "1"; }
    uint64_t num(const std::string& k) const { return std::stoull(get(k)); }
    bool has(const std::string& k, const std::string& word) const {
        std::istringstream in(get(k));
        for (std::string w; in >> w;)
            if (w == word) return true;
        return false;
    }
};

Fixture loadFixture(const std::string& name) {
    Fixture f;
    const std::string path = MC2_REPO_ROOT + "/tests/fixtures/vk_profiles/" + name + ".caps";
    std::ifstream in(path);
    REQUIRE_MESSAGE(in.good(), "missing fixture " << path);
    for (std::string line; std::getline(in, line);) {
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find(" = ");
        REQUIRE(eq != std::string::npos);
        const std::string key = line.substr(0, eq), val = line.substr(eq + 3);
        if (key == "extension") f.extensions.push_back(val);
        else f.kv[key] = val;
    }
    return f;
}

// Mirror of gather(): Vulkan 1.1 SubgroupProperties bits are STAGE_COMPUTE = 0x20 and BASIC 1 / ARITHMETIC 4 / BALLOT 8.
DeviceCaps capsFromFixture(const Fixture& f, uint32_t instanceApi) {
    DeviceCaps c;
    c.deviceApi = static_cast<uint32_t>(f.num("apiVersion"));
    c.instanceApi = instanceApi;
    c.maxImageDimension2D = static_cast<uint32_t>(f.num("limits.maxImageDimension2D"));
    c.maxBoundDescriptorSets = static_cast<uint32_t>(f.num("limits.maxBoundDescriptorSets"));
    c.maxPushConstantsSize = static_cast<uint32_t>(f.num("limits.maxPushConstantsSize"));
    c.maxStorageBufferRange = f.num("limits.maxStorageBufferRange");
    c.independentBlend = f.flag("VkPhysicalDeviceFeatures.independentBlend");
    c.samplerAnisotropy = f.flag("VkPhysicalDeviceFeatures.samplerAnisotropy");
    c.shaderSampledImageArrayDynamicIndexing = f.flag("VkPhysicalDeviceFeatures.shaderSampledImageArrayDynamicIndexing");
    if (c.usableApi() >= makeApi(1, 1)) {
        c.subgroupSize = static_cast<uint32_t>(f.num("subgroup.subgroupSize"));
        c.subgroupStages = f.has("subgroup.supportedStages", "VK_SHADER_STAGE_COMPUTE_BIT") ? 0x20u : 0u;
        c.subgroupOps = (f.has("subgroup.supportedOperations", "VK_SUBGROUP_FEATURE_BASIC_BIT") ? 1u : 0u) |
                        (f.has("subgroup.supportedOperations", "VK_SUBGROUP_FEATURE_ARITHMETIC_BIT") ? 4u : 0u) |
                        (f.has("subgroup.supportedOperations", "VK_SUBGROUP_FEATURE_BALLOT_BIT") ? 8u : 0u);
    }
    c.extensions = f.extensions;
    auto depthOk = [&](const char* fmt) {
        const std::string k = std::string(fmt) + ".optimalTilingFeatures";
        return f.has(k, "VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT") && f.has(k, "VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT");
    };
    c.depthD32 = depthOk("VK_FORMAT_D32_SFLOAT");
    c.depthD32S8 = depthOk("VK_FORMAT_D32_SFLOAT_S8_UINT");
    if (c.usableApi() < kFloorApi) return c;   // gather() returns here: no 1.2 / 1.3 feature struct is read
    const std::string v12 = "VkPhysicalDeviceVulkan12Features.";
    c.timelineSemaphore = f.flag(v12 + "timelineSemaphore");
    c.bufferDeviceAddress = f.flag(v12 + "bufferDeviceAddress");
    c.descriptorIndexing = f.flag(v12 + "descriptorIndexing");
    c.runtimeDescriptorArray = f.flag(v12 + "runtimeDescriptorArray");
    c.shaderSampledImageArrayNonUniformIndexing = f.flag(v12 + "shaderSampledImageArrayNonUniformIndexing");
    c.descriptorBindingPartiallyBound = f.flag(v12 + "descriptorBindingPartiallyBound");
    const bool core13 = c.usableApi() >= kUseApi13;
    // gather() chains the KHR struct only when the extension is listed, so a missing extension reads as false.
    c.dynamicRendering = core13 ? f.flag("VkPhysicalDeviceVulkan13Features.dynamicRendering")
                                : (c.hasExtension(kExtDynamicRendering) && f.flag("VkPhysicalDeviceDynamicRenderingFeaturesKHR.dynamicRendering"));
    c.synchronization2 = core13 ? f.flag("VkPhysicalDeviceVulkan13Features.synchronization2")
                                : (c.hasExtension(kExtSynchronization2) && f.flag("VkPhysicalDeviceSynchronization2FeaturesKHR.synchronization2"));
    return c;
}

using Names = std::vector<std::string>;

// One entry per shipped GPU. Every file is a gpuinfo export of a card that runs the Vulkan baseline profiles, and the facts below were read from the
// fixture and checked against evaluate()'s rules:
//   api_1_2: apiVersion >= 1.2 for all (1.3.270 .. 1.4.x); limits: image >= 8192, sets >= 4, push >= 128, storage range >= 128 MiB;
//   the six Vulkan12 bits (timeline, BDA, descriptor indexing + runtime array + non-uniform sampled-image + partially bound) set;
//   core-1.3 dynamicRendering / synchronization2 set and the KHR extensions listed; independentBlend, samplerAnisotropy, sampled-image dynamic
//   indexing set; compute stage plus BASIC/ARITHMETIC/BALLOT subgroup ops; D32 and D32S8 attachable and sampleable; VK_KHR_swapchain and
//   VK_EXT_memory_budget listed. So the verdict is accepted for each, with nothing missing, and every feature is enabled.
// Boundary values pinned per card (a tightened limit constant would refuse exactly these):
//   rx7800xt_win: maxPushConstantsSize 128 (== kMinPushConstantsSize); llvmpipe_linux: maxStorageBufferRange 128 MiB (== kMin), 8 descriptor sets.
struct Gpu {
    const char* fixture;
    const char* label;
};
const Gpu kGpus[] = {
    {"rx7800xt_win", "RX 7800 XT, Windows 2.0.294 (Vulkan 1.3.270)"},
    {"rx7800xt_radv", "RX 7800 XT, RADV navi32 (Mesa 24.0)"},
    {"rx9070xt_win", "RX 9070 XT, Windows 2.0.395"},
    {"rx5700_win", "RX 5700, Windows 2.0.353"},
    {"rx570_radv", "RX 570 (Polaris10), RADV Mesa 26.0"},
    {"rtx2070_linux", "RTX 2070, NVIDIA 610.43 Linux"},
    {"gtx1650_win", "GTX 1650, NVIDIA 610.62 Windows"},
    {"uhd750_win", "Intel UHD 750, Windows 0.405"},
    {"llvmpipe_linux", "llvmpipe (LLVM 22.1.6, 256-bit), Mesa 26.1"},
};

// The features evaluate() enables for a fully capable device, in its push order (timeline, BDA, the descriptor-indexing group with the optional
// dynamic-indexing bit, dynamicRendering, synchronization2, independentBlend, samplerAnisotropy).
const Names kAllFeatures = {"timelineSemaphore", "bufferDeviceAddress", "descriptorIndexing", "runtimeDescriptorArray",
                            "shaderSampledImageArrayNonUniformIndexing", "descriptorBindingPartiallyBound", "shaderSampledImageArrayDynamicIndexing",
                            "dynamicRendering", "synchronization2", "independentBlend", "samplerAnisotropy"};

uint32_t instanceOf(unsigned minor) { return makeApi(1, minor); }

}  // namespace

TEST_CASE("vk_profile_oracle: every shipping GPU is accepted on a 1.3 instance (core dynamic rendering and sync2)") {
    // usableApi = min(device, 1.3) = 1.3 >= kUseApi13, so both facilities route Core and no KHR extension is requested. With a present window the
    // swapchain is Required and listed; VK_EXT_memory_budget is listed by every fixture, so it is enabled; nothing is missing.
    const Names expectExt = {"VK_KHR_swapchain", "VK_EXT_memory_budget"};
    for (const Gpu& g : kGpus) {
        CAPTURE(g.label);
        const DeviceCaps c = capsFromFixture(loadFixture(g.fixture), instanceOf(3));
        const Evaluation e = evaluate(c, true);
        CHECK(e.accepted);
        CHECK(e.missingRequired.empty());
        CHECK(e.missingWanted.empty());
        CHECK(e.enabledFeatures == kAllFeatures);
        CHECK(e.deviceExtensions == expectExt);
        CHECK(routeOf(e, "dynamic_rendering") == Route::Core);
        CHECK(routeOf(e, "synchronization2") == Route::Core);
        CHECK(routeOf(e, "subgroup_compute") == Route::Limit);
        CHECK(routeOf(e, "memory_budget") == Route::Ext);
        CHECK(e.items.size() == std::size(kItemIds));
    }
}

TEST_CASE("vk_profile_oracle: on a 1.2 instance the same GPUs fall back to the KHR extensions for dynamic rendering and sync2") {
    // instanceApi 1.2 caps usableApi at 1.2 < kUseApi13: routeFor() needs the extension listed AND the KHR struct bit. All fixtures list both
    // and set both, so each is still accepted, now with Route::Ext for those two items and the extensions enabled in table order
    // (swapchain, dynamic_rendering, synchronization2, memory_budget). The feature list is unchanged.
    const Names expectExt = {"VK_KHR_swapchain", "VK_KHR_dynamic_rendering", "VK_KHR_synchronization2", "VK_EXT_memory_budget"};
    for (const Gpu& g : kGpus) {
        CAPTURE(g.label);
        const DeviceCaps c = capsFromFixture(loadFixture(g.fixture), instanceOf(2));
        const Evaluation e = evaluate(c, true);
        CHECK(e.accepted);
        CHECK(e.missingRequired.empty());
        CHECK(e.enabledFeatures == kAllFeatures);
        CHECK(e.deviceExtensions == expectExt);
        CHECK(routeOf(e, "dynamic_rendering") == Route::Ext);
        CHECK(routeOf(e, "synchronization2") == Route::Ext);
    }
}

TEST_CASE("vk_profile_oracle: no present window makes the swapchain optional and keeps it out of the extension list") {
    // wantPresent=false: "swapchain" is Wanted and VK_KHR_swapchain is not added to deviceExtensions, but the device is still accepted.
    const DeviceCaps c = capsFromFixture(loadFixture("rx7800xt_win"), instanceOf(3));
    const Evaluation e = evaluate(c, false);
    CHECK(e.accepted);
    CHECK(e.deviceExtensions == Names{"VK_EXT_memory_budget"});
    CHECK(e.missingWanted.empty());   // the extension is present, so even the optional item is satisfied
}

TEST_CASE("vk_profile_oracle: fixtures pin the limit boundaries the table is checked against") {
    CHECK(loadFixture("rx7800xt_win").num("limits.maxPushConstantsSize") == kMinPushConstantsSize);
    CHECK(loadFixture("llvmpipe_linux").num("limits.maxStorageBufferRange") == kMinStorageBufferRange);
    CHECK(loadFixture("llvmpipe_linux").num("limits.maxBoundDescriptorSets") >= kMinBoundDescriptorSets);
    for (const Gpu& g : kGpus) {
        CAPTURE(g.label);
        CHECK(loadFixture(g.fixture).num("apiVersion") >= kFloorApi);
    }
}

namespace {
struct Mutation {
    const char* what;
    const char* fixture;
    unsigned instanceMinor;
    bool wantPresent;
    void (*mutate)(DeviceCaps&);
    Names expectRequiredMissing;   // in table order
    Names expectWantedMissing;
    const char* why;
};

void dropExt(DeviceCaps& c, const char* name) {
    c.extensions.erase(std::remove(c.extensions.begin(), c.extensions.end(), name), c.extensions.end());
}
}  // namespace

TEST_CASE("vk_profile_oracle: each Required item refuses a real device that loses it; Wanted items only report") {
    // Each case perturbs one real fixture by exactly one fact and names the rule in evaluate() that must react. Table order of kItemIds is
    // api_1_2, swapchain, limits, timeline_semaphore, buffer_device_address, descriptor_indexing, dynamic_rendering, synchronization2,
    // independent_blend, subgroup_compute, memory_budget, sampler_anisotropy, depth_formats.
    const Mutation cases[] = {
        {"llvmpipe storage range one byte under 128 MiB", "llvmpipe_linux", 3, true,
         [](DeviceCaps& c) { c.maxStorageBufferRange = kMinStorageBufferRange - 1; }, {"limits"}, {}, "limitsOk: maxStorageBufferRange >= 128 MiB"},
        {"7800 XT Windows push constants 127", "rx7800xt_win", 3, true, [](DeviceCaps& c) { c.maxPushConstantsSize = 127; }, {"limits"}, {},
         "limitsOk: push >= 128"},
        {"3 bound descriptor sets", "rx9070xt_win", 3, true, [](DeviceCaps& c) { c.maxBoundDescriptorSets = 3; }, {"limits"}, {}, "limitsOk: sets >= 4"},
        {"max image dimension 8191", "uhd750_win", 3, true, [](DeviceCaps& c) { c.maxImageDimension2D = 8191; }, {"limits"}, {},
         "limitsOk: 2D >= 8192 (largest shadow map)"},
        {"no timeline semaphore", "rx5700_win", 3, true, [](DeviceCaps& c) { c.timelineSemaphore = false; }, {"timeline_semaphore"}, {},
         "timeline = 1.2+ AND the bit"},
        {"no buffer device address", "rx570_radv", 3, true, [](DeviceCaps& c) { c.bufferDeviceAddress = false; }, {"buffer_device_address"}, {},
         "bda = 1.2+ AND the bit"},
        {"descriptorBindingPartiallyBound off", "gtx1650_win", 3, true, [](DeviceCaps& c) { c.descriptorBindingPartiallyBound = false; },
         {"descriptor_indexing"}, {}, "descriptor_indexing needs the umbrella bit AND all three individual bits"},
        {"no swapchain extension with a present window", "rtx2070_linux", 3, true, [](DeviceCaps& c) { dropExt(c, "VK_KHR_swapchain"); },
         {"swapchain"}, {}, "wantPresent makes swapchain Required"},
        {"no dynamic rendering bit (core 1.3)", "rx7800xt_radv", 3, true, [](DeviceCaps& c) { c.dynamicRendering = false; }, {"dynamic_rendering"}, {},
         "routeFor: bit off -> None"},
        {"1.2 instance, VK_KHR_synchronization2 not listed", "rx7800xt_win", 2, true, [](DeviceCaps& c) {
             dropExt(c, "VK_KHR_synchronization2");
             c.synchronization2 = false;   // gather() chains the KHR struct only when the extension is listed
         }, {"synchronization2"}, {}, "below 1.3, routeFor needs the extension"},
        {"no independent blend", "rx9070xt_win", 3, true, [](DeviceCaps& c) { c.independentBlend = false; }, {"independent_blend"}, {},
         "two-attachment pipelines need it"},
        {"no compute-stage subgroup ops", "rx570_radv", 3, true, [](DeviceCaps& c) { c.subgroupStages = 0; }, {"subgroup_compute"}, {},
         "stage mask must include COMPUTE (0x20)"},
        {"no subgroup ballot", "llvmpipe_linux", 3, true, [](DeviceCaps& c) { c.subgroupOps &= ~8u; }, {"subgroup_compute"}, {},
         "ops must hold BASIC|ARITHMETIC|BALLOT"},
        {"no D32_SFLOAT_S8_UINT depth", "gtx1650_win", 3, true, [](DeviceCaps& c) { c.depthD32S8 = false; }, {"depth_formats"}, {},
         "both depth formats are Required"},
        {"1.1 instance", "rx7800xt_win", 1, true, nullptr,
         {"api_1_2", "timeline_semaphore", "buffer_device_address", "descriptor_indexing", "dynamic_rendering", "synchronization2"}, {},
         "instance below the floor: api_1_2 fails and gather() reads no 1.2 / 1.3 bits (usable 1.1), so the five facilities are missing too"},
        // Wanted items: accepted, reported in missingWanted, and the matching feature or extension is simply not enabled.
        {"no anisotropy", "uhd750_win", 3, true, [](DeviceCaps& c) { c.samplerAnisotropy = false; }, {}, {"sampler_anisotropy"}, "Wanted"},
        {"no memory budget extension", "rx570_radv", 3, true, [](DeviceCaps& c) { dropExt(c, "VK_EXT_memory_budget"); }, {}, {"memory_budget"}, "Wanted"},
        {"no swapchain extension, headless", "llvmpipe_linux", 3, false, [](DeviceCaps& c) { dropExt(c, "VK_KHR_swapchain"); }, {}, {"swapchain"},
         "wantPresent=false makes swapchain Wanted"},
    };
    for (const Mutation& m : cases) {
        CAPTURE(m.what);
        CAPTURE(m.why);
        DeviceCaps c = capsFromFixture(loadFixture(m.fixture), instanceOf(m.instanceMinor));
        if (m.mutate) m.mutate(c);
        const Evaluation e = evaluate(c, m.wantPresent);
        CHECK(e.missingRequired == m.expectRequiredMissing);
        CHECK(e.missingWanted == m.expectWantedMissing);
        CHECK(e.accepted == m.expectRequiredMissing.empty());
        if (!e.accepted) {   // a refused device gets nothing enabled
            CHECK(e.enabledFeatures.empty());
            CHECK(e.deviceExtensions.empty());
        }
    }
}

TEST_CASE("vk_profile_oracle: Wanted misses drop only their own feature or extension") {
    {
        DeviceCaps c = capsFromFixture(loadFixture("uhd750_win"), instanceOf(3));
        c.samplerAnisotropy = false;
        const Evaluation e = evaluate(c, true);
        CHECK_FALSE(featureEnabled(e, "samplerAnisotropy"));
        CHECK(featureEnabled(e, "independentBlend"));
    }
    {
        DeviceCaps c = capsFromFixture(loadFixture("rx570_radv"), instanceOf(3));
        dropExt(c, "VK_EXT_memory_budget");
        CHECK(evaluate(c, true).deviceExtensions == Names{"VK_KHR_swapchain"});
    }
    {   // the optional dynamic-indexing bit is not part of the descriptor_indexing verdict, only of what is enabled
        DeviceCaps c = capsFromFixture(loadFixture("rx9070xt_win"), instanceOf(3));
        c.shaderSampledImageArrayDynamicIndexing = false;
        const Evaluation e = evaluate(c, true);
        CHECK(e.accepted);
        CHECK(featureEnabled(e, "descriptorIndexing"));
        CHECK_FALSE(featureEnabled(e, "shaderSampledImageArrayDynamicIndexing"));
    }
}
