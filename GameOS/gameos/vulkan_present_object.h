// vulkan_present_object.h
//
// L6.6-COMPOSITE-PRESENT-SCAFFOLD -- the PERSISTENT Vulkan present object,
// extracted VERBATIM from the L5 throwaway probe (vulkan_swapchain_present.cpp).
// Reusable acquire/present/resize machinery with the present-semaphore-reuse fix
// (ONE render-finished semaphore PER SWAPCHAIN IMAGE, indexed by imageIndex).
//
// SCOPE / HARD BOUNDARY (supervisor ruling): this object owns its OWN SDL Vulkan
// window + surface + instance/device/swapchain -- a SEPARATE window, exactly like
// the L5 probe. It NEVER touches the GL game window, the GL swap path, or FBO-0.
// Full game-window Vulkan present is BLOCKED on L7 shared-image interop /
// ownership migration (ledgered wall in VULKAN-NEXT-SLICES.md L6.6).
//
// Whole header is inert unless MC2_VULKAN is defined (matches the TU it lives in;
// GL shipping build64 is byte-identical). Fail-soft at every step: any error logs
// a reason + returns false, never crashes.

#ifndef MC2_VULKAN_PRESENT_OBJECT_H
#define MC2_VULKAN_PRESENT_OBJECT_H

#ifdef MC2_VULKAN

#include <volk.h>
#include "VkBootstrap.h"

#include <cstdint>
#include <string>
#include <vector>

struct SDL_Window;

namespace mc2vk {

// Per-swapchain GPU resources that must be rebuilt on resize.
//
// SYNC NOTE (VULKAN-SWAPCHAIN-PRESENT-1, reused verbatim): the render-finished
// semaphore that the PRESENT waits on MUST be PER SWAPCHAIN IMAGE, not per
// frame-in-flight. Present completion (when the semaphore's wait is retired) is
// tied to the swapchain image, not to our host-side frame counter. With one
// shared render semaphore and N>1 images, vkQueueSubmit can re-signal that
// semaphore while a prior present still holds a pending wait on it -> the
// "semaphore is being signaled ... but it may still be in use by VkSwapchainKHR"
// hazard that sync validation flags. One render semaphore per image (indexed by
// the acquired imageIndex) closes it.
struct SwapchainResources {
    vkb::Swapchain             swapchain{};
    bool                       have = false;
    std::vector<VkImage>       images;
    std::vector<VkImageView>   views;
    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkSemaphore>   renderSems; // one per image (present-wait target)
};

// Health snapshot the present object accumulates (mirrors the L5 probe's).
struct PresentHealth {
    bool             swapchain_available = false;
    bool             surface_created     = false;
    VkFormat         format              = VK_FORMAT_UNDEFINED;
    VkPresentModeKHR present_mode        = VK_PRESENT_MODE_FIFO_KHR;
    uint32_t         image_count         = 0;
    uint32_t         extent_w            = 0;
    uint32_t         extent_h            = 0;
    uint32_t         resize_count        = 0;
    uint32_t         present_frames      = 0;   // frames presented VK_SUCCESS
    std::string      fallback_reason;           // empty == clean
    uint32_t         validation_errors   = 0;
};

// PERSISTENT Vulkan present object. init() stands up its OWN Vulkan window +
// swapchain (fail-soft); acquireAndPresentOneFrame() clears + draws the
// fullscreen triangle to the next swapchain image and presents it (handling
// OUT_OF_DATE/SUBOPTIMAL by an internal rebuild); handleResize() forces a
// rebuild to a new drawable size; destroy() tears everything down in reverse.
// Reusable across many frames -- no per-call window creation.
class VulkanPresentObject {
public:
    VulkanPresentObject() = default;
    ~VulkanPresentObject() { destroy(); }

    VulkanPresentObject(const VulkanPresentObject&) = delete;
    VulkanPresentObject& operator=(const VulkanPresentObject&) = delete;

    // Stand up window/surface/device/swapchain/pipeline. spvDir holds the
    // compiled fullscreen.{vert,frag}.spv. `hidden` forces an offscreen window
    // (headless CI). Returns false + sets health.fallback_reason on any error
    // (fail-soft: partial state is torn down before returning).
    bool init(const char* spvDir, bool hidden, uint32_t w, uint32_t h);

    // Present ONE frame (clear to teal + fullscreen triangle). Pumps SDL events
    // so the window stays responsive. Rebuilds the swapchain internally on
    // OUT_OF_DATE/SUBOPTIMAL. Returns false (fail-soft) on a hard error; sets
    // health.present_frames/resize_count as it goes.
    bool acquireAndPresentOneFrame();

    // Force a swapchain rebuild to `w`x`h` (e.g. after an SDL_SetWindowSize).
    // The next acquire/present would rebuild anyway on OUT_OF_DATE; this drives
    // it deterministically for the probe's mid-run resize. Fail-soft.
    bool handleResize(uint32_t w, uint32_t h);

    // Tear down in reverse order (safe to call repeatedly / on partial init).
    void destroy();

    bool valid() const { return device_ != VK_NULL_HANDLE && scr_.have; }
    SDL_Window* window() const { return window_; }
    const PresentHealth& health() const { return health_; }
    PresentHealth& mutableHealth() { return health_; }

private:
    bool rebuildSwapchain(uint32_t w, uint32_t h);

    // SDL / instance
    SDL_Window*                window_             = nullptr;
    bool                       sdlVideoInitedHere_ = false;
    bool                       sdlVulkanLoaded_    = false;
    vkb::Instance              vkbInstance_{};
    VkInstance                 instance_           = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT   messenger_          = VK_NULL_HANDLE;
    bool                       wantValidation_     = false;

    // device / queues
    VkSurfaceKHR               surface_            = VK_NULL_HANDLE;
    VkPhysicalDevice           phys_               = VK_NULL_HANDLE;
    VkDevice                   device_             = VK_NULL_HANDLE;
    VkQueue                    gfxQueue_           = VK_NULL_HANDLE;
    VkQueue                    presentQueue_       = VK_NULL_HANDLE;
    uint32_t                   gfxFamily_          = UINT32_MAX;
    uint32_t                   presentFamily_      = UINT32_MAX;

    // render pass / pipeline / shaders
    VkRenderPass               rpass_              = VK_NULL_HANDLE;
    VkPipelineLayout           playout_            = VK_NULL_HANDLE;
    VkPipeline                 pipe_               = VK_NULL_HANDLE;
    VkShaderModule             vert_               = VK_NULL_HANDLE;
    VkShaderModule             frag_               = VK_NULL_HANDLE;

    // command + per-frame sync (1 frame in flight)
    VkCommandPool              cpool_              = VK_NULL_HANDLE;
    VkCommandBuffer            cbuf_               = VK_NULL_HANDLE;
    VkSemaphore                acquireSem_         = VK_NULL_HANDLE; // render sems are per-image
    VkFence                    inFlight_           = VK_NULL_HANDLE;

    SwapchainResources         scr_;
    PresentHealth              health_;
};

}  // namespace mc2vk

#endif // MC2_VULKAN
#endif // MC2_VULKAN_PRESENT_OBJECT_H
