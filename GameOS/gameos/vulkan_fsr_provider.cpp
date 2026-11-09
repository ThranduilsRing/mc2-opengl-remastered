// FSR 3.1 through ffx-api: the first RenderCore::IUpscalerProvider. Loads the vendored amd_fidelityfx_vk.dll at
// runtime; with no DLL, no device support or a failed create/dispatch it reports unavailable and present falls back to
// the next provider or native (spatial AA). Present-owner object: every call runs on the thread that records the
// composite. Registered in vk_upscalers.cpp.
#include "../../RenderCore/UpscalerProvider.h"
#ifdef _WIN32
#include "vulkan_context.h"
#include "vk_image_resource.h"
#include "vk_gpu_profile.h"
#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_upscale.h>
#include <ffx_api/vk/ffx_api_vk.h>
#include <ffx_api/ffx_api_loader.h>
#include <atomic>
#include <iterator>
#include <memory>
#include "mc2fmt.h"
#include "diagnostic_trace.h"
#include "Gates/gate_cold.h"

namespace mc2vk {
namespace {
using RenderCore::UpscalerFrame;
using RenderCore::UpscalerImage;

std::atomic<bool> g_providerFailed{false};

// Published FSR 3.1 ratios (render = display / ratio).
constexpr RenderCore::UpscalerQualityMode kFsrModes[] = {
    {RenderCore::UpscalerQuality::NativeAA, 1.0f},
    {RenderCore::UpscalerQuality::Quality, 1.0f / 1.5f},
    {RenderCore::UpscalerQuality::Balanced, 1.0f / 1.7f},
    {RenderCore::UpscalerQuality::Performance, 1.0f / 2.0f},
    {RenderCore::UpscalerQuality::UltraPerformance, 1.0f / 3.0f},
};
// FSR takes current-to-previous offsets (the engine direction) in render pixels, no flip.
constexpr RenderCore::UpscalerCaps kFsrCaps{kFsrModes, static_cast<int>(std::size(kFsrModes)),
                                            RenderCore::MotionVectorUnits::RenderPixels, {1.0f, 1.0f}, true, false};

bool createUpscaleContext(const ffxFunctions& fns, VulkanContext& c, uint32_t renderW, uint32_t renderH,
                          uint32_t displayW, uint32_t displayH, ffxContext* out, bool depthReversed = true,
                          bool depthInfinite = false) {
    ffxCreateBackendVKDesc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backend.vkDevice = c.device();
    backend.vkPhysicalDevice = c.physicalDevice();
    backend.vkDeviceProcAddr = vkGetDeviceProcAddr;
    ffxCreateContextDescUpscale up{};
    up.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    up.header.pNext = &backend.header;
    up.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE |
               (depthReversed ? FFX_UPSCALE_ENABLE_DEPTH_INVERTED : 0u) | (depthInfinite ? FFX_UPSCALE_ENABLE_DEPTH_INFINITE : 0u);
    up.maxRenderSize = {renderW, renderH};
    up.maxUpscaleSize = {displayW, displayH};
    if (fns.CreateContext(out, &up.header, nullptr) != FFX_API_RETURN_OK) { *out = nullptr; return false; }
    return true;
}
FfxApiResource wrap(const UpscalerImage& img, uint32_t state) {
    const VkImage image = reinterpret_cast<VkImage>(img.image);
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = static_cast<VkFormat>(img.format); ci.extent = {img.width, img.height, 1};
    ci.mipLevels = 1; ci.arrayLayers = 1; ci.usage = img.usage;
    return ffxApiGetResourceVK(reinterpret_cast<void*>(image), ffxApiGetImageResourceDescriptionVK(image, ci, 0), state);
}

// Device features, a loadable DLL with its entry points and a probe context the SDK creates, checked once (logs one
// [VK_PRESENT] fsr_probe line); false for good after any create or dispatch failure. Valid before the first prepare(),
// because the material samplers are created before present first asks for the provider.
bool fsrAvailable(VulkanContext& ctx) {
    if (g_providerFailed.load(std::memory_order_relaxed)) return false;
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
            else if (!createUpscaleContext(fns, ctx, 64, 64, 64, 64, &probe)) reason = "create_context";
            else { fns.DestroyContext(&probe, nullptr); ok = true; }
        }
        if (mc2_diag::tagRequested("VK_PRESENT"))
            mc2::fmt::print_err("[VK_PRESENT] fsr_probe available={} reason={}\n", ok ? 1 : 0, reason);
        return ok;
    }();
    return available;
}

class FsrUpscaler final : public RenderCore::IUpscalerProvider {
public:
    // Static exit: present already called releaseResources(); anything left belongs to a device that is gone, so it is
    // dropped, never destroyed (no Vulkan calls at static exit).
    ~FsrUpscaler() override { (void)impl_.release(); }
    std::string_view name() const override { return "fsr"; }
    bool available() const override { return fsrAvailable(getOrCreateEngineVulkanContext()); }
    const RenderCore::UpscalerCaps& caps() const override { return kFsrCaps; }
    bool prepare(const UpscalerFrame& f) override;
    bool dispatch(const UpscalerFrame& f) override;
    UpscalerImage output() const override;
    void resetHistory() override { if (impl_) impl_->resetFrames = 2; }
    void releaseResources() override { impl_.reset(); }

private:
    struct Impl {
        VulkanContext* ctx = nullptr;
        HMODULE dll = nullptr;
        ffxFunctions fns{};
        ffxContext context = nullptr;
        VkImageResource output{};
        uint32_t renderW = 0, renderH = 0, displayW = 0, displayH = 0;
        uint64_t sceneImage = 0, depthImage = 0;
        bool failed = false, outputReady = false;
        int resetFrames = 2, wantedFrames = 0;

        void destroyContext() {
            if (context && fns.DestroyContext) fns.DestroyContext(&context, nullptr);
            context = nullptr;
            if (ctx) vkImageResourceDestroy(&output, ctx->device());
            output = {};
            outputReady = false;
            renderW = renderH = displayW = displayH = 0;
        }
        ~Impl() {
            destroyContext();
            if (dll) FreeLibrary(dll);
        }
        void fail() { failed = true; g_providerFailed.store(true, std::memory_order_relaxed); }
        bool create(VulkanContext& c, const UpscalerFrame& f) {
            ctx = &c;
            if (!c.temporalFeaturesEnabled()) {   // the FFX shaders need 16-bit types and separate depth/stencil layouts
                mc2::fmt::print_err("[VK_PRESENT] fsr=off reason=device_features (shaderInt16/shaderFloat16/16-bit storage/separateDepthStencilLayouts)\n");
                return false;
            }
            if (!dll) {
                dll = LoadLibraryA("amd_fidelityfx_vk.dll");
                if (!dll) return false;
                ffxLoadFunctions(&fns, dll);
                if (!fns.CreateContext || !fns.DestroyContext || !fns.Dispatch) return false;
            }
            if (!vkImageResourceCreate(&output, c.device(), c.physicalDevice(), f.displayWidth, f.displayHeight,
                    VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, "scene.fsrOutput"))
                return false;
            if (!createUpscaleContext(fns, c, f.renderWidth, f.renderHeight, f.displayWidth, f.displayHeight, &context,
                                      f.depthReversed, f.depthInfinite))
                return false;
            renderW = f.renderWidth; renderH = f.renderHeight; displayW = f.displayWidth; displayH = f.displayHeight;
            resetFrames = 2;
            return true;
        }
    };
    std::unique_ptr<Impl> impl_;
};

bool FsrUpscaler::prepare(const UpscalerFrame& f) {
    if (!f.color.valid() || !f.depth.valid() || !f.displayWidth || !f.displayHeight ||
        f.color.width != f.renderWidth || f.color.height != f.renderHeight ||
        f.depth.width != f.renderWidth || f.depth.height != f.renderHeight)
        return false;
    if (!impl_) impl_ = std::make_unique<Impl>();
    Impl& s = *impl_;
    if (s.failed) return false;
    // Test-only failure injection (not a feature gate): fails the provider as a real context/dispatch failure does, at the Nth wanted frame.
    if (static const int failFrame = mc2::gate::get<"MC2_TEST_FSR_FAIL_FRAME">(); failFrame > 0 && ++s.wantedFrames >= failFrame) {
        s.fail();
        return false;
    }
    if (s.context && (s.renderW != f.renderWidth || s.renderH != f.renderHeight ||
                      s.displayW != f.displayWidth || s.displayH != f.displayHeight))
        s.destroyContext();    // resize
    if (!s.context && !s.create(getOrCreateEngineVulkanContext(), f)) {
        s.fail();
        return false;
    }
    if (s.sceneImage != f.color.image || s.depthImage != f.depth.image) {   // mission load recreated the targets
        s.sceneImage = f.color.image; s.depthImage = f.depth.image;
        s.resetFrames = 2;
    }
    return true;
}

bool FsrUpscaler::dispatch(const UpscalerFrame& f) {
    if (!impl_ || !impl_->context || impl_->failed || !f.commandList) return false;
    Impl& s = *impl_;
    const VkCommandBuffer cb = static_cast<VkCommandBuffer>(f.commandList);
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
    const VkImage depthImage = reinterpret_cast<VkImage>(f.depth.image);
    b.image = depthImage;
    b.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
    b.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);

    const uint32_t kRead = FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
    ffxDispatchDescUpscale d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = cb;
    d.color = wrap(f.color, kRead);
    d.depth = wrap(f.depth, kRead);
    d.motionVectors = wrap(f.motionVectors, kRead);
    if (f.reactive.valid()) d.reactive = wrap(f.reactive, kRead);
    if (f.transparency.valid()) d.transparencyAndComposition = wrap(f.transparency, kRead);
    d.output = wrap(output(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    d.jitterOffset = {f.jitterPx[0], f.jitterPx[1]};
    d.motionVectorScale = {f.motionVectorScale[0], f.motionVectorScale[1]};
    d.renderSize = {f.renderWidth, f.renderHeight};
    d.upscaleSize = {f.displayWidth, f.displayHeight};
    d.frameTimeDelta = f.frameDeltaMs;
    d.preExposure = f.preExposure;
    d.reset = f.reset || s.resetFrames > 0;
    d.cameraNear = f.cameraNear; d.cameraFar = f.cameraFar; d.cameraFovAngleVertical = f.cameraFovY; d.viewSpaceToMetersFactor = 1.0f;
    if (s.resetFrames > 0) --s.resetFrames;
    const bool ok = s.fns.Dispatch(&s.context, &d.header) == FFX_API_RETURN_OK;

    b.image = depthImage;
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
    if (!ok) s.fail();
    return ok;
}

UpscalerImage FsrUpscaler::output() const {
    UpscalerImage o;
    if (!impl_ || !impl_->output.image) return o;
    o.image = reinterpret_cast<uint64_t>(impl_->output.image.get());
    o.view = reinterpret_cast<uint64_t>(impl_->output.view.get());
    o.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    o.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    o.width = impl_->displayW; o.height = impl_->displayH;
    return o;
}
}  // namespace

// releaseResources() frees the GPU objects before the device goes (present teardown).
RenderCore::IUpscalerProvider* fsrUpscaler() {
    static FsrUpscaler p;
    return &p;
}
}  // namespace mc2vk
#else
// The vendored ffx-api loader is DLL-only; the registry falls back to native AA.
namespace mc2vk {
RenderCore::IUpscalerProvider* fsrUpscaler() { return nullptr; }
}
#endif
