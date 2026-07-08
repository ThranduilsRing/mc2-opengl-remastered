// vulkan_context.cpp
//
// VK-P13-SELECTOR-1 / T2 -- the standalone shared engine Vulkan context.
// Owns instance + physical device + logical device + graphics queue + VMA +
// volk device dispatch. Born once at engine init (gameosmain) when
// renderBackend()==Vulkan; fail-soft on any error. Device ownership lives HERE,
// not in VulkanPresentObject (the T2 acceptance criterion).
//
// Whole TU is inert unless MC2_VULKAN is defined. Device bring-up keeps the
// proven volk + VMA + validation-preset convention used by earlier Vulkan
// region probes, so we do not re-litigate device selection.

#ifdef MC2_VULKAN

// The KHR external-memory/-semaphore Win32 entrypoints + extension-name macros
// (VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME etc.) are gated behind
// VK_USE_PLATFORM_WIN32_KHR in volk/vulkan_core.h. Define it BEFORE the header
// (which pulls in volk.h) so those identifiers are visible for the interop
// device extensions.
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include "vulkan_context.h"

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include "vk_mem_alloc.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace mc2vk {

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

// The single process-wide shared context.
VulkanContext g_engineContext;

// Debug-utils callback pointer target: increments the live context's error
// counter. Because there is exactly one engine context, a file-static back
// pointer is sufficient (set in init()).
VulkanContext* g_cbContext = nullptr;

VKAPI_ATTR VkBool32 VKAPI_CALL engine_ctx_debug_cb(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT* pData,
    void* /*user*/) {
    const char* msg = (pData && pData->pMessage) ? pData->pMessage : "(no message)";
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        if (g_cbContext) ++g_cbContext->mutableHealth().validationErrors;
        std::fprintf(stderr, "[VK_ENGINE_CONTEXT_VALIDATION] ERROR: %s\n", msg);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        std::fprintf(stderr, "[VK_ENGINE_CONTEXT_VALIDATION] warning: %s\n", msg);
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
    const char* env = std::getenv("MC2_VULKAN_VALIDATION");
    if (env == nullptr) return false;
    auto eq = [&](const char* s) { return std::strcmp(env, s) == 0; };
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
    ctxlog("validation preset '%s' unrecognized; using 'core'.", env);
    resolvedName = "core";
    return true;
}

}  // namespace

bool VulkanContext::init(bool wantInterop, bool wantPresent) {
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
    // created, so the fail-soft -> forceGLFallback path can be exercised on
    // demand (no broken-ICD juggling). Presence-gated; default OFF.
    {
        const char* ff = std::getenv("MC2_VK_FORCE_FAIL");
        if (ff && ff[0] && ff[0] != '0') {
            return fail("MC2_VK_FORCE_FAIL=1 forced failure (fail-soft proof knob)");
        }
    }

    g_cbContext = this;

    // ---- volk runtime loader ----
    VkResult r = volkInitialize();
    if (r != VK_SUCCESS) {
        return fail("volkInitialize failed (no Vulkan runtime)");
    }
    initedVolk_ = true;

    // ---- validation preset ----
    std::vector<VkValidationFeatureEnableEXT> valFeatures;
    const char* valPreset = "off";
    bool wantValidation = resolveValidationPreset(valFeatures, valPreset);
    health_.validationEnabled = wantValidation;

    std::vector<const char*> layers;
    std::vector<const char*> instExts;

    // Interop-capable instance caps (external memory/semaphore) so the shared
    // device can back exportable images the GL side imports (P6/P8/P14).
    if (wantInterop) {
        instExts.push_back(VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME);
        instExts.push_back(VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME);
        instExts.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    }

    // VK-P13 T3: surface instance extensions so the shared device can create a
    // VkSurfaceKHR for the hidden present window (SDL_Vulkan_CreateSurface needs
    // the instance to carry these). On Windows the surface pair is well-known
    // (SDL requires exactly these), so we do not need an SDL window before
    // instance creation. GET_PHYSICAL_DEVICE_PROPERTIES_2 may already be present
    // via wantInterop; duplicate names in ppEnabledExtensionNames are harmless
    // but we de-dup to keep the loader quiet.
    if (wantPresent) {
        instExts.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
        instExts.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
    }
    // De-dup instExts (interop + present can both add PDP2/etc.).
    {
        std::vector<const char*> uniq;
        for (const char* e : instExts) {
            bool seen = false;
            for (const char* u : uniq) if (std::strcmp(u, e) == 0) { seen = true; break; }
            if (!seen) uniq.push_back(e);
        }
        instExts.swap(uniq);
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
        }
    }

    // ---- Instance ----
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "MC2 (engine Vulkan context)";
    app.pEngineName = "MC2-GameOS";
    app.apiVersion = VK_API_VERSION_1_1;

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
            if (mr == VK_SUCCESS) ctxlog("validation messenger engaged (preset=%s).", valPreset);
            else ctxlog("vkCreateDebugUtilsMessengerEXT failed (%d); no error capture.", (int)mr);
        } else {
            ctxlog("vkCreateDebugUtilsMessengerEXT not found; no error capture.");
        }
    }

    // ---- physical device (prefer discrete when interop is requested) ----
    uint32_t devCount = 0;
    r = vkEnumeratePhysicalDevices(instance_, &devCount, nullptr);
    if (r != VK_SUCCESS || devCount == 0) return fail("no physical devices");
    std::vector<VkPhysicalDevice> devs(devCount);
    vkEnumeratePhysicalDevices(instance_, &devCount, devs.data());
    phys_ = devs[0];
    if (wantInterop && devCount > 1) {
        for (auto pd : devs) {
            VkPhysicalDeviceProperties p{};
            vkGetPhysicalDeviceProperties(pd, &p);
            if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { phys_ = pd; break; }
        }
    }
    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(phys_, &props);
        std::strncpy(health_.deviceName, props.deviceName, sizeof(health_.deviceName) - 1);
        health_.deviceName[sizeof(health_.deviceName) - 1] = '\0';
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

    std::vector<const char*> devExts;
    if (wantInterop) {
        // External-memory/-semaphore Win32 exts so P6/P8/P14 can import/export
        // shared images/semaphores against THIS device (same set the island uses).
        devExts.push_back(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
        devExts.push_back(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
        devExts.push_back(VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME);
        devExts.push_back(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);
        devExts.push_back(VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME);
        devExts.push_back(VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME);
    }
    // VK-P13 T3: swapchain device extension for the hidden present window.
    if (wantPresent) {
        devExts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
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

    // ---- VMA allocator (volk-imported functions) ----
    {
        VmaAllocatorCreateInfo aci{};
        aci.physicalDevice = phys_;
        aci.device = device_;
        aci.instance = instance_;
        aci.vulkanApiVersion = VK_API_VERSION_1_1;
        VmaVulkanFunctions fns{};
        r = vmaImportVulkanFunctionsFromVolk(&aci, &fns);
        if (r != VK_SUCCESS) return fail("vmaImportVulkanFunctionsFromVolk failed");
        aci.pVulkanFunctions = &fns;
        r = vmaCreateAllocator(&aci, &allocator_);
        if (r != VK_SUCCESS) { allocator_ = nullptr; return fail("vmaCreateAllocator failed"); }
    }

    health_.created = true;
    health_.fallbackReason.clear();
    ctxlog("engine Vulkan device created: device_name=\"%s\" interop=%d present=%d validation=%s.",
           health_.deviceName, wantInterop ? 1 : 0, wantPresent ? 1 : 0, valPreset);
    return true;
}

void VulkanContext::destroy() {
    if (allocator_) { vmaDestroyAllocator(allocator_); allocator_ = nullptr; }
    if (device_ != VK_NULL_HANDLE) {
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
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
    if (g_cbContext == this) g_cbContext = nullptr;
    health_.created = false;
}

void VulkanContext::logHealth() const {
    std::fprintf(stderr,
                 "[VK_ENGINE_CONTEXT] created=%d attempted=%d device_name=\"%s\" "
                 "validation_enabled=%d validation_errors=%lu fallback_reason=\"%s\"\n",
                 health_.created ? 1 : 0, health_.attempted ? 1 : 0,
                 health_.deviceName, health_.validationEnabled ? 1 : 0,
                 health_.validationErrors,
                 health_.fallbackReason.empty() ? "" : health_.fallbackReason.c_str());
    std::fflush(stderr);
}

VulkanContext& getOrCreateEngineVulkanContext() {
    return g_engineContext;
}

bool engineVulkanContextValid() {
    return g_engineContext.valid();
}

// VK-P9a WATER-INTEROP: on-demand shared-context birth, DECOUPLED from present.
// The shared engine VulkanContext is the first-class durable device (P13 T2);
// present is just ONE consumer. A VK-interop-region gate (e.g. MC2_VK_WATER_INTEROP)
// must be able to bring the SAME shared device up on the GL-present path, WITHOUT
// MC2_RENDER_BACKEND=vulkan and WITHOUT a healthy present/window. Lazy + idempotent:
//   - If the context is already valid (born by =vulkan in gameosmain, or a prior
//     call here), return true immediately -- NO second init, NO second volkLoadDevice
//     (the device is shared; init() is a no-op after success).
//   - Else init(wantInterop=true, wantPresent=false): brings up instance/device/queue/
//     VMA + the SINGLE volkLoadDevice, with the external-memory/-semaphore Win32 exts
//     but NO surface/swapchain (present-independent). Interop islands reuse this device.
//   - init() is one-shot: after a failed attempt it returns false without retrying, so
//     a repeated caller never thrashes. Fail-soft: returns false, caller falls back to GL.
// This is the generalization that unblocks every VK geometry/interop region uniformly.
bool ensureEngineVulkanContextForInterop() {
    if (g_engineContext.valid()) return true;               // already born (=vulkan or prior lazy)
    return g_engineContext.init(/*wantInterop=*/true, /*wantPresent=*/false);
}

}  // namespace mc2vk

#endif  // MC2_VULKAN
