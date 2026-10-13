// vulkan_context.cpp
//
// VK-P13-SELECTOR-1 / T2 -- the standalone shared engine Vulkan context.
// Owns instance + physical device + logical device + graphics queue + VMA +
// volk device dispatch. Born once at engine init (gameosmain) when
// renderBackend()==Vulkan; fail-soft on any error. Device ownership lives HERE;
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
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include "vulkan_context.h"
#include "vk_device_profile.h"
#include "Gates/gate.h"
#include "Gates/gate_cold.h"   // str<>()

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

// The `[VK_PROFILE v1]` line goes to stderr beside the other [VK_ENGINE_CONTEXT] lines; the smoke parser grades a refused verdict red.
void profileLog(const std::string& line) {
    std::fprintf(stderr, "%s\n", line.c_str());
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
        if (mc2::gate::get<"MC2_VK_FORCE_FAIL">()) {
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
        refused.items.push_back({"api_1_2", profile::Need::Required, false, "-"});
        refused.missingRequired.push_back("api_1_2");
        profileLog(profile::formatLine("(no device queried: the Vulkan loader is below 1.2)", none, refused));
        return fail("the Vulkan loader reports instance version " + profile::versionText(loaderApi) + ", below the 1.2 floor");
    }

    // ---- validation preset ----
    std::vector<VkValidationFeatureEnableEXT> valFeatures;
    const char* valPreset = "off";
    bool wantValidation = resolveValidationPreset(valFeatures, valPreset);
    health_.validationEnabled = wantValidation;

    std::vector<const char*> layers;
    std::vector<const char*> instExts;

    // No interop instance caps are requested.
    // get_physical_device_properties_2 is VK 1.1 core anyway (apiVersion 1.1).

    // VK-P13 T3: surface instance extensions so the shared device can create a
    // VkSurfaceKHR for the hidden present window (SDL_Vulkan_CreateSurface needs
    // the instance to carry these). On Windows the surface pair is well-known
    // (SDL requires exactly these), so we do not need an SDL window before
    // instance creation.
    if (wantPresent) {
        instExts.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
        instExts.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
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

    // ---- device profile: the first discrete device that meets every REQUIRED item, else the first device that does; if none does, the engine
    // refuses Vulkan (fail-soft to GL) and the refused device's line says which items it lacks ----
    profile::DeviceCaps caps;
    profile::Evaluation verdict;
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
        verdict = pick->ev;
        std::strncpy(health_.deviceName, pick->name.c_str(), sizeof(health_.deviceName) - 1);
        health_.deviceName[sizeof(health_.deviceName) - 1] = '\0';
        health_.profileLine = profile::formatLine(pick->name, pick->caps, pick->ev);
        profileLog(health_.profileLine);
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

    std::vector<const char*> devExts;
    // VK-RUNG8-ITEM3: the external-memory/-semaphore Win32 device extensions (the last
    // hard MoltenVK/Metal-gate blocker) were REMOVED -- no VK resource is exportable
    // anymore. VK_KHR_dedicated_allocation + VK_KHR_get_memory_requirements_2 are VK 1.1
    // core (apiVersion 1.1), so the surviving dedicated allocations need no extension.
    // vkCreateDevice on MoltenVK no longer fails on an unsupported extension here.
    // VK-P13 T3: swapchain device extension for the hidden present window.
    if (wantPresent) {
        devExts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }

    // VK_EXT_memory_budget: the OS's per-heap budget and usage, so the VRAM line of the smoke report is the driver's number and not VMA's estimate.
    {
        uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(phys_, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> have(extCount);
        if (extCount) vkEnumerateDeviceExtensionProperties(phys_, nullptr, &extCount, have.data());
        for (const auto& e : have) {
            if (std::strcmp(e.extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) == 0) { memoryBudgetExt_ = true; break; }
        }
        if (memoryBudgetExt_) devExts.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    }

    // The profile's extension routes (dynamic rendering / synchronization2 on a 1.2 device), then the feature bits it selected: only supported bits
    // are enabled, each through the struct that defines it (the core 1.2 / 1.3 aggregate when the version allows, the KHR struct otherwise).
    for (const std::string& e : verdict.deviceExtensions) devExts.push_back(e.c_str());

    VkPhysicalDeviceFeatures2 enableFeatures{};
    enableFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    VkPhysicalDeviceVulkan12Features en12{};
    en12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    VkPhysicalDeviceVulkan13Features en13{};
    en13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceDynamicRenderingFeaturesKHR enDr{};
    enDr.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR;
    VkPhysicalDeviceSynchronization2FeaturesKHR enS2{};
    enS2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR;
    {
        using profile::featureEnabled;
        en12.timelineSemaphore = featureEnabled(verdict, "timelineSemaphore") ? VK_TRUE : VK_FALSE;
        en12.bufferDeviceAddress = featureEnabled(verdict, "bufferDeviceAddress") ? VK_TRUE : VK_FALSE;
        en12.descriptorIndexing = featureEnabled(verdict, "descriptorIndexing") ? VK_TRUE : VK_FALSE;
        en12.runtimeDescriptorArray = featureEnabled(verdict, "runtimeDescriptorArray") ? VK_TRUE : VK_FALSE;
        en12.shaderSampledImageArrayNonUniformIndexing = featureEnabled(verdict, "shaderSampledImageArrayNonUniformIndexing") ? VK_TRUE : VK_FALSE;
        en12.descriptorBindingPartiallyBound = featureEnabled(verdict, "descriptorBindingPartiallyBound") ? VK_TRUE : VK_FALSE;
        const bool drCore = std::strcmp(profile::routeOf(verdict, "dynamic_rendering"), "core") == 0;
        const bool s2Core = std::strcmp(profile::routeOf(verdict, "synchronization2"), "core") == 0;
        if (featureEnabled(verdict, "dynamicRendering")) (drCore ? en13.dynamicRendering : enDr.dynamicRendering) = VK_TRUE;
        if (featureEnabled(verdict, "synchronization2")) (s2Core ? en13.synchronization2 : enS2.synchronization2) = VK_TRUE;
        void** tail = &enableFeatures.pNext;
        auto chain = [&](auto& s) { *tail = &s; tail = &s.pNext; };
        chain(en12);
        if (drCore || s2Core) chain(en13);
        if (featureEnabled(verdict, "dynamicRendering") && !drCore) chain(enDr);
        if (featureEnabled(verdict, "synchronization2") && !s2Core) chain(enS2);
        bufferDeviceAddress_ = en12.bufferDeviceAddress == VK_TRUE;
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

    // ---- VMA allocator (volk-imported functions) ----
    {
        VmaAllocatorCreateInfo aci{};
        aci.physicalDevice = phys_;
        aci.device = device_;
        aci.instance = instance_;
        aci.vulkanApiVersion = caps.usableApi();   // never above what the device or the instance was created for
        if (memoryBudgetExt_) aci.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
        if (bufferDeviceAddress_) aci.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
        VmaVulkanFunctions fns{};
        r = vmaImportVulkanFunctionsFromVolk(&aci, &fns);
        if (r != VK_SUCCESS) return fail("vmaImportVulkanFunctionsFromVolk failed");
        aci.pVulkanFunctions = &fns;
        r = vmaCreateAllocator(&aci, &allocator_);
        if (r != VK_SUCCESS) { allocator_ = nullptr; return fail("vmaCreateAllocator failed"); }
    }

    health_.created = true;
    health_.fallbackReason.clear();
    ctxlog("engine Vulkan device created: device_name=\"%s\" present=%d validation=%s.",
           health_.deviceName, wantPresent ? 1 : 0, valPreset);
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
    memoryBudgetExt_ = false;
    bufferDeviceAddress_ = false;
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

// On-demand shared-context birth, DECOUPLED from present. The shared engine
// VulkanContext is the first-class durable device (P13 T2); present is just ONE
// consumer; native regions can also borrow the shared context WITHOUT
// MC2_RENDER_BACKEND=vulkan and WITHOUT a healthy present/window. Lazy + idempotent:
//   - If the context is already valid (born by =vulkan in gameosmain, or a prior
//     call here), return true immediately -- NO second init, NO second volkLoadDevice
//     (the device is shared; init() is a no-op after success).
//   - Else init(wantPresent=false): brings up instance/device/queue/VMA + the SINGLE
//     volkLoadDevice, plain VK 1.1 core (no interop exts, no surface/swapchain). Native
//     VK regions reuse this device.
//   - init() is one-shot: after a failed attempt it returns false without retrying, so
//     a repeated caller never thrashes. Fail-soft: returns false, caller falls back to GL.
// VK-RUNG8-ITEM3: renamed from ensureEngineVulkanContext (no interop family).
bool ensureEngineVulkanContext() {
    if (g_engineContext.valid()) return true;               // already born (=vulkan or prior lazy)
    return g_engineContext.init(/*wantPresent=*/false);
}

}  // namespace mc2vk

#endif  // MC2_VULKAN
