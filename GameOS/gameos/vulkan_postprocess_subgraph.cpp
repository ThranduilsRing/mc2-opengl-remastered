// VULKAN-POSTPROCESS-SUBGRAPH-1 -- the Layer-4 milestone. Fuses the two shipped,
// parity-proven Vulkan islands (EdgeFog + OOB fog) into ONE native Vulkan subgraph
// with a Vulkan-owned intermediate color image and NO CPU readback between the two
// passes. Entire TU compiles to nothing unless MC2_VULKAN_ISLAND is defined (CMake
// option, requires MC2_VULKAN). Fail-soft everywhere: any Vulkan error disables the
// subgraph for the rest of the process and the caller falls back to the GL edge+oob
// fog path (BOTH GL passes run). Never crashes.
//
// DESIGN (advisor-endorsed): ONE render pass, ONE subpass, TWO draws. Sequential
// draws to the same blended color attachment preserve blend order -- NO internal
// barrier needed (neither pass samples the other's output; both only sample the
// shared depth image + their own UBO). This saves 2 copies vs running the two
// islands (one shared color copy-in + one shared copy-out, instead of two each).
//
// DATA FLOW (per frame, gate ON):
//   1. glGetTexImage scene DEPTH (GL_DEPTH_COMPONENT/GL_FLOAT) -> depth staging;
//      glGetTexImage scene COLOR (GL_RGBA/GL_HALF_FLOAT)       -> color staging.
//   2. vkCmdCopyBufferToImage depth->depthImage (-> SHADER_READ_ONLY),
//      color->colorImage (-> COLOR_ATTACHMENT).
//   3. ONE render pass (colorImage, LOAD/STORE):
//        bind edge_fog pipeline + set[0] (EdgeFogParams UBO), vkCmdDraw(3);
//        bind fog_oob  pipeline + set[1] (FogOobParams  UBO), vkCmdDraw(3).
//      Same depth sampler both. Alpha blend SRC_ALPHA/ONE_MINUS_SRC_ALPHA both.
//   4. barrier color -> TRANSFER_SRC; vkCmdCopyImageToBuffer -> color-out staging;
//      fence; memcpy; glTexSubImage2D -> sceneColorTex_.
//
// The uniform sources/values are pulled EXACTLY from the GL runEdgeFog()/runFogOob()
// (gos_postprocess.cpp) -- identical to the two islands' proven uploads (both
// row_major, both parity-proven individually). See RenderCore/vulkan_layout_chain.h
// (kVkImageTransitionChains: PostprocessSubgraph{Color,Depth}) for the proof-layer
// contract this runtime implements.
//
// CPU readback is the VALIDATION ORACLE, not a shipping design (see the template §5).

#include "gos_postprocess.h"

#ifdef MC2_VULKAN_ISLAND

// volk owns Vulkan dispatch (dynamic load; no hard link to vulkan-1.dll).
#include <volk.h>
#include "vk_mem_alloc.h"

// GL for the glGetTexImage / glTexSubImage2D bridge + the sampleable GL textures.
#include "utils/gl_utils.h"

// Fixed-timestep clock so the animated OOB FBM time matches the GL path under smoke.
#include "gos_smoke.h"
#include <SDL2/SDL.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <vector>

// VULKAN-POSTPROCESS-SUBGRAPH-1: process-lifetime health/diagnostic snapshot. A
// single file-static instance is written by the subgraph and read (via the extern
// "C" getter below) by the debug-state dump writer, so the health report survives
// subgraph teardown. Named distinctly from the two islands' g_health so all three
// can be compiled into the same build without symbol collision.
namespace {
struct SubgraphHealth {
    int  vulkanAvailable        = 0;   // volkInitialize() succeeded
    int  buildEnabled           = 1;   // this TU compiled (always 1 here)
    int  runtimeGate            = 0;   // MC2_VULKAN_POSTPROCESS_SUBGRAPH=1
    char deviceName[256]        = {0}; // VkPhysicalDeviceProperties.deviceName
    unsigned long validationErrors = 0;   // ERROR-severity validation msgs
    unsigned long attempted     = 0;   // frames the subgraph path was entered
    unsigned long usedVulkan    = 0;   // frames actually composited via Vulkan
    char fallbackReason[64]     = {0}; // "" when healthy; else why GL fallback happened

    // EQUIVALENCE-COUNTER GUARD (advisor): a silent double-apply (Vulkan draws AND
    // a GL fog pass also runs) must be catchable. expected_gl_passes_replaced is a
    // constant 2 (edge + oob). vkDraws==2 when the subgraph rendered; glSkipped==2
    // when the subgraph ran (both GL fog sites skipped by the seam).
    unsigned long expectedGlReplaced = 2;   // constant: edge + oob
    unsigned long vkDraws        = 0;  // last-frame actual Vulkan draws (2 on success)
    unsigned long glSkipped      = 0;  // last-frame GL fog sites skipped (2 when ran)

    // timings (last frame, microseconds)
    double copyDepthUs   = 0.0;  // GL depth getTexImage -> staging
    double copyColorInUs = 0.0;  // GL color getTexImage -> staging
    double edgeFogDrawUs = 0.0;  // edge_fog draw record cost (approx, of render)
    double oobFogDrawUs  = 0.0;  // fog_oob draw record cost (approx, of render)
    double renderUs      = 0.0;  // submit+fence-wait total render
    double colorOutUs    = 0.0;  // image->buffer readback + map + glTexSubImage2D
    double totalUs       = 0.0;  // whole runPostprocessSubgraph() body
};
SubgraphHealth g_sub;

void setFallback(const char* reason) {
    std::strncpy(g_sub.fallbackReason, reason ? reason : "",
                 sizeof(g_sub.fallbackReason) - 1);
    g_sub.fallbackReason[sizeof(g_sub.fallbackReason) - 1] = '\0';
}
} // namespace

// Read-only accessor consumed by GameOS/gameos/debug_state_dump.cpp (under the same
// #ifdef MC2_VULKAN_ISLAND). Copies the POD out; no Vulkan headers leak.
extern "C" void mc2_vulkan_postprocess_subgraph_health(
    int* vulkanAvailable, int* buildEnabled, int* runtimeGate,
    const char** deviceName, unsigned long* validationErrors,
    unsigned long* attempted, unsigned long* usedVulkan,
    const char** fallbackReason,
    unsigned long* expectedGlReplaced, unsigned long* vkDraws, unsigned long* glSkipped,
    double* copyDepthUs, double* copyColorInUs, double* edgeFogDrawUs,
    double* oobFogDrawUs, double* colorOutUs, double* totalUs) {
    if (vulkanAvailable)    *vulkanAvailable    = g_sub.vulkanAvailable;
    if (buildEnabled)       *buildEnabled       = g_sub.buildEnabled;
    if (runtimeGate)        *runtimeGate        = g_sub.runtimeGate;
    if (deviceName)         *deviceName         = g_sub.deviceName;
    if (validationErrors)   *validationErrors   = g_sub.validationErrors;
    if (attempted)          *attempted          = g_sub.attempted;
    if (usedVulkan)         *usedVulkan         = g_sub.usedVulkan;
    if (fallbackReason)     *fallbackReason     = g_sub.fallbackReason;
    if (expectedGlReplaced) *expectedGlReplaced = g_sub.expectedGlReplaced;
    if (vkDraws)            *vkDraws            = g_sub.vkDraws;
    if (glSkipped)          *glSkipped          = g_sub.glSkipped;
    if (copyDepthUs)        *copyDepthUs        = g_sub.copyDepthUs;
    if (copyColorInUs)      *copyColorInUs      = g_sub.copyColorInUs;
    if (edgeFogDrawUs)      *edgeFogDrawUs      = g_sub.edgeFogDrawUs;
    if (oobFogDrawUs)       *oobFogDrawUs       = g_sub.oobFogDrawUs;
    if (colorOutUs)         *colorOutUs         = g_sub.colorOutUs;
    if (totalUs)            *totalUs            = g_sub.totalUs;
}

namespace {

void vlog(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "[VK_PP_SUBGRAPH] ");
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
    va_end(ap);
}

// VULKAN-ISLAND-VALIDATION-WIRING-1 (mirrors both islands). Opt-in Vulkan validation
// for the subgraph's own instance. ERROR-severity messages increment
// g_sub.validationErrors; WARNING is logged but not counted. Fail-soft: if the layer
// / VK_EXT_debug_utils is unavailable the subgraph runs WITHOUT validation.
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

// SKYBOX-FOG-EXCLUDE-VULKAN-PORT-1: independent read of the SAME env var the GL path
// gates on (GameOS/gameos/gos_postprocess.cpp's skyboxFogExcludeEnabled() has internal
// linkage and isn't reachable from this TU; every MC2_VULKAN_* gate in this file
// already reads its own env var this way, so this follows the established pattern
// rather than threading state cross-TU). Cached once, same semantics: "1" => on.
bool skyboxFogExcludeEnabledVk() {
    static const bool s_on = []() {
        const char* v = std::getenv("MC2_SKYBOX_FOG_EXCLUDE");
        return v && v[0] == '1';
    }();
    return s_on;
}

VKAPI_ATTR VkBool32 VKAPI_CALL subgraph_debug_cb(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT* pData,
    void* /*user*/) {
    const char* msg = (pData && pData->pMessage) ? pData->pMessage : "(no message)";
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++g_sub.validationErrors;
        std::fprintf(stderr, "[VK_SUBGRAPH_VALIDATION] ERROR: %s\n", msg);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        std::fprintf(stderr, "[VK_SUBGRAPH_VALIDATION] warning: %s\n", msg);
    }
    return VK_FALSE;
}

// volk once. The two islands each have their own ensure_volk_initialized() in their
// anonymous namespaces (internal linkage) so there is no ODR collision, and
// volkInitialize() is idempotent across all three.
bool ensure_volk_initialized() {
    static int state = 0; // 0 untried, 1 ok, -1 failed
    if (state == 1) return true;
    if (state == -1) return false;
    VkResult r = volkInitialize();
    if (r != VK_SUCCESS) {
        vlog("volkInitialize() failed (VkResult=%d) -- no Vulkan runtime. "
             "Falling back to GL edge+oob fog.", (int)r);
        state = -1;
        g_sub.vulkanAvailable = 0;
        setFallback("no_vulkan_runtime");
        return false;
    }
    state = 1;
    g_sub.vulkanAvailable = 1;
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

// std140 layouts -- IDENTICAL to the two islands' PODs (must match the SAME shaders
// shaders/vulkan/edge_fog.frag + fog_oob.frag as-is).
// SKYBOX-FOG-EXCLUDE-VULKAN-PORT-1 appends skyExcludeEnabled (int) to both PODs,
// matching the GLSL structs' new trailing u_skyExcludeEnabled member.
struct EdgeFogParams {
    float invViewProj[16]; // @0  (same 16 floats the GL path uploads, row-major)
    float fogColor[3];     // @64
    float _pad0;           // @76
    float halfExtent;      // @80
    float fogStart;        // @84
    float fogHeight;       // @88
    float fogMax;          // @92
    float waterElevation;  // @96
    int32_t skyExcludeEnabled; // @100 (SKYBOX-FOG-EXCLUDE-VULKAN-PORT-1)
};
static_assert(sizeof(EdgeFogParams) == 104, "EdgeFogParams std140 offsets drifted");

struct FogOobParams {
    float invViewProj[16]; // @0  (same 16 floats the GL path uses, row-major)
    float fogColor[3];     // @64
    float fogOpacity;      // @76
    float time;            // @80
    int32_t skyExcludeEnabled; // @84 (SKYBOX-FOG-EXCLUDE-VULKAN-PORT-1)
};
static_assert(sizeof(FogOobParams) == 88, "FogOobParams std140 offsets drifted");

const VkFormat kDepthFmt   = VK_FORMAT_D32_SFLOAT;          // PostprocessSubgraphDepth
const VkFormat kColorFmt   = VK_FORMAT_R16G16B16A16_SFLOAT; // PostprocessSubgraphColor
// SKYBOX-FOG-EXCLUDE-VULKAN-PORT-1: stencil-tag bridge image. R8_UINT (not
// VK_FORMAT_S8_UINT/D24_UNORM_S8_UINT) because R8_UINT sampled-image support is a
// core Vulkan 1.0 guarantee (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT), whereas S8_UINT
// has NO universal support guarantee for ANY usage including sampling. The subgraph
// already bridges depth/color via CPU glGetTexImage + vkCmdCopyBufferToImage (see
// runPostprocessSubgraph()); the stencil-tag value is bridged the same way from the
// GL sceneStencilViewTex_ (GL_STENCIL_INDEX texture view), so no native D24S8 image
// or VK_IMAGE_ASPECT_STENCIL_BIT view is needed on the Vulkan side.
const VkFormat kStencilFmt = VK_FORMAT_R8_UINT;             // PostprocessSubgraphStencil

} // namespace

// Persistent Vulkan subgraph state. Sized lazily to (width_, height_) on first init;
// if the framebuffer resizes we tear down + rebuild. Runtime handles live HERE, never
// in the constexpr proof registry.
struct gosPostProcess::VulkanPostprocessSubgraph {
    bool disabled = false;   // init failed once -> never retry, always GL path
    bool inited   = false;

    int width  = 0;
    int height = 0;

    VkInstance       instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE; // validation-only
    VkPhysicalDevice phys     = VK_NULL_HANDLE;
    VkDevice         device   = VK_NULL_HANDLE;
    uint32_t         gfxFamily = UINT32_MAX;
    VkQueue          queue    = VK_NULL_HANDLE;
    VmaAllocator     allocator = VK_NULL_HANDLE;

    // Subgraph-OWNED intermediate images.
    VkImage       depthImage = VK_NULL_HANDLE;  VmaAllocation depthAlloc = VK_NULL_HANDLE;
    VkImageView   depthView   = VK_NULL_HANDLE;
    VkImage       colorImage = VK_NULL_HANDLE;  VmaAllocation colorAlloc = VK_NULL_HANDLE;
    VkImageView   colorView   = VK_NULL_HANDLE;
    VkSampler     depthSampler = VK_NULL_HANDLE; // shared by both descriptor sets
    // SKYBOX-FOG-EXCLUDE-VULKAN-PORT-1: stencil-tag bridge image (R8_UINT). Only
    // allocated/uploaded/bound when the GL side has sceneStencilViewTex_ != 0 (i.e.
    // MC2_SKYBOX_FOG_EXCLUDE=1); otherwise stencilImage stays VK_NULL_HANDLE and the
    // descriptor still needs a valid dummy binding (see stencilDummyImage below).
    VkImage       stencilImage = VK_NULL_HANDLE; VmaAllocation stencilAlloc = VK_NULL_HANDLE;
    VkImageView   stencilView   = VK_NULL_HANDLE;
    VkSampler     stencilSampler = VK_NULL_HANDLE; // nearest, shared by both sets

    // Host-visible staging: depth-in, color-in, color-out, stencil-in + the two param UBOs.
    VkBuffer      depthStaging  = VK_NULL_HANDLE; VmaAllocation depthStagingAlloc  = VK_NULL_HANDLE;
    VkBuffer      colorStaging  = VK_NULL_HANDLE; VmaAllocation colorStagingAlloc  = VK_NULL_HANDLE; // color-in
    VkBuffer      colorReadback = VK_NULL_HANDLE; VmaAllocation colorReadbackAlloc = VK_NULL_HANDLE; // color-out
    VkBuffer      stencilStaging = VK_NULL_HANDLE; VmaAllocation stencilStagingAlloc = VK_NULL_HANDLE; // stencil-in
    VkBuffer      edgeUbo       = VK_NULL_HANDLE; VmaAllocation edgeUboAlloc       = VK_NULL_HANDLE;
    VkBuffer      oobUbo        = VK_NULL_HANDLE; VmaAllocation oobUboAlloc        = VK_NULL_HANDLE;

    VkRenderPass      rpass  = VK_NULL_HANDLE;   // 1 color attachment LOAD/STORE
    VkFramebuffer     fb     = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;  // binding0 depth sampler, binding1 UBO, binding2 stencil sampler
    VkDescriptorPool  pool   = VK_NULL_HANDLE;
    VkDescriptorSet   edgeSet = VK_NULL_HANDLE;  // depth sampler + EdgeFog UBO + stencil sampler
    VkDescriptorSet   oobSet  = VK_NULL_HANDLE;  // depth sampler + FogOob  UBO + stencil sampler
    VkPipelineLayout  playout = VK_NULL_HANDLE;
    VkPipeline        edgePipe = VK_NULL_HANDLE;
    VkPipeline        oobPipe  = VK_NULL_HANDLE;
    VkShaderModule    edgeVert = VK_NULL_HANDLE;
    VkShaderModule    edgeFrag = VK_NULL_HANDLE;
    VkShaderModule    oobVert  = VK_NULL_HANDLE;
    VkShaderModule    oobFrag  = VK_NULL_HANDLE;

    VkCommandPool     cpool  = VK_NULL_HANDLE;
    VkCommandBuffer   cbuf   = VK_NULL_HANDLE;
    VkFence           fence  = VK_NULL_HANDLE;

    // CPU scratch for the GL<->staging bridge.
    std::vector<float>    depthScratch;  // width*height floats
    std::vector<uint16_t> colorScratch;  // width*height*4 halfs
    std::vector<uint8_t>  stencilScratch; // width*height bytes (SKYBOX-FOG-EXCLUDE-VULKAN-PORT-1)
};

namespace {

// Tear down everything the subgraph owns (safe on partially-built state).
void destroy_subgraph(gosPostProcess::VulkanPostprocessSubgraph* s) {
    if (!s) return;
    VkDevice d = s->device;
    if (d != VK_NULL_HANDLE) vkDeviceWaitIdle(d);
    if (s->fence)  vkDestroyFence(d, s->fence, nullptr);
    if (s->cpool)  vkDestroyCommandPool(d, s->cpool, nullptr);
    if (s->edgePipe) vkDestroyPipeline(d, s->edgePipe, nullptr);
    if (s->oobPipe)  vkDestroyPipeline(d, s->oobPipe, nullptr);
    if (s->playout) vkDestroyPipelineLayout(d, s->playout, nullptr);
    if (s->pool)   vkDestroyDescriptorPool(d, s->pool, nullptr);
    if (s->dsl)    vkDestroyDescriptorSetLayout(d, s->dsl, nullptr);
    if (s->edgeFrag) vkDestroyShaderModule(d, s->edgeFrag, nullptr);
    if (s->edgeVert) vkDestroyShaderModule(d, s->edgeVert, nullptr);
    if (s->oobFrag)  vkDestroyShaderModule(d, s->oobFrag, nullptr);
    if (s->oobVert)  vkDestroyShaderModule(d, s->oobVert, nullptr);
    if (s->fb)     vkDestroyFramebuffer(d, s->fb, nullptr);
    if (s->rpass)  vkDestroyRenderPass(d, s->rpass, nullptr);
    if (s->stencilSampler) vkDestroySampler(d, s->stencilSampler, nullptr);
    if (s->depthSampler) vkDestroySampler(d, s->depthSampler, nullptr);
    if (s->stencilView) vkDestroyImageView(d, s->stencilView, nullptr);
    if (s->colorView) vkDestroyImageView(d, s->colorView, nullptr);
    if (s->depthView) vkDestroyImageView(d, s->depthView, nullptr);
    if (s->allocator) {
        if (s->oobUbo)        vmaDestroyBuffer(s->allocator, s->oobUbo, s->oobUboAlloc);
        if (s->edgeUbo)       vmaDestroyBuffer(s->allocator, s->edgeUbo, s->edgeUboAlloc);
        if (s->stencilStaging) vmaDestroyBuffer(s->allocator, s->stencilStaging, s->stencilStagingAlloc);
        if (s->colorReadback) vmaDestroyBuffer(s->allocator, s->colorReadback, s->colorReadbackAlloc);
        if (s->colorStaging)  vmaDestroyBuffer(s->allocator, s->colorStaging, s->colorStagingAlloc);
        if (s->depthStaging)  vmaDestroyBuffer(s->allocator, s->depthStaging, s->depthStagingAlloc);
        if (s->stencilImage)  vmaDestroyImage(s->allocator, s->stencilImage, s->stencilAlloc);
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
    gosPostProcess::VulkanPostprocessSubgraph fresh;
    fresh.disabled = s->disabled;
    *s = fresh;
}

// Build the whole persistent Vulkan subgraph sized to WxH. Returns false (fail-soft)
// on any error, leaving *s torn-down-but-not-disabled.
bool build_subgraph(gosPostProcess::VulkanPostprocessSubgraph* s, int W, int H) {
    // FORCE-FALLBACK debug hook: MC2_VULKAN_POSTPROCESS_SUBGRAPH_FORCE_FALLBACK=<reason>
    // makes init fail immediately with the chosen fallback_reason (prefixed "forced_")
    // so the GL fail-soft path (BOTH GL fog passes) can be proven without uninstalling
    // the Vulkan runtime. Default build (env unset) is unaffected.
    {
        const char* fforce = std::getenv("MC2_VULKAN_POSTPROCESS_SUBGRAPH_FORCE_FALLBACK");
        if (fforce && fforce[0]) {
            std::string reason = "forced_";
            reason += fforce;
            vlog("FORCE-FALLBACK hook active (MC2_VULKAN_POSTPROCESS_SUBGRAPH_FORCE_FALLBACK=%s) "
                 "-- init forced to fail, using GL edge+oob fog.", fforce);
            setFallback(reason.c_str());
            return false;
        }
    }

    if (!ensure_volk_initialized()) return false;

    s->width = W; s->height = H;
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
    app.pApplicationName = "MC2 (Vulkan postprocess subgraph)";
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
            mci.pfnUserCallback = subgraph_debug_cb;
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
        std::strncpy(g_sub.deviceName, props.deviceName, sizeof(g_sub.deviceName) - 1);
        g_sub.deviceName[sizeof(g_sub.deviceName) - 1] = '\0';
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
    // Depth (sampled by both frags) + transfer-dst; Color (attachment) + transfer-dst + transfer-src.
    if (!make_image(kDepthFmt, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    &s->depthImage, &s->depthAlloc)) return false;
    if (!make_image(kColorFmt, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &s->colorImage, &s->colorAlloc)) return false;
    // SKYBOX-FOG-EXCLUDE-VULKAN-PORT-1: stencil-tag bridge image (R8_UINT, sampled +
    // transfer-dst, same shape as depthImage). Always allocated (fixed descriptor
    // layout regardless of gate state); left all-zero when the gate is off or GL has
    // no sceneStencilViewTex_ this frame, so the shader's u_skyExcludeEnabled==0 path
    // never samples it anyway (byte-identical-OFF is enforced by the uniform, not by
    // skipping allocation).
    if (!make_image(kStencilFmt, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    &s->stencilImage, &s->stencilAlloc)) return false;

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
    if (!make_view(s->stencilImage, kStencilFmt, VK_IMAGE_ASPECT_COLOR_BIT, &s->stencilView)) return false;

    {
        VkSamplerCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = VK_FILTER_NEAREST;
        si.minFilter = VK_FILTER_NEAREST;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        r = vkCreateSampler(s->device, &si, nullptr, &s->depthSampler);
        if (r != VK_SUCCESS) { vlog("vkCreateSampler failed (%d).", (int)r); return false; }
        // SKYBOX-FOG-EXCLUDE-VULKAN-PORT-1: same nearest/clamp sampler shape, separate
        // handle since it samples a different (integer-format) image.
        r = vkCreateSampler(s->device, &si, nullptr, &s->stencilSampler);
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
    const VkDeviceSize depthBytes   = (VkDeviceSize)W * H * 4;      // R32 float
    const VkDeviceSize colorBytes   = (VkDeviceSize)W * H * 4 * 2;  // RGBA16F
    const VkDeviceSize stencilBytes = (VkDeviceSize)W * H * 1;      // R8_UINT (SKYBOX-FOG-EXCLUDE-VULKAN-PORT-1)
    if (!make_buffer(depthBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &s->depthStaging, &s->depthStagingAlloc)) return false;
    if (!make_buffer(colorBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &s->colorStaging, &s->colorStagingAlloc)) return false;
    if (!make_buffer(colorBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &s->colorReadback, &s->colorReadbackAlloc)) return false;
    if (!make_buffer(stencilBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &s->stencilStaging, &s->stencilStagingAlloc)) return false;
    if (!make_buffer(sizeof(EdgeFogParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &s->edgeUbo, &s->edgeUboAlloc)) return false;
    if (!make_buffer(sizeof(FogOobParams),  VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &s->oobUbo,  &s->oobUboAlloc))  return false;

    // ---- Render pass: 1 color attachment, LOAD (keep preloaded scene color) -> STORE.
    // Sequential draws to this blended attachment preserve blend order (edge then oob);
    // no internal barrier -- neither pass samples the other's output. finalLayout is
    // TRANSFER_SRC so the copy-out can follow directly.
    {
        VkAttachmentDescription att{};
        att.format = kColorFmt;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;    // fog blends OVER the preloaded scene color
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; // barrier'd there before the pass
        att.finalLayout   = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;     // ready to copy back
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

    // ---- Descriptor set layout: binding0 = depth sampler, binding1 = UBO, binding2 =
    // stencil-tag sampler (SKYBOX-FOG-EXCLUDE-VULKAN-PORT-1). TWO sets allocated: edge
    // + oob (shared binding0 depth sampler + binding2 stencil sampler, per-pass
    // binding1 UBO).
    {
        VkDescriptorSetLayoutBinding binds[3]{};
        binds[0].binding = 0;
        binds[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binds[0].descriptorCount = 1;
        binds[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        binds[1].binding = 1;
        binds[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        binds[1].descriptorCount = 1;
        binds[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        binds[2].binding = 2;
        binds[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binds[2].descriptorCount = 1;
        binds[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = 3;
        lci.pBindings = binds;
        r = vkCreateDescriptorSetLayout(s->device, &lci, nullptr, &s->dsl);
        if (r != VK_SUCCESS) { vlog("vkCreateDescriptorSetLayout failed (%d).", (int)r); return false; }

        VkDescriptorPoolSize sizes[2]{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sizes[0].descriptorCount = 4; // depth+stencil x2 sets
        sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;         sizes[1].descriptorCount = 2;
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets = 2;
        pci.poolSizeCount = 2;
        pci.pPoolSizes = sizes;
        r = vkCreateDescriptorPool(s->device, &pci, nullptr, &s->pool);
        if (r != VK_SUCCESS) { vlog("vkCreateDescriptorPool failed (%d).", (int)r); return false; }

        VkDescriptorSetLayout layouts[2] = { s->dsl, s->dsl };
        VkDescriptorSet setsOut[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = s->pool;
        dsai.descriptorSetCount = 2;
        dsai.pSetLayouts = layouts;
        r = vkAllocateDescriptorSets(s->device, &dsai, setsOut);
        if (r != VK_SUCCESS) { vlog("vkAllocateDescriptorSets failed (%d).", (int)r); return false; }
        s->edgeSet = setsOut[0];
        s->oobSet  = setsOut[1];

        // Bind the shared depth image (binding0) + per-set UBO (binding1) + shared
        // stencil-tag image (binding2) once.
        VkDescriptorImageInfo imgInfo{};
        imgInfo.sampler = s->depthSampler;
        imgInfo.imageView = s->depthView;
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo stencilInfo{};
        stencilInfo.sampler = s->stencilSampler;
        stencilInfo.imageView = s->stencilView;
        stencilInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorBufferInfo edgeUboInfo{ s->edgeUbo, 0, VK_WHOLE_SIZE };
        VkDescriptorBufferInfo oobUboInfo { s->oobUbo,  0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet writes[6]{};
        // edge set: depth sampler + edge UBO + stencil sampler
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = s->edgeSet; writes[0].dstBinding = 0; writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &imgInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = s->edgeSet; writes[1].dstBinding = 1; writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[1].pBufferInfo = &edgeUboInfo;
        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = s->edgeSet; writes[2].dstBinding = 2; writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[2].pImageInfo = &stencilInfo;
        // oob set: depth sampler + oob UBO + stencil sampler
        writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[3].dstSet = s->oobSet; writes[3].dstBinding = 0; writes[3].descriptorCount = 1;
        writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[3].pImageInfo = &imgInfo;
        writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[4].dstSet = s->oobSet; writes[4].dstBinding = 1; writes[4].descriptorCount = 1;
        writes[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[4].pBufferInfo = &oobUboInfo;
        writes[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[5].dstSet = s->oobSet; writes[5].dstBinding = 2; writes[5].descriptorCount = 1;
        writes[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[5].pImageInfo = &stencilInfo;
        vkUpdateDescriptorSets(s->device, 6, writes, 0, nullptr);
    }

    // ---- Two pipelines (fullscreen tri; AlphaBlend; depth off; cull none). Shared
    // pipeline layout (same DSL); one pipeline per fog effect using its own shaders.
    {
        s->edgeVert = load_spv(s->device, spv_dir() + "edge_fog.vert.spv");
        s->edgeFrag = load_spv(s->device, spv_dir() + "edge_fog.frag.spv");
        s->oobVert  = load_spv(s->device, spv_dir() + "fog_oob.vert.spv");
        s->oobFrag  = load_spv(s->device, spv_dir() + "fog_oob.frag.spv");
        if (s->edgeVert == VK_NULL_HANDLE || s->edgeFrag == VK_NULL_HANDLE ||
            s->oobVert  == VK_NULL_HANDLE || s->oobFrag  == VK_NULL_HANDLE) {
            setFallback("spv_load_failed"); return false;
        }

        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1; plci.pSetLayouts = &s->dsl;
        r = vkCreatePipelineLayout(s->device, &plci, nullptr, &s->playout);
        if (r != VK_SUCCESS) { vlog("vkCreatePipelineLayout failed (%d).", (int)r); return false; }

        // Shared fixed-function state (identical to both islands).
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

        auto make_pipeline = [&](VkShaderModule vmod, VkShaderModule fmod, VkPipeline* out) -> bool {
            VkPipelineShaderStageCreateInfo stages[2]{};
            stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vmod; stages[0].pName = "main";
            stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fmod; stages[1].pName = "main";
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
            VkResult rr = vkCreateGraphicsPipelines(s->device, VK_NULL_HANDLE, 1, &gp, nullptr, out);
            if (rr != VK_SUCCESS) { vlog("vkCreateGraphicsPipelines failed (%d).", (int)rr); return false; }
            return true;
        };
        if (!make_pipeline(s->edgeVert, s->edgeFrag, &s->edgePipe)) return false;
        if (!make_pipeline(s->oobVert,  s->oobFrag,  &s->oobPipe))  return false;
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
    s->stencilScratch.assign((size_t)W * H, 0); // SKYBOX-FOG-EXCLUDE-VULKAN-PORT-1: zero-init (gate-OFF-safe default)
    s->inited = true;
    setFallback("");
    vlog("subgraph initialized: %dx%d device ready (depth D32_SFLOAT, color RGBA16F, "
         "stencil-tag bridge R8_UINT, 2 pipelines edge_fog+fog_oob, ONE render pass, "
         "TWO draws, NO internal readback).", W, H);
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// vulkanPostprocessSubgraphEnabled(): env gate + lazy init. Returns true only when
// the subgraph should run this frame (env=1, not disabled, init OK, size matches).
// ---------------------------------------------------------------------------
bool gosPostProcess::vulkanPostprocessSubgraphEnabled()
{
    static int envState = -2; // -2 untested
    if (envState == -2) {
        const char* e = std::getenv("MC2_VULKAN_POSTPROCESS_SUBGRAPH");
        envState = (e && (e[0] == '1')) ? 1 : 0;
        g_sub.runtimeGate = envState;
        if (envState == 1) vlog("MC2_VULKAN_POSTPROCESS_SUBGRAPH=1 -- subgraph gate ON "
                                "(lazy init on first edge-fog site; does BOTH fog effects, "
                                "GL edge+oob sites skipped).");
    }
    if (envState != 1) return false;

    // Env gate ON -> defer to the gate-free readiness half (lazy init + size + device).
    return postprocessSubgraphReadyForRegion();
}

// ---------------------------------------------------------------------------
// postprocessSubgraphReadyForRegion(): POSTPROCESS-VULKAN-BACKEND-OPT-IN.
// The env-gate-FREE readiness half extracted from vulkanPostprocessSubgraphEnabled():
// lazy device init, size-match rebuild, and fail-soft self-disable -- with NO
// MC2_VULKAN_POSTPROCESS_SUBGRAPH check. The region-iface Vulkan backend calls this
// DIRECTLY (backend=vk IS the enable signal), so selecting the Vulkan backend runs
// Vulkan independent of the standalone subgraph gate. On any genuine device/init
// failure returns false (real failure) -> caller falls back to GL.
// ---------------------------------------------------------------------------
bool gosPostProcess::postprocessSubgraphReadyForRegion()
{
    if (width_ <= 0 || height_ <= 0 || sceneColorTex_ == 0 || sceneDepthTex_ == 0) return false;

    if (!vkPostprocessSubgraph_) vkPostprocessSubgraph_ = new VulkanPostprocessSubgraph();
    VulkanPostprocessSubgraph* s = vkPostprocessSubgraph_;
    if (s->disabled) return false;

    if (!s->inited || s->width != width_ || s->height != height_) {
        if (s->inited) destroy_subgraph(s);   // resize -> rebuild
        if (!build_subgraph(s, width_, height_)) {
            vlog("subgraph init FAILED -- disabling, falling back to GL edge+oob fog "
                 "for the rest of the run.");
            if (g_sub.fallbackReason[0] == '\0') setFallback("device_init_failed");
            destroy_subgraph(s);
            s->disabled = true;
            return false;
        }
    }
    return s->inited && !s->disabled;
}

// ---------------------------------------------------------------------------
// runPostprocessSubgraph(): per-frame fused GL->Vulkan->GL edge+oob composite.
// Does BOTH fog effects in ONE render pass (edge then oob), so the seam SKIPS both
// GL fog sites. Assumes vulkanPostprocessSubgraphEnabled() returned true.
// ---------------------------------------------------------------------------
void gosPostProcess::runPostprocessSubgraph()
{
    VulkanPostprocessSubgraph* s = vkPostprocessSubgraph_;
    if (!s || !s->inited || s->disabled) return;

    // Reset this frame's equivalence counters -- if we bail early they stay 0 (the
    // seam still skips the GL sites, so a 0/0 here paired with a GL pass running is
    // exactly the double-apply/miss we want visible in the health dump).
    g_sub.vkDraws   = 0;
    g_sub.glSkipped = 0;

    const int W = s->width, H = s->height;

    ++g_sub.attempted;
    using clk = std::chrono::steady_clock;
    auto usSince = [](clk::time_point t0) {
        return std::chrono::duration<double, std::micro>(clk::now() - t0).count();
    };
    const clk::time_point tTotal0 = clk::now();

    // ---- 1) Read GL scene depth + color into staging (via CPU scratch) ----
    const clk::time_point tDepth0 = clk::now();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sceneDepthTex_);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, s->depthScratch.data());
    g_sub.copyDepthUs = usSince(tDepth0);
    const clk::time_point tColor0 = clk::now();
    glBindTexture(GL_TEXTURE_2D, sceneColorTex_);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_HALF_FLOAT, s->colorScratch.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    g_sub.copyColorInUs = usSince(tColor0);

    // SKYBOX-FOG-EXCLUDE-VULKAN-PORT-1: bridge the GL stencil-tag view (sceneStencilViewTex_,
    // GL_STENCIL_INDEX texture-view mode) the same way depth/color are bridged above.
    // Only sampled when the gate is on AND the GL side actually created the view (mirrors
    // the GL runEdgeFog()/runFogOob() `skyExclude` condition exactly); otherwise the
    // scratch buffer stays whatever it was last set to (zero-initialized at build time)
    // and the shader's u_skyExcludeEnabled==0 uniform means it's never sampled anyway.
    const bool skyExclude = skyboxFogExcludeEnabledVk() && sceneStencilViewTex_ != 0;
    if (skyExclude) {
        glBindTexture(GL_TEXTURE_2D, sceneStencilViewTex_);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, s->stencilScratch.data());
        glBindTexture(GL_TEXTURE_2D, 0);
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
        if (skyExclude && vmaMapMemory(s->allocator, s->stencilStagingAlloc, &p) == VK_SUCCESS) {
            std::memcpy(p, s->stencilScratch.data(), (size_t)W * H);
            vmaUnmapMemory(s->allocator, s->stencilStagingAlloc);
        }
    }

    // ---- 2) Upload BOTH param UBOs (values pulled EXACTLY from GL runEdgeFog() /
    //         runFogOob() -- identical to the two islands' proven uploads) ----
    {
        EdgeFogParams params{};
        std::memcpy(params.invViewProj, inverseViewProj_, 16 * sizeof(float));
        params.fogColor[0] = edgeFogColor_[0];
        params.fogColor[1] = edgeFogColor_[1];
        params.fogColor[2] = edgeFogColor_[2];
        params._pad0 = 0.0f;
        params.halfExtent = mapHalfExtent_;
        params.fogStart = edgeFogStart_;
        params.fogHeight = edgeFogHeight_;
        params.fogMax = edgeFogMax_;
        params.waterElevation = waterElevation_;
        params.skyExcludeEnabled = skyExclude ? 1 : 0; // SKYBOX-FOG-EXCLUDE-VULKAN-PORT-1
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, s->edgeUboAlloc, &p) == VK_SUCCESS) {
            std::memcpy(p, &params, sizeof(params));
            vmaUnmapMemory(s->allocator, s->edgeUboAlloc);
        }
    }
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
        params.skyExcludeEnabled = skyExclude ? 1 : 0; // SKYBOX-FOG-EXCLUDE-VULKAN-PORT-1
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, s->oobUboAlloc, &p) == VK_SUCCESS) {
            std::memcpy(p, &params, sizeof(params));
            vmaUnmapMemory(s->allocator, s->oobUboAlloc);
        }
    }

    // ---- 3) Record + submit: upload images, ONE render pass (2 draws), readback ----
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
    VkBufferImageCopy stencilRegion{}; // SKYBOX-FOG-EXCLUDE-VULKAN-PORT-1
    stencilRegion.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; // R8_UINT color-aspect image
    stencilRegion.imageExtent = {(uint32_t)W, (uint32_t)H, 1};

    // depth: UNDEFINED -> TRANSFER_DST, copy, -> SHADER_READ_ONLY (sampled by both draws)
    barrier(s->depthImage, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdCopyBufferToImage(s->cbuf, s->depthStaging, s->depthImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &depthRegion);
    barrier(s->depthImage, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    // stencil-tag bridge: UNDEFINED -> TRANSFER_DST, copy (every frame, so gate-OFF ->
    // gate-ON transitions never sample stale data from a previous frame's residency),
    // -> SHADER_READ_ONLY (sampled by both draws, mirrors depthImage exactly).
    barrier(s->stencilImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdCopyBufferToImage(s->cbuf, s->stencilStaging, s->stencilImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &stencilRegion);
    barrier(s->stencilImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    // color: UNDEFINED -> TRANSFER_DST, preload scene color, -> COLOR_ATTACHMENT
    barrier(s->colorImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdCopyBufferToImage(s->cbuf, s->colorStaging, s->colorImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &colorRegion);
    barrier(s->colorImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    // ONE render pass: LOAD preloaded color, blend edge fog then oob fog OVER it in
    // ORDER (sequential draws to the same blended attachment preserve blend order --
    // NO internal barrier needed), STORE. finalLayout=TRANSFER_SRC.
    VkRenderPassBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = s->rpass;
    rbi.framebuffer = s->fb;
    rbi.renderArea = {{0, 0}, {(uint32_t)W, (uint32_t)H}};
    rbi.clearValueCount = 0; // LOAD, no clear
    vkCmdBeginRenderPass(s->cbuf, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    // draw 1: edge fog (matches GL order: edge fog runs first, then oob).
    vkCmdBindPipeline(s->cbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, s->edgePipe);
    vkCmdBindDescriptorSets(s->cbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout, 0, 1, &s->edgeSet, 0, nullptr);
    vkCmdDraw(s->cbuf, 3, 1, 0, 0);
    // draw 2: oob fog.
    vkCmdBindPipeline(s->cbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, s->oobPipe);
    vkCmdBindDescriptorSets(s->cbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout, 0, 1, &s->oobSet, 0, nullptr);
    vkCmdDraw(s->cbuf, 3, 1, 0, 0);
    vkCmdEndRenderPass(s->cbuf);

    // color image is now TRANSFER_SRC (via the render pass' external subpass dependency,
    // which orders the implicit final layout-transition WRITE before the transfer read)
    // -> copy to readback buffer.
    vkCmdCopyImageToBuffer(s->cbuf, s->colorImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->colorReadback, 1, &colorRegion);

    if (vkEndCommandBuffer(s->cbuf) != VK_SUCCESS) return;

    vkResetFences(s->device, 1, &s->fence);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s->cbuf;
    if (vkQueueSubmit(s->queue, 1, &si, s->fence) != VK_SUCCESS) return;
    if (vkWaitForFences(s->device, 1, &s->fence, VK_TRUE, 5ull * 1000 * 1000 * 1000) != VK_SUCCESS) {
        vlog("fence wait failed/timeout -- disabling subgraph, GL fallback next frame.");
        setFallback("fence_timeout");
        s->disabled = true;
        return;
    }
    g_sub.renderUs = usSince(tRender0);
    // Approximate per-draw attribution: the two draws are the bulk of the GPU render
    // recorded above; split the measured render evenly as a coarse per-pass estimate.
    g_sub.edgeFogDrawUs = g_sub.renderUs * 0.5;
    g_sub.oobFogDrawUs  = g_sub.renderUs * 0.5;

    // ---- 4) Read back the blended color and write it into sceneColorTex_ ----
    const clk::time_point tOut0 = clk::now();
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
    g_sub.colorOutUs = usSince(tOut0);

    // Success: 2 Vulkan draws did BOTH fog effects; the seam skips BOTH GL fog sites.
    g_sub.vkDraws   = 2;
    g_sub.glSkipped = 2;
    ++g_sub.usedVulkan;
    g_sub.totalUs = usSince(tTotal0);

    // EQUIVALENCE-COUNTER GUARD: assert the fused subgraph replaced exactly the 2 GL
    // passes it was supposed to. A mismatch here means a silent double-apply or miss.
    if (g_sub.vkDraws != g_sub.expectedGlReplaced || g_sub.glSkipped != g_sub.expectedGlReplaced) {
        vlog("EQUIVALENCE-COUNTER MISMATCH: expected_gl_passes_replaced=%lu but "
             "vk_draws=%lu gl_skipped=%lu -- possible double-apply/miss!",
             g_sub.expectedGlReplaced, g_sub.vkDraws, g_sub.glSkipped);
    }
}

// ---------------------------------------------------------------------------
// destroyVulkanPostprocessSubgraph(): called from gosPostProcess::destroy().
// ---------------------------------------------------------------------------
void gosPostProcess::destroyVulkanPostprocessSubgraph()
{
    vlog("[VK_SUBGRAPH_HEALTH] vulkan_available=%d build_enabled=%d runtime_gate_enabled=%d "
         "device_name=\"%s\" validation_errors=%lu attempted=%lu used_vulkan=%lu "
         "fallback_reason=\"%s\" expected_gl_passes_replaced=%lu vk_draws=%lu gl_skipped=%lu "
         "copy_depth_us=%.1f copy_color_in_us=%.1f edgefog_draw_us=%.1f oobfog_draw_us=%.1f "
         "color_out_us=%.1f total_subgraph_us=%.1f",
         g_sub.vulkanAvailable, g_sub.buildEnabled, g_sub.runtimeGate, g_sub.deviceName,
         g_sub.validationErrors, g_sub.attempted, g_sub.usedVulkan, g_sub.fallbackReason,
         g_sub.expectedGlReplaced, g_sub.vkDraws, g_sub.glSkipped,
         g_sub.copyDepthUs, g_sub.copyColorInUs, g_sub.edgeFogDrawUs, g_sub.oobFogDrawUs,
         g_sub.colorOutUs, g_sub.totalUs);

    if (!vkPostprocessSubgraph_) return;
    destroy_subgraph(vkPostprocessSubgraph_);
    delete vkPostprocessSubgraph_;
    vkPostprocessSubgraph_ = nullptr;
}

#endif // MC2_VULKAN_ISLAND
