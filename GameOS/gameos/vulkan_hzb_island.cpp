// VK-REGION-HZB-1 -- the THIRD region grown through IRenderBackend::runRegion
// (after PostprocessFog and PostprocessSSAO). Proves the HZB reverse-Z depth-
// pyramid reduce chain on a headless Vulkan device, following the
// VULKAN-ISLAND-TEMPLATE-1 checklist exactly. Entire TU compiles to nothing
// unless MC2_VULKAN_ISLAND is defined (CMake option, requires MC2_VULKAN).
// Fail-soft everywhere: any Vulkan error disables the island for the rest of
// the process and the caller falls back to the GL HZB path (runHzbRegionGL()).
// Never crashes.
//
// DESIGN: N sequential fullscreen draws (one per pyramid level), each in its
// own render pass instance over ONE shared render-pass object + ONE pipeline
// (dynamic viewport/scissor -- level dims shrink down the ladder):
//   level 0 (seed):  samples the bridged full-res scene depth (D32_SFLOAT),
//                    uReduce=0 -> 1:1 reverse-Z copy into level-0 R32_SFLOAT.
//   level k (reduce): samples level k-1, uReduce=1 -> clamped 2x2 MIN.
//
// SEPARATE VkImage PER LEVEL (mirrors GL, and is REQUIRED here): the GL ladder
// is CEIL-halved (odd extents round UP, preserving edge texels per
// docs/hzb-depth-convention.md) while Vulkan mip-level dimensions are DEFINED
// floor-halved -- a single mipped VkImage cannot represent the ceil ladder at
// all (e.g. 75 -> ceil 38 vs floor 37). Separate images also remove any
// read/write feedback (source and destination are distinct objects), exactly
// like the GL side's separate textures.
//
// B13 (VULKAN-BARRIER-PLAN-RECON-1, per-mip HZB barrier): each level's render
// pass declares an EXTERNAL subpass dependency COLOR_ATTACHMENT_OUTPUT/WRITE ->
// FRAGMENT_SHADER/READ with finalLayout SHADER_READ_ONLY -- so level k's write
// is ordered before level k+1's sampled read with no explicit
// vkCmdPipelineBarrier (the same implicit-transition mechanism as the H1 fix
// and the SSAO pass1->pass2 ordering). That per-mip barrier IS the point of
// this slice.
//
// DATA FLOW (per frame, gate ON):
//   1. glGetTexImage scene DEPTH -> depth staging (same bridge as fog/ssao).
//   2. vkCmdCopyBufferToImage depth -> D32_SFLOAT image.
//   3. For each level 0..N-1: render pass (DONT_CARE -> STORE, fullscreen
//      triangle overwrites every texel), dynamic viewport = level dims,
//      descriptor set k (sampler = depth or level k-1; UBO k = uSrcTexel/uReduce).
//   4. Per-level barrier SHADER_READ -> TRANSFER_READ, then
//      vkCmdCopyImageToBuffer each level into ONE readback buffer at offsets.
//   5. fence; memcpy; glTexSubImage2D each level -> hzbLevelTex_[level], so
//      the GL probe (MC2_HZB_PROBE) cross-checks the VULKAN-built pyramid.
//
// The per-level uniform values are pulled EXACTLY from the GL runHzbReduce()
// uploads (uSrcTexel = 1/source-level dims; uReduce 0/1). min() of floats is
// bit-portable across compilers -- unlike ssao there is NO transcendental
// caveat; this region is byte-exact-provable (fixture oracle tolerance 0).
//
// CPU readback is the VALIDATION ORACLE, not a shipping design (template §5).
//
// SHADER SOURCE: shaders/vulkan/hzb_reduce.{vert,frag} -- a HAND PORT (same
// stopgap discipline as edge_fog/fog_oob/ssao, ladder §5a; L6.5 folds these
// onto the single-source dual-target bake). Explicit set/binding layout:
// sampler @ (0,0), params UBO @ (0,1).

#include "gos_postprocess.h"

#ifdef MC2_VULKAN_ISLAND

// volk owns Vulkan dispatch (dynamic load; no hard link to vulkan-1.dll).
#include <volk.h>
#include "vk_mem_alloc.h"

// GL for the glGetTexImage / glTexSubImage2D bridge + the sampleable GL textures.
#include "utils/gl_utils.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <vector>

// VK-REGION-HZB-1: process-lifetime health/diagnostic snapshot. A single
// file-static instance is written by the island and read (via the extern "C"
// getter below) by the debug-state dump writer, so the health report survives
// island teardown. Named distinctly from the other islands' health so all can
// be compiled into the same build without symbol collision.
namespace {
struct HzbIslandHealth {
    int  vulkanAvailable        = 0;   // volkInitialize() succeeded
    int  buildEnabled           = 1;   // this TU compiled (always 1 here)
    char deviceName[256]        = {0}; // VkPhysicalDeviceProperties.deviceName
    unsigned long validationErrors = 0;   // ERROR-severity validation msgs
    unsigned long attempted     = 0;   // frames the island path was entered
    unsigned long usedVulkan    = 0;   // frames the pyramid was actually built via Vulkan
    char fallbackReason[64]     = {0}; // "" when healthy; else why GL fallback happened

    unsigned long vkDraws       = 0;   // last-frame actual Vulkan draws (== level count)
    unsigned long levels        = 0;   // pyramid level count (ceil ladder)

    // timings (last frame, microseconds)
    double copyDepthUs  = 0.0;   // glGetTexImage depth bridge
    double buildUs      = 0.0;   // submit + fence-wait (all N level draws + copies)
    double readbackOutUs = 0.0;  // map + glTexSubImage2D all levels
    double totalUs      = 0.0;
};
HzbIslandHealth g_health;

void setFallback(const char* reason) {
    std::strncpy(g_health.fallbackReason, reason ? reason : "",
                 sizeof(g_health.fallbackReason) - 1);
    g_health.fallbackReason[sizeof(g_health.fallbackReason) - 1] = '\0';
}
} // namespace

// Read-only accessor consumed by GameOS/gameos/debug_state_dump.cpp (under the same
// #ifdef MC2_VULKAN_ISLAND). Copies the POD out; no Vulkan headers leak.
extern "C" void mc2_vulkan_hzb_island_health(
    int* vulkanAvailable, int* buildEnabled,
    const char** deviceName, unsigned long* validationErrors,
    unsigned long* attempted, unsigned long* usedVulkan,
    const char** fallbackReason, unsigned long* vkDraws, unsigned long* levels,
    double* copyDepthUs, double* buildUs, double* readbackOutUs, double* totalUs) {
    if (vulkanAvailable)  *vulkanAvailable  = g_health.vulkanAvailable;
    if (buildEnabled)     *buildEnabled     = g_health.buildEnabled;
    if (deviceName)       *deviceName       = g_health.deviceName;
    if (validationErrors) *validationErrors = g_health.validationErrors;
    if (attempted)        *attempted        = g_health.attempted;
    if (usedVulkan)       *usedVulkan       = g_health.usedVulkan;
    if (fallbackReason)   *fallbackReason   = g_health.fallbackReason;
    if (vkDraws)          *vkDraws          = g_health.vkDraws;
    if (levels)           *levels           = g_health.levels;
    if (copyDepthUs)      *copyDepthUs      = g_health.copyDepthUs;
    if (buildUs)          *buildUs          = g_health.buildUs;
    if (readbackOutUs)    *readbackOutUs    = g_health.readbackOutUs;
    if (totalUs)          *totalUs          = g_health.totalUs;
}

namespace {

void vlog(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "[VK_HZB_ISLAND] ");
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
    va_end(ap);
}

// VULKAN-ISLAND-VALIDATION-WIRING-1 (mirrors the other islands). Opt-in Vulkan
// validation for the island's own instance. ERROR-severity messages increment
// g_health.validationErrors; WARNING is logged but not counted.
bool resolve_validation_preset(std::vector<VkValidationFeatureEnableEXT>& feats,
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
    vlog("validation preset '%s' unrecognized; falling back to 'core'.", env);
    resolvedName = "core";
    return true;
}

VKAPI_ATTR VkBool32 VKAPI_CALL hzb_debug_cb(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT* pData,
    void* /*user*/) {
    const char* msg = (pData && pData->pMessage) ? pData->pMessage : "(no message)";
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++g_health.validationErrors;
        std::fprintf(stderr, "[VK_HZB_VALIDATION] ERROR: %s\n", msg);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        std::fprintf(stderr, "[VK_HZB_VALIDATION] warning: %s\n", msg);
    }
    return VK_FALSE;
}

// volk once. Each Vulkan TU has its own ensure_volk_initialized() in its
// anonymous namespace (internal linkage) so there is no ODR collision, and
// volkInitialize() is idempotent across all.
bool ensure_volk_initialized() {
    static int state = 0; // 0 untried, 1 ok, -1 failed
    if (state == 1) return true;
    if (state == -1) return false;
    VkResult r = volkInitialize();
    if (r != VK_SUCCESS) {
        vlog("volkInitialize() failed (VkResult=%d) -- no Vulkan runtime. "
             "Falling back to GL HZB.", (int)r);
        state = -1;
        g_health.vulkanAvailable = 0;
        setFallback("no_vulkan_runtime");
        return false;
    }
    state = 1;
    g_health.vulkanAvailable = 1;
    return true;
}

std::string spv_dir() {
    const char* env = std::getenv("MC2_VULKAN_SPV_DIR");
    std::string d = (env && *env) ? env : "shaders/vulkan";
    if (!d.empty() && d.back() != '/' && d.back() != '\\') d += '/';
    return d;
}

VkShaderModule load_spv(VkDevice device, const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { vlog("cannot open spv '%s'.", path.c_str()); return VK_NULL_HANDLE; }
    std::fseek(f, 0, SEEK_END);
    long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (len <= 0 || (len % 4) != 0) {
        vlog("spv '%s' bad size %ld.", path.c_str(), len);
        std::fclose(f);
        return VK_NULL_HANDLE;
    }
    std::vector<uint32_t> code(static_cast<size_t>(len) / 4);
    size_t got = std::fread(code.data(), 1, static_cast<size_t>(len), f);
    std::fclose(f);
    if (got != static_cast<size_t>(len)) { vlog("spv short read '%s'.", path.c_str()); return VK_NULL_HANDLE; }
    VkShaderModuleCreateInfo smci{};
    smci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = static_cast<size_t>(len);
    smci.pCode    = code.data();
    VkShaderModule mod = VK_NULL_HANDLE;
    VkResult r = vkCreateShaderModule(device, &smci, nullptr, &mod);
    if (r != VK_SUCCESS) { vlog("vkCreateShaderModule('%s') failed (%d).", path.c_str(), (int)r); return VK_NULL_HANDLE; }
    return mod;
}

// std140 layout -- IDENTICAL to the shaders/vulkan/hzb_reduce.frag HzbParams
// UBO. No mat4 -> no row/column-major question (LM1 does not bite here).
struct HzbLevelParams {
    float srcTexel[2]; // @0: 1.0 / source-level dimensions
    int   reduce;      // @8: 0 = seed copy, 1 = 2x2 MIN reduction
    int   _pad0;       // @12 (std140 rounds the block to 16B)
};
static_assert(sizeof(HzbLevelParams) == 16, "HzbLevelParams std140 offsets drifted");

const VkFormat kDepthFmt = VK_FORMAT_D32_SFLOAT;  // bridged scene depth (matches fog/ssao)
const VkFormat kLevelFmt = VK_FORMAT_R32_SFLOAT;  // pyramid level (matches GL GL_R32F exactly)

} // namespace

// Persistent Vulkan HZB island state. Sized lazily to (w, h, mipCount) on first
// init; if the pyramid resizes we tear down + rebuild.
struct gosPostProcess::VulkanHzbIsland {
    bool disabled = false;   // init failed once -> never retry, always GL path
    bool inited   = false;

    int fullW = 0, fullH = 0;   // level-0 dims (== hzbW_ x hzbH_ == scene dims)
    int mipCount = 0;

    VkInstance       instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE; // validation-only
    VkPhysicalDevice phys     = VK_NULL_HANDLE;
    VkDevice         device   = VK_NULL_HANDLE;
    uint32_t         gfxFamily = UINT32_MAX;
    VkQueue          queue    = VK_NULL_HANDLE;
    VmaAllocator     allocator = VK_NULL_HANDLE;

    // Island-OWNED images: bridged depth + one R32F image PER pyramid level
    // (ceil ladder -- see TU header for why a mipped image cannot be used).
    VkImage       depthImage  = VK_NULL_HANDLE;  VmaAllocation depthAlloc  = VK_NULL_HANDLE;
    VkImageView   depthView   = VK_NULL_HANDLE;
    VkSampler     sampler     = VK_NULL_HANDLE;  // NEAREST + CLAMP_TO_EDGE (matches GL levels + depth)

    struct Level {
        int           w = 0, h = 0;
        VkImage       image = VK_NULL_HANDLE;  VmaAllocation alloc = VK_NULL_HANDLE;
        VkImageView   view  = VK_NULL_HANDLE;
        VkFramebuffer fb    = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;  // sampler(depth or level k-1) + UBO k
        VkBuffer      ubo   = VK_NULL_HANDLE;  VmaAllocation uboAlloc = VK_NULL_HANDLE;
        VkDeviceSize  readbackOffset = 0;      // into the shared readback buffer
    };
    std::vector<Level> levels;

    // Host-visible staging: depth-in + one shared readback for ALL levels.
    VkBuffer      depthStaging  = VK_NULL_HANDLE; VmaAllocation depthStagingAlloc  = VK_NULL_HANDLE;
    VkBuffer      readback      = VK_NULL_HANDLE; VmaAllocation readbackAlloc      = VK_NULL_HANDLE;
    VkDeviceSize  readbackBytes = 0;

    // ONE render pass + ONE pipeline (dynamic viewport/scissor) shared by all
    // level draws; the per-level B13 ordering lives in the render pass's
    // external subpass dependency.
    VkRenderPass      rpass   = VK_NULL_HANDLE;  // R32F, DONT_CARE->STORE, final=SHADER_READ_ONLY
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;  // binding0 sampler, binding1 UBO
    VkDescriptorPool  pool    = VK_NULL_HANDLE;
    VkPipelineLayout  playout = VK_NULL_HANDLE;
    VkPipeline        pipe    = VK_NULL_HANDLE;
    VkShaderModule    vert    = VK_NULL_HANDLE;
    VkShaderModule    frag    = VK_NULL_HANDLE;

    VkCommandPool     cpool  = VK_NULL_HANDLE;
    VkCommandBuffer   cbuf   = VK_NULL_HANDLE;
    VkFence           fence  = VK_NULL_HANDLE;

    // CPU scratch for the GL<->staging bridge.
    std::vector<float> depthScratch;   // fullW*fullH floats
};

namespace {

// Tear down everything the island owns (safe on partially-built state).
void destroy_island(gosPostProcess::VulkanHzbIsland* s) {
    if (!s) return;
    VkDevice d = s->device;
    if (d != VK_NULL_HANDLE) vkDeviceWaitIdle(d);
    if (s->fence)  vkDestroyFence(d, s->fence, nullptr);
    if (s->cpool)  vkDestroyCommandPool(d, s->cpool, nullptr);
    if (s->pipe)    vkDestroyPipeline(d, s->pipe, nullptr);
    if (s->playout) vkDestroyPipelineLayout(d, s->playout, nullptr);
    if (s->pool)    vkDestroyDescriptorPool(d, s->pool, nullptr);
    if (s->dsl)     vkDestroyDescriptorSetLayout(d, s->dsl, nullptr);
    if (s->frag)    vkDestroyShaderModule(d, s->frag, nullptr);
    if (s->vert)    vkDestroyShaderModule(d, s->vert, nullptr);
    for (auto& lv : s->levels) {
        if (lv.fb)   vkDestroyFramebuffer(d, lv.fb, nullptr);
        if (lv.view) vkDestroyImageView(d, lv.view, nullptr);
    }
    if (s->rpass)   vkDestroyRenderPass(d, s->rpass, nullptr);
    if (s->sampler) vkDestroySampler(d, s->sampler, nullptr);
    if (s->depthView) vkDestroyImageView(d, s->depthView, nullptr);
    if (s->allocator) {
        for (auto& lv : s->levels) {
            if (lv.ubo)   vmaDestroyBuffer(s->allocator, lv.ubo, lv.uboAlloc);
            if (lv.image) vmaDestroyImage(s->allocator, lv.image, lv.alloc);
        }
        if (s->readback)     vmaDestroyBuffer(s->allocator, s->readback, s->readbackAlloc);
        if (s->depthStaging) vmaDestroyBuffer(s->allocator, s->depthStaging, s->depthStagingAlloc);
        if (s->depthImage)   vmaDestroyImage(s->allocator, s->depthImage, s->depthAlloc);
        vmaDestroyAllocator(s->allocator);
    }
    if (d != VK_NULL_HANDLE) vkDestroyDevice(d, nullptr);
    if (s->debugMessenger && s->instance) {
        auto destroyFn = (PFN_vkDestroyDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(s->instance, "vkDestroyDebugUtilsMessengerEXT");
        if (destroyFn) destroyFn(s->instance, s->debugMessenger, nullptr);
    }
    if (s->instance) vkDestroyInstance(s->instance, nullptr);
    gosPostProcess::VulkanHzbIsland fresh;
    fresh.disabled = s->disabled;
    *s = fresh;
}

// Build the whole persistent Vulkan HZB island sized to (fullW x fullH,
// mipCount levels). Returns false (fail-soft) on any error, leaving *s
// torn-down-but-not-disabled.
bool build_island(gosPostProcess::VulkanHzbIsland* s, int fullW, int fullH, int mipCount) {
    // FORCE-FALLBACK debug hook: MC2_VULKAN_HZB_ISLAND_FORCE_FALLBACK=<reason>
    // makes init fail immediately with the chosen fallback_reason (prefixed
    // "forced_") so the GL fail-soft path can be proven without uninstalling
    // the Vulkan runtime. Default build (env unset) is unaffected.
    {
        const char* fforce = std::getenv("MC2_VULKAN_HZB_ISLAND_FORCE_FALLBACK");
        if (fforce && fforce[0]) {
            std::string reason = "forced_";
            reason += fforce;
            vlog("FORCE-FALLBACK hook active (MC2_VULKAN_HZB_ISLAND_FORCE_FALLBACK=%s) "
                 "-- init forced to fail, using GL HZB.", fforce);
            setFallback(reason.c_str());
            return false;
        }
    }

    if (!ensure_volk_initialized()) return false;

    s->fullW = fullW; s->fullH = fullH; s->mipCount = mipCount;
    VkResult r;

    // ---- Optional validation layer + debug-utils messenger (MC2_VULKAN_VALIDATION) ----
    std::vector<const char*> layers;
    std::vector<const char*> instExts;
    std::vector<VkValidationFeatureEnableEXT> valFeatures;
    const char* valPreset = "off";
    bool wantValidation = resolve_validation_preset(valFeatures, valPreset);
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
            vlog("validation ON: layer %s enabled (preset=%s, +%zu feature(s)).",
                 want, valPreset, valFeatures.size());
        } else {
            wantValidation = false;
            vlog("validation requested but %s unavailable; continuing WITHOUT validation.", want);
        }
    }

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "MC2 (Vulkan HZB island)";
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
    r = vkCreateInstance(&ici, nullptr, &s->instance);
    if (r != VK_SUCCESS) { vlog("vkCreateInstance failed (%d).", (int)r); return false; }
    volkLoadInstance(s->instance);

    if (wantValidation) {
        auto pfnCreate = (PFN_vkCreateDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(s->instance, "vkCreateDebugUtilsMessengerEXT");
        if (pfnCreate) {
            VkDebugUtilsMessengerCreateInfoEXT mci{};
            mci.sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            mci.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            mci.pfnUserCallback = hzb_debug_cb;
            VkResult mr = pfnCreate(s->instance, &mci, nullptr, &s->debugMessenger);
            if (mr == VK_SUCCESS) vlog("validation messenger engaged (preset=%s).", valPreset);
            else vlog("vkCreateDebugUtilsMessengerEXT failed (%d); no error capture.", (int)mr);
        } else {
            vlog("vkCreateDebugUtilsMessengerEXT not found; no error capture.");
        }
    }

    // ---- Physical device + graphics queue family ----
    uint32_t devCount = 0;
    r = vkEnumeratePhysicalDevices(s->instance, &devCount, nullptr);
    if (r != VK_SUCCESS || devCount == 0) { vlog("no physical devices (%d,%u).", (int)r, devCount); return false; }
    std::vector<VkPhysicalDevice> devs(devCount);
    vkEnumeratePhysicalDevices(s->instance, &devCount, devs.data());
    s->phys = devs[0];

    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(s->phys, &props);
        std::strncpy(g_health.deviceName, props.deviceName, sizeof(g_health.deviceName) - 1);
        g_health.deviceName[sizeof(g_health.deviceName) - 1] = '\0';
    }

    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(s->phys, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qfCount);
    if (qfCount) vkGetPhysicalDeviceQueueFamilyProperties(s->phys, &qfCount, qfs.data());
    for (uint32_t q = 0; q < qfCount; ++q) {
        if (qfs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) { s->gfxFamily = q; break; }
    }
    if (s->gfxFamily == UINT32_MAX) { vlog("no graphics queue family."); return false; }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = s->gfxFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    r = vkCreateDevice(s->phys, &dci, nullptr, &s->device);
    if (r != VK_SUCCESS) { vlog("vkCreateDevice failed (%d).", (int)r); return false; }
    volkLoadDevice(s->device);
    vkGetDeviceQueue(s->device, s->gfxFamily, 0, &s->queue);

    {
        VmaAllocatorCreateInfo aci{};
        aci.physicalDevice = s->phys;
        aci.device = s->device;
        aci.instance = s->instance;
        aci.vulkanApiVersion = VK_API_VERSION_1_1;
        VmaVulkanFunctions fns{};
        r = vmaImportVulkanFunctionsFromVolk(&aci, &fns);
        if (r != VK_SUCCESS) { vlog("vmaImportVulkanFunctionsFromVolk failed (%d).", (int)r); return false; }
        aci.pVulkanFunctions = &fns;
        r = vmaCreateAllocator(&aci, &s->allocator);
        if (r != VK_SUCCESS) { vlog("vmaCreateAllocator failed (%d).", (int)r); s->allocator = VK_NULL_HANDLE; return false; }
    }

    auto make_image = [&](VkFormat fmt, VkImageUsageFlags usage, int w, int h,
                          VkImage* img, VmaAllocation* alloc) -> bool {
        VkImageCreateInfo ii{};
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = fmt;
        ii.extent = {(uint32_t)w, (uint32_t)h, 1};
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = usage;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        VkResult rr = vmaCreateImage(s->allocator, &ii, &aci, img, alloc, nullptr);
        if (rr != VK_SUCCESS) { vlog("vmaCreateImage failed (%d).", (int)rr); return false; }
        return true;
    };
    auto make_view = [&](VkImage img, VkFormat fmt, VkImageAspectFlags aspect, VkImageView* view) -> bool {
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = fmt;
        vi.subresourceRange = {aspect, 0, 1, 0, 1};
        VkResult rr = vkCreateImageView(s->device, &vi, nullptr, view);
        if (rr != VK_SUCCESS) { vlog("vkCreateImageView failed (%d).", (int)rr); return false; }
        return true;
    };
    auto make_buffer = [&](VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer* buf, VmaAllocation* alloc) -> bool {
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = size;
        bci.usage = usage;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
        VkResult rr = vmaCreateBuffer(s->allocator, &bci, &aci, buf, alloc, nullptr);
        if (rr != VK_SUCCESS) { vlog("vmaCreateBuffer failed (%d).", (int)rr); return false; }
        return true;
    };

    // ---- Bridged depth (sampled by the level-0 seed pass) ----
    if (!make_image(kDepthFmt, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    fullW, fullH, &s->depthImage, &s->depthAlloc)) return false;
    if (!make_view(s->depthImage, kDepthFmt, VK_IMAGE_ASPECT_DEPTH_BIT, &s->depthView)) return false;

    // ---- Sampler: NEAREST + CLAMP_TO_EDGE. Matches the GL side exactly (the
    // pyramid level textures AND sceneDepthTex_ are NEAREST/CLAMP; the clamped
    // 2x2 taps depend on CLAMP_TO_EDGE for odd-extent conservatism). ----
    {
        VkSamplerCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = VK_FILTER_NEAREST;
        si.minFilter = VK_FILTER_NEAREST;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        r = vkCreateSampler(s->device, &si, nullptr, &s->sampler);
        if (r != VK_SUCCESS) { vlog("vkCreateSampler failed (%d).", (int)r); return false; }
    }

    // ---- Render pass (ONE, shared by all level draws): R32F color, DONT_CARE
    // -> STORE (the fullscreen triangle overwrites every texel; GL does not
    // clear either), finalLayout SHADER_READ_ONLY. The EXTERNAL dependency
    // (COLOR_ATTACHMENT_OUTPUT/WRITE -> FRAGMENT_SHADER/READ) is the B13
    // per-mip barrier: level k's write ordered before level k+1's sampled read
    // via the implicit final transition (same mechanism as the H1 fix). ----
    {
        VkAttachmentDescription att{};
        att.format = kLevelFmt;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &ref;
        VkSubpassDependency dep{};
        dep.srcSubpass = 0;
        dep.dstSubpass = VK_SUBPASS_EXTERNAL;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dep.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        VkRenderPassCreateInfo rpci{};
        rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpci.attachmentCount = 1;
        rpci.pAttachments = &att;
        rpci.subpassCount = 1;
        rpci.pSubpasses = &sub;
        rpci.dependencyCount = 1;
        rpci.pDependencies = &dep;
        r = vkCreateRenderPass(s->device, &rpci, nullptr, &s->rpass);
        if (r != VK_SUCCESS) { vlog("vkCreateRenderPass failed (%d).", (int)r); return false; }
    }

    // ---- Per-level images/views/framebuffers/UBOs (ceil ladder, mirrors GL) ----
    {
        s->levels.resize((size_t)mipCount);
        int lw = fullW, lh = fullH;
        VkDeviceSize offset = 0;
        for (int level = 0; level < mipCount; ++level) {
            auto& lv = s->levels[(size_t)level];
            lv.w = lw; lv.h = lh;
            if (!make_image(kLevelFmt,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            lw, lh, &lv.image, &lv.alloc)) return false;
            if (!make_view(lv.image, kLevelFmt, VK_IMAGE_ASPECT_COLOR_BIT, &lv.view)) return false;
            VkFramebufferCreateInfo fci{};
            fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fci.renderPass = s->rpass;
            fci.attachmentCount = 1;
            fci.pAttachments = &lv.view;
            fci.width = (uint32_t)lw; fci.height = (uint32_t)lh; fci.layers = 1;
            r = vkCreateFramebuffer(s->device, &fci, nullptr, &lv.fb);
            if (r != VK_SUCCESS) { vlog("vkCreateFramebuffer(level %d) failed (%d).", level, (int)r); return false; }
            if (!make_buffer(sizeof(HzbLevelParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                             &lv.ubo, &lv.uboAlloc)) return false;
            lv.readbackOffset = offset;
            offset += (VkDeviceSize)lw * lh * 4;   // R32F = 4 bytes/texel
            // advance destination size to the next (smaller) ceil level.
            lw = (lw + 1) / 2; if (lw < 1) lw = 1;
            lh = (lh + 1) / 2; if (lh < 1) lh = 1;
        }
        s->readbackBytes = offset;
        if (!make_buffer(s->readbackBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         &s->readback, &s->readbackAlloc)) return false;

        // Upload the per-level params ONCE (static while the island lives):
        // uSrcTexel = 1/source-level dims -- level 0's source is the full-res
        // depth (== level-0 dims); level k's source is level k-1. uReduce 0/1.
        // Values match the GL runHzbReduce() uploads exactly.
        for (int level = 0; level < mipCount; ++level) {
            auto& lv = s->levels[(size_t)level];
            HzbLevelParams params{};
            if (level == 0) {
                params.srcTexel[0] = 1.0f / (float)fullW;
                params.srcTexel[1] = 1.0f / (float)fullH;
                params.reduce = 0;
            } else {
                const auto& prev = s->levels[(size_t)level - 1];
                params.srcTexel[0] = 1.0f / (float)prev.w;
                params.srcTexel[1] = 1.0f / (float)prev.h;
                params.reduce = 1;
            }
            void* p = nullptr;
            if (vmaMapMemory(s->allocator, lv.uboAlloc, &p) == VK_SUCCESS) {
                std::memcpy(p, &params, sizeof(params));
                vmaUnmapMemory(s->allocator, lv.uboAlloc);
            } else {
                vlog("vmaMapMemory(ubo level %d) failed.", level);
                return false;
            }
        }
    }

    const VkDeviceSize depthBytes = (VkDeviceSize)fullW * fullH * 4;   // R32 float
    if (!make_buffer(depthBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     &s->depthStaging, &s->depthStagingAlloc)) return false;

    // ---- Descriptor set layout + pool + per-level sets ----
    {
        // §5a family convention: binding 0 = the pass's ONE sampler (GL unit 0),
        // binding 1 = params UBO (N samplers -> binding N = 1).
        VkDescriptorSetLayoutBinding binds[2]{};
        binds[0].binding = 0; binds[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binds[0].descriptorCount = 1; binds[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        binds[1].binding = 1; binds[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        binds[1].descriptorCount = 1; binds[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = 2; lci.pBindings = binds;
        r = vkCreateDescriptorSetLayout(s->device, &lci, nullptr, &s->dsl);
        if (r != VK_SUCCESS) { vlog("vkCreateDescriptorSetLayout failed (%d).", (int)r); return false; }

        VkDescriptorPoolSize sizes[2]{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sizes[0].descriptorCount = (uint32_t)mipCount;
        sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;         sizes[1].descriptorCount = (uint32_t)mipCount;
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets = (uint32_t)mipCount;
        pci.poolSizeCount = 2;
        pci.pPoolSizes = sizes;
        r = vkCreateDescriptorPool(s->device, &pci, nullptr, &s->pool);
        if (r != VK_SUCCESS) { vlog("vkCreateDescriptorPool failed (%d).", (int)r); return false; }

        for (int level = 0; level < mipCount; ++level) {
            auto& lv = s->levels[(size_t)level];
            VkDescriptorSetAllocateInfo dsai{};
            dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            dsai.descriptorPool = s->pool;
            dsai.descriptorSetCount = 1;
            dsai.pSetLayouts = &s->dsl;
            r = vkAllocateDescriptorSets(s->device, &dsai, &lv.set);
            if (r != VK_SUCCESS) { vlog("vkAllocateDescriptorSets(level %d) failed (%d).", level, (int)r); return false; }

            VkDescriptorImageInfo srcInfo{};
            srcInfo.sampler = s->sampler;
            srcInfo.imageView = (level == 0) ? s->depthView : s->levels[(size_t)level - 1].view;
            srcInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkDescriptorBufferInfo uboInfo{ lv.ubo, 0, VK_WHOLE_SIZE };
            VkWriteDescriptorSet writes[2]{};
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = lv.set; writes[0].dstBinding = 0; writes[0].descriptorCount = 1;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[0].pImageInfo = &srcInfo;
            writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet = lv.set; writes[1].dstBinding = 1; writes[1].descriptorCount = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; writes[1].pBufferInfo = &uboInfo;
            vkUpdateDescriptorSets(s->device, 2, writes, 0, nullptr);
        }
    }

    // ---- Pipeline (ONE, dynamic viewport/scissor; fullscreen tri; no blend) ----
    {
        s->vert = load_spv(s->device, spv_dir() + "hzb_reduce.vert.spv");
        s->frag = load_spv(s->device, spv_dir() + "hzb_reduce.frag.spv");
        if (s->vert == VK_NULL_HANDLE || s->frag == VK_NULL_HANDLE) {
            setFallback("spv_load_failed"); return false;
        }

        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1; plci.pSetLayouts = &s->dsl;
        r = vkCreatePipelineLayout(s->device, &plci, nullptr, &s->playout);
        if (r != VK_SUCCESS) { vlog("vkCreatePipelineLayout failed (%d).", (int)r); return false; }

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = s->vert; stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = s->frag; stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vin{};
        vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vps{};
        vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vps.viewportCount = 1;   // dynamic
        vps.scissorCount  = 1;   // dynamic
        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState cba{};
        cba.blendEnable = VK_FALSE;   // overwrite, matches GL (no blend in HZB pass)
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1; cb.pAttachments = &cba;
        VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dsci{};
        dsci.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dsci.dynamicStateCount = 2;
        dsci.pDynamicStates = dyn;

        VkGraphicsPipelineCreateInfo gp{};
        gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gp.stageCount = 2; gp.pStages = stages;
        gp.pVertexInputState = &vin;
        gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vps;
        gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms;
        gp.pColorBlendState = &cb;
        gp.pDynamicState = &dsci;
        gp.layout = s->playout;
        gp.renderPass = s->rpass;
        gp.subpass = 0;
        r = vkCreateGraphicsPipelines(s->device, VK_NULL_HANDLE, 1, &gp, nullptr, &s->pipe);
        if (r != VK_SUCCESS) { vlog("vkCreateGraphicsPipelines failed (%d).", (int)r); return false; }
    }

    {
        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = s->gfxFamily;
        r = vkCreateCommandPool(s->device, &pci, nullptr, &s->cpool);
        if (r != VK_SUCCESS) { vlog("vkCreateCommandPool failed (%d).", (int)r); return false; }
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = s->cpool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        r = vkAllocateCommandBuffers(s->device, &cai, &s->cbuf);
        if (r != VK_SUCCESS) { vlog("vkAllocateCommandBuffers failed (%d).", (int)r); return false; }
        VkFenceCreateInfo fnci{};
        fnci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        r = vkCreateFence(s->device, &fnci, nullptr, &s->fence);
        if (r != VK_SUCCESS) { vlog("vkCreateFence failed (%d).", (int)r); return false; }
    }

    s->depthScratch.resize((size_t)fullW * fullH);
    s->inited = true;
    setFallback("");
    g_health.levels = (unsigned long)mipCount;
    vlog("island initialized: %dx%d, %d ceil-ladder levels (depth D32_SFLOAT, "
         "levels R32_SFLOAT SEPARATE images -- Vulkan floor-mips cannot represent "
         "the ceil ladder; per-level B13 barrier via render-pass external dependency).",
         fullW, fullH, mipCount);
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// hzbRegionVulkanReady(): lazy device init, size-match rebuild, fail-soft
// self-disable, NO env-gate check (backend=vk selection IS the enable signal;
// mirrors ssaoRegionVulkanReady() exactly).
// ---------------------------------------------------------------------------
bool gosPostProcess::hzbRegionVulkanReady()
{
    if (hzbW_ <= 0 || hzbH_ <= 0 || hzbMipCount_ < 1 || sceneDepthTex_ == 0)
        return false;

    if (!vkHzbIsland_) vkHzbIsland_ = new VulkanHzbIsland();
    VulkanHzbIsland* s = vkHzbIsland_;
    if (s->disabled) return false;

    if (!s->inited || s->fullW != hzbW_ || s->fullH != hzbH_ ||
        s->mipCount != hzbMipCount_) {
        if (s->inited) destroy_island(s);   // resize -> rebuild
        if (!build_island(s, hzbW_, hzbH_, hzbMipCount_)) {
            vlog("island init FAILED -- disabling, falling back to GL HZB "
                 "for the rest of the run.");
            if (g_health.fallbackReason[0] == '\0') setFallback("device_init_failed");
            destroy_island(s);
            s->disabled = true;
            return false;
        }
    }
    return s->inited && !s->disabled;
}

// ---------------------------------------------------------------------------
// runHzbVulkan(): per-frame GL->Vulkan->GL pyramid build (N level draws).
// Assumes hzbRegionVulkanReady() returned true.
// ---------------------------------------------------------------------------
void gosPostProcess::runHzbVulkan()
{
    VulkanHzbIsland* s = vkHzbIsland_;
    if (!s || !s->inited || s->disabled) return;

    const int fullW = s->fullW, fullH = s->fullH;
    const int mips = s->mipCount;

    ++g_health.attempted;
    using clk = std::chrono::steady_clock;
    auto usSince = [](clk::time_point t0) {
        return std::chrono::duration<double, std::micro>(clk::now() - t0).count();
    };
    const clk::time_point tTotal0 = clk::now();

    // ---- 1) Read GL scene depth into staging (same bridge as fog/ssao) ----
    const clk::time_point tDepth0 = clk::now();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sceneDepthTex_);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, s->depthScratch.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    g_health.copyDepthUs = usSince(tDepth0);

    {
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, s->depthStagingAlloc, &p) == VK_SUCCESS) {
            std::memcpy(p, s->depthScratch.data(), (size_t)fullW * fullH * 4);
            vmaUnmapMemory(s->allocator, s->depthStagingAlloc);
        }
    }

    // ---- 2) Record + submit: depth upload, N level draws, N level readbacks ----
    const clk::time_point tBuild0 = clk::now();
    vkResetCommandBuffer(s->cbuf, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(s->cbuf, &bi) != VK_SUCCESS) return;

    auto barrier = [&](VkImage img, VkImageAspectFlags aspect, VkImageLayout oldL, VkImageLayout newL,
                       VkAccessFlags srcA, VkAccessFlags dstA, VkPipelineStageFlags srcS, VkPipelineStageFlags dstS) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = oldL; b.newLayout = newL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img;
        b.subresourceRange = {aspect, 0, 1, 0, 1};
        b.srcAccessMask = srcA; b.dstAccessMask = dstA;
        vkCmdPipelineBarrier(s->cbuf, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
    };

    // depth: UNDEFINED -> TRANSFER_DST, copy, -> SHADER_READ_ONLY (sampled by seed pass)
    VkBufferImageCopy depthRegion{};
    depthRegion.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    depthRegion.imageExtent = {(uint32_t)fullW, (uint32_t)fullH, 1};
    barrier(s->depthImage, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdCopyBufferToImage(s->cbuf, s->depthStaging, s->depthImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &depthRegion);
    barrier(s->depthImage, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    // ---- N level draws. Each render pass instance's EXTERNAL dependency is
    // the B13 per-mip barrier ordering this level's write before the next
    // level's sampled read (implicit final transition to SHADER_READ_ONLY). ----
    for (int level = 0; level < mips; ++level) {
        auto& lv = s->levels[(size_t)level];
        VkRenderPassBeginInfo rbi{};
        rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rbi.renderPass = s->rpass;
        rbi.framebuffer = lv.fb;
        rbi.renderArea = {{0, 0}, {(uint32_t)lv.w, (uint32_t)lv.h}};
        rbi.clearValueCount = 0;   // DONT_CARE load; fullscreen tri overwrites all
        vkCmdBeginRenderPass(s->cbuf, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(s->cbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipe);
        VkViewport vp{0.0f, 0.0f, (float)lv.w, (float)lv.h, 0.0f, 1.0f};
        VkRect2D sc{{0, 0}, {(uint32_t)lv.w, (uint32_t)lv.h}};
        vkCmdSetViewport(s->cbuf, 0, 1, &vp);
        vkCmdSetScissor(s->cbuf, 0, 1, &sc);
        vkCmdBindDescriptorSets(s->cbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout, 0, 1, &lv.set, 0, nullptr);
        vkCmdDraw(s->cbuf, 3, 1, 0, 0);
        vkCmdEndRenderPass(s->cbuf);
    }

    // ---- Readback: each level SHADER_READ_ONLY -> TRANSFER_SRC (chained off
    // the render pass' external dep -> no R-A-W; H1 discipline), then copy into
    // the shared readback buffer at the level's offset. ----
    for (int level = 0; level < mips; ++level) {
        auto& lv = s->levels[(size_t)level];
        barrier(lv.image, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy creg{};
        creg.bufferOffset = lv.readbackOffset;
        creg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        creg.imageExtent = {(uint32_t)lv.w, (uint32_t)lv.h, 1};
        vkCmdCopyImageToBuffer(s->cbuf, lv.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               s->readback, 1, &creg);
    }

    if (vkEndCommandBuffer(s->cbuf) != VK_SUCCESS) return;

    vkResetFences(s->device, 1, &s->fence);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s->cbuf;
    if (vkQueueSubmit(s->queue, 1, &si, s->fence) != VK_SUCCESS) return;
    if (vkWaitForFences(s->device, 1, &s->fence, VK_TRUE, 5ull * 1000 * 1000 * 1000) != VK_SUCCESS) {
        vlog("fence wait failed/timeout -- disabling island, GL fallback next frame.");
        setFallback("fence_timeout");
        s->disabled = true;
        return;
    }
    g_health.buildUs = usSince(tBuild0);

    // ---- 3) Write every level back into the GL pyramid textures, so the GL
    // probe (MC2_HZB_PROBE) and any future consumer see the Vulkan-built data. ----
    const clk::time_point tOut0 = clk::now();
    {
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, s->readbackAlloc, &p) == VK_SUCCESS) {
            const unsigned char* base = static_cast<const unsigned char*>(p);
            for (int level = 0; level < mips; ++level) {
                auto& lv = s->levels[(size_t)level];
                glBindTexture(GL_TEXTURE_2D, hzbLevelTex_[level]);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, lv.w, lv.h,
                                GL_RED, GL_FLOAT, base + lv.readbackOffset);
            }
            glBindTexture(GL_TEXTURE_2D, 0);
            vmaUnmapMemory(s->allocator, s->readbackAlloc);
        }
    }
    glActiveTexture(GL_TEXTURE0);
    g_health.readbackOutUs = usSince(tOut0);

    // Success: N Vulkan draws built the whole pyramid. Mirror GL's success
    // accounting (hzbBuildCount_ gates the probe and the periodic log).
    ++hzbBuildCount_;
    if (hzbBuildCount_ <= 3 || (hzbBuildCount_ % 600) == 0)
        vlog("built pyramid %dx%d mips=%d via Vulkan (count=%llu)",
             fullW, fullH, mips, (unsigned long long)hzbBuildCount_);
    g_health.vkDraws = (unsigned long)mips;
    ++g_health.usedVulkan;
    g_health.totalUs = usSince(tTotal0);
}

// ---------------------------------------------------------------------------
// destroyVulkanHzbIsland(): called from gosPostProcess::destroy().
// ---------------------------------------------------------------------------
void gosPostProcess::destroyVulkanHzbIsland()
{
    vlog("[VK_HZB_ISLAND_HEALTH] vulkan_available=%d build_enabled=%d device_name=\"%s\" "
         "validation_errors=%lu attempted=%lu used_vulkan=%lu fallback_reason=\"%s\" "
         "vk_draws=%lu levels=%lu copy_depth_us=%.1f build_us=%.1f readback_out_us=%.1f total_us=%.1f",
         g_health.vulkanAvailable, g_health.buildEnabled, g_health.deviceName,
         g_health.validationErrors, g_health.attempted, g_health.usedVulkan, g_health.fallbackReason,
         g_health.vkDraws, g_health.levels, g_health.copyDepthUs, g_health.buildUs,
         g_health.readbackOutUs, g_health.totalUs);

    if (!vkHzbIsland_) return;
    destroy_island(vkHzbIsland_);
    delete vkHzbIsland_;
    vkHzbIsland_ = nullptr;
}

#endif // MC2_VULKAN_ISLAND
