// vulkan_context.h
//
// VK-P13-SELECTOR-1 / T2 -- the PERSISTENT, STANDALONE, SHARED engine Vulkan
// device/context. This is the durable P13 deliverable.
//
// ACCEPTANCE CRITERION (VK-P13-SELECTOR-PLAN §7 T2, point 2): the VkInstance /
// VkPhysicalDevice / VkDevice / queues are owned HERE, in this shared context.
// Consumers (engine_present_vk and the islands) borrow it by asking
// getOrCreateEngineVulkanContext() and must not create their own device.
// A grep for VkDevice ownership must land here.
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

#include "vk_device_profile.h"
#ifdef MC2_VULKAN_ISLAND
#include "vk_shared_light_feed.h"
#endif

#include <array>
#include <cstddef>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

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
    std::atomic<unsigned long> validationErrors{0};   // ERROR-severity validation msgs; the layer calls back from any driver thread
    std::string  fallbackReason;             // empty == clean; set on any failure
    std::string  profileLine;                // the [VK_PROFILE v1] line of the device the profile chose (or refused); empty before device selection
};

#ifdef MC2_VULKAN_ISLAND
// The state an island keeps while its device objects exist. The context owns it (see VulkanContext::islandSlot), so an island's objects are released
// by the context ahead of the allocator and the device they were made on, whatever path ends the process: no island needs a static owner or a teardown
// call of its own for that.
struct IslandState {
    virtual ~IslandState() = default;
};
// One slot per island that has been moved under the context; the next converted island adds its name here.
enum class IslandSlot : size_t { Vfx, Sky, Count };
#endif

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
    // preset (same convention as the islands). VK-RUNG8-ITEM3 removed the
    // `wantInterop` external-memory/-semaphore Win32 extension family (the last
    // MoltenVK/Metal-gate blocker): every VK resource is now VK-only. `wantPresent`
    // (VK-P13 T3) requests the surface (VK_KHR_surface + VK_KHR_win32_surface
    // instance) + swapchain (VK_KHR_swapchain device) extensions so the shared
    // device can drive the hidden-window VK present path. Additive and off unless
    // requested. Returns false + sets health.fallbackReason on any error (fail-soft:
    // any partial state is torn down before returning).
    bool init(bool wantPresent = false);

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

    // What the device profile decided for the device this context chose: every facility's route, the feature bits and device extensions that were
    // enabled. The five facilities of the owner ruling (dynamic rendering, synchronization2, timeline semaphores, descriptor indexing, buffer device
    // address) are required, so a valid context always has them; what an island still has to ask is the ROUTE (core, or the KHR extension on a 1.2
    // device: the entry points to load differ) and whether an optional extension (memory budget) is on. Empty and not accepted before a device was
    // chosen, after a refusal and after destroy().
    const profile::Evaluation& profile() const { return profile_; }

    // The Windows adapter LUID of the chosen device (8 bytes, the layout of a LUID), so a counter that is per DXGI adapter reads the adapter this device
    // runs on and not a guess. False when the driver reports none or no device was chosen.
    bool adapterLuid(uint8_t (&out)[8]) const {
        if (!luidValid_) return false;
        for (size_t i = 0; i < sizeof(out); ++i) out[i] = luid_[i];
        return true;
    }
    bool featureEnabled(std::string_view name) const { return profile::featureEnabled(profile_, name); }
    profile::Route route(std::string_view item) const { return profile::routeOf(profile_, item); }

    // Records a synchronization2 barrier through the entry point of the device's route: the core 1.3 name, or the KHR name on a 1.2 device that has the
    // extension. The ruling makes the facility required, so the device always has one of the two.
    void cmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo& dep) const {
        if (route("synchronization2") == profile::Route::Core) vkCmdPipelineBarrier2(cb, &dep);
        else vkCmdPipelineBarrier2KHR(cb, &dep);
    }

    const EngineContextHealth& health() const { return health_; }
    EngineContextHealth&       mutableHealth() { return health_; }

    // Device-local memory as VMA sees it, summed over the device-local heaps: the OS budget, the process usage and the bytes handed out by
    // allocations. With VK_EXT_memory_budget enabled (requested whenever the device has it) the budget and usage are the OS's numbers; without
    // it they are VMA's own estimate. False when no allocator is live.
    bool deviceLocalMemory(uint64_t& budgetBytes, uint64_t& usageBytes, uint64_t& allocatedBytes) const;

#ifdef MC2_VULKAN_ISLAND
    // The ring of per-frame LightsData buffers the mech and color-prop islands share. It lives here because it belongs to no island: destroy() frees it
    // ahead of the allocator and the device it was made on.
    SharedLightFeed& lightFeed() { return lightFeed_; }

    // The slot an island parks its state in. The island creates and fills it; destroy() idles the GPU and releases every parked island, in slot order.
    std::unique_ptr<IslandState>& islandSlot(IslandSlot slot) { return islands_[static_cast<size_t>(slot)]; }
    // Idles the GPU, then releases one parked island (for an island that rebuilds itself, e.g. on a resize). Safe on an empty slot.
    void retireIsland(IslandSlot slot);
#endif

    // Waits for the device to drain before the caller drops objects that may be in flight, and logs a failed wait (a lost device). A no-op without a device.
    void waitIdle(const char* why) const;

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
    profile::Evaluation profile_ = [] { profile::Evaluation e; e.accepted = false; return e; }();   // the chosen device's verdict (see profile())
    uint8_t          luid_[8]           = {0, 0, 0, 0, 0, 0, 0, 0};
    bool             luidValid_         = false;

    EngineContextHealth health_;
#ifdef MC2_VULKAN_ISLAND
    SharedLightFeed lightFeed_;
    std::array<std::unique_ptr<IslandState>, static_cast<size_t>(IslandSlot::Count)> islands_;
#endif
};

// Object names for the validation layer and the capture tools: a message that says `VkImage 0x310000000031` is decoded by creation order, one that says
// `shared.scene.normal` is not. The names are live only while the debug-utils messenger is (a validation run); otherwise every call returns at once.
// `name` comes from the creating site and says what the object is for.
bool debugNamesEnabled();
void setObjectName(VkDevice device, VkObjectType type, uint64_t handle, std::string_view name);
constexpr VkObjectType objectTypeOf(VkImage)    { return VK_OBJECT_TYPE_IMAGE; }
constexpr VkObjectType objectTypeOf(VkBuffer)   { return VK_OBJECT_TYPE_BUFFER; }
constexpr VkObjectType objectTypeOf(VkPipeline) { return VK_OBJECT_TYPE_PIPELINE; }

// `nameObject(device, image, "terrain.normal")`.
template <class Handle>
inline void nameObject(VkDevice device, Handle handle, std::string_view name) {
    setObjectName(device, objectTypeOf(handle), reinterpret_cast<uint64_t>(handle), name);
}

// The same for an object that exists once per slot: `name[i0]` or `name[i0][i1]`; a negative index is left out.
template <class Handle>
inline void nameObjectAt(VkDevice device, Handle handle, std::string_view name, int i0 = -1, int i1 = -1) {
    if (!debugNamesEnabled()) return;
    std::string full(name);
    if (i0 >= 0) full += "[" + std::to_string(i0) + "]";
    if (i1 >= 0) full += "[" + std::to_string(i1) + "]";
    nameObject(device, handle, full);
}

// Process-wide accessor for the shared engine context. Returns the same object
// on every call. The first call does NOT auto-init -- call init() explicitly at
// engine init (gameosmain) so init order + fail-soft coercion are controlled by
// the caller. Never returns null.
VulkanContext& getOrCreateEngineVulkanContext();

// Convenience: true if the engine context has been born and holds a live device.
bool engineVulkanContextValid();

// On-demand shared-context birth DECOUPLED from present. Lazy + idempotent: returns
// true if the shared context is already valid; else brings it up once via
// init(wantPresent=false) -- the SAME shared device (one volkLoadDevice), no surface/
// swapchain, so native VK regions can run before the present path owns the backend,
// WITHOUT MC2_RENDER_BACKEND=vulkan and without a healthy present/window. init() is
// one-shot (no retry after failure). Fail-soft: false -> caller uses GL.
// VK-RUNG8-ITEM3: renamed from ensureEngineVulkanContext -- there is no
// interop extension family anymore; the shared device is Vulkan 1.2 or newer (see vk_device_profile.h for the floor).
bool ensureEngineVulkanContext();

}  // namespace mc2vk

#endif  // MC2_VULKAN
#endif  // MC2_VULKAN_CONTEXT_H
