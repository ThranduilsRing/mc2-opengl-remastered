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
#include "vk_validation_line.h"
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
void profileLog(std::string_view line) {
    std::fprintf(stderr, "%.*s\n", static_cast<int>(line.size()), line.data());
    std::fflush(stderr);
}

// The single process-wide shared context.
VulkanContext g_engineContext;

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
            // A validation run that silently has no layer would look green with no coverage: the smoke grades this error red.
            validationLog("error", "layer_unavailable", std::string(want) + " was requested (MC2_VULKAN_VALIDATION) and the Vulkan loader does not offer it; this run is NOT validated");
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
            if (mr == VK_SUCCESS) {
                ctxlog("validation messenger engaged (preset=%s).", valPreset);
                g_debugNames = true;
                validationLog("info", "layer_enabled", std::string("VK_LAYER_KHRONOS_validation messenger engaged, preset=") + valPreset);
            } else {
                ctxlog("vkCreateDebugUtilsMessengerEXT failed (%d); no error capture.", (int)mr);
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
    // refuses Vulkan (fail-soft to GL) and the refused device's line says which items it lacks ----
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

    // The feature bits the profile selected. Seven are required by the owner ruling of 2026-10-13 (timeline semaphores, buffer device address, the
    // descriptor-indexing group that is the bindless table's definition, dynamic rendering, synchronization2) and enabled although no island consumes them
    // yet; independentBlend is required because the islands' two-attachment pipelines already need it. An accepted device has every one and no island
    // carries a fallback arm for any. Each bit goes through the struct that defines it (the core 1.0 features, the core 1.2 / 1.3 aggregate when the
    // route is core, the KHR struct otherwise).

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
        enableFeatures.features.independentBlend = featureEnabled("independentBlend") ? VK_TRUE : VK_FALSE;
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

    health_.created = true;
    health_.fallbackReason.clear();
    ctxlog("engine Vulkan device created: device_name=\"%s\" present=%d validation=%s.",
           health_.deviceName, wantPresent ? 1 : 0, valPreset);
    return true;
}

void VulkanContext::destroy() {
    g_debugNames = false;
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

void setObjectName(VkDevice device, VkObjectType type, uint64_t handle, const char* name) {
    if (!g_debugNames || device == VK_NULL_HANDLE || handle == 0 || !name || !vkSetDebugUtilsObjectNameEXT) return;
    VkDebugUtilsObjectNameInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    info.objectType = type;
    info.objectHandle = handle;
    info.pObjectName = name;
    vkSetDebugUtilsObjectNameEXT(device, &info);
}

void setObjectNamef(VkDevice device, VkObjectType type, uint64_t handle, const char* fmt, ...) {
    if (!g_debugNames) return;
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    setObjectName(device, type, handle, buf);
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
//     volkLoadDevice, Vulkan 1.2+ with the profile's features (no interop exts, no surface/swapchain). Native
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
