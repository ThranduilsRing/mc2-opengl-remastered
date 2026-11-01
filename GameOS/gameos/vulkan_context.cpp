#include "../../RenderCore/TextureService.h"
// vulkan_context.cpp
//
// VK-P13-SELECTOR-1 / T2 -- the standalone shared engine Vulkan context.
// Owns instance + physical device + logical device + graphics queue + VMA +
// volk device dispatch. Born once at engine init (gameosmain);
// any error is reported to the caller (fatal). Device ownership lives HERE;
// consumers borrow it.
//
// Whole TU is inert unless MC2_VULKAN is defined. Device bring-up keeps the
// proven volk + VMA + validation-preset convention used by earlier Vulkan
// region probes, so we do not re-litigate device selection.

#ifdef MC2_VULKAN

// VK-RUNG8-ITEM3: the external-memory/-semaphore Win32 device extension family was
// removed (Metal-gate blocker). This define now survives ONLY for the Win32 SURFACE
// extension-name macro (VK_KHR_WIN32_SURFACE_EXTENSION_NAME), gated behind
// VK_USE_PLATFORM_WIN32_KHR in volk/vulkan_core.h, enabled below when wantPresent.
// On a macOS/Metal port this whole define + the surface pair become a platform branch
// (VK_EXT_metal_surface via SDL_Vulkan_GetInstanceExtensions).
#if defined(_WIN32) && !defined(VK_USE_PLATFORM_WIN32_KHR)
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include "vulkan_context.h"
#include "present_thread.h"
#include "../../mclib/sim_overlap.h"
#ifndef _WIN32
#define NO_SDL_VULKAN_TYPEDEFS
#include <SDL2/SDL_vulkan.h>
#endif
#include <tracy/Tracy.hpp>
#include "mc2_diag_verbose.h"
#include "diagnostic_trace.h"
#include "vk_device_profile.h"
#include "vk_gpu_profile.h"
#include "vk_validation_line.h"
#ifdef MC2_VULKAN_ISLAND
#include "vk_image_resource.h"
#include "vk_texture_residency.h"
#endif
#include "Gates/gate.h"
#include "Gates/gate_cold.h"   // str<>()

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include "vk_mem_alloc.h"

#include <cassert>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>
#include "../../mclib/battle_bench_sink.h"

namespace mc2vk {

VulkanContext::VulkanContext() = default;
VulkanContext::~VulkanContext() { destroy(); }

namespace {

void ctxlog(const char* fmt, ...) {
    std::fprintf(stderr, "[VK_ENGINE_CONTEXT] ");
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
}

// The `[VK_PROFILE v1]` line goes to stderr beside the other [VK_ENGINE_CONTEXT] lines; the smoke parser grades a refused verdict red.
void profileLog(std::string_view line) {
    std::fprintf(stderr, "%.*s\n", static_cast<int>(line.size()), line.data());
    std::fflush(stderr);
}

// The single process-wide shared context. It is heap-allocated and deliberately released from its unique_ptr at birth, so no destructor of any duration
// ever runs: a static-duration object would run destroy() (island teardown, light feed) during static destruction, when the other process-wide owners it
// depends on may already be gone. The explicit destroy() at engine shutdown is the only teardown; a process that exits any other way leaves the device
// to the driver, as the OS reclaims it.
VulkanContext& engineContext() {
    static VulkanContext* const context = std::make_unique<VulkanContext>().release();
    return *context;
}

// True while the debug-utils messenger is up (a validation run); object names are only attached then.
bool g_debugNames = false;

// Debug-utils callback pointer target: increments the live context's error
// counter. Because there is exactly one engine context, a file-static back
// pointer is sufficient (set in init()).
VulkanContext* g_cbContext = nullptr;

// One validation-layer message in the engine log (vk_validation_line.h); the smoke parser grades an error red and counts the warnings. The layer limits
// repeats of one message id itself, so the log stays small.
void validationLog(std::string_view severity, std::string_view id, std::string_view text) {
    const std::string line = formatValidationLine(severity, id, text);
    std::fprintf(stderr, "%s\n", line.c_str());
    std::fflush(stderr);
}

VKAPI_ATTR VkBool32 VKAPI_CALL engine_ctx_debug_cb(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT* pData,
    void* /*user*/) {
    const char* msg = (pData && pData->pMessage) ? pData->pMessage : "(no message)";
    const char* id = (pData && pData->pMessageIdName) ? pData->pMessageIdName : "-";
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        if (g_cbContext) g_cbContext->mutableHealth().validationErrors.fetch_add(1, std::memory_order_relaxed);
        validationLog("error", id, msg);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        validationLog("warning", id, msg);
    }
    return VK_FALSE;
}

// Validation preset resolver (same values the islands accept via
// MC2_VULKAN_VALIDATION). Kept local so the context has no link dependency on
// the island TU (which is #ifdef MC2_VULKAN_ISLAND and may not be compiled).
bool resolveValidationPreset(std::vector<VkValidationFeatureEnableEXT>& feats,
                             const char*& resolvedName) {
    feats.clear();
    resolvedName = "off";
    if (!mc2::gate::isSet<"MC2_VULKAN_VALIDATION">()) return false;
    const std::string env = mc2::gate::str<"MC2_VULKAN_VALIDATION">();   // "" when set but empty
    auto eq = [&](const char* s) { return env == s; };
    if (eq("0") || eq("off")) { resolvedName = "off"; return false; }
    if (eq("1") || eq("core") || eq("")) { resolvedName = "core"; return true; }
    if (eq("sync")) {
        resolvedName = "sync";
        feats.push_back(VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT);
        return true;
    }
    if (eq("gpu-assisted")) {
        resolvedName = "gpu-assisted";
        feats.push_back(VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT);
        feats.push_back(VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_RESERVE_BINDING_SLOT_EXT);
        return true;
    }
    if (eq("best-practices")) {
        resolvedName = "best-practices";
        feats.push_back(VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT);
        return true;
    }
    if (eq("debug-printf")) {
        resolvedName = "debug-printf";
        feats.push_back(VK_VALIDATION_FEATURE_ENABLE_DEBUG_PRINTF_EXT);
        return true;
    }
    ctxlog("validation preset '%s' unrecognized; using 'core'.", env.c_str());
    resolvedName = "core";
    return true;
}

}  // namespace

bool VulkanContext::init(bool wantPresent) {
    mc2::overlap::assertOwner(mc2::overlap::Owner::Render, "VulkanContext::init");
    health_.attempted = true;

    // Idempotent: already up, or already tried and failed.
    if (device_ != VK_NULL_HANDLE) return true;
    if (!health_.fallbackReason.empty()) return false;

    auto fail = [&](const std::string& reason) -> bool {
        health_.fallbackReason = reason;
        ctxlog("engine context init FAILED: %s -- fail-soft to GL.", reason.c_str());
        destroy();               // tear down partial state
        health_.fallbackReason = reason; // destroy() clears; restore for the caller
        return false;
    };

    // Fail-soft proof knob (plan T4): MC2_VK_FORCE_FAIL=1 forces the engine
    // context bring-up to fail deterministically BEFORE any Vulkan object is
    // created, so the fatalBackendFailure path can be exercised on
    // demand (no broken-ICD juggling). Presence-gated; default OFF.
    {
        if (mc2::gate::get<"MC2_VK_FORCE_FAIL">()) {
            return fail("MC2_VK_FORCE_FAIL=1 forced failure (failure-path proof knob)");
        }
    }

    g_cbContext = this;

    // ---- volk runtime loader ----
    VkResult r = volkInitialize();
    if (r != VK_SUCCESS) {
        return fail("volkInitialize failed (no Vulkan runtime)");
    }
    initedVolk_ = true;

    // ---- instance version: the 1.2 floor, and 1.3 when the loader has it (core dynamic rendering / synchronization2) ----
    // Raising the version enables no device feature by itself: the device profile below queries and enables each one it wants.
    uint32_t loaderApi = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&loaderApi);
    const uint32_t instanceApi = loaderApi < profile::kUseApi13 ? loaderApi : profile::kUseApi13;
    if (instanceApi < profile::kFloorApi) {
        profile::DeviceCaps none;
        none.instanceApi = loaderApi;
        profile::Evaluation refused;
        refused.accepted = false;
        refused.items.push_back({"api_1_2", profile::Need::Required, false, profile::Route::None});
        refused.missingRequired.push_back("api_1_2");
        profileLog(profile::formatLine("(no device queried: the instance version the engine can request is below the floor)", none, refused));
        return fail("the engine can request Vulkan instance version " + profile::versionText(instanceApi) + " (the loader reports " + profile::versionText(loaderApi) +
                    "), below the " + profile::floorText() + " floor");
    }

    // ---- validation preset ----
    std::vector<VkValidationFeatureEnableEXT> valFeatures;
    const char* valPreset = "off";
    bool wantValidation = resolveValidationPreset(valFeatures, valPreset);
    health_.validationEnabled = wantValidation;

    std::vector<const char*> layers;
    std::vector<const char*> instExts;

    // No interop instance caps are requested: physical-device properties2 is core at the 1.2 floor.

    // VK-P13 T3: surface instance extensions so the shared device can create a
    // VkSurfaceKHR for the hidden present window (SDL_Vulkan_CreateSurface needs
    // the instance to carry these). On Windows the surface pair is well-known
    // (SDL requires exactly these), so we do not need an SDL window before
    // instance creation.
    if (wantPresent) {
#ifdef _WIN32
        instExts.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
        instExts.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#else
        unsigned count = 0;
        if (!SDL_Vulkan_GetInstanceExtensions(nullptr, &count, nullptr)) return false;
        instExts.resize(count);
        if (!SDL_Vulkan_GetInstanceExtensions(nullptr, &count, instExts.data())) return false;
#endif
    }

    if (wantValidation) {
        uint32_t layerCount = 0;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
        std::vector<VkLayerProperties> avail(layerCount);
        if (layerCount) vkEnumerateInstanceLayerProperties(&layerCount, avail.data());
        const char* want = "VK_LAYER_KHRONOS_validation";
        bool found = false;
        for (const auto& lp : avail) {
            if (std::strcmp(lp.layerName, want) == 0) { found = true; break; }
        }
        if (found) {
            layers.push_back(want);
            instExts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            ctxlog("validation ON: layer %s enabled (preset=%s, +%zu feature(s)).",
                   want, valPreset, valFeatures.size());
        } else {
            wantValidation = false;
            health_.validationEnabled = false;
            ctxlog("validation requested but %s unavailable; continuing WITHOUT.", want);
            // A validation run that silently has no layer would look green with no coverage: the smoke grades this error red.
            validationLog("error", "layer_unavailable", std::string(want) + " was requested (MC2_VULKAN_VALIDATION) and the Vulkan loader does not offer it; this run is NOT validated");
        }
    }

    // VK-GPU-ZONES: VK_EXT_debug_utils whenever the loader offers it (a validation run already added it above), so command-buffer labels and
    // object names reach RGP / RenderDoc / Nsight in a normal run. Absent extension = no labels, no names.
    bool debugUtilsExt = false;
    for (const char* e : instExts) if (std::strcmp(e, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0) debugUtilsExt = true;
    if (!debugUtilsExt) {
        uint32_t extCount = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> availExts(extCount);
        if (extCount) vkEnumerateInstanceExtensionProperties(nullptr, &extCount, availExts.data());
        for (const auto& ep : availExts) {
            if (std::strcmp(ep.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0) {
                instExts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
                debugUtilsExt = true;
                break;
            }
        }
    }

    // ---- Instance ----
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "MC2 (engine Vulkan context)";
    app.pEngineName = "MC2-GameOS";
    app.apiVersion = instanceApi;

    VkValidationFeaturesEXT valFeaturesInfo{};
    valFeaturesInfo.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
    valFeaturesInfo.enabledValidationFeatureCount = static_cast<uint32_t>(valFeatures.size());
    valFeaturesInfo.pEnabledValidationFeatures    = valFeatures.empty() ? nullptr : valFeatures.data();

    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    ici.enabledLayerCount       = static_cast<uint32_t>(layers.size());
    ici.ppEnabledLayerNames     = layers.empty() ? nullptr : layers.data();
    ici.enabledExtensionCount   = static_cast<uint32_t>(instExts.size());
    ici.ppEnabledExtensionNames = instExts.empty() ? nullptr : instExts.data();
    if (wantValidation && !valFeatures.empty()) ici.pNext = &valFeaturesInfo;

    r = vkCreateInstance(&ici, nullptr, &instance_);
    if (r != VK_SUCCESS) return fail("vkCreateInstance failed");
    volkLoadInstance(instance_);
    if (debugUtilsExt && vkSetDebugUtilsObjectNameEXT) g_debugNames = true;   // names and labels need only the extension, not the messenger

    // ---- debug messenger ----
    if (wantValidation) {
        auto pfnCreate = (PFN_vkCreateDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT");
        if (pfnCreate) {
            VkDebugUtilsMessengerCreateInfoEXT mci{};
            mci.sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            mci.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            mci.pfnUserCallback = engine_ctx_debug_cb;
            VkResult mr = pfnCreate(instance_, &mci, nullptr, &messenger_);
            if (mr == VK_SUCCESS) {
                ctxlog("validation messenger engaged (preset=%s).", valPreset);
                g_debugNames = true;
                validationLog("info", "layer_enabled", std::string("VK_LAYER_KHRONOS_validation messenger engaged, preset=") + valPreset);
            } else {
                ctxlog("vkCreateDebugUtilsMessengerEXT failed (%d); no error capture.", static_cast<int>(mr));
                validationLog("error", "messenger_unavailable", "vkCreateDebugUtilsMessengerEXT failed (" + std::to_string(static_cast<int>(mr)) + "); this run is NOT validated");
            }
        } else {
            ctxlog("vkCreateDebugUtilsMessengerEXT not found; no error capture.");
            validationLog("error", "messenger_unavailable", "vkCreateDebugUtilsMessengerEXT is not exported; this run is NOT validated");
        }
    }

    // ---- physical device (prefer discrete when interop is requested) ----
    uint32_t devCount = 0;
    r = vkEnumeratePhysicalDevices(instance_, &devCount, nullptr);
    if (r != VK_SUCCESS || devCount == 0) return fail("no physical devices");
    std::vector<VkPhysicalDevice> devs(devCount);
    vkEnumeratePhysicalDevices(instance_, &devCount, devs.data());

    // ---- device profile: the first discrete device that meets every REQUIRED item, else the first device that does; if none does, the engine
    // refuses Vulkan (fatal, no GL fallback) and the refused device's line says which items it lacks ----
    profile::DeviceCaps caps;
    {
        struct Candidate { VkPhysicalDevice pd; profile::DeviceCaps caps; profile::Evaluation ev; std::string name; bool discrete; };
        std::vector<Candidate> cands;
        for (auto pd : devs) {
            VkPhysicalDeviceProperties p{};
            vkGetPhysicalDeviceProperties(pd, &p);
            Candidate c{pd, profile::gather(pd, instanceApi), {}, p.deviceName, p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU};
            c.ev = profile::evaluate(c.caps, wantPresent);
            cands.push_back(std::move(c));
        }
        const Candidate* pick = nullptr;
        // MC2_VK_DEVICE: the harness routes pass/fail runs to a named device (the loader ignores VK_LOADER_DRIVERS_SELECT here).
        std::string hint = mc2::gate::str<"MC2_VK_DEVICE">();
        if (!hint.empty()) {
            const auto lower = [](std::string v) { for (char& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); return v; };
            const std::string want = lower(hint);
            for (const Candidate& c : cands)
                if (c.ev.accepted && lower(c.name).find(want) != std::string::npos) { pick = &c; break; }
            if (!pick) {
                ctxlog("MC2_VK_DEVICE=%s matches no accepted device; using the default pick", hint.c_str());
            }
        }
        if (!pick)
            for (const Candidate& c : cands)
                if (c.ev.accepted && c.discrete) { pick = &c; break; }
        if (!pick)
            for (const Candidate& c : cands)
                if (c.ev.accepted) { pick = &c; break; }
        const bool accepted = pick != nullptr;
        if (!pick) {                                      // report the device the engine would have preferred
            pick = &cands[0];
            for (const Candidate& c : cands)
                if (c.discrete) { pick = &c; break; }
        }
        phys_ = pick->pd;
        caps = pick->caps;
        profile_ = pick->ev;
        luidValid_ = caps.deviceLuidValid;
        for (size_t i = 0; i < sizeof(luid_); ++i) luid_[i] = caps.deviceLuid[i];
        std::strncpy(health_.deviceName, pick->name.c_str(), sizeof(health_.deviceName) - 1);
        health_.deviceName[sizeof(health_.deviceName) - 1] = '\0';
        health_.profileLine = profile::formatLine(pick->name, pick->caps, pick->ev);
        profileLog(health_.profileLine);
        VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props.pNext = caps.usableApi() >= profile::kFloorApi ? &driver : nullptr;
        vkGetPhysicalDeviceProperties2(phys_, &props);
        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties(phys_, &memory);
        uint64_t localBytes = 0;
        for (uint32_t i = 0; i < memory.memoryHeapCount; ++i)
            if (memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) localBytes += memory.memoryHeaps[i].size;
        const auto& limits = props.properties.limits;
        const auto capacity = (std::min)({4096u, limits.maxPerStageDescriptorSamplers, limits.maxPerStageDescriptorSampledImages,
            limits.maxDescriptorSetSamplers, limits.maxDescriptorSetSampledImages, limits.maxPerStageResources});
        if (!accepted || mc2_diag::tagRequested("VK_CAPABILITY"))
        std::fprintf(stderr, "[VK_CAPABILITY v1] gpu=\"%s\" driver=\"%s %s\" driver_version=%u vram_mib=%llu low_floor_mib=512 bindless=%u samplers=%u refused=%s\n",
            pick->name.c_str(), driver.driverName, driver.driverInfo, props.properties.driverVersion,
            static_cast<unsigned long long>(localBytes / (1024 * 1024)), capacity, limits.maxPerStageDescriptorSamplers,
            profile::joinOr(pick->ev.missingRequired).c_str());
        if (!accepted) return fail("device profile: required item(s) missing on " + pick->name + ": " + profile::joinOr(pick->ev.missingRequired));
    }

    // ---- graphics queue family ----
    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys_, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qfCount);
    if (qfCount) vkGetPhysicalDeviceQueueFamilyProperties(phys_, &qfCount, qfs.data());
    for (uint32_t q = 0; q < qfCount; ++q) {
        if (qfs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) { gfxFamily_ = q; break; }
    }
    if (gfxFamily_ == UINT32_MAX) return fail("no graphics queue family");

    // ---- logical device ----
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = gfxFamily_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    // Every device extension comes from the profile, in table order: the swapchain of a present window, the KHR routes of dynamic rendering /
    // synchronization2 on a 1.2 device, and the memory-budget extension when the device has it (the VRAM line is then the OS's number and not VMA's
    // estimate). No external-memory extension is requested: no VK resource is exportable.
    std::vector<const char*> devExts;
    for (const std::string& e : profile_.deviceExtensions) devExts.push_back(e.c_str());
    if (caps.hasExtension(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME)) devExts.push_back(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
    if (caps.hasExtension(VK_AMD_BUFFER_MARKER_EXTENSION_NAME)) devExts.push_back(VK_AMD_BUFFER_MARKER_EXTENSION_NAME);

    // The feature bits the profile selected. Eight bits are required by the owner ruling of 2026-10-13 (timeline semaphores, buffer device address, the
    // descriptor-indexing group that is the bindless table's definition, dynamic rendering, synchronization2) and enabled although no island consumes them
    // yet; independentBlend is required because the islands' two-attachment pipelines already need it. An accepted device has every one and no island
    // carries a fallback arm for any. Each bit goes through the struct that defines it (the core 1.0 features, the core 1.2 / 1.3 aggregate when the
    // route is core, the KHR struct otherwise).

    VkPhysicalDeviceFeatures2 enableFeatures{};
    enableFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    // Optional water-only near-plane clamp for cinematic cameras. Devices
    // without this core feature retain ordinary raster clipping.
    VkPhysicalDeviceFeatures optionalWaterFeatures{};
    vkGetPhysicalDeviceFeatures(phys_, &optionalWaterFeatures);
    enableFeatures.features.depthClamp = optionalWaterFeatures.depthClamp;
    VkPhysicalDeviceVulkan12Features en12{};
    en12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    VkPhysicalDeviceVulkan13Features en13{};
    en13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceDynamicRenderingFeaturesKHR enDr{};
    enDr.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR;
    VkPhysicalDeviceSynchronization2FeaturesKHR enS2{};
    enS2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR;
    {
        enableFeatures.features.independentBlend = featureEnabled("independentBlend") ? VK_TRUE : VK_FALSE;
        enableFeatures.features.samplerAnisotropy = featureEnabled("samplerAnisotropy") ? VK_TRUE : VK_FALSE;
        enableFeatures.features.shaderSampledImageArrayDynamicIndexing = featureEnabled("shaderSampledImageArrayDynamicIndexing") ? VK_TRUE : VK_FALSE;
        en12.timelineSemaphore = featureEnabled("timelineSemaphore") ? VK_TRUE : VK_FALSE;
        en12.bufferDeviceAddress = featureEnabled("bufferDeviceAddress") ? VK_TRUE : VK_FALSE;
        en12.descriptorIndexing = featureEnabled("descriptorIndexing") ? VK_TRUE : VK_FALSE;
        en12.runtimeDescriptorArray = featureEnabled("runtimeDescriptorArray") ? VK_TRUE : VK_FALSE;
        en12.shaderSampledImageArrayNonUniformIndexing = featureEnabled("shaderSampledImageArrayNonUniformIndexing") ? VK_TRUE : VK_FALSE;
        en12.descriptorBindingPartiallyBound = featureEnabled("descriptorBindingPartiallyBound") ? VK_TRUE : VK_FALSE;
        const bool drCore = route("dynamic_rendering") == profile::Route::Core;
        const bool s2Core = route("synchronization2") == profile::Route::Core;
        if (featureEnabled("dynamicRendering")) (drCore ? en13.dynamicRendering : enDr.dynamicRendering) = VK_TRUE;
        if (featureEnabled("synchronization2")) (s2Core ? en13.synchronization2 : enS2.synchronization2) = VK_TRUE;
        void** tail = &enableFeatures.pNext;
        auto chain = [&](auto& s) { *tail = &s; tail = &s.pNext; };
        chain(en12);
        if (drCore || s2Core) chain(en13);
        if (featureEnabled("dynamicRendering") && !drCore) chain(enDr);
        if (featureEnabled("synchronization2") && !s2Core) chain(enS2);
    }

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &enableFeatures;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount   = static_cast<uint32_t>(devExts.size());
    dci.ppEnabledExtensionNames = devExts.empty() ? nullptr : devExts.data();
    r = vkCreateDevice(phys_, &dci, nullptr, &device_);
    if (r != VK_SUCCESS) { device_ = VK_NULL_HANDLE; return fail("vkCreateDevice failed"); }

    // NOTE (R2): volkLoadDevice writes the PROCESS-GLOBAL volk device dispatch
    // table. The R2 interlock in gameosmain guarantees no postprocess-interop
    // island is active in the same session, so this device is the sole owner of
    // the global table -- no clobber.
    volkLoadDevice(device_);
    vkGetDeviceQueue(device_, gfxFamily_, 0, &gfxQueue_);
    infra::init(device_, phys_);

    // ---- the graphics timeline (CompletionTracker's semaphore; the profile requires timeline semaphores) ----
    {
        VkSemaphoreTypeCreateInfo tci{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        tci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        tci.initialValue = 0;
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        sci.pNext = &tci;
        if (vkCreateSemaphore(device_, &sci, nullptr, &gfxTimeline_) != VK_SUCCESS) { gfxTimeline_ = VK_NULL_HANDLE; return fail("graphics timeline semaphore"); }
        tracker_ = CompletionTracker{};
    }
#ifdef MC2_VULKAN_ISLAND
    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(phys_, &props);
        const VkDeviceSize a = std::max<VkDeviceSize>({props.limits.minUniformBufferOffsetAlignment, props.limits.minStorageBufferOffsetAlignment, 16});
        frameAlign_ = a;
    }
#endif

    // ---- VMA allocator (volk-imported functions) ----
    {
        VmaAllocatorCreateInfo aci{};
        aci.physicalDevice = phys_;
        aci.device = device_;
        aci.instance = instance_;
        aci.vulkanApiVersion = caps.usableApi();   // never above what the device or the instance was created for
        if (route("memory_budget") == profile::Route::Ext) aci.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
        if (featureEnabled("bufferDeviceAddress")) aci.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
        VmaVulkanFunctions fns{};
        r = vmaImportVulkanFunctionsFromVolk(&aci, &fns);
        if (r != VK_SUCCESS) return fail("vmaImportVulkanFunctionsFromVolk failed");
        aci.pVulkanFunctions = &fns;
        r = vmaCreateAllocator(&aci, &allocator_);
        if (r != VK_SUCCESS) { allocator_ = nullptr; return fail("vmaCreateAllocator failed"); }
    }

#ifdef MC2_VULKAN_ISLAND
    if (!RenderCore::activeTextureService) {
        textureServiceOwner_ = std::make_unique<RenderCore::TextureService>();
        RenderCore::activeTextureService = textureServiceOwner_.get();
    }
    textures_ = std::make_unique<TextureResidency>(*this, RenderCore::textureService());
    if (!textures_->init()) return fail("texture service initialization");
#endif
    health_.created = true;
    health_.fallbackReason.clear();
#ifdef MC2_VULKAN_ISLAND
    gpuprof::init(*this);   // Tracy GPU context + debug labels + MC2_VK_GPU_TIMESTAMPS ring, on the one graphics queue
#endif
    ctxlog("engine Vulkan device created: device_name=\"%s\" present=%d validation=%s.",
           health_.deviceName, wantPresent ? 1 : 0, valPreset);
    return true;
}

#ifdef MC2_VULKAN_ISLAND
void VulkanContext::retireIsland(IslandSlot slot) {
    std::unique_ptr<IslandState>& parked = islandSlot(slot);
    if (!parked) return;
    // The order of these lines is the shutdown order: with the image-owner lines of vk_image_resource.cpp they let a log show the destroy() precondition.
    static constexpr const char* kSlotNames[] = {"Vfx", "Sky", "LegacyShape", "Preview", "Mech", "ColorProp", "Water", "Shadow", "Terrain", "PassTimer", "ActorCull", "Vegetation"};
    static_assert(sizeof(kSlotNames) / sizeof(kSlotNames[0]) == static_cast<size_t>(IslandSlot::Count), "name every island slot");
    if (mc2_diag::tagRequested("VK_SHUTDOWN")) ctxlog("retire island slot=%s.", kSlotNames[static_cast<size_t>(slot)]);
    waitIdle("retire_island");
    deferred_.drainAll();   // parked owners may name this island's pools (a retired sprite page frees its set): they go while the island is still alive
    parked.reset();
}
#endif

void VulkanContext::waitIdle(std::string_view why) const {
    mc2::overlap::assertOwner(mc2::overlap::Owner::Render, "VulkanContext::waitIdle");
    if (device_ == VK_NULL_HANDLE) return;
    std::lock_guard<std::mutex> lock(submitMutex_);
    const VkResult r = vkDeviceWaitIdle(device_);
    // A lost device ends the wait with an error; the release that follows is still safe, but the log must say the GPU did not drain.
    if (r != VK_SUCCESS) ctxlog("vkDeviceWaitIdle failed before %.*s: VkResult=%d.", static_cast<int>(why.size()), why.data(), static_cast<int>(r));
    if (r == VK_ERROR_DEVICE_LOST) infra::deviceLost();
}

VkResult VulkanContext::submit(const VkSubmitInfo& si, VkFence fence, uint64_t* outValue) const {
    mc2::overlap::assertOwner(mc2::overlap::Owner::Render, "VulkanContext::submit");
    if (device_ == VK_NULL_HANDLE || gfxTimeline_ == VK_NULL_HANDLE) return VK_ERROR_DEVICE_LOST;
    std::lock_guard<std::mutex> lock(submitMutex_);   // the value and the queue order must agree
#ifdef MC2_VULKAN_ISLAND
    flushUploadsLocked();   // queued uploads run ahead of every submit that may sample them
#endif
    return submitLocked(si, fence, outValue);
}

VkResult VulkanContext::present(VkQueue queue, const VkPresentInfoKHR& info) const {
    std::lock_guard<std::mutex> lock(submitMutex_);
    present_thread::seal(); // Next-frame submits must stay behind this present, even when main wakes immediately.
    mc2::overlap::assertOwner(mc2::overlap::Owner::Present, "vkQueuePresentKHR");
    return vkQueuePresentKHR(queue, &info);
}

#ifdef MC2_VULKAN_ISLAND
GpuTicket VulkanContext::flushUploadsLocked() const {
    if (!uploads_.pending()) return {};
    OwnedCommandPool pool;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (!uploads_.build(*this, pool, cb)) return {};
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    uint64_t value = 0;
    if (submitLocked(si, VK_NULL_HANDLE, &value) != VK_SUCCESS) ctxlog("the upload batch submit failed: the queued uploads are lost.");
    // The command buffer lives in its pool and the dedicated staging buffers belong to the batch: both go with the frame.
    deferred_.retire(tracker_, std::move(pool));
    deferred_.retire(tracker_, uploads_.takeDedicated());
    return {value, 0};
}

GpuTicket VulkanContext::flushUploads() const {
    std::lock_guard<std::mutex> lock(submitMutex_);
    return flushUploadsLocked();
}
#endif

VkResult VulkanContext::submitLocked(const VkSubmitInfo& si, VkFence fence, uint64_t* outValue) const {
    assert(si.pNext == nullptr && "submit(): the timeline info is appended here; pass a plain VkSubmitInfo");
    if (si.signalSemaphoreCount > 7 || si.waitSemaphoreCount > 8) return VK_ERROR_INITIALIZATION_FAILED;
    const uint64_t value = tracker_.noteSubmit(QueueKind::Graphics);
    // The caller's binary signals first, the timeline last (binary entries ignore their value).
    VkSemaphore signals[8];
    uint64_t signalValues[8] = {};
    uint64_t waitValues[8] = {};
    for (uint32_t i = 0; i < si.signalSemaphoreCount; ++i) signals[i] = si.pSignalSemaphores[i];
    signals[si.signalSemaphoreCount] = gfxTimeline_;
    signalValues[si.signalSemaphoreCount] = value;
    VkTimelineSemaphoreSubmitInfo tsi{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    tsi.waitSemaphoreValueCount = si.waitSemaphoreCount;
    tsi.pWaitSemaphoreValues = si.waitSemaphoreCount ? waitValues : nullptr;
    tsi.signalSemaphoreValueCount = si.signalSemaphoreCount + 1;
    tsi.pSignalSemaphoreValues = signalValues;
    VkSubmitInfo out = si;
    out.pNext = &tsi;
    out.signalSemaphoreCount = si.signalSemaphoreCount + 1;
    out.pSignalSemaphores = signals;
    const VkResult r = vkQueueSubmit(gfxQueue_, 1, &out, fence);
    if (r != VK_SUCCESS) {
        // A refused submit signals nothing: give the value back so the counter and the timeline stay in step.
        tracker_.unnoteSubmit(QueueKind::Graphics, value);
        if (r == VK_ERROR_DEVICE_LOST) infra::deviceLost();
        return r;
    }
    if (outValue) *outValue = value;
    return r;
}

void VulkanContext::poll() const {
    mc2::overlap::assertOwner(mc2::overlap::Owner::Render, "VulkanContext::poll");
    if (gfxTimeline_ == VK_NULL_HANDLE) return;
    uint64_t v = 0;
    const VkResult result = vkGetSemaphoreCounterValue(device_, gfxTimeline_, &v);
    if (result == VK_SUCCESS) {
        std::lock_guard<std::mutex> lock(submitMutex_);
        tracker_.setCompleted(QueueKind::Graphics, v);
    }
    if (result == VK_ERROR_DEVICE_LOST) infra::deviceLost();
}

bool VulkanContext::isComplete(const GpuTicket& t) const {
    if (tracker_.isComplete(t)) return true;
    poll();
    return tracker_.isComplete(t);
}

bool VulkanContext::wait(const GpuTicket& t, uint64_t timeoutNs, std::string_view why) const {
    mc2::overlap::assertOwner(mc2::overlap::Owner::Render, "VulkanContext::wait");
    if (isComplete(t)) return true;
    if (gfxTimeline_ == VK_NULL_HANDLE) return false;
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &gfxTimeline_;
    wi.pValues = &t.gfx;
    const VkResult r = vkWaitSemaphores(device_, &wi, timeoutNs);
    if (r != VK_SUCCESS) ctxlog("timeline wait for %.*s ended with VkResult=%d (ticket gfx=%llu).", static_cast<int>(why.size()), why.data(), static_cast<int>(r), static_cast<unsigned long long>(t.gfx));
    poll();
    return tracker_.isComplete(t);
}

void VulkanContext::beginFrame() {
    mc2::overlap::assertOwner(mc2::overlap::Owner::Render, "VulkanContext::beginFrame");
    if (tracker_.frameOpen()) return;
    { ZoneScopedN("VKContext.Poll"); poll(); }
    tracker_.beginFrame();
    { ZoneScopedN("VKContext.Collect"); deferred_.collect(tracker_); }
    { ZoneScopedN("VKContext.InfraFrame"); infra::frame(); }
#ifdef MC2_VULKAN_ISLAND
    // Retire the active descriptor/buffer slot before any per-frame writes. framesInFlight_ <= kSlots, so its ticket also covers the older transient
    // ring slot F-kSlots. This keeps the N=1/2 overrides safe while the transient ring always has kSlots regions.
    const uint64_t f = tracker_.frameNo();
    // The slot restarts only when that completion is proven: a timed-out wait falls back to a device idle, and if even that leaves the ticket
    // incomplete (device lost) the slot keeps its head, so the CPU never overwrites bytes the GPU may still read.
    bool slotFree = true;
    if (f > framesInFlight_) {
        ZoneScopedN("VKContext.RingWait"); mc2::battlebench::WaitScope gpuWait1(mc2::battlebench::WaitGpuMainNs);
        const GpuTicket t = tracker_.frameTicket(f - framesInFlight_);
        if (!wait(t, 5ull * 1000 * 1000 * 1000, "frame_ring_slot")) {
            waitIdle("frame_ring_slot_timeout");
            poll();
            slotFree = tracker_.isComplete(t);
            if (!slotFree) ctxlog("frame %llu: ticket of frame %llu never completed; ring slot NOT restarted.", static_cast<unsigned long long>(f), static_cast<unsigned long long>(f - framesInFlight_));
        }
    }
    frameRing_.beginFrame(f, slotFree);
    if (textures_) {
        { ZoneScopedN("VKContext.TexturePump"); textures_->pump(); }
        { ZoneScopedN("VKContext.TextureDescriptors"); textures_->descriptors(); }
    }
#endif
}

void VulkanContext::drainDeferred(std::string_view why) {
    if (deferred_.size() == 0) return;
    waitIdle(why);
    poll();
    const uint32_t n = deferred_.drainAll();
    ctxlog("drained %u deferred destroy entr%s (%.*s).", n, n == 1 ? "y" : "ies", static_cast<int>(why.size()), why.data());
}

void VulkanContext::endFrame() {
    mc2::overlap::assertOwner(mc2::overlap::Owner::Render, "VulkanContext::endFrame");
    ZoneScopedN("VKContext.EndFrame");
#ifdef MC2_VULKAN_ISLAND
    flushUploads();   // an upload queued this frame is submitted inside it, so the frame ticket covers its staging
#endif
#ifdef MC2_VULKAN_ISLAND
    if (textures_) textures_->sealFrame();
#endif
    tracker_.endFrame();
    deferred_.sealFrame(tracker_.frameTicket(tracker_.frameNo()));
    if (tracker_.frameNo() % 600 == 0 && mc2DiagVerbose()) {   // the periodic summaries are MC2_DIAG_VERBOSE lines (a default build stays quiet)
        poll();
#ifdef MC2_VULKAN_ISLAND
        if (uploads_.queued() != 0)
            std::fprintf(stderr, "[VK_UPLOAD v1] flushes=%llu queued=%llu dedicated_stages=%llu dropped=%llu frame_ring_growths=%u\n",
                         static_cast<unsigned long long>(uploads_.flushes()), static_cast<unsigned long long>(uploads_.queued()),
                         static_cast<unsigned long long>(uploads_.dedicatedStages()), static_cast<unsigned long long>(uploads_.dropped()), frameRing_.growths());
#endif
        const DeferredDestroy::Stats ds = deferred_.stats(tracker_);
        std::fprintf(stderr, "[VK_TIMELINE v1] deferred_entries=%u deferred_bytes=%llu oldest_age_frames=%llu\n", ds.entries,
                     static_cast<unsigned long long>(ds.bytes), static_cast<unsigned long long>(ds.oldestAgeFrames));
#ifdef MC2_VULKAN_ISLAND
        std::fprintf(stderr, "[VK_FRAMERING v1] capacity=%llu peak_used=%llu\n", static_cast<unsigned long long>(frameRing_.capacityPerSlot()),
                     static_cast<unsigned long long>(frameRing_.peakPerSlot()));
#endif
        std::fprintf(stderr, "[VK_TIMELINE v1] frame=%llu last_submitted=%llu completed=%llu in_flight=%llu\n",
                     static_cast<unsigned long long>(tracker_.frameNo()),
                     static_cast<unsigned long long>(tracker_.lastSubmitted(QueueKind::Graphics)),
                     static_cast<unsigned long long>(tracker_.completed(QueueKind::Graphics)),
                     static_cast<unsigned long long>(tracker_.lastSubmitted(QueueKind::Graphics) - tracker_.completed(QueueKind::Graphics)));
        std::fflush(stderr);
    }
}

void VulkanContext::destroy() {
    mc2::overlap::assertOwner(mc2::overlap::Owner::Render, "VulkanContext::destroy");
#ifdef MC2_VULKAN_ISLAND
    gpuprof::shutdown(*this);
#endif
    g_debugNames = false;
    if (device_ != VK_NULL_HANDLE) { waitIdle("teardown"); deferred_.drainAll(); }   // parked owners go ahead of the allocator and the device
#ifdef MC2_VULKAN_ISLAND
    // The islands consume the light feed, so they go first; both go before the allocator and the device their objects were made on.
    for (size_t i = 0; i < islands_.size(); ++i) retireIsland(static_cast<IslandSlot>(i));
    textures_.reset();
    deferred_.drainAll();
    if (textureServiceOwner_) { RenderCore::activeTextureService = nullptr; textureServiceOwner_.reset(); }
    lightFeed_.release();
    frameRing_.release();
    readbacks_.releaseAll();
    // PRECONDITION: the owners that live outside the context (the shared scene colour / depth / normal images, the present path's shared image, the
    // shadow island's images) were destroyed by the engine shutdown ahead of this call. A resource still alive here would release its image on a device
    // that is gone, so the count is checked and a violation is logged where the validation layer would otherwise report it far from its cause.
    if (const int live = vkImageResourceLiveCount(); live != 0) {
        ctxlog("destroy() with %d image resource(s) still alive: shutdown order violated.", live);
        assert(live == 0 && "image resources must be destroyed before the engine Vulkan context");
    }
#endif
    if (allocator_) { vmaDestroyAllocator(allocator_); allocator_ = nullptr; }
    infra::shutdown();
    if (device_ != VK_NULL_HANDLE) {
        if (gfxTimeline_ != VK_NULL_HANDLE) { vkDestroySemaphore(device_, gfxTimeline_, nullptr); gfxTimeline_ = VK_NULL_HANDLE; }
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    gfxQueue_ = VK_NULL_HANDLE;
    gfxFamily_ = UINT32_MAX;
    phys_ = VK_NULL_HANDLE;
    if (messenger_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE) {
        auto pfnDestroy = (PFN_vkDestroyDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT");
        if (pfnDestroy) pfnDestroy(instance_, messenger_, nullptr);
        messenger_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE) {
#ifdef _WIN32
        // NVIDIA 560.94 can leave an ICD worker running after vkDestroyInstance
        // unloads its code (nvoglv64.dll_unloaded+0xe76052). Keep the module mapped
        // until process exit terminates those workers; Vulkan objects still tear down.
        HMODULE nvidiaIcd = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"nvoglv64.dll", &nvidiaIcd)) {
            ctxlog("NVIDIA ICD retained until process exit (shutdown worker lifetime).");
        }
#endif
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
    if (g_cbContext == this) g_cbContext = nullptr;
    profile_ = profile::Evaluation{};
    profile_.accepted = false;
    luidValid_ = false;
    health_.created = false;
}

bool VulkanContext::deviceLocalMemory(uint64_t& budgetBytes, uint64_t& usageBytes, uint64_t& allocatedBytes) const {
    if (!allocator_ || phys_ == VK_NULL_HANDLE) return false;
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(phys_, &mp);
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS] = {};
    vmaGetHeapBudgets(allocator_, budgets);
    budgetBytes = usageBytes = allocatedBytes = 0;
    for (uint32_t i = 0; i < mp.memoryHeapCount; ++i) {
        if (!(mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)) continue;
        budgetBytes += budgets[i].budget;
        usageBytes += budgets[i].usage;
        allocatedBytes += budgets[i].statistics.allocationBytes;
    }
    return true;
}

void VulkanContext::logHealth() const {
    std::fprintf(stderr,
                 "[VK_ENGINE_CONTEXT] created=%d attempted=%d device_name=\"%s\" "
                 "validation_enabled=%d validation_errors=%lu fallback_reason=\"%s\"\n",
                 health_.created ? 1 : 0, health_.attempted ? 1 : 0,
                 health_.deviceName, health_.validationEnabled ? 1 : 0,
                 health_.validationErrors.load(),
                 health_.fallbackReason.empty() ? "" : health_.fallbackReason.c_str());
    std::fflush(stderr);
}

bool debugNamesEnabled() { return g_debugNames; }

void setObjectName(VkDevice device, VkObjectType type, uint64_t handle, std::string_view name) {
    if (handle) infra::breadcrumb("resource", name, handle);
    if (!g_debugNames || device == VK_NULL_HANDLE || handle == 0 || !vkSetDebugUtilsObjectNameEXT) return;
    const std::string terminated(name);
    VkDebugUtilsObjectNameInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    info.objectType = type;
    info.objectHandle = handle;
    info.pObjectName = terminated.c_str();
    vkSetDebugUtilsObjectNameEXT(device, &info);
}

VulkanContext& getOrCreateEngineVulkanContext(std::source_location site) {
    present_thread::waitSealed(site);
    mc2::overlap::assertOwner(mc2::overlap::Owner::Render, "getOrCreateEngineVulkanContext");
    return engineContext();
}

bool engineVulkanContextValid(std::source_location site) {
    present_thread::waitSealed(site);
    return engineContext().valid();
}

// On-demand shared-context birth, DECOUPLED from present. The shared engine
// VulkanContext is the first-class durable device (P13 T2); present is just ONE
// consumer; native regions can also borrow the shared context WITHOUT
// MC2_RENDER_BACKEND=vulkan and WITHOUT a healthy present/window. Lazy + idempotent:
//   - If the context is already valid (born by =vulkan in gameosmain, or a prior
//     call here), return true immediately -- NO second init, NO second volkLoadDevice
//     (the device is shared; init() is a no-op after success).
//   - Else init(wantPresent=false): brings up instance/device/queue/VMA + the SINGLE
//     volkLoadDevice, Vulkan 1.2+ with the profile's features (no interop exts, no surface/swapchain). Native
//     VK regions reuse this device.
//   - init() is one-shot: after a failed attempt it returns false without retrying, so
//     a repeated caller never thrashes. Fail-soft: returns false, caller falls back to GL.
// VK-RUNG8-ITEM3: renamed from ensureEngineVulkanContext (no interop family).
bool ensureEngineVulkanContext() {
    if (engineContext().valid()) return true;               // already born (=vulkan or prior lazy)
    return engineContext().init(/*wantPresent=*/false);
}

}  // namespace mc2vk

#endif  // MC2_VULKAN
