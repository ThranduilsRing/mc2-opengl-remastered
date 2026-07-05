// VULKAN-OOB-FOG-ISLAND-1 -- the SECOND offscreen Vulkan render island in the MC2
// frame loop, proving the VULKAN-ISLAND-TEMPLATE-1 checklist repeats cleanly.
// Structurally IDENTICAL to vulkan_edge_fog_island.cpp (the proven reference); only
// the shader (fog_oob) + its simpler uniform set differ. Entire TU compiles to
// nothing unless MC2_VULKAN_ISLAND is defined (CMake option, requires MC2_VULKAN).
// Fail-soft everywhere: any Vulkan error disables the island for the rest of the
// process and the caller falls back to the GL runFogOob() path. Never crashes --
// this is the OpenGL-user-without-a-Vulkan-runtime path too.
//
// What it does (per frame, only when MC2_VULKAN_OOB_FOG_ISLAND=1 + init OK):
//   1. glGetTexImage the scene DEPTH  (GL_DEPTH_COMPONENT/GL_FLOAT)  -> VMA staging
//   2. glGetTexImage the scene COLOR  (GL_RGBA/GL_HALF_FLOAT)        -> VMA staging
//   3. vkCmdCopyBufferToImage depth staging -> D32_SFLOAT depth image (sampled)
//   4. vkCmdCopyBufferToImage color staging -> R16G16B16A16_SFLOAT color image
//   5. upload the FogOobParams UBO (invViewProj + fogColor + fogOpacity + time)
//   6. render-pass LOAD the color image, draw a fullscreen triangle with AlphaBlend
//      (SRC_ALPHA / ONE_MINUS_SRC_ALPHA, depth test/write OFF, cull none) -- exactly
//      the GL OOB-fog blend; the frag samples the depth image + UBO.
//   7. vkCmdCopyImageToBuffer color image -> readback buffer, fence wait
//   8. glTexSubImage2D the result back into sceneColorTex_
//
// CPU readback is the VALIDATION ORACLE, not a shipping design (see the template
// §5). A one-shot DEPTH-CONVENTION MICRO-CHECK logs the unprojected world-Z of a
// known NDC point via the same invViewProj math the shader uses, next to the GL
// (row-major) reference, so a human can eyeball whether the Vulkan unprojection
// matches GL (catches inverted-Z / row-vs-col-major bugs).

#include "gos_postprocess.h"

#ifdef MC2_VULKAN_ISLAND

// volk owns Vulkan dispatch (dynamic load; no hard link to vulkan-1.dll).
#include <volk.h>
#include "vk_mem_alloc.h"

// GL for the glGetTexImage / glTexSubImage2D bridge + the sampleable GL textures.
#include "utils/gl_utils.h"

// Fixed-timestep clock so the animated FBM time matches the GL path under smoke.
#include "gos_smoke.h"
#include <SDL2/SDL.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <vector>

// VULKAN-OOB-FOG-ISLAND-1: process-lifetime health/diagnostic snapshot for the OOB-
// fog Vulkan island. A single file-static instance is written by the island and read
// (via the extern "C" getter below) by the debug-state dump writer, so the health
// report survives island teardown. All fields default to the "compiled but never
// ran" state. Named distinctly from the EdgeFog island's g_health so both islands
// can be compiled into the same build without symbol collision.
namespace {
struct OobIslandHealth {
    int  vulkanAvailable        = 0;   // volkInitialize() succeeded
    int  islandBuildEnabled     = 1;   // this TU compiled (always 1 here)
    int  islandRuntimeGate      = 0;   // MC2_VULKAN_OOB_FOG_ISLAND=1
    char deviceName[256]        = {0}; // VkPhysicalDeviceProperties.deviceName
    unsigned long validationErrors = 0;   // ERROR-severity validation msgs
    unsigned long oobFogAttempted  = 0;    // frames the island path was entered
    unsigned long oobFogUsedVulkan = 0;    // frames actually composited via Vulkan
    char fallbackReason[64]     = {0}; // "" when healthy; else why GL fallback happened
    double readbackUs           = 0.0; // last-frame image->buffer readback + map (us)
    double copyDepthUs          = 0.0; // last-frame GL getTexImage -> staging copy (us)
    double renderUs             = 0.0; // last-frame submit+fence-wait render (us)
};
OobIslandHealth g_oobHealth;

void setFallback(const char* reason) {
    std::strncpy(g_oobHealth.fallbackReason, reason ? reason : "",
                 sizeof(g_oobHealth.fallbackReason) - 1);
    g_oobHealth.fallbackReason[sizeof(g_oobHealth.fallbackReason) - 1] = '\0';
}
} // namespace

// Read-only accessor consumed by GameOS/gameos/debug_state_dump.cpp (under the same
// #ifdef MC2_VULKAN_ISLAND). Copies the POD out; no Vulkan headers leak.
extern "C" void mc2_vulkan_oob_island_health(
    int* vulkanAvailable, int* islandBuildEnabled, int* islandRuntimeGate,
    const char** deviceName, unsigned long* validationErrors,
    unsigned long* oobFogAttempted, unsigned long* oobFogUsedVulkan,
    const char** fallbackReason,
    double* readbackUs, double* copyDepthUs, double* renderUs) {
    if (vulkanAvailable)    *vulkanAvailable    = g_oobHealth.vulkanAvailable;
    if (islandBuildEnabled) *islandBuildEnabled  = g_oobHealth.islandBuildEnabled;
    if (islandRuntimeGate)  *islandRuntimeGate   = g_oobHealth.islandRuntimeGate;
    if (deviceName)         *deviceName          = g_oobHealth.deviceName;
    if (validationErrors)   *validationErrors    = g_oobHealth.validationErrors;
    if (oobFogAttempted)    *oobFogAttempted     = g_oobHealth.oobFogAttempted;
    if (oobFogUsedVulkan)   *oobFogUsedVulkan     = g_oobHealth.oobFogUsedVulkan;
    if (fallbackReason)     *fallbackReason      = g_oobHealth.fallbackReason;
    if (readbackUs)         *readbackUs          = g_oobHealth.readbackUs;
    if (copyDepthUs)        *copyDepthUs         = g_oobHealth.copyDepthUs;
    if (renderUs)           *renderUs            = g_oobHealth.renderUs;
}

namespace {

void vlog(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "[VK_OOB_FOG] ");
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
    va_end(ap);
}

// VULKAN-ISLAND-VALIDATION-WIRING-1 (mirrors the EdgeFog island). Opt-in Vulkan
// validation for the island's own instance. ERROR-severity messages increment
// g_oobHealth.validationErrors; WARNING is logged but not counted. Fail-soft: if
// the layer / VK_EXT_debug_utils is unavailable the island runs WITHOUT validation.
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

VKAPI_ATTR VkBool32 VKAPI_CALL island_debug_cb(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT* pData,
    void* /*user*/) {
    const char* msg = (pData && pData->pMessage) ? pData->pMessage : "(no message)";
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++g_oobHealth.validationErrors;
        std::fprintf(stderr, "[VK_ISLAND_VALIDATION] ERROR: %s\n", msg);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        std::fprintf(stderr, "[VK_ISLAND_VALIDATION] warning: %s\n", msg);
    }
    return VK_FALSE;
}

// volk once. Note: the EdgeFog island TU also has an ensure_volk_initialized(); both
// live in anonymous namespaces (internal linkage) so there is no ODR collision, and
// volkInitialize() is idempotent across both islands.
bool ensure_volk_initialized() {
    static int state = 0; // 0 untried, 1 ok, -1 failed
    if (state == 1) return true;
    if (state == -1) return false;
    VkResult r = volkInitialize();
    if (r != VK_SUCCESS) {
        vlog("volkInitialize() failed (VkResult=%d) -- no Vulkan runtime. "
             "Falling back to GL OOB fog.", (int)r);
        state = -1;
        g_oobHealth.vulkanAvailable = 0;
        setFallback("no_vulkan_runtime");
        return false;
    }
    state = 1;
    g_oobHealth.vulkanAvailable = 1;
    return true;
}

// Row-major mat4 (the 16 floats the GL path effectively uses) times a vec4.
// out[r] = sum_c m[r*4 + c] * v[c]. Used only by the micro-check.
struct Vec4 { double x, y, z, w; };
Vec4 mul_rowmajor(const float* m, const Vec4& v) {
    Vec4 o;
    o.x = (double)m[0]  * v.x + (double)m[1]  * v.y + (double)m[2]  * v.z + (double)m[3]  * v.w;
    o.y = (double)m[4]  * v.x + (double)m[5]  * v.y + (double)m[6]  * v.z + (double)m[7]  * v.w;
    o.z = (double)m[8]  * v.x + (double)m[9]  * v.y + (double)m[10] * v.z + (double)m[11] * v.w;
    o.w = (double)m[12] * v.x + (double)m[13] * v.y + (double)m[14] * v.z + (double)m[15] * v.w;
    return o;
}
// Column-major reading (the pre-row_major bug), logged for contrast.
Vec4 mul_colmajor(const float* m, const Vec4& v) {
    Vec4 o;
    o.x = (double)m[0]  * v.x + (double)m[4]  * v.y + (double)m[8]  * v.z + (double)m[12] * v.w;
    o.y = (double)m[1]  * v.x + (double)m[5]  * v.y + (double)m[9]  * v.z + (double)m[13] * v.w;
    o.z = (double)m[2]  * v.x + (double)m[6]  * v.y + (double)m[10] * v.z + (double)m[14] * v.w;
    o.w = (double)m[3]  * v.x + (double)m[7]  * v.y + (double)m[11] * v.z + (double)m[15] * v.w;
    return o;
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

// std140 layout matching shaders/vulkan/fog_oob.frag FogOobParams. Total 84B.
// Field offsets: invViewProj @0, u_fogColor @64, u_fogOpacity @76, u_time @80.
// (In std140 a float following a vec3 packs into the vec3's trailing slot, so
// u_fogOpacity sits at @76 -- no explicit pad needed.)
struct FogOobParams {
    float invViewProj[16]; // @0  (same 16 floats the GL path uses, row-major)
    float fogColor[3];     // @64
    float fogOpacity;      // @76
    float time;            // @80
};
static_assert(sizeof(FogOobParams) == 84, "FogOobParams std140 offsets drifted");

const VkFormat kDepthFmt = VK_FORMAT_D32_SFLOAT;
const VkFormat kColorFmt = VK_FORMAT_R16G16B16A16_SFLOAT;

} // namespace

// Persistent Vulkan island state. Sized lazily to (width_, height_) on first init;
// if the framebuffer resizes we tear down + rebuild.
struct gosPostProcess::VulkanOobFogIsland {
    bool disabled = false;
    bool inited   = false;
    bool microCheckDone = false;

    int width  = 0;
    int height = 0;

    VkInstance       instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
    VkPhysicalDevice phys     = VK_NULL_HANDLE;
    VkDevice         device   = VK_NULL_HANDLE;
    uint32_t         gfxFamily = UINT32_MAX;
    VkQueue          queue    = VK_NULL_HANDLE;
    VmaAllocator     allocator = VK_NULL_HANDLE;

    VkImage       depthImage = VK_NULL_HANDLE;  VmaAllocation depthAlloc = VK_NULL_HANDLE;
    VkImageView   depthView   = VK_NULL_HANDLE;
    VkImage       colorImage = VK_NULL_HANDLE;  VmaAllocation colorAlloc = VK_NULL_HANDLE;
    VkImageView   colorView   = VK_NULL_HANDLE;
    VkSampler     sampler     = VK_NULL_HANDLE;

    VkBuffer      depthStaging = VK_NULL_HANDLE; VmaAllocation depthStagingAlloc = VK_NULL_HANDLE;
    VkBuffer      colorStaging = VK_NULL_HANDLE; VmaAllocation colorStagingAlloc = VK_NULL_HANDLE;
    VkBuffer      colorReadback = VK_NULL_HANDLE; VmaAllocation colorReadbackAlloc = VK_NULL_HANDLE;
    VkBuffer      ubo          = VK_NULL_HANDLE;  VmaAllocation uboAlloc = VK_NULL_HANDLE;

    VkRenderPass      rpass  = VK_NULL_HANDLE;
    VkFramebuffer     fb     = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkDescriptorPool  pool   = VK_NULL_HANDLE;
    VkDescriptorSet   set    = VK_NULL_HANDLE;
    VkPipelineLayout  playout = VK_NULL_HANDLE;
    VkPipeline        pipe   = VK_NULL_HANDLE;
    VkShaderModule    vert   = VK_NULL_HANDLE;
    VkShaderModule    frag   = VK_NULL_HANDLE;

    VkCommandPool     cpool  = VK_NULL_HANDLE;
    VkCommandBuffer   cbuf   = VK_NULL_HANDLE;
    VkFence           fence  = VK_NULL_HANDLE;

    std::vector<float>    depthScratch;
    std::vector<uint16_t> colorScratch;
};

namespace {

void destroy_island(gosPostProcess::VulkanOobFogIsland* s) {
    if (!s) return;
    VkDevice d = s->device;
    if (d != VK_NULL_HANDLE) vkDeviceWaitIdle(d);
    if (s->fence)  vkDestroyFence(d, s->fence, nullptr);
    if (s->cpool)  vkDestroyCommandPool(d, s->cpool, nullptr);
    if (s->pipe)   vkDestroyPipeline(d, s->pipe, nullptr);
    if (s->playout) vkDestroyPipelineLayout(d, s->playout, nullptr);
    if (s->pool)   vkDestroyDescriptorPool(d, s->pool, nullptr);
    if (s->dsl)    vkDestroyDescriptorSetLayout(d, s->dsl, nullptr);
    if (s->frag)   vkDestroyShaderModule(d, s->frag, nullptr);
    if (s->vert)   vkDestroyShaderModule(d, s->vert, nullptr);
    if (s->fb)     vkDestroyFramebuffer(d, s->fb, nullptr);
    if (s->rpass)  vkDestroyRenderPass(d, s->rpass, nullptr);
    if (s->sampler) vkDestroySampler(d, s->sampler, nullptr);
    if (s->colorView) vkDestroyImageView(d, s->colorView, nullptr);
    if (s->depthView) vkDestroyImageView(d, s->depthView, nullptr);
    if (s->allocator) {
        if (s->ubo)           vmaDestroyBuffer(s->allocator, s->ubo, s->uboAlloc);
        if (s->colorReadback) vmaDestroyBuffer(s->allocator, s->colorReadback, s->colorReadbackAlloc);
        if (s->colorStaging)  vmaDestroyBuffer(s->allocator, s->colorStaging, s->colorStagingAlloc);
        if (s->depthStaging)  vmaDestroyBuffer(s->allocator, s->depthStaging, s->depthStagingAlloc);
        if (s->colorImage)    vmaDestroyImage(s->allocator, s->colorImage, s->colorAlloc);
        if (s->depthImage)    vmaDestroyImage(s->allocator, s->depthImage, s->depthAlloc);
        vmaDestroyAllocator(s->allocator);
    }
    if (d != VK_NULL_HANDLE) vkDestroyDevice(d, nullptr);
    if (s->debugMessenger && s->instance) {
        auto destroyFn = (PFN_vkDestroyDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(s->instance, "vkDestroyDebugUtilsMessengerEXT");
        if (destroyFn) destroyFn(s->instance, s->debugMessenger, nullptr);
    }
    if (s->instance) vkDestroyInstance(s->instance, nullptr);
    gosPostProcess::VulkanOobFogIsland fresh;
    fresh.disabled       = s->disabled;
    fresh.microCheckDone = s->microCheckDone;
    *s = fresh;
}

bool build_island(gosPostProcess::VulkanOobFogIsland* s, int W, int H) {
    // VULKAN-ISLAND-FALLBACK-PROOF-1: deterministic FORCE-FALLBACK debug hook.
    // MC2_VULKAN_OOB_FOG_ISLAND_FORCE_FALLBACK=<reason> makes ensure-init fail
    // immediately with the chosen fallback_reason (prefixed "forced_"), so the GL
    // fail-soft path can be proven without uninstalling the Vulkan runtime.
    {
        const char* fforce = std::getenv("MC2_VULKAN_OOB_FOG_ISLAND_FORCE_FALLBACK");
        if (fforce && fforce[0]) {
            std::string reason = "forced_";
            reason += fforce;
            vlog("FORCE-FALLBACK hook active (MC2_VULKAN_OOB_FOG_ISLAND_FORCE_FALLBACK=%s) "
                 "-- init forced to fail, using GL OOB fog.", fforce);
            setFallback(reason.c_str());
            return false;
        }
    }

    if (!ensure_volk_initialized()) return false;

    s->width = W; s->height = H;
    VkResult r;

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
    app.pApplicationName = "MC2 (Vulkan OOB-fog island)";
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
            mci.pfnUserCallback = island_debug_cb;
            VkResult mr = pfnCreate(s->instance, &mci, nullptr, &s->debugMessenger);
            if (mr == VK_SUCCESS) vlog("validation messenger engaged (preset=%s).", valPreset);
            else vlog("vkCreateDebugUtilsMessengerEXT failed (%d); no error capture.", (int)mr);
        } else {
            vlog("vkCreateDebugUtilsMessengerEXT not found; no error capture.");
        }
    }

    uint32_t devCount = 0;
    r = vkEnumeratePhysicalDevices(s->instance, &devCount, nullptr);
    if (r != VK_SUCCESS || devCount == 0) { vlog("no physical devices (%d,%u).", (int)r, devCount); return false; }
    std::vector<VkPhysicalDevice> devs(devCount);
    vkEnumeratePhysicalDevices(s->instance, &devCount, devs.data());
    s->phys = devs[0];

    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(s->phys, &props);
        std::strncpy(g_oobHealth.deviceName, props.deviceName, sizeof(g_oobHealth.deviceName) - 1);
        g_oobHealth.deviceName[sizeof(g_oobHealth.deviceName) - 1] = '\0';
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

    auto make_image = [&](VkFormat fmt, VkImageUsageFlags usage, VkImage* img, VmaAllocation* alloc) -> bool {
        VkImageCreateInfo ii{};
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = fmt;
        ii.extent = {(uint32_t)W, (uint32_t)H, 1};
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
    if (!make_image(kDepthFmt, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    &s->depthImage, &s->depthAlloc)) return false;
    if (!make_image(kColorFmt, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &s->colorImage, &s->colorAlloc)) return false;

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
    if (!make_view(s->depthImage, kDepthFmt, VK_IMAGE_ASPECT_DEPTH_BIT, &s->depthView)) return false;
    if (!make_view(s->colorImage, kColorFmt, VK_IMAGE_ASPECT_COLOR_BIT, &s->colorView)) return false;

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
    const VkDeviceSize depthBytes = (VkDeviceSize)W * H * 4;      // R32 float
    const VkDeviceSize colorBytes = (VkDeviceSize)W * H * 4 * 2;  // RGBA16F
    if (!make_buffer(depthBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &s->depthStaging, &s->depthStagingAlloc)) return false;
    if (!make_buffer(colorBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &s->colorStaging, &s->colorStagingAlloc)) return false;
    if (!make_buffer(colorBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &s->colorReadback, &s->colorReadbackAlloc)) return false;
    if (!make_buffer(sizeof(FogOobParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &s->ubo, &s->uboAlloc)) return false;

    {
        VkAttachmentDescription att{};
        att.format = kColorFmt;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.finalLayout   = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &ref;
        // External dependency: order the render pass' implicit final layout transition
        // (COLOR_ATTACHMENT_OPTIMAL -> TRANSFER_SRC, a WRITE performed at vkCmdEndRenderPass)
        // before the following vkCmdCopyImageToBuffer transfer read. Without this the
        // transition-write vs transfer-read is a sync-validation READ_AFTER_WRITE hazard.
        VkSubpassDependency dep{};
        dep.srcSubpass = 0;
        dep.dstSubpass = VK_SUBPASS_EXTERNAL;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dep.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
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

        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = s->rpass;
        fci.attachmentCount = 1;
        fci.pAttachments = &s->colorView;
        fci.width = W; fci.height = H; fci.layers = 1;
        r = vkCreateFramebuffer(s->device, &fci, nullptr, &s->fb);
        if (r != VK_SUCCESS) { vlog("vkCreateFramebuffer failed (%d).", (int)r); return false; }
    }

    {
        VkDescriptorSetLayoutBinding binds[2]{};
        binds[0].binding = 0;
        binds[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binds[0].descriptorCount = 1;
        binds[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        binds[1].binding = 1;
        binds[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        binds[1].descriptorCount = 1;
        binds[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = 2;
        lci.pBindings = binds;
        r = vkCreateDescriptorSetLayout(s->device, &lci, nullptr, &s->dsl);
        if (r != VK_SUCCESS) { vlog("vkCreateDescriptorSetLayout failed (%d).", (int)r); return false; }

        VkDescriptorPoolSize sizes[2]{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sizes[0].descriptorCount = 1;
        sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;         sizes[1].descriptorCount = 1;
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets = 1;
        pci.poolSizeCount = 2;
        pci.pPoolSizes = sizes;
        r = vkCreateDescriptorPool(s->device, &pci, nullptr, &s->pool);
        if (r != VK_SUCCESS) { vlog("vkCreateDescriptorPool failed (%d).", (int)r); return false; }

        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = s->pool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &s->dsl;
        r = vkAllocateDescriptorSets(s->device, &dsai, &s->set);
        if (r != VK_SUCCESS) { vlog("vkAllocateDescriptorSets failed (%d).", (int)r); return false; }

        VkDescriptorImageInfo imgInfo{};
        imgInfo.sampler = s->sampler;
        imgInfo.imageView = s->depthView;
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorBufferInfo uboInfo{ s->ubo, 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = s->set; writes[0].dstBinding = 0; writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &imgInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = s->set; writes[1].dstBinding = 1; writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[1].pBufferInfo = &uboInfo;
        vkUpdateDescriptorSets(s->device, 2, writes, 0, nullptr);
    }

    {
        s->vert = load_spv(s->device, spv_dir() + "fog_oob.vert.spv");
        s->frag = load_spv(s->device, spv_dir() + "fog_oob.frag.spv");
        if (s->vert == VK_NULL_HANDLE || s->frag == VK_NULL_HANDLE) { setFallback("spv_load_failed"); return false; }

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
        VkViewport vp{0.0f, 0.0f, (float)W, (float)H, 0.0f, 1.0f};
        VkRect2D sc{{0, 0}, {(uint32_t)W, (uint32_t)H}};
        VkPipelineViewportStateCreateInfo vps{};
        vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &sc;
        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        // AlphaBlend: SRC_ALPHA / ONE_MINUS_SRC_ALPHA (matches the GL OOB-fog blend).
        VkPipelineColorBlendAttachmentState cba{};
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1; cb.pAttachments = &cba;

        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1; plci.pSetLayouts = &s->dsl;
        r = vkCreatePipelineLayout(s->device, &plci, nullptr, &s->playout);
        if (r != VK_SUCCESS) { vlog("vkCreatePipelineLayout failed (%d).", (int)r); return false; }

        VkGraphicsPipelineCreateInfo gp{};
        gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gp.stageCount = 2; gp.pStages = stages;
        gp.pVertexInputState = &vin;
        gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vps;
        gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms;
        gp.pColorBlendState = &cb;
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

    s->depthScratch.resize((size_t)W * H);
    s->colorScratch.resize((size_t)W * H * 4);
    s->inited = true;
    setFallback("");
    vlog("island initialized: %dx%d device ready (depth D32_SFLOAT, color RGBA16F).", W, H);
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// vulkanOobFogIslandEnabled(): env gate + lazy init.
// ---------------------------------------------------------------------------
bool gosPostProcess::vulkanOobFogIslandEnabled()
{
    static int envState = -2; // -2 untested
    if (envState == -2) {
        const char* e = std::getenv("MC2_VULKAN_OOB_FOG_ISLAND");
        envState = (e && (e[0] == '1')) ? 1 : 0;
        g_oobHealth.islandRuntimeGate = envState;
        if (envState == 1) vlog("MC2_VULKAN_OOB_FOG_ISLAND=1 -- island gate ON (lazy init on first OOB fog).");
    }
    if (envState != 1) return false;

    if (width_ <= 0 || height_ <= 0 || sceneColorTex_ == 0 || sceneDepthTex_ == 0) return false;

    if (!vkOobFogIsland_) vkOobFogIsland_ = new VulkanOobFogIsland();
    VulkanOobFogIsland* s = vkOobFogIsland_;
    if (s->disabled) return false;

    if (!s->inited || s->width != width_ || s->height != height_) {
        if (s->inited) destroy_island(s);
        if (!build_island(s, width_, height_)) {
            vlog("island init FAILED -- disabling, falling back to GL OOB fog for the rest of the run.");
            if (g_oobHealth.fallbackReason[0] == '\0') setFallback("device_init_failed");
            destroy_island(s);
            s->disabled = true;
            return false;
        }
    }
    return s->inited && !s->disabled;
}

// ---------------------------------------------------------------------------
// runFogOobVulkan(): per-frame GL->Vulkan->GL OOB-fog composite.
// ---------------------------------------------------------------------------
void gosPostProcess::runFogOobVulkan()
{
    VulkanOobFogIsland* s = vkOobFogIsland_;
    if (!s || !s->inited || s->disabled) return;

    // Same gameplay gates the GL runFogOob() applies.
    if (!fogOobEnabled_) return;
    if (!sceneHasTerrain_) return;

    const int W = s->width, H = s->height;

    ++g_oobHealth.oobFogAttempted;
    using clk = std::chrono::steady_clock;
    auto usSince = [](clk::time_point t0) {
        return std::chrono::duration<double, std::micro>(clk::now() - t0).count();
    };
    const clk::time_point tCopy0 = clk::now();

    // ---- 1) Read GL scene depth + color into staging (via CPU scratch) ----
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sceneDepthTex_);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, s->depthScratch.data());
    glBindTexture(GL_TEXTURE_2D, sceneColorTex_);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_HALF_FLOAT, s->colorScratch.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    // ---- DEPTH-CONVENTION MICRO-CHECK (one-shot) ----
    // The GL runFogOob() builds invT = transpose(inverseViewProj_) then uploads via
    // setMat4 (GL_TRUE, transposes again) -> the shader receives inverseViewProj_'s
    // 16 floats read row-major. This island packs those SAME 16 floats and qualifies
    // the UBO row_major, so mul_rowmajor(inverseViewProj_) mirrors what the shader
    // computes. GL-Z should ~= VK-Z; the col-major reading (pre-fix bug) is logged
    // for contrast so a regression re-diverges visibly.
    if (!s->microCheckDone) {
        s->microCheckDone = true;
        const float* m = inverseViewProj_;
        Vec4 ndc{0.0, 0.0, 0.5, 1.0};
        Vec4 glW  = mul_rowmajor(m, ndc);
        Vec4 vkW  = mul_rowmajor(m, ndc);
        Vec4 vkWcol = mul_colmajor(m, ndc);
        double glZ = glW.w != 0.0 ? glW.z / glW.w : 0.0;
        double vkZ = vkW.w != 0.0 ? vkW.z / vkW.w : 0.0;
        double vkZcol = vkWcol.w != 0.0 ? vkWcol.z / vkWcol.w : 0.0;
        vlog("MICRO-CHECK depth-convention @ ndc(0,0,0.5): GL-world-Z=%.4f  VK-world-Z=%.4f  "
             "(match=%s)  [pre-fix col-major would be %.4f] -- GL-Z ~= VK-Z means the "
             "invViewProj/Z convention is consistent (row_major UBO).",
             glZ, vkZ, (glZ * vkZ > 0.0 && (glZ==0.0 || (vkZ/glZ > 0.5 && vkZ/glZ < 2.0))) ? "yes" : "NO",
             vkZcol);
    }

    {
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, s->depthStagingAlloc, &p) == VK_SUCCESS) {
            std::memcpy(p, s->depthScratch.data(), (size_t)W * H * 4);
            vmaUnmapMemory(s->allocator, s->depthStagingAlloc);
        }
        if (vmaMapMemory(s->allocator, s->colorStagingAlloc, &p) == VK_SUCCESS) {
            std::memcpy(p, s->colorScratch.data(), (size_t)W * H * 4 * 2);
            vmaUnmapMemory(s->allocator, s->colorStagingAlloc);
        }
    }

    g_oobHealth.copyDepthUs = usSince(tCopy0);

    // ---- 2) Upload the param UBO ----
    // invViewProj: pack the SAME 16 floats the GL path uses (inverseViewProj_,
    // row-major) -- matches the GL runFogOob() net transform (see micro-check).
    // time: same fixed-timestep-aware clock the GL runFogOob() uses so the animated
    // FBM matches bit-for-bit under MC2_SMOKE_FIXED_TIMESTEP.
    {
        FogOobParams params{};
        std::memcpy(params.invViewProj, inverseViewProj_, 16 * sizeof(float));
        params.fogColor[0] = oobFogColor_[0];
        params.fogColor[1] = oobFogColor_[1];
        params.fogColor[2] = oobFogColor_[2];
        params.fogOpacity  = oobFogOpacity_;
        params.time = SmokeMode::fixedTimestepEnabled()
                          ? (float)SmokeMode::fixedClockSeconds()
                          : (float)SDL_GetTicks() / 1000.0f;
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, s->uboAlloc, &p) == VK_SUCCESS) {
            std::memcpy(p, &params, sizeof(params));
            vmaUnmapMemory(s->allocator, s->uboAlloc);
        }
    }

    // ---- 3) Record + submit: upload images, render pass, readback ----
    const clk::time_point tRender0 = clk::now();
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

    VkBufferImageCopy depthRegion{};
    depthRegion.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    depthRegion.imageExtent = {(uint32_t)W, (uint32_t)H, 1};
    VkBufferImageCopy colorRegion{};
    colorRegion.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    colorRegion.imageExtent = {(uint32_t)W, (uint32_t)H, 1};

    barrier(s->depthImage, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdCopyBufferToImage(s->cbuf, s->depthStaging, s->depthImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &depthRegion);
    barrier(s->depthImage, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    barrier(s->colorImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdCopyBufferToImage(s->cbuf, s->colorStaging, s->colorImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &colorRegion);
    barrier(s->colorImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    VkRenderPassBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = s->rpass;
    rbi.framebuffer = s->fb;
    rbi.renderArea = {{0, 0}, {(uint32_t)W, (uint32_t)H}};
    rbi.clearValueCount = 0;
    vkCmdBeginRenderPass(s->cbuf, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(s->cbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipe);
    vkCmdBindDescriptorSets(s->cbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout, 0, 1, &s->set, 0, nullptr);
    vkCmdDraw(s->cbuf, 3, 1, 0, 0);
    vkCmdEndRenderPass(s->cbuf);

    // color image is now TRANSFER_SRC (via the render pass' external subpass dependency,
    // which orders the implicit final layout-transition WRITE before the transfer read).
    vkCmdCopyImageToBuffer(s->cbuf, s->colorImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->colorReadback, 1, &colorRegion);

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
    g_oobHealth.renderUs = usSince(tRender0);

    // ---- 4) Read back the blended color and write it into sceneColorTex_ ----
    const clk::time_point tReadback0 = clk::now();
    {
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, s->colorReadbackAlloc, &p) == VK_SUCCESS) {
            std::memcpy(s->colorScratch.data(), p, (size_t)W * H * 4 * 2);
            vmaUnmapMemory(s->allocator, s->colorReadbackAlloc);
            glBindTexture(GL_TEXTURE_2D, sceneColorTex_);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_HALF_FLOAT, s->colorScratch.data());
            glBindTexture(GL_TEXTURE_2D, 0);
        }
    }
    glActiveTexture(GL_TEXTURE0);

    g_oobHealth.readbackUs = usSince(tReadback0);
    ++g_oobHealth.oobFogUsedVulkan;
}

// ---------------------------------------------------------------------------
// destroyVulkanOobFogIsland(): called from gosPostProcess::destroy().
// ---------------------------------------------------------------------------
void gosPostProcess::destroyVulkanOobFogIsland()
{
    vlog("[VK_ISLAND_HEALTH] vulkan_available=%d island_build_enabled=%d "
         "island_runtime_gate_enabled=%d device_name=\"%s\" validation_errors=%lu "
         "oobfog_attempted=%lu oobfog_used_vulkan=%lu oobfog_fallback_reason=\"%s\" "
         "readback_us=%.1f copy_depth_us=%.1f render_us=%.1f",
         g_oobHealth.vulkanAvailable, g_oobHealth.islandBuildEnabled, g_oobHealth.islandRuntimeGate,
         g_oobHealth.deviceName, g_oobHealth.validationErrors, g_oobHealth.oobFogAttempted,
         g_oobHealth.oobFogUsedVulkan, g_oobHealth.fallbackReason,
         g_oobHealth.readbackUs, g_oobHealth.copyDepthUs, g_oobHealth.renderUs);

    if (!vkOobFogIsland_) return;
    destroy_island(vkOobFogIsland_);
    delete vkOobFogIsland_;
    vkOobFogIsland_ = nullptr;
}

#endif // MC2_VULKAN_ISLAND
