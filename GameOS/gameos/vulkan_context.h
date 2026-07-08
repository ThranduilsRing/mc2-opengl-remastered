// vulkan_context.h
//
// VK-P13-SELECTOR-1 / T2 -- the PERSISTENT, STANDALONE, SHARED engine Vulkan
// device/context. This is the durable P13 deliverable.
//
// ACCEPTANCE CRITERION (VK-P13-SELECTOR-PLAN §7 T2, point 2): the VkInstance /
// VkPhysicalDevice / VkDevice / queues are owned HERE, in this shared context --
// NOT inside VulkanPresentObject. P6/P8/P9/P14 all reuse the SAME device by
// asking getOrCreateEngineVulkanContext(). VulkanPresentObject (wired later in
// T3) takes this context as INPUT and must not create its own device for the
// engine path. A grep for VkDevice ownership must land here.
//
// The context is born once at engine init when renderBackend()==Vulkan
// (gameosmain), fail-soft: any failure logs a reason, leaves the accessor
// returning a context whose device() is VK_NULL_HANDLE, and the caller forces
// the session back to GL. Never crashes.
//
// Whole header is inert unless MC2_VULKAN is defined (matches the TU); the
// GL-only shipping build never sees a Vulkan symbol from here.

#ifndef MC2_VULKAN_CONTEXT_H
#define MC2_VULKAN_CONTEXT_H

#ifdef MC2_VULKAN

#include <volk.h>

#include <cstdint>
#include <string>

// VMA allocator handle (forward-declared to keep vk_mem_alloc.h out of this
// header; the .cpp includes it). VmaAllocator is a pointer typedef.
struct VmaAllocator_T;
typedef struct VmaAllocator_T* VmaAllocator;

namespace mc2vk {

// Health snapshot for the [VK_ENGINE_CONTEXT] diagnostic line + fail-soft trace.
struct EngineContextHealth {
    bool         created            = false; // device successfully born
    bool         attempted          = false; // init() was called at least once
    char         deviceName[256]    = {0};   // VkPhysicalDeviceProperties.deviceName
    bool         validationEnabled  = false;
    unsigned long validationErrors  = 0;     // ERROR-severity validation msgs
    std::string  fallbackReason;             // empty == clean; set on any failure
};

// The standalone shared engine Vulkan context. Owns instance + physical device
// + logical device + graphics queue + VMA allocator + volk device dispatch.
// Reusable across the whole process; a single instance lives behind
// getOrCreateEngineVulkanContext().
class VulkanContext {
public:
    VulkanContext() = default;
    ~VulkanContext() { destroy(); }

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    // Bring up instance/device/queue/VMA. Idempotent: a second call after a
    // successful init is a no-op returning true; after a failed init it returns
    // false without retrying. Reads MC2_VULKAN_VALIDATION for the validation
    // preset (same convention as the islands). `wantInterop` requests the
    // external-memory/-semaphore Win32 device extensions so P6/P8/P14 interop
    // can import/export against this device. `wantPresent` (VK-P13 T3) requests
    // the surface (VK_KHR_surface + VK_KHR_win32_surface instance) + swapchain
    // (VK_KHR_swapchain device) extensions so the shared device can drive the
    // hidden-window VK present path. Both are additive and off unless requested.
    // Returns false + sets health.fallbackReason on any error (fail-soft: any
    // partial state is torn down before returning).
    bool init(bool wantInterop, bool wantPresent = false);

    // Tear down in reverse order (safe on partial init / repeated calls).
    void destroy();

    // True once a device was successfully born and not yet destroyed.
    bool valid() const { return device_ != VK_NULL_HANDLE; }

    // Accessors for downstream consumers (present object, interop islands).
    VkInstance       instance()      const { return instance_; }
    VkPhysicalDevice physicalDevice() const { return phys_; }
    VkDevice         device()        const { return device_; }
    VkQueue          graphicsQueue() const { return gfxQueue_; }
    uint32_t         graphicsFamily() const { return gfxFamily_; }
    VmaAllocator     allocator()     const { return allocator_; }

    const EngineContextHealth& health() const { return health_; }
    EngineContextHealth&       mutableHealth() { return health_; }

    // Emit the [VK_ENGINE_CONTEXT] health line (device_name / validation_errors
    // / created / fallback_reason) to stderr for the T5 device-birth proof.
    void logHealth() const;

private:
    bool             initedVolk_        = false;
    VkInstance       instance_          = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice phys_              = VK_NULL_HANDLE;
    VkDevice         device_            = VK_NULL_HANDLE;
    VkQueue          gfxQueue_          = VK_NULL_HANDLE;
    uint32_t         gfxFamily_         = UINT32_MAX;
    VmaAllocator     allocator_         = nullptr;

    EngineContextHealth health_;
};

// Process-wide accessor for the shared engine context. Returns the same object
// on every call. The first call does NOT auto-init -- call init() explicitly at
// engine init (gameosmain) so init order + fail-soft coercion are controlled by
// the caller. Never returns null.
VulkanContext& getOrCreateEngineVulkanContext();

// Convenience: true if the engine context has been born and holds a live device.
bool engineVulkanContextValid();

// VK-P9a WATER-INTEROP: on-demand shared-context birth DECOUPLED from present.
// Lazy + idempotent: returns true if the shared context is already valid; else
// brings it up once via init(wantInterop=true, wantPresent=false) -- the SAME
// shared device (one volkLoadDevice), interop exts but no surface/swapchain, so a
// Native VK regions can run before the present path owns the backend.
// WITHOUT MC2_RENDER_BACKEND=vulkan and without a healthy present/window. init()
// is one-shot (no retry after failure). Fail-soft: false -> caller uses GL.
bool ensureEngineVulkanContextForInterop();

}  // namespace mc2vk

#endif  // MC2_VULKAN
#endif  // MC2_VULKAN_CONTEXT_H
