#include "../../RenderCore/TextureService.h"
#include "load_scope.h"
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
#include "gpu_memory_report.h"
#include "vk_rt_selftest.h"
#include "present_thread.h"
#include "../../mclib/sim_overlap.h"
#ifndef _WIN32
#define NO_SDL_VULKAN_TYPEDEFS
#include <span>
#include <string_view>
#include <SDL2/SDL_vulkan.h>
#endif
#include <tracy/Tracy.hpp>
#include "mc2_diag_verbose.h"
#include "diagnostic_trace.h"
#include "hitch_stats.h"
#include "vk_device_profile.h"
#include "vk_gpu_profile.h"
#include "vk_validation_line.h"
#include "vk_image_resource.h"
#include "vk_texture_residency.h"
#include "vk_one_shot.h"
#include "Gates/gate.h"
#include "Gates/gate_cold.h"   // str<>()

#include "vk_mem_alloc.h"

#include "platform_stdlib.h"   // _putenv_s on Linux
#include <cassert>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <chrono>
#include <memory>
#include <mutex>
#include <vector>
#include "../../mclib/battle_bench_wait.h"

namespace mc2vk {

VulkanContext::VulkanContext() = default;
VulkanContext::~VulkanContext() { destroy(); }

namespace {

// The present worker holds submitMutex_ across vkQueuePresentKHR, and a Win32 WSI call may SendMessage to the window thread (main). Main keeps
// answering sent messages while it waits here, or both threads stop (pumpWindowMessages is a no-op off main).
std::unique_lock<std::timed_mutex> lockSubmit(std::timed_mutex& m) {
    std::unique_lock<std::timed_mutex> lock(m, std::try_to_lock);
    if (lock.owns_lock()) return lock;
    ZoneScopedN("VK.QueueLockWait");
    if (present_thread::onWorker()) { lock.lock(); return lock; }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!lock.try_lock_for(std::chrono::milliseconds(2))) {
        present_thread::pumpWindowMessages();
        if (std::chrono::steady_clock::now() < deadline) continue;
        present_thread::hungWait("submitMutex_ held >30s by the present worker");
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    }
    return lock;
}

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
thread_local bool g_canaryArmed = false;   // true only on the calling thread inside runValidationCanary (the layer reports synchronously on that thread)
std::atomic<int> g_canaryHits{0};

// AMD best-practices canary (preset best-practices): the engine always has a one-time vkCmdClearColorImage first-layout clear and a few
// GPU-side vkCmdCopyImage, so BestPractices-AMD-ClearAttachment-ClearImage-color / vkImage-AvoidImageToImageCopy must show up when the AMD
// checks are really on. If neither has been seen by teardown, the setting did not apply and the run's AMD table is empty by accident.
std::atomic<bool> g_amdChecksExpected{false};
std::atomic<bool> g_amdCanarySeen{false};

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
    if (g_amdChecksExpected.load(std::memory_order_relaxed) &&
        (std::strstr(id, "AvoidImageToImageCopy") || std::strstr(id, "ClearAttachment-ClearImage") ||   // layer 1.3.x names (no "AMD-" infix); 1.4.x adds -color/-depth-stencil
         std::strstr(id, "CmdBuffer-highBarrierCount") || std::strstr(id, "CreatePipelinesLayout-KeepLayoutSmall")))
        g_amdCanarySeen.store(true, std::memory_order_relaxed);
    // The known-bad canary (runValidationCanary) is a deliberate violation: it proves the layer is live on this host and is not an engine error.
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) && g_canaryArmed && std::strstr(id, "VkBufferCreateInfo-size") != nullptr) {
        g_canaryHits.fetch_add(1, std::memory_order_relaxed);
        validationLog("info", "canary_fired", std::string(id));
        return VK_FALSE;
    }
    // Every involved object (type, handle, debug name) and the open command-buffer / queue labels lead the text, ahead of the layer's long message (the text is
    // capped, the id stays first), so a hit names the pass that recorded it.
    std::string where;
    if (pData) {
        char buf[96];
        for (uint32_t i = 0; i < pData->objectCount && i < 8; ++i) {
            const VkDebugUtilsObjectNameInfoEXT& o = pData->pObjects[i];
            std::snprintf(buf, sizeof(buf), "%s%d:0x%llx:", where.empty() ? "obj=" : ";", static_cast<int>(o.objectType), static_cast<unsigned long long>(o.objectHandle));
            where += buf;
            where += o.pObjectName ? o.pObjectName : "-";
        }
        auto labels = [&](std::string_view key, std::span<const VkDebugUtilsLabelEXT> l) {
            for (size_t i = 0; i < l.size() && i < 6; ++i) {
                if (i == 0) { if (!where.empty()) where += ' '; where += key; where += '='; } else where += '>';
                where += l[i].pLabelName ? l[i].pLabelName : "-";
            }
        };
        labels("cmdlabel", {pData->pCmdBufLabels, pData->cmdBufLabelCount});
        labels("queuelabel", {pData->pQueueLabels, pData->queueLabelCount});
        if (!where.empty()) where += " | ";
    }
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        if (g_cbContext) g_cbContext->mutableHealth().validationErrors.fetch_add(1, std::memory_order_relaxed);
        validationLog("error", id, where + msg);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        validationLog("warning", id, where + msg);
    }
    return VK_FALSE;
}

// The per-host canary: one deliberately invalid call (a zero-size buffer, VUID-VkBufferCreateInfo-size-00912) right after the device exists. The layer must
// report it; the engine logs `canary_fired` (info) on a hit and `canary_missed` (error) when the layer stayed silent, so a validated run that never proved
// the layer live is red, never silently green.
void runValidationCanary(VkDevice device) {
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = 0;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    g_canaryHits.store(0);
    g_canaryArmed = true;
    VkBuffer b = VK_NULL_HANDLE;
    const VkResult r = vkCreateBuffer(device, &bci, nullptr, &b);
    g_canaryArmed = false;
    if (r == VK_SUCCESS && b != VK_NULL_HANDLE) vkDestroyBuffer(device, b, nullptr);
    if (g_canaryHits.load() == 0)
        validationLog("error", "canary_missed", "the deliberate zero-size vkCreateBuffer drew no validation message: the layer is not live on this host");
}

// Validation preset resolver (same values the islands accept via
// MC2_VULKAN_VALIDATION). Kept local so the context has no link dependency on
// the island TU (always built; MC2_VULKAN_ISLAND is not a define in this tree).
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
        feats.push_back(VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT);   // RESERVE_BINDING_SLOT is obsolete: the layer no longer needs a reserved descriptor slot
        return true;
    }
    if (eq("best-practices")) {
        resolvedName = "best-practices";
        feats.push_back(VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT);
        // The 18 BestPractices-AMD checks have no VkValidationFeatureEnableEXT value; the layer reads VK_LAYER_ENABLES at vkCreateInstance
        // (the same switch as the layer setting validate_best_practices_amd, and it works on every layer version). Preset-only: no other
        // preset and no release run touches the environment. The duplicate limit is raised so the per-check counts mean something.
        const char* prior = std::getenv("VK_LAYER_ENABLES");
        std::string enables = (prior && *prior) ? std::string(prior) : std::string();
        if (enables.find("VALIDATION_CHECK_ENABLE_VENDOR_SPECIFIC_AMD") == std::string::npos) {
            if (!enables.empty()) enables += ",";
            enables += "VALIDATION_CHECK_ENABLE_VENDOR_SPECIFIC_AMD";
        }
        _putenv_s("VK_LAYER_ENABLES", enables.c_str());
        if (!std::getenv("VK_LAYER_DUPLICATE_MESSAGE_LIMIT")) _putenv_s("VK_LAYER_DUPLICATE_MESSAGE_LIMIT", "100000");   // true per-id counts (the default cap of 200 hid every hot class)
        g_amdChecksExpected.store(true, std::memory_order_relaxed);
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

    uint32_t layerSpec = 0, layerImpl = 0;
    bool layerSettingsExt = false;
    if (wantValidation) {
        uint32_t layerCount = 0;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
        std::vector<VkLayerProperties> avail(layerCount);
        if (layerCount) vkEnumerateInstanceLayerProperties(&layerCount, avail.data());
        const char* want = "VK_LAYER_KHRONOS_validation";
        bool found = false;
        for (const auto& lp : avail) {
            if (std::strcmp(lp.layerName, want) == 0) {
                found = true;
                layerSpec = lp.specVersion;
                layerImpl = lp.implementationVersion;
                break;
            }
        }
        if (found) {
            layers.push_back(want);
            instExts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            ctxlog("validation ON: layer %s enabled (preset=%s, +%zu feature(s), layer spec %u.%u.%u impl %u).",
                   want, valPreset, valFeatures.size(), VK_API_VERSION_MAJOR(layerSpec), VK_API_VERSION_MINOR(layerSpec), VK_API_VERSION_PATCH(layerSpec), layerImpl);
            // VK_EXT_layer_settings (layer 1.3.268+) carries the engine-owned settings; an older layer runs on the feature list alone and the run is red.
            uint32_t n = 0;
            vkEnumerateInstanceExtensionProperties(want, &n, nullptr);
            std::vector<VkExtensionProperties> lext(n);
            if (n) vkEnumerateInstanceExtensionProperties(want, &n, lext.data());
            for (const auto& ep : lext)
                if (std::strcmp(ep.extensionName, VK_EXT_LAYER_SETTINGS_EXTENSION_NAME) == 0) { layerSettingsExt = true; break; }
            if (layerSettingsExt) instExts.push_back(VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);
            else validationLog("warning", "layer_settings_unavailable","VK_LAYER_KHRONOS_validation does not offer VK_EXT_layer_settings (spec " + std::to_string(layerSpec) + "): the engine's sync/best-practices settings and duplicate cap are not applied; update the Vulkan SDK");
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
    // vk-present-instance 3: the messenger create info rides the instance create chain, so create-time messages are captured too
    // (the persistent messenger below is created from the same info and destroyed before the instance).
    VkDebugUtilsMessengerCreateInfoEXT mci{};
    mci.sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    mci.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    mci.pfnUserCallback = engine_ctx_debug_cb;
    // Engine-owned layer settings (not the vkconfig / environment): the duplicate-message cap for every preset, the sync preset's extra-property
    // option, the best-practices switches. They are chained next to the feature list, which stays as the fallback for the same preset.
    const char* const kLayer = "VK_LAYER_KHRONOS_validation";
    const bool presetBPLimit = std::strcmp(valPreset, "best-practices") == 0;
    const uint32_t dupLimit = presetBPLimit ? 100000u : 200u, on = 1;
    const bool presetSync = std::strcmp(valPreset, "sync") == 0, presetBP = std::strcmp(valPreset, "best-practices") == 0;
    std::vector<VkLayerSettingEXT> lset;
    auto addSetting = [&]<size_t N>(const char (&name)[N], VkLayerSettingTypeEXT type, const uint32_t* v) { lset.push_back({kLayer, name, type, 1, v}); };
    addSetting("duplicate_message_limit", VK_LAYER_SETTING_TYPE_UINT32_EXT, &dupLimit);
    if (presetSync) {
        addSetting("validate_sync", VK_LAYER_SETTING_TYPE_BOOL32_EXT, &on);
        addSetting("syncval_message_extra_properties", VK_LAYER_SETTING_TYPE_BOOL32_EXT, &on);
    }
    if (presetBP) {
        addSetting("validate_best_practices", VK_LAYER_SETTING_TYPE_BOOL32_EXT, &on);
        addSetting("validate_best_practices_amd", VK_LAYER_SETTING_TYPE_BOOL32_EXT, &on);
    }
    VkLayerSettingsCreateInfoEXT lsci{};
    lsci.sType = VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT;
    lsci.settingCount = static_cast<uint32_t>(lset.size());
    lsci.pSettings = lset.data();
    const void** tail = &ici.pNext;
    if (wantValidation && !valFeatures.empty()) { *tail = &valFeaturesInfo; tail = &valFeaturesInfo.pNext; }
    if (wantValidation && layerSettingsExt) { *tail = &lsci; tail = &lsci.pNext; }
    if (wantValidation && debugUtilsExt) { *tail = &mci; }

    r = vkCreateInstance(&ici, nullptr, &instance_);
    if (r != VK_SUCCESS) return fail("vkCreateInstance failed");
    volkLoadInstance(instance_);
    if (debugUtilsExt && vkSetDebugUtilsObjectNameEXT) g_debugNames = true;   // names and labels need only the extension, not the messenger

    // ---- debug messenger ----
    if (wantValidation) {
        auto pfnCreate = (PFN_vkCreateDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT");
        if (pfnCreate) {
            VkResult mr = pfnCreate(instance_, &mci, nullptr, &messenger_);
            if (mr == VK_SUCCESS) {
                ctxlog("validation messenger engaged (preset=%s).", valPreset);
                g_debugNames = true;
                validationLog("info", "layer_enabled", std::string("VK_LAYER_KHRONOS_validation messenger engaged, preset=") + valPreset + " layer_spec=" + std::to_string(VK_API_VERSION_MAJOR(layerSpec)) + "." + std::to_string(VK_API_VERSION_MINOR(layerSpec)) + "." + std::to_string(VK_API_VERSION_PATCH(layerSpec)) + " layer_impl=" + std::to_string(layerImpl) + " settings=" + std::to_string(layerSettingsExt ? lset.size() : 0) + " dup_limit=" + std::to_string(layerSettingsExt ? dupLimit : 0));
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
        struct Candidate { VkPhysicalDevice pd; profile::DeviceCaps caps; profile::Evaluation ev; std::string name; bool discrete; uint64_t vram; };
        std::vector<Candidate> cands;
        for (auto pd : devs) {
            VkPhysicalDeviceProperties p{};
            vkGetPhysicalDeviceProperties(pd, &p);
            VkPhysicalDeviceMemoryProperties mem{};
            vkGetPhysicalDeviceMemoryProperties(pd, &mem);
            uint64_t vram = 0;
            for (uint32_t h = 0; h < mem.memoryHeapCount; ++h)
                if (mem.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) vram += mem.memoryHeaps[h].size;
            Candidate c{pd, profile::gather(pd, instanceApi), {}, p.deviceName, p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, vram};
            c.ev = profile::evaluate(c.caps, wantPresent);
            cands.push_back(std::move(c));
        }
        const Candidate* pick = nullptr;
        const auto lower = [](std::string v) { for (char& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); return v; };
        const auto uuidOf = [](const profile::DeviceCaps& cp) {
            char b[40]; const uint8_t* u = cp.deviceUuid;
            std::snprintf(b, sizeof(b), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
            return std::string(b);
        };
        // MC2_VK_DEVICE: the harness routes pass/fail runs to a named device (the loader ignores VK_LOADER_DRIVERS_SELECT here).
        // "name@uuid" additionally pins the device UUID (two identical cards share a name); no hint = the default pick (owner-started runs).
        std::string hint = mc2::gate::str<"MC2_VK_DEVICE">();
        if (!hint.empty()) {
            std::string uuidWant;
            if (const size_t at = hint.find('@'); at != std::string::npos) { uuidWant = lower(hint.substr(at + 1)); hint.resize(at); }
            const std::string want = lower(hint);
            for (const Candidate& c : cands)
                if (c.ev.accepted && lower(c.name).find(want) != std::string::npos
                    && (uuidWant.empty() || uuidOf(c.caps).find(uuidWant) != std::string::npos)) { pick = &c; break; }
            if (!pick) {
                ctxlog("MC2_VK_DEVICE=%s matches no accepted device; using the default pick", hint.c_str());
            }
        }
        // vk-present-instance 5: the most device-local VRAM among accepted discrete devices (not the first listed), then among any accepted device.
        for (const bool discreteOnly : {true, false}) {
            if (pick) break;
            for (const Candidate& c : cands)
                if (c.ev.accepted && (c.discrete || !discreteOnly) && (!pick || c.vram > pick->vram)) pick = &c;
        }
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
        for (size_t i = 0; i < cands.size(); ++i)
            if (&cands[i] == pick) ctxlog("[VK_DEVICE] index=%zu of %zu uuid=%s name=\"%s\" MC2_VK_DEVICE=\"%s\"", i, cands.size(), uuidOf(pick->caps).c_str(), pick->name.c_str(), mc2::gate::str<"MC2_VK_DEVICE">().c_str());
        health_.profileLine = profile::formatLine(pick->name, pick->caps, pick->ev);
        profileLog(health_.profileLine);
        ctxlog("vk caps report %s", profile::writeCapsReport(pick->pd, instanceApi, "debug_state/vk_caps.json") ? "written to debug_state/vk_caps.json" : "write failed");   // once per run, beside the diagnostic trace
        rtSel_ = profile::selectRt(caps);
        rtCaps_ = caps.rt;
        profileLog(profile::formatRtLine(caps, rtSel_));
        ctxlog("subgroup size=%u stages=0x%x ops=0x%x", caps.subgroupSize, caps.subgroupStages, caps.subgroupOps);
        VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props.pNext = caps.usableApi() >= profile::kFloorApi ? &driver : nullptr;
        vkGetPhysicalDeviceProperties2(phys_, &props);
        if (props.pNext) driverId_ = driver.driverID;
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
    {
        std::vector<uint32_t> flags(qfCount);
        for (uint32_t q = 0; q < qfCount; ++q) flags[q] = qfs[q].queueFlags;
        static_assert(VK_QUEUE_GRAPHICS_BIT == 1 && VK_QUEUE_COMPUTE_BIT == 2);   // the bits pickGraphicsFamily tests
        gfxFamily_ = mc2vk::profile::pickGraphicsFamily(flags);   // GRAPHICS and COMPUTE preferred: every compute dispatch uses this family
    }
    if (gfxFamily_ == UINT32_MAX) {
        std::string families;
        for (uint32_t q = 0; q < qfCount; ++q) {
            char b[64];
            std::snprintf(b, sizeof b, " family%u=flags:0x%x,count:%u", q, static_cast<unsigned>(qfs[q].queueFlags), qfs[q].queueCount);
            families += b;
        }
        return fail("no graphics queue family on " + std::string(health_.deviceName) + " (" + std::to_string(qfCount) + " families:" + families + ")");
    }

    // ---- logical device ----
    const float priorities[] = {1.0f, 1.0f};
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = gfxFamily_;
    qci.queueCount = wantPresent && qfs[gfxFamily_].queueCount > 1 ? 2u : 1u;
    qci.pQueuePriorities = priorities;

    // Every device extension comes from the profile, in table order: the swapchain of a present window, the KHR routes of dynamic rendering /
    // synchronization2 on a 1.2 device, and the memory-budget extension when the device has it (the VRAM line is then the OS's number and not VMA's
    // estimate). No external-memory extension is requested: no VK resource is exportable.
    std::vector<const char*> devExts;
    for (const std::string& e : profile_.deviceExtensions) devExts.push_back(e.c_str());
    if (caps.hasExtension(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME)) devExts.push_back(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
    if (caps.hasExtension(VK_AMD_BUFFER_MARKER_EXTENSION_NAME)) devExts.push_back(VK_AMD_BUFFER_MARKER_EXTENSION_NAME);
    // The FidelityFX VK backend fetches the KHR-suffixed memory-requirements entry points through vkGetDeviceProcAddr; the loader returns null for a
    // KHR name unless the extension is enabled (core 1.1 promotion does not alias it), and the backend calls the pointer unchecked.
    for (const char* e : {VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME, VK_KHR_BIND_MEMORY_2_EXTENSION_NAME, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME})
        if (caps.hasExtension(e) && std::ranges::find_if(devExts, [e](std::string_view x) { return x == e; }) == devExts.end()) devExts.push_back(e);

    // Ray tracing is an optional tier: the base AS extensions plus ray query / RT pipeline when the device lists them (profile::selectRt).
    for (const std::string& e : rtSel_.extensions)
        if (std::ranges::find_if(devExts, [&e](std::string_view x) { return x == e; }) == devExts.end()) devExts.push_back(e.c_str());

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
    enableFeatures.features.pipelineStatisticsQuery = optionalWaterFeatures.pipelineStatisticsQuery;
    enableFeatures.features.occlusionQueryPrecise = optionalWaterFeatures.occlusionQueryPrecise;
    VkPhysicalDeviceVulkan11Features en11{};
    en11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    VkPhysicalDeviceVulkan12Features en12{};
    en12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    // Optional temporal-AA (FidelityFX FSR) features: its 16-bit shader permutations declare Float16 / Int16 / 16-bit storage, and it barriers the
    // D32S8 scene depth with the depth aspect alone (separateDepthStencilLayouts). Enabled when the device has them; FSR is skipped otherwise.
    {
        VkPhysicalDeviceVulkan11Features q11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
        VkPhysicalDeviceVulkan12Features q12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        q11.pNext = &q12;
        VkPhysicalDeviceFeatures2 q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        q.pNext = &q11;
        vkGetPhysicalDeviceFeatures2(phys_, &q);
        enableFeatures.features.shaderInt16 = q.features.shaderInt16;
        en11.storageBuffer16BitAccess = q11.storageBuffer16BitAccess;
        en11.uniformAndStorageBuffer16BitAccess = q11.uniformAndStorageBuffer16BitAccess;
        en12.shaderFloat16 = q12.shaderFloat16;
        en12.separateDepthStencilLayouts = q12.separateDepthStencilLayouts;
        temporalFeatures_ = q.features.shaderInt16 && q11.storageBuffer16BitAccess && q11.uniformAndStorageBuffer16BitAccess &&
                            q12.shaderFloat16 && q12.separateDepthStencilLayouts;
    }
    VkPhysicalDeviceVulkan13Features en13{};
    en13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceDynamicRenderingFeaturesKHR enDr{};
    enDr.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR;
    VkPhysicalDeviceSynchronization2FeaturesKHR enS2{};
    enS2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR;
    {
        enableFeatures.features.independentBlend = featureEnabled("independentBlend") ? VK_TRUE : VK_FALSE;
        enableFeatures.features.samplerAnisotropy = featureEnabled("samplerAnisotropy") ? VK_TRUE : VK_FALSE;
        enableFeatures.features.pipelineStatisticsQuery = featureEnabled("pipelineStatisticsQuery") ? VK_TRUE : VK_FALSE;
        enableFeatures.features.shaderSampledImageArrayDynamicIndexing = featureEnabled("shaderSampledImageArrayDynamicIndexing") ? VK_TRUE : VK_FALSE;
        enableFeatures.features.multiDrawIndirect = featureEnabled("multiDrawIndirect") ? VK_TRUE : VK_FALSE;
        enableFeatures.features.drawIndirectFirstInstance = featureEnabled("drawIndirectFirstInstance") ? VK_TRUE : VK_FALSE;
        en11.shaderDrawParameters = featureEnabled("shaderDrawParameters") ? VK_TRUE : VK_FALSE;
        en12.drawIndirectCount = featureEnabled("drawIndirectCount") ? VK_TRUE : VK_FALSE;
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
        chain(en11);
        chain(en12);
        if (drCore || s2Core) chain(en13);
        if (featureEnabled("dynamicRendering") && !drCore) chain(enDr);
        if (featureEnabled("synchronization2") && !s2Core) chain(enS2);
    }

    VkPhysicalDeviceAccelerationStructureFeaturesKHR enAs{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR enRq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR enRp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
    if (rtSel_.accel) {
        VkBaseOutStructure* last = reinterpret_cast<VkBaseOutStructure*>(&enableFeatures);
        while (last->pNext) last = last->pNext;
        auto chainRt = [&last](auto& s) { last->pNext = reinterpret_cast<VkBaseOutStructure*>(&s); last = reinterpret_cast<VkBaseOutStructure*>(&s); };
        enAs.accelerationStructure = VK_TRUE;
        chainRt(enAs);
        if (rtSel_.rayQuery) { enRq.rayQuery = VK_TRUE; chainRt(enRq); }
        if (rtSel_.rayPipeline) { enRp.rayTracingPipeline = VK_TRUE; chainRt(enRp); }
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
    if (messenger_ != VK_NULL_HANDLE) runValidationCanary(device_);
    vkGetDeviceQueue(device_, gfxFamily_, 0, &gfxQueue_);
    vkGetDeviceQueue(device_, gfxFamily_, qci.queueCount - 1, &presentQueue_);
    ctxlog("queue family=%u graphics=0 present=%u (shared=%s)", gfxFamily_, qci.queueCount - 1,
           presentQueue_ == gfxQueue_ ? "yes" : "no");
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
    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(phys_, &props);
        const VkDeviceSize a = std::max<VkDeviceSize>({props.limits.minUniformBufferOffsetAlignment, props.limits.minStorageBufferOffsetAlignment, 16});
        frameAlign_ = a;
    }

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

    if (!RenderCore::activeTextureService) {
        textureServiceOwner_ = std::make_unique<RenderCore::TextureService>();
        RenderCore::activeTextureService = textureServiceOwner_.get();
    }
    textures_ = std::make_unique<TextureResidency>(*this, RenderCore::textureService());
    if (!textures_->init()) return fail("texture service initialization");
    health_.created = true;
    health_.fallbackReason.clear();
    gpuprof::init(*this);   // Tracy GPU context + debug labels + MC2_VK_GPU_TIMESTAMPS ring, on the one graphics queue
    ctxlog("engine Vulkan device created: device_name=\"%s\" present=%d validation=%s.",
           health_.deviceName, wantPresent ? 1 : 0, valPreset);
    if (mc2::gate::get<"MC2_VK_RT_SELFTEST">()) rt::runSelfTest(*this);   // test hook: GPU proof of the RT infrastructure, renders nothing
    return true;
}

void VulkanContext::retireIsland(IslandSlot slot) {
    std::unique_ptr<IslandState>& parked = islandSlot(slot);
    if (!parked) return;
    // The order of these lines is the shutdown order: with the image-owner lines of vk_image_resource.cpp they let a log show the destroy() precondition.
    static constexpr const char* kSlotNames[] = {"Vfx", "Sky", "LegacyShape", "Preview", "Mech", "ColorProp", "Water", "Shadow", "Terrain", "PassTimer", "ActorCull", "Vegetation", "RtScene"};
    static_assert(sizeof(kSlotNames) / sizeof(kSlotNames[0]) == static_cast<size_t>(IslandSlot::Count), "name every island slot");
    if (mc2_diag::tagRequested("VK_SHUTDOWN")) ctxlog("retire island slot=%s.", kSlotNames[static_cast<size_t>(slot)]);
    waitIdle("retire_island");
    deferred_.drainAll();   // parked owners may name this island's pools (a retired sprite page frees its set): they go while the island is still alive
    parked.reset();
}

void VulkanContext::waitIdle(std::string_view why) const {
    mc2::overlap::assertOwner(mc2::overlap::Owner::Render, "VulkanContext::waitIdle");
    if (device_ == VK_NULL_HANDLE) return;
    ZoneScopedN("VK.WaitIdle");
    const auto idleT0 = std::chrono::steady_clock::now();
    const auto lock = lockSubmit(submitMutex_);
    flushPendingLocked();
    const auto presentLock = lockSubmit(presentMutex_);   // device idle externally synchronizes every queue
    const VkResult r = [&] { MC2_LOAD_SCOPE("gpuwait.deviceIdle"); return vkDeviceWaitIdle(device_); }();
    if (r == VK_SUCCESS) hostSyncedGfx_ = (std::max)(hostSyncedGfx_, tracker_.lastSubmitted(QueueKind::Graphics));   // an idle device is a host sync point for everything submitted
    {
        auto& hs = mc2_hitch2::stats();
        const double idleMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - idleT0).count();
        hs.waitIdleCount.fetch_add(1, std::memory_order_relaxed);
        hs.waitIdleNs.fetch_add(static_cast<uint64_t>(idleMs * 1e6), std::memory_order_relaxed);
        mc2_hitch2::scopes().add("VK.WaitIdle", idleMs);
        // First 24 calls after mission_ready name their caller (the why tag) and the wait.
        if (hs.missionReady.load(std::memory_order_relaxed) && hs.waitIdleLogged.fetch_add(1, std::memory_order_relaxed) < 24 && mc2_diag::tagRequested("HITCH"))
            ctxlog("[HITCH v2] waitIdle why=%.*s ms=%.2f", static_cast<int>(why.size()), why.data(), idleMs);
        // Mid-mission device idles are stalls the contract forbids (only drainDeferred waits): name each distinct caller once, tag or no tag. Render thread only.
        if (hs.missionReady.load(std::memory_order_relaxed)) {
            static std::mutex s_warnedMutex;
            static std::vector<std::string> s_warned;
            const std::string w(why);
            std::lock_guard<std::mutex> warnLock(s_warnedMutex);
            if (s_warned.size() < 32 && std::find(s_warned.begin(), s_warned.end(), w) == s_warned.end()) {
                s_warned.push_back(w);
                ctxlog("WARNING waitIdle after mission ready why=%s ms=%.2f (retire the resource on a ticket instead).", w.c_str(), idleMs);
            }
        }
    }
    // A lost device ends the wait with an error; the release that follows is still safe, but the log must say the GPU did not drain.
    if (r != VK_SUCCESS) ctxlog("vkDeviceWaitIdle failed before %.*s: VkResult=%d.", static_cast<int>(why.size()), why.data(), static_cast<int>(r));
    if (r == VK_ERROR_DEVICE_LOST) infra::deviceLost();
}

VkResult VulkanContext::submit(const VkSubmitInfo& si, VkFence fence, uint64_t* outValue) const {
    mc2::overlap::assertOwner(mc2::overlap::Owner::Render, "VulkanContext::submit");
    if (device_ == VK_NULL_HANDLE || gfxTimeline_ == VK_NULL_HANDLE || infra::deviceLostLatched()) return VK_ERROR_DEVICE_LOST;
    const auto lock = lockSubmit(submitMutex_);   // the value and the queue order must agree
    flushUploadsLocked();   // queued uploads run ahead of every submit that may sample them
    return submitLocked(si, fence, outValue);
}

VkResult VulkanContext::submitPresentTail(VkCommandBuffer cb, VkSemaphore acquire, VkSemaphore rendered, VkFence fence) const {
    mc2::overlap::assertOwner(mc2::overlap::Owner::Present, "VulkanContext::submitPresentTail");
    const auto lock = lockSubmit(submitMutex_);
    if (infra::deviceLostLatched()) return VK_ERROR_DEVICE_LOST;
    VkCommandBufferSubmitInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    command.commandBuffer = cb;
    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = acquire;
    wait.stageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = rendered;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &command;
    submit.waitSemaphoreInfoCount = acquire ? 1 : 0;
    submit.pWaitSemaphoreInfos = &wait;
    submit.signalSemaphoreInfoCount = rendered ? 1 : 0;
    submit.pSignalSemaphoreInfos = &signal;
    const VkResult result = queueSubmit2(gfxQueue_, submit, fence);
    if (result == VK_ERROR_DEVICE_LOST) infra::deviceLost();
    return result;
}

VkResult VulkanContext::present(VkQueue queue, const VkPresentInfoKHR& info) const {
    assert(queue == presentQueue_ || queue == gfxQueue_);
    const auto lock = lockSubmit(queue == gfxQueue_ ? submitMutex_ : presentMutex_);
    // Render work was flushed before seal. Main may already be recording another frame: never touch its batch here.
    mc2::overlap::assertOwner(mc2::overlap::Owner::Present, "vkQueuePresentKHR");
    if (infra::deviceLostLatched()) return VK_ERROR_DEVICE_LOST;   // no queue work on a lost device
    return vkQueuePresentKHR(queue, &info);
}

GpuTicket VulkanContext::flushUploadsLocked() const {
    if (!uploads_.pending()) return {};
    MC2_LOAD_SCOPE("upload.record");
    OwnedCommandPool pool;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (!uploads_.build(*this, pool, cb)) return {};
    mc2_hitch2::noteUploadsFlushed();
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
    const auto lock = lockSubmit(submitMutex_);
    return flushUploadsLocked();
}

VkResult VulkanContext::submitLocked(const VkSubmitInfo& si, VkFence fence, uint64_t* outValue) const {
    // A deferred batch that failed earlier already returned VK_SUCCESS to its callers: surface it here, once, so an island disables itself.
    if (present_thread::detail::takeDeferredFailure(pendingFailed_)) return VK_ERROR_UNKNOWN;
    assert(si.pNext == nullptr && "submit(): the timeline info is appended here; pass a plain VkSubmitInfo");
    if (si.signalSemaphoreCount > 7 || si.waitSemaphoreCount > 8) return VK_ERROR_INITIALIZATION_FAILED;
    if (fence == VK_NULL_HANDLE && si.waitSemaphoreCount == 0 && si.signalSemaphoreCount == 0 && si.commandBufferCount > 0) {
        const uint64_t deferred = tracker_.noteSubmit(QueueKind::Graphics);
        const auto now = std::chrono::steady_clock::now();
        for (uint32_t i = 0; i < si.commandBufferCount; ++i) batch_.addPlain(si.pCommandBuffers[i], deferred, now);
        if (outValue) *outValue = deferred;
        // A batch older than 0.4 ms goes to the queue now: held until a wait or the present, the GPU idled while the CPU recorded the rest of the frame.
        // Reserved recordings are not recorded for that: the prefix before the first goes out, and the recordings resolve together once the oldest has
        // waited 1 ms (or at a flush point), so one batch of islands records in parallel.
        if (batch_.size() >= 64 || now - batch_.since() > std::chrono::microseconds(400)) {
            if (!batch_.hasRecords()) flushPendingLocked();
            else if (now - batch_.firstRecordReservedAt() > std::chrono::microseconds(1000)) flushPendingLocked();
            else flushPrefixLocked(batch_.firstRecordEntry());
        }
        return VK_SUCCESS;
    }
    flushPendingLocked();   // keep queue order: everything deferred so far goes before this submit
    // A failed deferred prefix cannot be hidden by a successful fenced tail.
    if (present_thread::detail::takeDeferredFailure(pendingFailed_)) return VK_ERROR_UNKNOWN;
    if (infra::deviceLostLatched()) return VK_ERROR_DEVICE_LOST;
    const uint64_t value = tracker_.noteSubmit(QueueKind::Graphics);
    // vkQueueSubmit2: the caller's VkSubmitInfo is translated (binary semaphores keep value 0; the stage masks are widened to the 64-bit
    // sync2 type; a caller's wait stage of 0 means TOP_OF_PIPE as in the legacy call). The caller's binary signals go first, the
    // timeline last, signalled at ALL_COMMANDS so it covers every stage of the batch.
    VkSemaphoreSubmitInfo waits[8]{};
    VkSemaphoreSubmitInfo signals[8]{};
    VkCommandBufferSubmitInfo cbs[16]{};
    if (si.commandBufferCount > 16) return VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t i = 0; i < si.waitSemaphoreCount; ++i) {
        waits[i] = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        waits[i].semaphore = si.pWaitSemaphores[i];
        waits[i].stageMask = si.pWaitDstStageMask && si.pWaitDstStageMask[i] ? VkPipelineStageFlags2(si.pWaitDstStageMask[i]) : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    }
    for (uint32_t i = 0; i < si.signalSemaphoreCount; ++i) {
        signals[i] = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        signals[i].semaphore = si.pSignalSemaphores[i];
        signals[i].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    }
    signals[si.signalSemaphoreCount] = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signals[si.signalSemaphoreCount].semaphore = gfxTimeline_;
    signals[si.signalSemaphoreCount].value = value;
    signals[si.signalSemaphoreCount].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    for (uint32_t i = 0; i < si.commandBufferCount; ++i) {
        cbs[i] = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        cbs[i].commandBuffer = si.pCommandBuffers[i];
    }
    VkSubmitInfo2 out{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    out.waitSemaphoreInfoCount = si.waitSemaphoreCount;
    out.pWaitSemaphoreInfos = waits;
    out.commandBufferInfoCount = si.commandBufferCount;
    out.pCommandBufferInfos = cbs;
    out.signalSemaphoreInfoCount = si.signalSemaphoreCount + 1;
    out.pSignalSemaphoreInfos = signals;
    const VkResult r = queueSubmit2(gfxQueue_, out, fence);
    if (r != VK_SUCCESS) {
        // A refused submit signals nothing: give the value back so the counter and the timeline stay in step.
        tracker_.unnoteSubmit(QueueKind::Graphics, value);
        if (r == VK_ERROR_DEVICE_LOST) infra::deviceLost();
        return r;
    }
    if (outValue) *outValue = value;
    return r;
}

VkResult VulkanContext::queueSubmit2(VkQueue q, const VkSubmitInfo2& si, VkFence fence) const {
    return route("synchronization2") == profile::Route::Core ? vkQueueSubmit2(q, 1, &si, fence) : vkQueueSubmit2KHR(q, 1, &si, fence);
}

void VulkanContext::flushPendingLocked() const {
    const auto plan = batch_.planFlush(present_thread::onWorker());
    if (plan.violation) {
        // The present enqueue resolves every reservation on main before the worker runs (present_thread::enqueue). Queueing past an unresolved one
        // would present a frame missing its islands and signal the timeline out of order, so only the prefix goes: loud once, never a crash.
        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            ctxlog("SUBMIT_ORDER FAIL: the present worker found unresolved recorded islands (batch %zu entries, first reservation at %zu); only the prefix is queued.",
                   batch_.size(), batch_.firstRecordEntry());
        }
    }
    if (plan.resolveFirst) resolveRecordsLocked();   // the render thread resolves the recordings (their done() callbacks belong to it)
    flushPrefixLocked(plan.count);
}

void VulkanContext::resolveRecordsLocked() const {
    if (!batch_.hasRecords()) return;
    ZoneScopedN("VKContext.ResolveRecords");
    VulkanContext* self = const_cast<VulkanContext*>(this);
    batch_.resolve([self] { return self->acquireRecordCommandBuffer(); },
        [this](int n, const std::function<void(int)>& body) {
            if (n >= 2 && recordExecutor_.run && recordExecutor_.active && recordExecutor_.active()) recordExecutor_.run(n, body);
            else for (int i = 0; i < n; ++i) body(i);
        });
}

void VulkanContext::resolveRecorded() {
    if (device_ == VK_NULL_HANDLE || present_thread::onWorker()) return;
    const auto lock = lockSubmit(submitMutex_);
    resolveRecordsLocked();
}

VkResult VulkanContext::submitRecorded(RecordFn rec, RecordDoneFn done, uint64_t* outValue) {
    mc2::overlap::assertOwner(mc2::overlap::Owner::Render, "VulkanContext::submitRecorded");
    const auto fail = [&](VkResult r) { if (done) done(false); return r; };
    if (device_ == VK_NULL_HANDLE || gfxTimeline_ == VK_NULL_HANDLE || infra::deviceLostLatched()) return fail(VK_ERROR_DEVICE_LOST);
    if (!recordExecutor_.run || !recordExecutor_.active || !recordExecutor_.active()) {
        // No worker threads: record here and submit now, the same commands in the same order.
        VkCommandBuffer cb = acquireRecordCommandBuffer();
        if (cb == VK_NULL_HANDLE || !rec(cb)) return fail(VK_ERROR_UNKNOWN);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        const VkResult r = submit(si, VK_NULL_HANDLE, outValue);
        if (done) done(r == VK_SUCCESS);
        return r;
    }
    const auto lock = lockSubmit(submitMutex_);
    flushUploadsLocked();   // queued uploads run ahead of every submit that may sample them
    if (pendingFailed_) { pendingFailed_ = false; return fail(VK_ERROR_UNKNOWN); }
    const uint64_t value = tracker_.noteSubmit(QueueKind::Graphics);
    const auto now = std::chrono::steady_clock::now();
    batch_.reserve(value, std::move(rec), std::move(done), now);
    if (outValue) *outValue = value;
    return VK_SUCCESS;
}

void VulkanContext::flushPrefixLocked(size_t count) const {
    const auto flush = batch_.takePrefix(count);
    if (flush.count == 0) return;
    const uint64_t value = flush.value;
    std::vector<VkCommandBufferSubmitInfo> cbs;   // a recording that failed has no buffer: only its value is signalled
    cbs.reserve(flush.cbs.size());
    for (VkCommandBuffer cb : flush.cbs) {
        VkCommandBufferSubmitInfo c{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        c.commandBuffer = cb;
        cbs.push_back(c);
    }
    if (infra::deviceLostLatched()) return;   // no queue work on a lost device
    VkSemaphoreSubmitInfo sig{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    sig.semaphore = gfxTimeline_;
    sig.value = value;
    sig.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 out{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    out.commandBufferInfoCount = static_cast<uint32_t>(cbs.size());
    out.pCommandBufferInfos = cbs.data();
    out.signalSemaphoreInfoCount = 1;
    out.pSignalSemaphoreInfos = &sig;
    VkResult r = queueSubmit2(gfxQueue_, out, VK_NULL_HANDLE);
    if (r != VK_SUCCESS) {
        ctxlog("the batched submit failed: VkResult=%d (the timeline value is signalled empty to stay in step).", static_cast<int>(r));
        if (r == VK_ERROR_DEVICE_LOST) { infra::deviceLost(); return; }
        pendingFailed_ = true;
        out.commandBufferInfoCount = 0;
        out.pCommandBufferInfos = nullptr;
        r = queueSubmit2(gfxQueue_, out, VK_NULL_HANDLE);
        // The timeline could not be kept in step: every later wait on this value would burn its full timeout. Treat the device as lost.
        if (r != VK_SUCCESS) { ctxlog("the empty timeline signal failed too (VkResult=%d): marking the device lost.", static_cast<int>(r)); infra::deviceLost(); }
    }
}

void VulkanContext::kick() const {
    mc2::overlap::assertOwner(mc2::overlap::Owner::Render, "VulkanContext::kick");
    if (device_ == VK_NULL_HANDLE || gfxTimeline_ == VK_NULL_HANDLE) return;
    const auto lock = lockSubmit(submitMutex_);
    flushPendingLocked();
}

void VulkanContext::poll() const {
    mc2::overlap::assertOwner(mc2::overlap::Owner::Render, "VulkanContext::poll");
    if (gfxTimeline_ == VK_NULL_HANDLE) return;
    { const auto lock = lockSubmit(submitMutex_); flushPendingLocked(); }   // a poll means someone wants completion: the batch must be on the queue
    uint64_t v = 0;
    const VkResult result = vkGetSemaphoreCounterValue(device_, gfxTimeline_, &v);
    if (result == VK_SUCCESS) {
        const auto lock = lockSubmit(submitMutex_);
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
    if (tracker_.isComplete(t)) return true;
    if (gfxTimeline_ == VK_NULL_HANDLE) return false;
    // No counter poll before the wait: a value the GPU already passed still goes through vkWaitSemaphores, the only host sync point the
    // validation layers (and the readback contract) recognise. A caller that waits then touches or reuses the memory right away (one-shot
    // readbacks) would otherwise retire nothing. The deferred batch is flushed first so the value is on the queue.
    { const auto lock = lockSubmit(submitMutex_); flushPendingLocked(); }
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &gfxTimeline_;
    wi.pValues = &t.gfx;
    const auto waitT0 = std::chrono::steady_clock::now();
    const VkResult r = [&] { MC2_LOAD_SCOPE("gpuwait.timeline"); return vkWaitSemaphores(device_, &wi, timeoutNs); }();
    // Only a wait that blocked is counted; a one-shot caller's open scope counts its own.
    if (!mc2_hitch2::inOneShotScope()) mc2_hitch2::noteWait(mc2_hitch2::kSlot, why, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waitT0).count());
    if (r != VK_SUCCESS) ctxlog("timeline wait for %.*s ended with VkResult=%d (ticket gfx=%llu).", static_cast<int>(why.size()), why.data(), static_cast<int>(r), static_cast<unsigned long long>(t.gfx));
    if (r == VK_SUCCESS && t.gfx > hostSyncedGfx_) hostSyncedGfx_ = t.gfx;
    poll();
    return tracker_.isComplete(t);
}

bool VulkanContext::hostSynced(const GpuTicket& t, std::string_view why) const {
    mc2::overlap::assertOwner(mc2::overlap::Owner::Render, "VulkanContext::hostSynced");
    if (t.gfx <= hostSyncedGfx_) return true;
    if (gfxTimeline_ == VK_NULL_HANDLE) return tracker_.isComplete(t);
    if (!tracker_.isComplete(t)) {
        poll();
        if (!tracker_.isComplete(t)) return false;   // still in flight: no wait, the frame does not block on a readback
    }
    // The GPU passed the value already, so a zero-timeout wait returns at once; it is still the vkWaitSemaphores the layers retire accesses on.
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &gfxTimeline_;
    wi.pValues = &t.gfx;
    const VkResult r = vkWaitSemaphores(device_, &wi, 0);
    if (r == VK_SUCCESS) { hostSyncedGfx_ = t.gfx; return true; }
    if (r != VK_TIMEOUT) ctxlog("timeline host sync for %.*s ended with VkResult=%d (ticket gfx=%llu).", static_cast<int>(why.size()), why.data(), static_cast<int>(r), static_cast<unsigned long long>(t.gfx));
    if (r == VK_ERROR_DEVICE_LOST) infra::deviceLost();
    return false;
}

void VulkanContext::beginFrame() {
    mc2::overlap::requireOwner(mc2::overlap::Owner::Render, "VulkanContext::beginFrame");
    if (tracker_.frameOpen()) return;
    { ZoneScopedN("VKContext.Poll"); poll(); }
    tracker_.beginFrame();
    { ZoneScopedN("VKContext.Collect"); deferred_.collect(tracker_); }
    { ZoneScopedN("VKContext.InfraFrame"); infra::frame(); }
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
    recordPools_.beginFrame(static_cast<uint32_t>(f % kMaxFramesInFlight), slotFree,
                            [&](uint32_t slot, uint32_t participant) { vkResetCommandPool(device_, recordCmdPools_[slot][participant].get(), 0); });
    if (textures_ && slotFree) { ZoneScopedN("VKContext.TexturePump"); textures_->beginFrame(f, frameContext().slot); }
}

VkCommandBuffer VulkanContext::acquireRecordCommandBuffer() {
    const uint32_t slot = static_cast<uint32_t>(tracker_.frameNo() % kMaxFramesInFlight);
    return recordPools_.acquire(slot, [&](uint32_t s, uint32_t participant) -> VkCommandBuffer {
        OwnedCommandPool& pool = recordCmdPools_[s][participant];
        if (!pool) {
            VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;   // re-recorded every frame; the whole pool resets when its slot restarts
            pci.queueFamilyIndex = gfxFamily_;
            if (!create(pool, device_, [&](VkCommandPool* h) { return vkCreateCommandPool(device_, &pci, nullptr, h); })) return VK_NULL_HANDLE;
        }
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool.get();
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        return vkAllocateCommandBuffers(device_, &ai, &cb) == VK_SUCCESS ? cb : VK_NULL_HANDLE;
    });
}

void VulkanContext::drainDeferred(std::string_view why) {
    if (deferred_.size() == 0) return;
    waitIdle(why);
    poll();
    const uint32_t n = deferred_.drainAll();
    ctxlog("drained %u deferred destroy entr%s (%.*s).", n, n == 1 ? "y" : "ies", static_cast<int>(why.size()), why.data());
}

void VulkanContext::endFrame() {
    mc2::overlap::requireOwner(mc2::overlap::Owner::Render, "VulkanContext::endFrame");
    ZoneScopedN("VKContext.EndFrame");
    flushUploads();   // an upload queued this frame is submitted inside it, so the frame ticket covers its staging
    { const auto lock = lockSubmit(submitMutex_); flushPendingLocked(); }   // the sealed frame ticket must be on the queue
    tracker_.endFrame();
    deferred_.sealFrame(tracker_.frameTicket(tracker_.frameNo()));
    if (tracker_.frameNo() % 600 == 0 && mc2DiagVerbose()) {   // the periodic summaries are MC2_DIAG_VERBOSE lines (a default build stays quiet)
        poll();
        if (uploads_.queued() != 0)
            std::fprintf(stderr, "[VK_UPLOAD v1] flushes=%llu queued=%llu dedicated_stages=%llu dropped=%llu frame_ring_growths=%u\n",
                         static_cast<unsigned long long>(uploads_.flushes()), static_cast<unsigned long long>(uploads_.queued()),
                         static_cast<unsigned long long>(uploads_.dedicatedStages()), static_cast<unsigned long long>(uploads_.dropped()), frameRing_.growths());
        const DeferredDestroy::Stats ds = deferred_.stats(tracker_);
        std::fprintf(stderr, "[VK_TIMELINE v1] deferred_entries=%u deferred_bytes=%llu oldest_age_frames=%llu\n", ds.entries,
                     static_cast<unsigned long long>(ds.bytes), static_cast<unsigned long long>(ds.oldestAgeFrames));
        std::fprintf(stderr, "[VK_FRAMERING v1] capacity=%llu peak_used=%llu\n", static_cast<unsigned long long>(frameRing_.capacityPerSlot()),
                     static_cast<unsigned long long>(frameRing_.peakPerSlot()));
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
    mc2::gpumem::finish();
    gpuprof::shutdown(*this);
    if (g_amdChecksExpected.load() && !g_amdCanarySeen.load())
        validationLog("warning", "amd_canary_missing", "preset best-practices ran but none of AvoidImageToImageCopy / ClearAttachment-ClearImage / CmdBuffer-highBarrierCount / KeepLayoutSmall was reported: the AMD checks did not apply (layer too old, or VK_LAYER_ENABLES ignored)");
    g_debugNames = false;
    if (device_ != VK_NULL_HANDLE) { waitIdle("teardown"); deferred_.drainAll(); }   // parked owners go ahead of the allocator and the device
    // The islands consume the light feed, so they go first; both go before the allocator and the device their objects were made on.
    for (size_t i = 0; i < islands_.size(); ++i) retireIsland(static_cast<IslandSlot>(i));
    textures_.reset();
    deferred_.drainAll();
    releaseOneShotPool();   // the shared one-shot command pool goes with the device, after every island that might submit one-shots
    if (textureServiceOwner_) { RenderCore::activeTextureService = nullptr; textureServiceOwner_.reset(); }
    lightFeed_.release();
    frameRing_.release();
    recordPools_.clear();
    for (auto& slot : recordCmdPools_) for (auto& pool : slot) pool.reset();   // their command buffers go with them
    readbacks_.releaseAll();
    // PRECONDITION: the owners that live outside the context (the shared scene colour / depth / normal images, the present path's shared image, the
    // shadow island's images) were destroyed by the engine shutdown ahead of this call. A resource still alive here would release its image on a device
    // that is gone, so the count is checked and a violation is logged where the validation layer would otherwise report it far from its cause.
    if (const int live = vkImageResourceLiveCount(); live != 0) {
        ctxlog("destroy() with %d image resource(s) still alive: shutdown order violated.", live);
        assert(live == 0 && "image resources must be destroyed before the engine Vulkan context");
    }
    if (allocator_) { vmaDestroyAllocator(allocator_); allocator_ = nullptr; }
    infra::shutdown();
    if (device_ != VK_NULL_HANDLE) {
        if (gfxTimeline_ != VK_NULL_HANDLE) { vkDestroySemaphore(device_, gfxTimeline_, nullptr); gfxTimeline_ = VK_NULL_HANDLE; }
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    gfxQueue_ = VK_NULL_HANDLE;
    presentQueue_ = VK_NULL_HANDLE;
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
    rtSel_ = profile::RtSelection{};
    rtCaps_ = profile::RtCaps{};
    profile_.accepted = false;
    luidValid_ = false;
    temporalFeatures_ = false;
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
    if (allocator_ && phys_ != VK_NULL_HANDLE) {
        VkPhysicalDeviceMemoryProperties mp{};
        vkGetPhysicalDeviceMemoryProperties(phys_, &mp);
        VmaBudget budgets[VK_MAX_MEMORY_HEAPS] = {};
        vmaGetHeapBudgets(allocator_, budgets);
        const auto mb = [](VkDeviceSize b) { return static_cast<unsigned long long>(b >> 20); };
        if (mc2_diag::tagRequested("VK_MEM"))
            for (uint32_t i = 0; i < mp.memoryHeapCount; ++i)
                std::fprintf(stderr, "[VK_MEM v1] heap=%u device_local=%d size_mb=%llu budget_mb=%llu usage_mb=%llu alloc_mb=%llu blocks_mb=%llu allocs=%u blocks=%u\n",
                             i, (mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? 1 : 0, mb(mp.memoryHeaps[i].size), mb(budgets[i].budget), mb(budgets[i].usage),
                             mb(budgets[i].statistics.allocationBytes), mb(budgets[i].statistics.blockBytes), budgets[i].statistics.allocationCount, budgets[i].statistics.blockCount);
    }
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

VulkanContext& engineVulkanContextNoWait() { return engineContext(); }

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

size_t deferredDestroyDepth() { return getOrCreateEngineVulkanContext().deferred().size(); }

}  // namespace mc2vk

#endif  // MC2_VULKAN
