#include "vulkan_fsr_provider.h"
#include "vulkan_context.h"
#include "vulkan_motion_vectors.h"
#include "vk_image_resource.h"
#include "vk_gpu_profile.h"
#include "../../RenderCore/TemporalInputs.h"
#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_upscale.h>
#include <ffx_api/vk/ffx_api_vk.h>
#include <ffx_api/ffx_api_loader.h>
#include <atomic>
#include <chrono>
#include "mc2fmt.h"
#include "diagnostic_trace.h"

namespace mc2vk {
namespace {
std::atomic<bool> g_providerFailed{false};
// Native AA: render size == upscale size. The availability probe uses the same call at a small size.
bool createUpscaleContext(const ffxFunctions& fns, VulkanContext& c, uint32_t width, uint32_t height, ffxContext* out) {
    ffxCreateBackendVKDesc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backend.vkDevice = c.device();
    backend.vkPhysicalDevice = c.physicalDevice();
    backend.vkDeviceProcAddr = vkGetDeviceProcAddr;
    ffxCreateContextDescUpscale up{};
    up.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    up.header.pNext = &backend.header;
    up.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
    up.maxRenderSize = {width, height};
    up.maxUpscaleSize = {width, height};
    if (fns.CreateContext(out, &up.header, nullptr) != FFX_API_RETURN_OK) { *out = nullptr; return false; }
    return true;
}
FfxApiResource wrap(VkImage image, VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage, uint32_t state) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = format; ci.extent = {w, h, 1};
    ci.mipLevels = 1; ci.arrayLayers = 1; ci.usage = usage;
    return ffxApiGetResourceVK(reinterpret_cast<void*>(image), ffxApiGetImageResourceDescriptionVK(image, ci, 0), state);
}
}

struct FsrProvider::Impl {
    VulkanContext* ctx = nullptr;
    HMODULE dll = nullptr;
    ffxFunctions fns{};
    ffxContext context = nullptr;
    VkImageResource output{};
    uint32_t w = 0, h = 0;
    VkImage sceneImage = VK_NULL_HANDLE, depthImage = VK_NULL_HANDLE;
    bool failed = false, registered = false, outputReady = false;
    int resetFrames = 2;
    std::chrono::steady_clock::time_point last{};

    void setRegistered(bool on) {
        if (on == registered) return;
        registered = on;
        RenderCore::temporal::consumerCount().fetch_add(on ? 1 : -1, std::memory_order_relaxed);
    }
    void destroyContext() {
        if (context && fns.DestroyContext) fns.DestroyContext(&context, nullptr);
        context = nullptr;
        if (ctx) vkImageResourceDestroy(&output, ctx->device());
        output = {};
        outputReady = false;
        w = h = 0;
    }
    ~Impl() {
        setRegistered(false);
        destroyContext();
        if (dll) FreeLibrary(dll);
    }
    bool create(VulkanContext& c, uint32_t width, uint32_t height) {
        ctx = &c;
        if (!c.temporalFeaturesEnabled()) {   // the FFX shaders need 16-bit types and separate depth/stencil layouts: keep the spatial AA path
            mc2::fmt::print_err("[VK_PRESENT] fsr=off reason=device_features (shaderInt16/shaderFloat16/16-bit storage/separateDepthStencilLayouts)\n");
            return false;
        }
        if (!dll) {
            dll = LoadLibraryA("amd_fidelityfx_vk.dll");
            if (!dll) return false;
            ffxLoadFunctions(&fns, dll);
            if (!fns.CreateContext || !fns.DestroyContext || !fns.Dispatch) return false;
        }
        if (!vkImageResourceCreate(&output, c.device(), c.physicalDevice(), width, height, VK_FORMAT_R16G16B16A16_SFLOAT,
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, "scene.fsrOutput")) return false;
        if (!createUpscaleContext(fns, c, width, height, &context)) return false;
        w = width; h = height;
        resetFrames = 2;
        return true;
    }
};

FsrProvider::FsrProvider() = default;
FsrProvider::~FsrProvider() = default;

bool FsrProvider::prepare(VulkanContext& ctx, const VkImageResource& scene, const VkImageResource& depth, bool wanted) {
    if (!wanted || !scene.image || !depth.image || !scene.w || !scene.h || depth.w != scene.w || depth.h != scene.h) {
        if (impl_) impl_->setRegistered(false);
        return false;
    }
    if (!impl_) impl_ = std::make_unique<Impl>();
    Impl& s = *impl_;
    if (s.failed) return false;
    if (s.context && (s.w != scene.w || s.h != scene.h)) s.destroyContext();    // resize
    if (!s.context && !s.create(ctx, scene.w, scene.h)) {
        s.failed = true; s.setRegistered(false); g_providerFailed.store(true, std::memory_order_relaxed);
        return false;
    }
    if (s.sceneImage != scene.image.get() || s.depthImage != depth.image.get()) {   // mission load recreated the targets
        s.sceneImage = scene.image.get(); s.depthImage = depth.image.get();
        s.resetFrames = 2;
    }
    s.setRegistered(true);
    return true;
}

bool FsrProvider::record(VkCommandBuffer cb, const VkImageResource& scene, const VkImageResource& depth,
                         const MotionVectors& mv, uint32_t renderFrame) {
    if (!impl_ || !impl_->context || impl_->failed) return false;
    Impl& s = *impl_;
    MC2_VK_GPU_SCOPE(gpuScope, cb, "VKIsl.Post.FsrAA");
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    if (!s.outputReady) {
        b.image = s.output.image.get();
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        s.outputReady = true;
    }
    VkMemoryBarrier in{VK_STRUCTURE_TYPE_MEMORY_BARRIER};   // scene colour, MV and reactive writes -> compute reads
    in.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    in.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &in, 0, nullptr, 0, nullptr);
    b.image = depth.image.get();
    b.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
    b.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);

    const uint32_t kRead = FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
    float jitter[2];
    RenderCore::temporal::jitterPixels(renderFrame, RenderCore::temporal::phaseCount(1, 1), jitter);
    const auto now = std::chrono::steady_clock::now();
    const float dtMs = s.last.time_since_epoch().count() ? std::chrono::duration<float, std::milli>(now - s.last).count() : 16.6f;
    s.last = now;

    ffxDispatchDescUpscale d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = cb;
    d.color = wrap(scene.image.get(), scene.format, s.w, s.h, scene.usage, kRead);
    d.depth = wrap(depth.image.get(), depth.format, s.w, s.h, depth.usage, kRead);
    d.motionVectors = wrap(mv.image(), VK_FORMAT_R16G16_SFLOAT, s.w, s.h, VK_IMAGE_USAGE_SAMPLED_BIT, kRead);
    d.reactive = wrap(mv.reactiveImage(), VK_FORMAT_R8_UNORM, s.w, s.h, VK_IMAGE_USAGE_SAMPLED_BIT, kRead);
    d.output = wrap(s.output.image.get(), VK_FORMAT_R16G16B16A16_SFLOAT, s.w, s.h,
                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    d.jitterOffset = {jitter[0], jitter[1]};
    d.motionVectorScale = {static_cast<float>(s.w), static_cast<float>(s.h)};   // motion is in UV units
    d.renderSize = {s.w, s.h};
    d.upscaleSize = {s.w, s.h};
    d.frameTimeDelta = dtMs;
    d.preExposure = 1.0f;
    d.reset = s.resetFrames > 0;
    d.cameraNear = 1.0f; d.cameraFar = 20000.0f; d.cameraFovAngleVertical = 0.6f; d.viewSpaceToMetersFactor = 1.0f;
    if (s.resetFrames > 0) --s.resetFrames;
    const bool ok = s.fns.Dispatch(&s.context, &d.header) == FFX_API_RETURN_OK;

    b.image = depth.image.get();
    b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);
    VkMemoryBarrier m{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    m.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; m.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &m, 0, nullptr, 0, nullptr);
    if (!ok) { s.failed = true; s.setRegistered(false); g_providerFailed.store(true, std::memory_order_relaxed); }
    return ok;
}

bool fsrProviderAvailable(VulkanContext& ctx) {
    if (g_providerFailed.load(std::memory_order_relaxed)) return false;
    // Probed once, independent of present: the material samplers are created before the first FsrProvider::prepare,
    // so the answer must already hold what prepare() will find (device features, DLL entry points, a context the
    // SDK actually creates on this device). The probe context is destroyed at once; the DLL stays loaded.
    static const bool available = [&ctx] {
        const char* reason = "ok";
        bool ok = false;
        if (!ctx.valid()) reason = "no_device";
        else if (!ctx.temporalFeaturesEnabled()) reason = "device_features";
        else if (HMODULE dll = LoadLibraryA("amd_fidelityfx_vk.dll"); !dll) reason = "dll_missing";
        else {
            ffxFunctions fns{};
            ffxLoadFunctions(&fns, dll);
            ffxContext probe = nullptr;
            if (!fns.CreateContext || !fns.DestroyContext || !fns.Dispatch) reason = "dll_entry_points";
            else if (!createUpscaleContext(fns, ctx, 64, 64, &probe)) reason = "create_context";
            else { fns.DestroyContext(&probe, nullptr); ok = true; }
        }
        if (mc2_diag::tagRequested("VK_PRESENT"))
            mc2::fmt::print_err("[VK_PRESENT] fsr_probe available={} reason={}\n", ok ? 1 : 0, reason);
        return ok;
    }();
    return available;
}

VkImageView FsrProvider::outputView() const { return impl_ ? impl_->output.view.get() : VK_NULL_HANDLE; }
void FsrProvider::resetHistory() { if (impl_) impl_->resetFrames = 2; }
}
