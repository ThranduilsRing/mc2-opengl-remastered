// VK-REGION-SSAO-1 -- the FIRST NEW region grown through IRenderBackend::runRegion
// beyond PostprocessFog (RENDER-BACKEND-REGION-IFACE-1). Proves the SSAO region
// (occlusion pass1 + multiplicative apply pass2) on a headless Vulkan device,
// following the VULKAN-ISLAND-TEMPLATE-1 checklist exactly. Entire TU compiles to
// nothing unless MC2_VULKAN_ISLAND is defined (CMake option, requires MC2_VULKAN).
// Fail-soft everywhere: any Vulkan error disables the island for the rest of the
// process and the caller falls back to the GL SSAO path (runSSAORegionGL()).
// Never crashes.
//
// DESIGN: TWO render passes (unlike the fog subgraph's one-pass/two-draw fusion --
// SSAO's two passes write DIFFERENT targets at DIFFERENT resolutions, so they
// cannot share one render pass/framebuffer):
//   pass1 (occlusion): half-res target (Vulkan-owned SsaoOcclusion intermediate,
//     R8 single-channel), samples full-res depth+normal, NO blend (STORE overwrite).
//   pass2 (apply): full-res scene-color target (LOAD existing color), samples the
//     pass1 output, MULTIPLICATIVE blend (DST_COLOR/ZERO) -- matches the GL
//     BLENDMODE-MULTIPLY-1 PostProcessSsaoApply row.
// An intra-pass barrier (SSAO intra-pass barrier B14, VULKAN-BARRIER-PLAN-RECON-1)
// orders pass1's COLOR_ATTACHMENT_WRITE before pass2's FRAGMENT_SHADER_READ of the
// occlusion image (pass1 writes it, pass2 samples it -- a genuine cross-pass
// read-after-write, distinct from H1's copy-readback hazard).
//
// DATA FLOW (per frame, gate ON):
//   1. glGetTexImage scene DEPTH + NORMAL (GBuffer1) -> depth/normal staging.
//      glGetTexImage scene COLOR (for the pass2 LOAD-preload, same as fog) -> color staging.
//   2. vkCmdCopyBufferToImage depth/normal/color -> their Vulkan images.
//   3. Render pass 1 (half-res occlusion FBO): bind ssao pipeline + set0
//      (depth+normal samplers, SSAOParams UBO), vkCmdDraw(3). STORE.
//   4. Barrier: occlusion image COLOR_ATTACHMENT_OUTPUT/WRITE -> FRAGMENT_SHADER/READ
//      (B14 -- pass2 samples what pass1 just wrote).
//   5. Render pass 2 (full-res color FBO, LOAD preloaded scene color): bind
//      ssao_apply pipeline + set1 (occlusion sampler, SSAOApplyParams UBO),
//      vkCmdDraw(3) with MULTIPLICATIVE blend. STORE. finalLayout=TRANSFER_SRC.
//   6. vkCmdCopyImageToBuffer color image -> readback buffer (render pass' external
//      subpass dependency orders the copy after the implicit layout transition --
//      the SAME H1 fix as fog/oob/subgraph, VULKAN-ISLAND-READBACK-SYNC-BARRIER-1).
//   7. fence; memcpy; glTexSubImage2D -> sceneColorTex_.
//
// The uniform sources/values are pulled EXACTLY from the GL runSSAO() (SSAOParams
// mirrors ssaoProg_'s uploads; SSAOApplyParams mirrors ssaoApplyProg_'s uploads).
//
// CPU readback is the VALIDATION ORACLE, not a shipping design (see the template §5).
//
// SHADER SOURCE: shaders/vulkan/ssao.{vert,frag} + ssao_apply.{vert,frag} are a
// HAND PORT, not a reuse of the baked shaders/spv/ssao.frag.default.*.spv artifact.
// That artifact (SPIRV-POSTPROCESS-FAMILY-1, compiled for the GL glShaderBinary
// consumer) has BOTH sceneDepthTex and sceneNormalTex decorate to (set=0,binding=0)
// -- verified via spirv-dis; the GLSL source has no explicit layout(binding=), so
// glslang free-numbers same-set samplers with no collision check, which is fine for
// GL's reflection-based consumer but is a genuine descriptor-set collision for a
// Vulkan pipeline. It cannot be consumed as-is by vkCreateDescriptorSetLayout. This
// island therefore hand-ports the shader (same discipline as edge_fog/fog_oob) with
// its own explicit set/binding layout, baked to its own ssao.frag.spv under
// MC2_VULKAN via MC2_VK_SHADER_BASENAMES.

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
#include <cmath>
#include <chrono>
#include <string>
#include <vector>

// VK-REGION-SSAO-1: process-lifetime health/diagnostic snapshot. A single
// file-static instance is written by the island and read (via the extern "C"
// getter below) by the debug-state dump writer, so the health report survives
// island teardown. Named distinctly from the fog islands'/subgraph's health so
// all can be compiled into the same build without symbol collision.
namespace {
struct SsaoIslandHealth {
    int  vulkanAvailable        = 0;   // volkInitialize() succeeded
    int  buildEnabled           = 1;   // this TU compiled (always 1 here)
    char deviceName[256]        = {0}; // VkPhysicalDeviceProperties.deviceName
    unsigned long validationErrors = 0;   // ERROR-severity validation msgs
    unsigned long attempted     = 0;   // frames the island path was entered
    unsigned long usedVulkan    = 0;   // frames actually composited via Vulkan
    char fallbackReason[64]     = {0}; // "" when healthy; else why GL fallback happened

    unsigned long vkDraws        = 0;  // last-frame actual Vulkan draws (2 on success)

    // timings (last frame, microseconds)
    double copyDepthUs   = 0.0;
    double copyNormalUs  = 0.0;
    double copyColorInUs = 0.0;
    double occlusionUs   = 0.0;  // pass1 submit+fence-wait
    double applyUs       = 0.0;  // pass2 submit+fence-wait
    double colorOutUs    = 0.0;
    double totalUs       = 0.0;
};
SsaoIslandHealth g_health;

void setFallback(const char* reason) {
    std::strncpy(g_health.fallbackReason, reason ? reason : "",
                 sizeof(g_health.fallbackReason) - 1);
    g_health.fallbackReason[sizeof(g_health.fallbackReason) - 1] = '\0';
}
} // namespace

// Read-only accessor consumed by GameOS/gameos/debug_state_dump.cpp (under the same
// #ifdef MC2_VULKAN_ISLAND). Copies the POD out; no Vulkan headers leak.
extern "C" void mc2_vulkan_ssao_island_health(
    int* vulkanAvailable, int* buildEnabled,
    const char** deviceName, unsigned long* validationErrors,
    unsigned long* attempted, unsigned long* usedVulkan,
    const char** fallbackReason, unsigned long* vkDraws,
    double* copyDepthUs, double* copyNormalUs, double* copyColorInUs,
    double* occlusionUs, double* applyUs, double* colorOutUs, double* totalUs) {
    if (vulkanAvailable)  *vulkanAvailable  = g_health.vulkanAvailable;
    if (buildEnabled)     *buildEnabled     = g_health.buildEnabled;
    if (deviceName)       *deviceName       = g_health.deviceName;
    if (validationErrors) *validationErrors = g_health.validationErrors;
    if (attempted)        *attempted        = g_health.attempted;
    if (usedVulkan)       *usedVulkan       = g_health.usedVulkan;
    if (fallbackReason)   *fallbackReason   = g_health.fallbackReason;
    if (vkDraws)          *vkDraws          = g_health.vkDraws;
    if (copyDepthUs)      *copyDepthUs      = g_health.copyDepthUs;
    if (copyNormalUs)     *copyNormalUs     = g_health.copyNormalUs;
    if (copyColorInUs)    *copyColorInUs    = g_health.copyColorInUs;
    if (occlusionUs)      *occlusionUs      = g_health.occlusionUs;
    if (applyUs)          *applyUs          = g_health.applyUs;
    if (colorOutUs)       *colorOutUs       = g_health.colorOutUs;
    if (totalUs)          *totalUs          = g_health.totalUs;
}

namespace {

void vlog(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "[VK_SSAO_ISLAND] ");
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
    va_end(ap);
}

// VULKAN-ISLAND-VALIDATION-WIRING-1 (mirrors the fog islands + subgraph). Opt-in
// Vulkan validation for the island's own instance. ERROR-severity messages
// increment g_health.validationErrors; WARNING is logged but not counted.
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

VKAPI_ATTR VkBool32 VKAPI_CALL ssao_debug_cb(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT* pData,
    void* /*user*/) {
    const char* msg = (pData && pData->pMessage) ? pData->pMessage : "(no message)";
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++g_health.validationErrors;
        std::fprintf(stderr, "[VK_SSAO_VALIDATION] ERROR: %s\n", msg);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        std::fprintf(stderr, "[VK_SSAO_VALIDATION] warning: %s\n", msg);
    }
    return VK_FALSE;
}

// volk once. Each Vulkan TU (both fog islands, the subgraph, this one) has its
// own ensure_volk_initialized() in its anonymous namespace (internal linkage) so
// there is no ODR collision, and volkInitialize() is idempotent across all.
bool ensure_volk_initialized() {
    static int state = 0; // 0 untried, 1 ok, -1 failed
    if (state == 1) return true;
    if (state == -1) return false;
    VkResult r = volkInitialize();
    if (r != VK_SUCCESS) {
        vlog("volkInitialize() failed (VkResult=%d) -- no Vulkan runtime. "
             "Falling back to GL SSAO.", (int)r);
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

// std140 layouts -- IDENTICAL to the shaders/vulkan/ssao{,_apply}.frag UBOs.
struct SSAOOcclusionParams {
    float invViewProj[16]; // @0   (row-major, matches the GL path's direct upload)
    float viewProj[16];    // @64
    float screenSize[2];   // @128
    float aoRadius;        // @136
    float aoBias;          // @140
    float aoStrength;      // @144
    float aoPower;         // @148
};
static_assert(sizeof(SSAOOcclusionParams) == 152, "SSAOOcclusionParams std140 offsets drifted");

struct SSAOApplyParams {
    float ssaoTexel[2]; // @0
    int   debugMode;    // @8
    int   _pad0;        // @12 (std140 vec2 rounds the block to 16B)
};
static_assert(sizeof(SSAOApplyParams) == 16, "SSAOApplyParams std140 offsets drifted");

const VkFormat kFullResColorFmt = VK_FORMAT_R16G16B16A16_SFLOAT; // scene color (matches fog subgraph)
const VkFormat kDepthFmt        = VK_FORMAT_D32_SFLOAT;          // scene depth
const VkFormat kNormalFmt       = VK_FORMAT_R16G16B16A16_SFLOAT; // GBuffer1 world normal (matches sceneNormalTex_)
// N0 BUG-3 (occlusion format parity): GL's half-res AO buffer (ssaoColorTex_) is
// GL_R16F. The island originally used R8_UNORM ("close enough for a [0,1]
// factor"), but that quantizes the AO to 256 levels where GL keeps ~half-float
// precision -- so the LINEAR-upsampled, multiplied scene color diverges from GL
// by more than the pixel-exact golden floor tolerates (the parity oracle SHAs the
// final PNG). Match GL exactly with R16_SFLOAT (single-channel half-float,
// renderable + linear-filterable on all targets incl. AMD 7900 XTX).
const VkFormat kOcclusionFmt    = VK_FORMAT_R16_SFLOAT;          // half-res AO (matches GL ssaoColorTex_ GL_R16F precision)

} // namespace

// Persistent Vulkan SSAO island state. Sized lazily to (fullW,fullH,halfW,halfH)
// on first init; if the framebuffer resizes we tear down + rebuild.
struct gosPostProcess::VulkanSSAOIsland {
    bool disabled = false;   // init failed once -> never retry, always GL path
    bool inited   = false;

    int fullW = 0, fullH = 0;
    int halfW = 0, halfH = 0;

    VkInstance       instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE; // validation-only
    VkPhysicalDevice phys     = VK_NULL_HANDLE;
    VkDevice         device   = VK_NULL_HANDLE;
    uint32_t         gfxFamily = UINT32_MAX;
    VkQueue          queue    = VK_NULL_HANDLE;
    VmaAllocator     allocator = VK_NULL_HANDLE;

    // Island-OWNED images.
    VkImage       depthImage  = VK_NULL_HANDLE;  VmaAllocation depthAlloc  = VK_NULL_HANDLE;
    VkImageView   depthView   = VK_NULL_HANDLE;
    VkImage       normalImage = VK_NULL_HANDLE;  VmaAllocation normalAlloc = VK_NULL_HANDLE;
    VkImageView   normalView  = VK_NULL_HANDLE;
    VkImage       colorImage  = VK_NULL_HANDLE;  VmaAllocation colorAlloc  = VK_NULL_HANDLE;
    VkImageView   colorView   = VK_NULL_HANDLE;
    VkImage       occImage    = VK_NULL_HANDLE;  VmaAllocation occAlloc    = VK_NULL_HANDLE; // half-res occlusion intermediate
    VkImageView   occView     = VK_NULL_HANDLE;
    VkSampler     depthSampler  = VK_NULL_HANDLE; // NEAREST, shared depth+normal sampler (occlusion pass)
    VkSampler     occSampler    = VK_NULL_HANDLE; // LINEAR (N0 BUG-2): half-res AO upsample in the apply pass (matches GL ssaoColorTex_ GL_LINEAR)

    // Host-visible staging: depth-in, normal-in, color-in, color-out + the two UBOs.
    VkBuffer      depthStaging  = VK_NULL_HANDLE; VmaAllocation depthStagingAlloc  = VK_NULL_HANDLE;
    VkBuffer      normalStaging = VK_NULL_HANDLE; VmaAllocation normalStagingAlloc = VK_NULL_HANDLE;
    VkBuffer      colorStaging  = VK_NULL_HANDLE; VmaAllocation colorStagingAlloc  = VK_NULL_HANDLE; // color-in
    VkBuffer      colorReadback = VK_NULL_HANDLE; VmaAllocation colorReadbackAlloc = VK_NULL_HANDLE; // color-out
    VkBuffer      occlusionUbo  = VK_NULL_HANDLE; VmaAllocation occlusionUboAlloc  = VK_NULL_HANDLE;
    VkBuffer      applyUbo      = VK_NULL_HANDLE; VmaAllocation applyUboAlloc      = VK_NULL_HANDLE;

    // Pass 1: half-res occlusion render pass/framebuffer/pipeline.
    VkRenderPass      occRpass  = VK_NULL_HANDLE;  // 1 color attachment, CLEAR/STORE (overwrite, no blend)
    VkFramebuffer     occFb     = VK_NULL_HANDLE;
    VkDescriptorSetLayout occDsl = VK_NULL_HANDLE; // binding0 depth, binding1 normal, binding2 UBO
    VkDescriptorSet   occSet    = VK_NULL_HANDLE;
    VkPipelineLayout  occPlayout = VK_NULL_HANDLE;
    VkPipeline        occPipe   = VK_NULL_HANDLE;
    VkShaderModule    occVert   = VK_NULL_HANDLE;
    VkShaderModule    occFrag   = VK_NULL_HANDLE;

    // Pass 2: full-res apply render pass/framebuffer/pipeline.
    VkRenderPass      applyRpass  = VK_NULL_HANDLE; // 1 color attachment LOAD/STORE, finalLayout=TRANSFER_SRC
    VkFramebuffer     applyFb     = VK_NULL_HANDLE;
    VkDescriptorSetLayout applyDsl = VK_NULL_HANDLE; // binding0 occlusion sampler, binding1 UBO
    VkDescriptorSet   applySet    = VK_NULL_HANDLE;
    VkPipelineLayout  applyPlayout = VK_NULL_HANDLE;
    VkPipeline        applyPipe   = VK_NULL_HANDLE;
    VkShaderModule    applyVert   = VK_NULL_HANDLE;
    VkShaderModule    applyFrag   = VK_NULL_HANDLE;

    VkDescriptorPool  pool   = VK_NULL_HANDLE;   // shared by both descriptor sets

    VkCommandPool     cpool  = VK_NULL_HANDLE;
    VkCommandBuffer   cbuf   = VK_NULL_HANDLE;
    VkFence           fence  = VK_NULL_HANDLE;

    // CPU scratch for the GL<->staging bridge.
    std::vector<float>    depthScratch;   // fullW*fullH floats
    std::vector<uint16_t> normalScratch;  // fullW*fullH*4 halfs (RGBA16F GBuffer1)
    std::vector<uint16_t> colorScratch;   // fullW*fullH*4 halfs
};

namespace {

// Tear down everything the island owns (safe on partially-built state).
void destroy_island(gosPostProcess::VulkanSSAOIsland* s) {
    if (!s) return;
    VkDevice d = s->device;
    if (d != VK_NULL_HANDLE) vkDeviceWaitIdle(d);
    if (s->fence)  vkDestroyFence(d, s->fence, nullptr);
    if (s->cpool)  vkDestroyCommandPool(d, s->cpool, nullptr);
    if (s->occPipe)   vkDestroyPipeline(d, s->occPipe, nullptr);
    if (s->applyPipe) vkDestroyPipeline(d, s->applyPipe, nullptr);
    if (s->occPlayout)   vkDestroyPipelineLayout(d, s->occPlayout, nullptr);
    if (s->applyPlayout) vkDestroyPipelineLayout(d, s->applyPlayout, nullptr);
    if (s->pool)   vkDestroyDescriptorPool(d, s->pool, nullptr);
    if (s->occDsl)   vkDestroyDescriptorSetLayout(d, s->occDsl, nullptr);
    if (s->applyDsl) vkDestroyDescriptorSetLayout(d, s->applyDsl, nullptr);
    if (s->occFrag)  vkDestroyShaderModule(d, s->occFrag, nullptr);
    if (s->occVert)  vkDestroyShaderModule(d, s->occVert, nullptr);
    if (s->applyFrag) vkDestroyShaderModule(d, s->applyFrag, nullptr);
    if (s->applyVert) vkDestroyShaderModule(d, s->applyVert, nullptr);
    if (s->occFb)   vkDestroyFramebuffer(d, s->occFb, nullptr);
    if (s->applyFb) vkDestroyFramebuffer(d, s->applyFb, nullptr);
    if (s->occRpass)   vkDestroyRenderPass(d, s->occRpass, nullptr);
    if (s->applyRpass) vkDestroyRenderPass(d, s->applyRpass, nullptr);
    if (s->occSampler)   vkDestroySampler(d, s->occSampler, nullptr);
    if (s->depthSampler) vkDestroySampler(d, s->depthSampler, nullptr);
    if (s->occView)    vkDestroyImageView(d, s->occView, nullptr);
    if (s->colorView)  vkDestroyImageView(d, s->colorView, nullptr);
    if (s->normalView) vkDestroyImageView(d, s->normalView, nullptr);
    if (s->depthView)  vkDestroyImageView(d, s->depthView, nullptr);
    if (s->allocator) {
        if (s->applyUbo)      vmaDestroyBuffer(s->allocator, s->applyUbo, s->applyUboAlloc);
        if (s->occlusionUbo)  vmaDestroyBuffer(s->allocator, s->occlusionUbo, s->occlusionUboAlloc);
        if (s->colorReadback) vmaDestroyBuffer(s->allocator, s->colorReadback, s->colorReadbackAlloc);
        if (s->colorStaging)  vmaDestroyBuffer(s->allocator, s->colorStaging, s->colorStagingAlloc);
        if (s->normalStaging) vmaDestroyBuffer(s->allocator, s->normalStaging, s->normalStagingAlloc);
        if (s->depthStaging)  vmaDestroyBuffer(s->allocator, s->depthStaging, s->depthStagingAlloc);
        if (s->occImage)    vmaDestroyImage(s->allocator, s->occImage, s->occAlloc);
        if (s->colorImage)  vmaDestroyImage(s->allocator, s->colorImage, s->colorAlloc);
        if (s->normalImage) vmaDestroyImage(s->allocator, s->normalImage, s->normalAlloc);
        if (s->depthImage)  vmaDestroyImage(s->allocator, s->depthImage, s->depthAlloc);
        vmaDestroyAllocator(s->allocator);
    }
    if (d != VK_NULL_HANDLE) vkDestroyDevice(d, nullptr);
    if (s->debugMessenger && s->instance) {
        auto destroyFn = (PFN_vkDestroyDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(s->instance, "vkDestroyDebugUtilsMessengerEXT");
        if (destroyFn) destroyFn(s->instance, s->debugMessenger, nullptr);
    }
    if (s->instance) vkDestroyInstance(s->instance, nullptr);
    gosPostProcess::VulkanSSAOIsland fresh;
    fresh.disabled = s->disabled;
    *s = fresh;
}

// Build the whole persistent Vulkan SSAO island sized to fullWxfullH (+half-res
// occlusion target). Returns false (fail-soft) on any error, leaving *s
// torn-down-but-not-disabled.
bool build_island(gosPostProcess::VulkanSSAOIsland* s, int fullW, int fullH, int halfW, int halfH) {
    // FORCE-FALLBACK debug hook: MC2_VULKAN_SSAO_ISLAND_FORCE_FALLBACK=<reason>
    // makes init fail immediately with the chosen fallback_reason (prefixed
    // "forced_") so the GL fail-soft path can be proven without uninstalling the
    // Vulkan runtime. Default build (env unset) is unaffected.
    {
        const char* fforce = std::getenv("MC2_VULKAN_SSAO_ISLAND_FORCE_FALLBACK");
        if (fforce && fforce[0]) {
            std::string reason = "forced_";
            reason += fforce;
            vlog("FORCE-FALLBACK hook active (MC2_VULKAN_SSAO_ISLAND_FORCE_FALLBACK=%s) "
                 "-- init forced to fail, using GL SSAO.", fforce);
            setFallback(reason.c_str());
            return false;
        }
    }

    if (!ensure_volk_initialized()) return false;

    s->fullW = fullW; s->fullH = fullH; s->halfW = halfW; s->halfH = halfH;
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
    app.pApplicationName = "MC2 (Vulkan SSAO island)";
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
            mci.pfnUserCallback = ssao_debug_cb;
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
    // Depth + normal (sampled, full-res, transfer-dst); Color (attachment,
    // full-res, transfer-dst+transfer-src); Occlusion (attachment+sampled,
    // half-res, no transfer -- stays Vulkan-owned between the two passes).
    if (!make_image(kDepthFmt, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    fullW, fullH, &s->depthImage, &s->depthAlloc)) return false;
    if (!make_image(kNormalFmt, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    fullW, fullH, &s->normalImage, &s->normalAlloc)) return false;
    if (!make_image(kFullResColorFmt, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT, fullW, fullH, &s->colorImage, &s->colorAlloc)) return false;
    if (!make_image(kOcclusionFmt, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    halfW, halfH, &s->occImage, &s->occAlloc)) return false;

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
    if (!make_view(s->depthImage,  kDepthFmt,        VK_IMAGE_ASPECT_DEPTH_BIT, &s->depthView))  return false;
    if (!make_view(s->normalImage, kNormalFmt,       VK_IMAGE_ASPECT_COLOR_BIT, &s->normalView)) return false;
    if (!make_view(s->colorImage,  kFullResColorFmt, VK_IMAGE_ASPECT_COLOR_BIT, &s->colorView))  return false;
    if (!make_view(s->occImage,    kOcclusionFmt,    VK_IMAGE_ASPECT_COLOR_BIT, &s->occView))    return false;

    auto make_sampler = [&](VkSampler* samp, VkFilter filter) -> bool {
        VkSamplerCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = filter;
        si.minFilter = filter;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VkResult rr = vkCreateSampler(s->device, &si, nullptr, samp);
        if (rr != VK_SUCCESS) { vlog("vkCreateSampler failed (%d).", (int)rr); return false; }
        return true;
    };
    // depthSampler samples full-res scene depth + normal in the occlusion pass;
    // GL's sceneDepthTex_/sceneNormalTex_ are NEAREST, so keep NEAREST.
    if (!make_sampler(&s->depthSampler, VK_FILTER_NEAREST)) return false;
    // N0 BUG-2 (apply-sampler filter parity): the apply pass upsamples the
    // half-res occlusion buffer with a 4-tap box at +/-0.5-texel offsets. GL's
    // ssaoColorTex_ is created with GL_LINEAR min/mag (gos_postprocess.cpp
    // ssaoColorTex_ setup), so those off-center taps get HARDWARE BILINEAR
    // interpolation on top of the box. A NEAREST sampler snaps each tap to a
    // texel center -> a blockier, subtly-different upsample (the moire-vs-grid
    // artifact). Use LINEAR here to reproduce GL's filter exactly.
    if (!make_sampler(&s->occSampler,   VK_FILTER_LINEAR))  return false;

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
    const VkDeviceSize depthBytes  = (VkDeviceSize)fullW * fullH * 4;      // R32 float
    const VkDeviceSize normalBytes = (VkDeviceSize)fullW * fullH * 4 * 2;  // RGBA16F
    const VkDeviceSize colorBytes  = (VkDeviceSize)fullW * fullH * 4 * 2;  // RGBA16F
    if (!make_buffer(depthBytes,  VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &s->depthStaging,  &s->depthStagingAlloc))  return false;
    if (!make_buffer(normalBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &s->normalStaging, &s->normalStagingAlloc)) return false;
    if (!make_buffer(colorBytes,  VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &s->colorStaging,  &s->colorStagingAlloc))  return false;
    if (!make_buffer(colorBytes,  VK_BUFFER_USAGE_TRANSFER_DST_BIT, &s->colorReadback, &s->colorReadbackAlloc)) return false;
    if (!make_buffer(sizeof(SSAOOcclusionParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &s->occlusionUbo, &s->occlusionUboAlloc)) return false;
    if (!make_buffer(sizeof(SSAOApplyParams),     VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &s->applyUbo,     &s->applyUboAlloc))     return false;

    // ---- Pass 1 render pass: half-res occlusion, CLEAR->STORE (overwrite, no blend). ----
    {
        VkAttachmentDescription att{};
        att.format = kOcclusionFmt;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        // finalLayout SHADER_READ_ONLY: this pass's implicit final transition IS
        // the B14 barrier the header comment describes -- pass2 samples this
        // image, ordered by the render pass' own external subpass dependency
        // below (same pattern as the H1 fix, applied to a cross-pass READ this
        // time instead of a copy-readback READ).
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
        r = vkCreateRenderPass(s->device, &rpci, nullptr, &s->occRpass);
        if (r != VK_SUCCESS) { vlog("vkCreateRenderPass(occ) failed (%d).", (int)r); return false; }

        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = s->occRpass;
        fci.attachmentCount = 1;
        fci.pAttachments = &s->occView;
        fci.width = (uint32_t)halfW; fci.height = (uint32_t)halfH; fci.layers = 1;
        r = vkCreateFramebuffer(s->device, &fci, nullptr, &s->occFb);
        if (r != VK_SUCCESS) { vlog("vkCreateFramebuffer(occ) failed (%d).", (int)r); return false; }
    }

    // ---- Pass 2 render pass: full-res color, LOAD (preloaded scene color) ->
    // STORE, MULTIPLICATIVE blend. finalLayout=TRANSFER_SRC (the H1 fix pattern
    // for the color readback). ----
    {
        VkAttachmentDescription att{};
        att.format = kFullResColorFmt;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;   // apply pass multiplies OVER preloaded scene color
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
        // VULKAN-ISLAND-READBACK-SYNC-BARRIER-1 (H1) pattern: order the render
        // pass' implicit final layout transition (WRITE) before the following
        // vkCmdCopyImageToBuffer (TRANSFER read).
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
        r = vkCreateRenderPass(s->device, &rpci, nullptr, &s->applyRpass);
        if (r != VK_SUCCESS) { vlog("vkCreateRenderPass(apply) failed (%d).", (int)r); return false; }

        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = s->applyRpass;
        fci.attachmentCount = 1;
        fci.pAttachments = &s->colorView;
        fci.width = (uint32_t)fullW; fci.height = (uint32_t)fullH; fci.layers = 1;
        r = vkCreateFramebuffer(s->device, &fci, nullptr, &s->applyFb);
        if (r != VK_SUCCESS) { vlog("vkCreateFramebuffer(apply) failed (%d).", (int)r); return false; }
    }

    // ---- Descriptor pool (both sets) + set layouts ----
    {
        VkDescriptorSetLayoutBinding occBinds[3]{};
        occBinds[0].binding = 0; occBinds[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        occBinds[0].descriptorCount = 1; occBinds[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        occBinds[1].binding = 1; occBinds[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        occBinds[1].descriptorCount = 1; occBinds[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        occBinds[2].binding = 2; occBinds[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        occBinds[2].descriptorCount = 1; occBinds[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo occLci{};
        occLci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        occLci.bindingCount = 3; occLci.pBindings = occBinds;
        r = vkCreateDescriptorSetLayout(s->device, &occLci, nullptr, &s->occDsl);
        if (r != VK_SUCCESS) { vlog("vkCreateDescriptorSetLayout(occ) failed (%d).", (int)r); return false; }

        VkDescriptorSetLayoutBinding applyBinds[2]{};
        applyBinds[0].binding = 0; applyBinds[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        applyBinds[0].descriptorCount = 1; applyBinds[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        applyBinds[1].binding = 1; applyBinds[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        applyBinds[1].descriptorCount = 1; applyBinds[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo applyLci{};
        applyLci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        applyLci.bindingCount = 2; applyLci.pBindings = applyBinds;
        r = vkCreateDescriptorSetLayout(s->device, &applyLci, nullptr, &s->applyDsl);
        if (r != VK_SUCCESS) { vlog("vkCreateDescriptorSetLayout(apply) failed (%d).", (int)r); return false; }

        VkDescriptorPoolSize sizes[2]{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sizes[0].descriptorCount = 3; // depth+normal+occ
        sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;         sizes[1].descriptorCount = 2; // occlusion+apply UBOs
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets = 2;
        pci.poolSizeCount = 2;
        pci.pPoolSizes = sizes;
        r = vkCreateDescriptorPool(s->device, &pci, nullptr, &s->pool);
        if (r != VK_SUCCESS) { vlog("vkCreateDescriptorPool failed (%d).", (int)r); return false; }

        VkDescriptorSetAllocateInfo occDsai{};
        occDsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        occDsai.descriptorPool = s->pool;
        occDsai.descriptorSetCount = 1;
        occDsai.pSetLayouts = &s->occDsl;
        r = vkAllocateDescriptorSets(s->device, &occDsai, &s->occSet);
        if (r != VK_SUCCESS) { vlog("vkAllocateDescriptorSets(occ) failed (%d).", (int)r); return false; }

        VkDescriptorSetAllocateInfo applyDsai{};
        applyDsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        applyDsai.descriptorPool = s->pool;
        applyDsai.descriptorSetCount = 1;
        applyDsai.pSetLayouts = &s->applyDsl;
        r = vkAllocateDescriptorSets(s->device, &applyDsai, &s->applySet);
        if (r != VK_SUCCESS) { vlog("vkAllocateDescriptorSets(apply) failed (%d).", (int)r); return false; }

        VkDescriptorImageInfo depthInfo{};
        depthInfo.sampler = s->depthSampler; depthInfo.imageView = s->depthView;
        depthInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo normalInfo{};
        normalInfo.sampler = s->depthSampler; normalInfo.imageView = s->normalView;
        normalInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo occInfo{};
        occInfo.sampler = s->occSampler; occInfo.imageView = s->occView;
        occInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorBufferInfo occUboInfo{ s->occlusionUbo, 0, VK_WHOLE_SIZE };
        VkDescriptorBufferInfo applyUboInfo{ s->applyUbo, 0, VK_WHOLE_SIZE };

        VkWriteDescriptorSet writes[5]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = s->occSet; writes[0].dstBinding = 0; writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[0].pImageInfo = &depthInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = s->occSet; writes[1].dstBinding = 1; writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[1].pImageInfo = &normalInfo;
        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = s->occSet; writes[2].dstBinding = 2; writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; writes[2].pBufferInfo = &occUboInfo;
        writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[3].dstSet = s->applySet; writes[3].dstBinding = 0; writes[3].descriptorCount = 1;
        writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[3].pImageInfo = &occInfo;
        writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[4].dstSet = s->applySet; writes[4].dstBinding = 1; writes[4].descriptorCount = 1;
        writes[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; writes[4].pBufferInfo = &applyUboInfo;
        vkUpdateDescriptorSets(s->device, 5, writes, 0, nullptr);
    }

    // ---- Two pipelines (fullscreen tri; depth off; cull none). Pass1 = no
    // blend (overwrite); pass2 = multiplicative blend (DST_COLOR/ZERO). ----
    {
        s->occVert   = load_spv(s->device, spv_dir() + "ssao.vert.spv");
        s->occFrag   = load_spv(s->device, spv_dir() + "ssao.frag.spv");
        s->applyVert = load_spv(s->device, spv_dir() + "ssao_apply.vert.spv");
        s->applyFrag = load_spv(s->device, spv_dir() + "ssao_apply.frag.spv");
        if (s->occVert == VK_NULL_HANDLE || s->occFrag == VK_NULL_HANDLE ||
            s->applyVert == VK_NULL_HANDLE || s->applyFrag == VK_NULL_HANDLE) {
            setFallback("spv_load_failed"); return false;
        }

        VkPipelineLayoutCreateInfo occPlci{};
        occPlci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        occPlci.setLayoutCount = 1; occPlci.pSetLayouts = &s->occDsl;
        r = vkCreatePipelineLayout(s->device, &occPlci, nullptr, &s->occPlayout);
        if (r != VK_SUCCESS) { vlog("vkCreatePipelineLayout(occ) failed (%d).", (int)r); return false; }

        VkPipelineLayoutCreateInfo applyPlci{};
        applyPlci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        applyPlci.setLayoutCount = 1; applyPlci.pSetLayouts = &s->applyDsl;
        r = vkCreatePipelineLayout(s->device, &applyPlci, nullptr, &s->applyPlayout);
        if (r != VK_SUCCESS) { vlog("vkCreatePipelineLayout(apply) failed (%d).", (int)r); return false; }

        VkPipelineVertexInputStateCreateInfo vin{};
        vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // Pass1: no blend, overwrite.
        VkPipelineColorBlendAttachmentState occCba{};
        occCba.blendEnable = VK_FALSE;
        occCba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo occCb{};
        occCb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        occCb.attachmentCount = 1; occCb.pAttachments = &occCba;

        // Pass2: multiplicative blend (scene *= ao) -- matches GL BLENDMODE-MULTIPLY-1
        // PostProcessSsaoApply row (glBlendFunc(GL_DST_COLOR, GL_ZERO)).
        VkPipelineColorBlendAttachmentState applyCba{};
        applyCba.blendEnable = VK_TRUE;
        applyCba.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
        applyCba.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        applyCba.colorBlendOp = VK_BLEND_OP_ADD;
        applyCba.srcAlphaBlendFactor = VK_BLEND_FACTOR_DST_ALPHA;
        applyCba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        applyCba.alphaBlendOp = VK_BLEND_OP_ADD;
        applyCba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo applyCb{};
        applyCb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        applyCb.attachmentCount = 1; applyCb.pAttachments = &applyCba;

        VkViewport occVp{0.0f, 0.0f, (float)halfW, (float)halfH, 0.0f, 1.0f};
        VkRect2D occSc{{0, 0}, {(uint32_t)halfW, (uint32_t)halfH}};
        VkPipelineViewportStateCreateInfo occVps{};
        occVps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        occVps.viewportCount = 1; occVps.pViewports = &occVp; occVps.scissorCount = 1; occVps.pScissors = &occSc;

        VkViewport applyVp{0.0f, 0.0f, (float)fullW, (float)fullH, 0.0f, 1.0f};
        VkRect2D applySc{{0, 0}, {(uint32_t)fullW, (uint32_t)fullH}};
        VkPipelineViewportStateCreateInfo applyVps{};
        applyVps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        applyVps.viewportCount = 1; applyVps.pViewports = &applyVp; applyVps.scissorCount = 1; applyVps.pScissors = &applySc;

        VkPipelineShaderStageCreateInfo occStages[2]{};
        occStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        occStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; occStages[0].module = s->occVert; occStages[0].pName = "main";
        occStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        occStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; occStages[1].module = s->occFrag; occStages[1].pName = "main";
        VkGraphicsPipelineCreateInfo occGp{};
        occGp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        occGp.stageCount = 2; occGp.pStages = occStages;
        occGp.pVertexInputState = &vin;
        occGp.pInputAssemblyState = &ia;
        occGp.pViewportState = &occVps;
        occGp.pRasterizationState = &rs;
        occGp.pMultisampleState = &ms;
        occGp.pColorBlendState = &occCb;
        occGp.layout = s->occPlayout;
        occGp.renderPass = s->occRpass;
        occGp.subpass = 0;
        r = vkCreateGraphicsPipelines(s->device, VK_NULL_HANDLE, 1, &occGp, nullptr, &s->occPipe);
        if (r != VK_SUCCESS) { vlog("vkCreateGraphicsPipelines(occ) failed (%d).", (int)r); return false; }

        VkPipelineShaderStageCreateInfo applyStages[2]{};
        applyStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        applyStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; applyStages[0].module = s->applyVert; applyStages[0].pName = "main";
        applyStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        applyStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; applyStages[1].module = s->applyFrag; applyStages[1].pName = "main";
        VkGraphicsPipelineCreateInfo applyGp{};
        applyGp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        applyGp.stageCount = 2; applyGp.pStages = applyStages;
        applyGp.pVertexInputState = &vin;
        applyGp.pInputAssemblyState = &ia;
        applyGp.pViewportState = &applyVps;
        applyGp.pRasterizationState = &rs;
        applyGp.pMultisampleState = &ms;
        applyGp.pColorBlendState = &applyCb;
        applyGp.layout = s->applyPlayout;
        applyGp.renderPass = s->applyRpass;
        applyGp.subpass = 0;
        r = vkCreateGraphicsPipelines(s->device, VK_NULL_HANDLE, 1, &applyGp, nullptr, &s->applyPipe);
        if (r != VK_SUCCESS) { vlog("vkCreateGraphicsPipelines(apply) failed (%d).", (int)r); return false; }
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
    s->normalScratch.resize((size_t)fullW * fullH * 4);
    s->colorScratch.resize((size_t)fullW * fullH * 4);
    s->inited = true;
    setFallback("");
    vlog("island initialized: full %dx%d / half %dx%d device ready (depth D32_SFLOAT, "
         "normal/color RGBA16F, occlusion R16_SFLOAT, 2 render passes, 2 draws, "
         "NO internal readback between passes -- occlusion image stays Vulkan-owned).",
         fullW, fullH, halfW, halfH);
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// ssaoRegionVulkanReady(): POSTPROCESS-VULKAN-BACKEND-OPT-IN-style readiness --
// lazy device init, size-match rebuild, fail-soft self-disable, NO env-gate
// check (backend=vk selection IS the enable signal; mirrors
// postprocessSubgraphReadyForRegion() exactly).
// ---------------------------------------------------------------------------
bool gosPostProcess::ssaoRegionVulkanReady()
{
    if (width_ <= 0 || height_ <= 0 || sceneDepthTex_ == 0 || sceneNormalTex_ == 0 ||
        sceneColorTex_ == 0 || ssaoW_ <= 0 || ssaoH_ <= 0) return false;

    if (!vkSSAOIsland_) vkSSAOIsland_ = new VulkanSSAOIsland();
    VulkanSSAOIsland* s = vkSSAOIsland_;
    if (s->disabled) return false;

    if (!s->inited || s->fullW != width_ || s->fullH != height_ ||
        s->halfW != ssaoW_ || s->halfH != ssaoH_) {
        if (s->inited) destroy_island(s);   // resize -> rebuild
        if (!build_island(s, width_, height_, ssaoW_, ssaoH_)) {
            vlog("island init FAILED -- disabling, falling back to GL SSAO "
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
// runSSAOVulkan(): per-frame GL->Vulkan->GL SSAO composite (occlusion + apply).
// Assumes ssaoRegionVulkanReady() returned true.
// ---------------------------------------------------------------------------
void gosPostProcess::runSSAOVulkan()
{
    VulkanSSAOIsland* s = vkSSAOIsland_;
    if (!s || !s->inited || s->disabled) return;

    const int fullW = s->fullW, fullH = s->fullH;
    const int halfW = s->halfW, halfH = s->halfH;

    ++g_health.attempted;
    using clk = std::chrono::steady_clock;
    auto usSince = [](clk::time_point t0) {
        return std::chrono::duration<double, std::micro>(clk::now() - t0).count();
    };
    const clk::time_point tTotal0 = clk::now();

    // ---- 1) Read GL scene depth + normal + color into staging ----
    const clk::time_point tDepth0 = clk::now();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sceneDepthTex_);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, s->depthScratch.data());
    g_health.copyDepthUs = usSince(tDepth0);

    const clk::time_point tNormal0 = clk::now();
    glBindTexture(GL_TEXTURE_2D, sceneNormalTex_);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_HALF_FLOAT, s->normalScratch.data());
    g_health.copyNormalUs = usSince(tNormal0);

    const clk::time_point tColor0 = clk::now();
    glBindTexture(GL_TEXTURE_2D, sceneColorTex_);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_HALF_FLOAT, s->colorScratch.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    g_health.copyColorInUs = usSince(tColor0);

    // ---- N0 bridge instrumentation (MC2_VK_SSAO_DUMP_INPUTS=1): dump the
    // readback contents' statistics so a deterministic input bug (depth/normal
    // plumbing) can be named without guessing. Prints ONCE per run (first frame
    // the island executes) so it does not spam. Half-float decode is inline.
    {
        static bool s_dumpedInputs = false;
        static unsigned long s_dumpFrame = 0;
        ++s_dumpFrame;
        const char* dbgEnv = std::getenv("MC2_VK_SSAO_DUMP_INPUTS");
        // Only dump once we have a real depth signal (so we capture an IN-MISSION
        // frame, not the first menu/load frame where the terrain MVP hasn't been
        // pushed yet). Scan a stride of the depth staging for any nonzero texel.
        bool haveDepthSignal = false;
        if (dbgEnv && dbgEnv[0] && dbgEnv[0] != '0' && !s_dumpedInputs) {
            const size_t nPixScan = (size_t)fullW * fullH;
            for (size_t i = 0; i < nPixScan; i += 97) {
                if (s->depthScratch[i] > 0.0001f) { haveDepthSignal = true; break; }
            }
            // Heartbeat: every 300 frames with no signal yet, note depth is still zero.
            if (!haveDepthSignal && (s_dumpFrame % 300 == 1)) {
                vlog("[N0-DUMP] frame=%lu depth STILL all-zero (no in-mission signal yet)",
                     s_dumpFrame);
            }
        }
        if (!s_dumpedInputs && dbgEnv && dbgEnv[0] && dbgEnv[0] != '0' && haveDepthSignal) {
            s_dumpedInputs = true;
            auto halfToFloat = [](uint16_t h) -> float {
                uint32_t sign = (uint32_t)(h >> 15) & 0x1u;
                uint32_t exp  = (uint32_t)(h >> 10) & 0x1Fu;
                uint32_t man  = (uint32_t)h & 0x3FFu;
                uint32_t f;
                if (exp == 0) {
                    if (man == 0) { f = sign << 31; }
                    else {
                        exp = 127 - 15 + 1;
                        while ((man & 0x400u) == 0) { man <<= 1; --exp; }
                        man &= 0x3FFu;
                        f = (sign << 31) | (exp << 23) | (man << 13);
                    }
                } else if (exp == 0x1F) {
                    f = (sign << 31) | (0xFFu << 23) | (man << 13);
                } else {
                    f = (sign << 31) | ((exp - 15 + 127) << 23) | (man << 13);
                }
                float out; std::memcpy(&out, &f, 4); return out;
            };
            const size_t nPix = (size_t)fullW * fullH;
            // Depth stats.
            double dMin = 1e30, dMax = -1e30, dSum = 0.0; size_t dNZ = 0;
            for (size_t i = 0; i < nPix; ++i) {
                float d = s->depthScratch[i];
                if (d < dMin) dMin = d; if (d > dMax) dMax = d; dSum += d;
                if (d > 0.0001f) ++dNZ;
            }
            // Normal stats: decode .xyz*2-1, report mean length + component means.
            double nLenSum = 0.0, nxSum = 0.0, nySum = 0.0, nzSum = 0.0; size_t nValid = 0;
            for (size_t i = 0; i < nPix; ++i) {
                float nx = halfToFloat(s->normalScratch[i * 4 + 0]) * 2.0f - 1.0f;
                float ny = halfToFloat(s->normalScratch[i * 4 + 1]) * 2.0f - 1.0f;
                float nz = halfToFloat(s->normalScratch[i * 4 + 2]) * 2.0f - 1.0f;
                double len = std::sqrt((double)nx * nx + (double)ny * ny + (double)nz * nz);
                nLenSum += len; nxSum += nx; nySum += ny; nzSum += nz;
                if (len > 0.1) ++nValid;
            }
            // Center-pixel probe.
            size_t c = ((size_t)(fullH / 2) * fullW) + (fullW / 2);
            float cnx = halfToFloat(s->normalScratch[c * 4 + 0]) * 2.0f - 1.0f;
            float cny = halfToFloat(s->normalScratch[c * 4 + 1]) * 2.0f - 1.0f;
            float cnz = halfToFloat(s->normalScratch[c * 4 + 2]) * 2.0f - 1.0f;
            vlog("[N0-DUMP] frame=%lu full=%dx%d depth[min=%.5f max=%.5f mean=%.5f nonzero=%zu/%zu] "
                 "normal[meanLen=%.4f meanXYZ=(%.4f,%.4f,%.4f) valid=%zu/%zu] "
                 "centerDepth=%.5f centerNormal=(%.3f,%.3f,%.3f) "
                 "aoRadius=%.4f aoBias=%.5f aoStrength=%.4f aoPower=%.4f",
                 s_dumpFrame, fullW, fullH, dMin, dMax, dSum / (double)nPix, dNZ, nPix,
                 nLenSum / (double)nPix, nxSum / (double)nPix, nySum / (double)nPix,
                 nzSum / (double)nPix, nValid, nPix,
                 s->depthScratch[c], cnx, cny, cnz,
                 aoRadius_, aoBias_, aoStrength_, aoPower_);
            // Matrix probe: print row 0 and row 3 of invViewProj + viewProj as the
            // island packs them (raw 16 floats, row-major convention).
            vlog("[N0-DUMP] invVP row0=(%.4f,%.4f,%.4f,%.4f) row3=(%.4f,%.4f,%.4f,%.4f) "
                 "VP row3=(%.4f,%.4f,%.4f,%.4f)",
                 inverseViewProj_[0], inverseViewProj_[1], inverseViewProj_[2], inverseViewProj_[3],
                 inverseViewProj_[12], inverseViewProj_[13], inverseViewProj_[14], inverseViewProj_[15],
                 viewProj_[12], viewProj_[13], viewProj_[14], viewProj_[15]);
        }
    }

    {
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, s->depthStagingAlloc, &p) == VK_SUCCESS) {
            std::memcpy(p, s->depthScratch.data(), (size_t)fullW * fullH * 4);
            vmaUnmapMemory(s->allocator, s->depthStagingAlloc);
        }
        if (vmaMapMemory(s->allocator, s->normalStagingAlloc, &p) == VK_SUCCESS) {
            std::memcpy(p, s->normalScratch.data(), (size_t)fullW * fullH * 4 * 2);
            vmaUnmapMemory(s->allocator, s->normalStagingAlloc);
        }
        if (vmaMapMemory(s->allocator, s->colorStagingAlloc, &p) == VK_SUCCESS) {
            std::memcpy(p, s->colorScratch.data(), (size_t)fullW * fullH * 4 * 2);
            vmaUnmapMemory(s->allocator, s->colorStagingAlloc);
        }
    }

    // ---- 2) Upload BOTH param UBOs (values pulled EXACTLY from GL runSSAO()'s
    //         ssaoProg_ / ssaoApplyProg_ uploads) ----
    {
        SSAOOcclusionParams params{};
        std::memcpy(params.invViewProj, inverseViewProj_, 16 * sizeof(float));
        std::memcpy(params.viewProj, viewProj_, 16 * sizeof(float));
        params.screenSize[0] = (float)width_;
        params.screenSize[1] = (float)height_;
        params.aoRadius   = aoRadius_;
        params.aoBias     = aoBias_;
        params.aoStrength = aoStrength_;
        params.aoPower    = aoPower_;
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, s->occlusionUboAlloc, &p) == VK_SUCCESS) {
            std::memcpy(p, &params, sizeof(params));
            vmaUnmapMemory(s->allocator, s->occlusionUboAlloc);
        }
    }
    {
        SSAOApplyParams params{};
        params.ssaoTexel[0] = 1.0f / (float)halfW;
        params.ssaoTexel[1] = 1.0f / (float)halfH;
        params.debugMode = ssaoDebug_;
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, s->applyUboAlloc, &p) == VK_SUCCESS) {
            std::memcpy(p, &params, sizeof(params));
            vmaUnmapMemory(s->allocator, s->applyUboAlloc);
        }
    }

    // ---- 3) Record + submit: upload images, TWO render passes, readback ----
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
    depthRegion.imageExtent = {(uint32_t)fullW, (uint32_t)fullH, 1};
    VkBufferImageCopy normalRegion{};
    normalRegion.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    normalRegion.imageExtent = {(uint32_t)fullW, (uint32_t)fullH, 1};
    VkBufferImageCopy colorRegion{};
    colorRegion.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    colorRegion.imageExtent = {(uint32_t)fullW, (uint32_t)fullH, 1};

    // depth: UNDEFINED -> TRANSFER_DST, copy, -> SHADER_READ_ONLY (sampled by occ pass)
    barrier(s->depthImage, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdCopyBufferToImage(s->cbuf, s->depthStaging, s->depthImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &depthRegion);
    barrier(s->depthImage, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    // normal: UNDEFINED -> TRANSFER_DST, copy, -> SHADER_READ_ONLY
    barrier(s->normalImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdCopyBufferToImage(s->cbuf, s->normalStaging, s->normalImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &normalRegion);
    barrier(s->normalImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    // color: UNDEFINED -> TRANSFER_DST, preload scene color, -> COLOR_ATTACHMENT (LOAD target for pass2)
    barrier(s->colorImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdCopyBufferToImage(s->cbuf, s->colorStaging, s->colorImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &colorRegion);
    barrier(s->colorImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    // ---- Pass 1: half-res occlusion (CLEAR->STORE, no blend). Its render pass'
    // external subpass dependency (COLOR_ATTACHMENT_OUTPUT/WRITE ->
    // FRAGMENT_SHADER/READ) is the B14 intra-pass barrier: it orders this
    // pass's write before pass2's read of the SAME image below, with NO
    // explicit vkCmdPipelineBarrier needed (same mechanism as the H1 fix). ----
    VkClearValue occClear{};
    occClear.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    VkRenderPassBeginInfo occRbi{};
    occRbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    occRbi.renderPass = s->occRpass;
    occRbi.framebuffer = s->occFb;
    occRbi.renderArea = {{0, 0}, {(uint32_t)halfW, (uint32_t)halfH}};
    occRbi.clearValueCount = 1;
    occRbi.pClearValues = &occClear;
    vkCmdBeginRenderPass(s->cbuf, &occRbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(s->cbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, s->occPipe);
    vkCmdBindDescriptorSets(s->cbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, s->occPlayout, 0, 1, &s->occSet, 0, nullptr);
    vkCmdDraw(s->cbuf, 3, 1, 0, 0);
    vkCmdEndRenderPass(s->cbuf);
    const double occlusionUs = usSince(tRender0);

    // ---- Pass 2: full-res apply (LOAD scene color, multiplicative blend, STORE). ----
    const clk::time_point tApply0 = clk::now();
    VkRenderPassBeginInfo applyRbi{};
    applyRbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    applyRbi.renderPass = s->applyRpass;
    applyRbi.framebuffer = s->applyFb;
    applyRbi.renderArea = {{0, 0}, {(uint32_t)fullW, (uint32_t)fullH}};
    applyRbi.clearValueCount = 0; // LOAD, no clear
    vkCmdBeginRenderPass(s->cbuf, &applyRbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(s->cbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, s->applyPipe);
    vkCmdBindDescriptorSets(s->cbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, s->applyPlayout, 0, 1, &s->applySet, 0, nullptr);
    vkCmdDraw(s->cbuf, 3, 1, 0, 0);
    vkCmdEndRenderPass(s->cbuf);

    // color image is now TRANSFER_SRC (via the apply render pass' external
    // subpass dependency -- the H1 fix pattern) -> copy to readback buffer.
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
    g_health.occlusionUs = occlusionUs;
    g_health.applyUs = usSince(tApply0);

    // ---- 4) Read back the blended color and write it into sceneColorTex_ ----
    const clk::time_point tOut0 = clk::now();
    {
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, s->colorReadbackAlloc, &p) == VK_SUCCESS) {
            // N0: before overwriting colorScratch (which still holds the INPUT scene
            // color), compute the effective AO ratio = mean(out.g)/mean(in.g). Prints
            // once per run on the first depth-signal frame.
            {
                static unsigned long s_aoFrame = 0;
                ++s_aoFrame;
                const char* dbgEnv = std::getenv("MC2_VK_SSAO_DUMP_INPUTS");
                // Dump AO ratio periodically (every 120 frames) so we see whether the
                // effective AO is stable across the run or degrades on the bookmark frames.
                if (dbgEnv && dbgEnv[0] && dbgEnv[0] != '0' && (s_aoFrame % 120 == 2)) {
                    auto h2f = [](uint16_t h) -> float {
                        uint32_t sign=(uint32_t)(h>>15)&1u, exp=(uint32_t)(h>>10)&0x1Fu, man=(uint32_t)h&0x3FFu, f;
                        if(exp==0){ if(man==0)f=sign<<31; else{exp=127-15+1; while((man&0x400u)==0){man<<=1;--exp;} man&=0x3FFu; f=(sign<<31)|(exp<<23)|(man<<13);} }
                        else if(exp==0x1F) f=(sign<<31)|(0xFFu<<23)|(man<<13);
                        else f=(sign<<31)|((exp-15+127)<<23)|(man<<13);
                        float o; std::memcpy(&o,&f,4); return o; };
                    const uint16_t* inHalf = s->colorScratch.data();     // still INPUT color
                    const uint16_t* outHalf = static_cast<const uint16_t*>(p); // OUTPUT color
                    const size_t nPix = (size_t)fullW * fullH;
                    double inSum = 0.0, outSum = 0.0; size_t cnt = 0;
                    for (size_t i = 0; i < nPix; i += 7) {
                        float ig = h2f(inHalf[i*4+1]);
                        float og = h2f(outHalf[i*4+1]);
                        if (ig > 1e-4f) { inSum += ig; outSum += og; ++cnt; }
                    }
                    double ratio = (inSum > 1e-9) ? (outSum / inSum) : -1.0;
                    vlog("[N0-DUMP-AO] frame=%lu effective AO ratio (mean out.g / mean in.g) = %.4f "
                         "(in=%.4f out=%.4f over %zu px) debugMode=%d",
                         s_aoFrame, ratio, cnt ? inSum/cnt : 0.0, cnt ? outSum/cnt : 0.0, cnt, ssaoDebug_);
                }
            }
            std::memcpy(s->colorScratch.data(), p, (size_t)fullW * fullH * 4 * 2);
            vmaUnmapMemory(s->allocator, s->colorReadbackAlloc);
            glBindTexture(GL_TEXTURE_2D, sceneColorTex_);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, fullW, fullH, GL_RGBA, GL_HALF_FLOAT, s->colorScratch.data());
            glBindTexture(GL_TEXTURE_2D, 0);
        }
    }
    glActiveTexture(GL_TEXTURE0);
    g_health.colorOutUs = usSince(tOut0);

    // Success: 2 Vulkan draws did the whole SSAO region (occlusion + apply).
    g_health.vkDraws = 2;
    ++g_health.usedVulkan;
    g_health.totalUs = usSince(tTotal0);
}

// ---------------------------------------------------------------------------
// destroyVulkanSSAOIsland(): called from gosPostProcess::destroy().
// ---------------------------------------------------------------------------
void gosPostProcess::destroyVulkanSSAOIsland()
{
    vlog("[VK_SSAO_ISLAND_HEALTH] vulkan_available=%d build_enabled=%d device_name=\"%s\" "
         "validation_errors=%lu attempted=%lu used_vulkan=%lu fallback_reason=\"%s\" "
         "vk_draws=%lu copy_depth_us=%.1f copy_normal_us=%.1f copy_color_in_us=%.1f "
         "occlusion_us=%.1f apply_us=%.1f color_out_us=%.1f total_us=%.1f",
         g_health.vulkanAvailable, g_health.buildEnabled, g_health.deviceName,
         g_health.validationErrors, g_health.attempted, g_health.usedVulkan, g_health.fallbackReason,
         g_health.vkDraws, g_health.copyDepthUs, g_health.copyNormalUs, g_health.copyColorInUs,
         g_health.occlusionUs, g_health.applyUs, g_health.colorOutUs, g_health.totalUs);

    if (!vkSSAOIsland_) return;
    destroy_island(vkSSAOIsland_);
    delete vkSSAOIsland_;
    vkSSAOIsland_ = nullptr;
}

#endif // MC2_VULKAN_ISLAND
