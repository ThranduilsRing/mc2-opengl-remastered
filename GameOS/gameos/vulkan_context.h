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
// The context is born once at engine init
// (gameosmain), fail-soft: any failure logs a reason, leaves the accessor
// returning a context whose device() is VK_NULL_HANDLE, and the caller forces
// the session back to GL. Never crashes.
//
// Whole header is inert unless MC2_VULKAN is defined (matches the TU); the
// GL-only shipping build never sees a Vulkan symbol from here.

#ifndef MC2_VULKAN_CONTEXT_H
#define MC2_VULKAN_CONTEXT_H

#ifdef MC2_VULKAN

#include <chrono>
#include <volk.h>
#include "vk_render_infra.h"

#include "vk_completion_tracker.h"
#include "deferred_submit_batch.h"
#include "vk_deferred_destroy.h"
#include "vk_device_profile.h"
#include "vk_frame_ring.h"
#include "vk_readback.h"
#include "vk_record_pools.h"
#include "vk_shared_light_feed.h"
#include "vk_upload.h"

#include <array>
#include <cstddef>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <source_location>
#include <mutex>
#include <string>
#include <string_view>

// VMA allocator handle (forward-declared to keep vk_mem_alloc.h out of this
// header; the .cpp includes it). VmaAllocator is a pointer typedef.
struct VmaAllocator_T;
using VmaAllocator = struct VmaAllocator_T*;

namespace RenderCore { class TextureService; }

namespace mc2vk {
class TextureResidency;

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

// The state an island keeps while its device objects exist. The context owns it (see VulkanContext::islandSlot), so an island's objects are released
// by the context ahead of the allocator and the device they were made on, whatever path ends the process: no island needs a static owner or a teardown
// call of its own for that.
struct IslandState {
    virtual ~IslandState() = default;
};
// One slot per island that has been moved under the context; the next converted island adds its name here.
enum class IslandSlot : size_t { Vfx, Sky, LegacyShape, Preview, Mech, ColorProp, Water, Shadow, Terrain, PassTimer, ActorCull, Vegetation, RtScene, Count };

// The standalone shared engine Vulkan context. Owns instance + physical device
// + logical device + graphics queue + VMA allocator + volk device dispatch.
// Reusable across the whole process; a single instance lives behind
// getOrCreateEngineVulkanContext().
class VulkanContext {
public:
    VulkanContext();
    ~VulkanContext();

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
    VkQueue          presentQueue()  const { return presentQueue_; }
    uint32_t         graphicsFamily() const { return gfxFamily_; }
    VmaAllocator     allocator()     const { return allocator_; }
    bool swapchainColorspaceEnabled() const { return swapchainColorspace_; }
    bool hdrMetadataEnabled() const { return hdrMetadata_; }

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
    // The ray-tracing tier selected for this device (tier none on a GPU without it: raster is unchanged) and the limits behind it. Optional capability:
    // the extensions and feature bits are enabled only for the parts the device lists; see profile::selectRt and the `[VK_RT v1]` line.
    const profile::RtSelection& rt() const { return rtSel_; }
    const profile::RtCaps& rtCaps() const { return rtCaps_; }
    // The optional extension tier actually enabled on this device (all parts 0 after an empty selection or a failed first device creation); no consumer yet.
    // See profile::selectExt and the `[VK_EXT v1]` line.
    const profile::ExtSelection& ext() const { return extSel_; }
    VkDriverId driverId() const { return driverId_; }   // VK_DRIVER_ID_MAX_ENUM when the 1.2 driver query was unavailable
    // True when the optional features FidelityFX FSR needs were enabled (shaderInt16, shaderFloat16, 16-bit storage, separateDepthStencilLayouts).
    bool temporalFeaturesEnabled() const { return temporalFeatures_; }
    profile::Route route(std::string_view item) const { return profile::routeOf(profile_, item); }

    // Records a synchronization2 barrier through the entry point of the device's route: the core 1.3 name, or the KHR name on a 1.2 device that has the
    // extension. The ruling makes the facility required, so the device always has one of the two.
    void cmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo& dep) const {
        if (route("synchronization2") == profile::Route::Core) vkCmdPipelineBarrier2(cb, &dep);
        else vkCmdPipelineBarrier2KHR(cb, &dep);
    }

    // vkCmdBeginRendering / vkCmdEndRendering through the entry point of the device's dynamic rendering route (core 1.3 or the KHR extension).
    void cmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo& info) const {
        if (route("dynamic_rendering") == profile::Route::Core) vkCmdBeginRendering(cb, &info);
        else vkCmdBeginRenderingKHR(cb, &info);
    }
    void cmdEndRendering(VkCommandBuffer cb) const {
        if (route("dynamic_rendering") == profile::Route::Core) vkCmdEndRendering(cb);
        else vkCmdEndRenderingKHR(cb);
    }

    // vkQueueSubmit2 through the entry point of the device's route (core 1.3 or KHR); the submit funnel is its only caller.
    VkResult queueSubmit2(VkQueue q, const VkSubmitInfo2& si, VkFence fence) const;   // defined in vulkan_context.cpp (the funnel's own file)

    const EngineContextHealth& health() const { return health_; }
    EngineContextHealth&       mutableHealth() { return health_; }

    // Device-local memory as VMA sees it, summed over the device-local heaps: the OS budget, the process usage and the bytes handed out by
    // allocations. With VK_EXT_memory_budget enabled (requested whenever the device has it) the budget and usage are the OS's numbers; without
    // it they are VMA's own estimate. False when no allocator is live.
    bool deviceLocalMemory(uint64_t& budgetBytes, uint64_t& usageBytes, uint64_t& allocatedBytes) const;

    // The ring of per-frame LightsData buffers the mech and color-prop islands share. It lives here because it belongs to no island: destroy() frees it
    // ahead of the allocator and the device it was made on.
    TextureResidency& textures() { return *textures_; }
    SharedLightFeed& lightFeed() { return lightFeed_; }

    // The slot an island parks its state in. The island creates and fills it; destroy() idles the GPU and releases every parked island, in slot order.
    std::unique_ptr<IslandState>& islandSlot(IslandSlot slot) { return islands_[static_cast<size_t>(slot)]; }
    // Idles the GPU, then releases one parked island (for an island that rebuilds itself, e.g. on a resize). Safe on an empty slot.
    void retireIsland(IslandSlot slot);

    // Render submit path of the engine device. Graphics work goes through this funnel (or the dedicated fence-owned Present tail) and none through a raw vkQueueSubmit (a contract row
    // greps for it): the submit gets the next value of the graphics timeline semaphore as one more signal, so a GpuTicket proves "everything submitted
    // up to here has finished". `si` keeps its binary waits and signals; `fence` (may be null) is passed through unchanged. `si.pNext` must be empty.
    // On success *outValue (when given) is the timeline value this submit signals. Thread-safe.
    VkResult submit(const VkSubmitInfo& si, VkFence fence, uint64_t* outValue = nullptr) const;
    // Blit-only continuation: no Render batch, uploads, frame timeline or allocators. Its dedicated fence owns its lifetime.
    VkResult submitPresentTail(VkCommandBuffer cb, VkSemaphore acquire, VkSemaphore rendered, VkFence fence,
                              VkPipelineStageFlags2 acquireStage = VK_PIPELINE_STAGE_2_TRANSFER_BIT) const;
    // WSI has its own queue lock when a second graphics-family queue is available.
    VkResult present(VkQueue queue, const VkPresentInfoKHR& info) const;
    // Puts the batched plain submits on the queue now. Call it after a big pass when more CPU recording follows: the age check in submit() runs only
    // on the next plain submit, so without a kick the GPU idles until then. Queue order is unchanged. Render owner only.
    void kick() const;
private:
    VkResult submitLocked(const VkSubmitInfo& si, VkFence fence, uint64_t* outValue) const;   // submitMutex_ held
    // Plain submits (command buffers only: no fence, no binary semaphores) are collected and go out as ONE vkQueueSubmit2 with one timeline
    // signal (the newest value) at the next flush point: a submit that carries a fence or a semaphore, present, poll (so isComplete/wait),
    // waitIdle and endFrame. Queue order equals call order; the older tickets complete with the batch.
    void flushPendingLocked() const;   // submitMutex_ held; resolves reserved recordings first (never on the present worker: the present enqueue resolves on main; a reservation left on the worker is flagged and only the prefix before it is queued)
    void flushPrefixLocked(size_t count) const;   // submitMutex_ held: queue the first `count` pending entries (signals the value of the last)
    void resolveRecordsLocked() const;            // submitMutex_ held
    GpuTicket flushUploadsLocked() const;
public:
    CompletionTracker&       tracker()       { return tracker_; }
    const CompletionTracker& tracker() const { return tracker_; }
    // Refreshes the completed counter from the timeline semaphore (cheap; once per frame begin is enough).
    void poll() const;
    bool isComplete(const GpuTicket& t) const;
    // Blocks until the ticket completes or the timeout passes. A timeout or a lost device is logged with `why`; returns whether the ticket is complete.
    bool wait(const GpuTicket& t, uint64_t timeoutNs, std::string_view why) const;
    // Non-blocking host sync point for a completed ticket: vkWaitSemaphores with a zero timeout. True once the host observed the ticket through a
    // wait, the only completion the validation layers and the readback contract recognise (a counter poll is not one). A readback slot is read or
    // recycled only after this returned true; a ticket still in flight returns false at once.
    bool hostSynced(const GpuTicket& t, std::string_view why) const;
    // Frame bracket: beginFrame polls the counter, opens a frame, collects the deferred-destroy entries that completed and restarts the frame-ring slot
    // (a call on an open frame does nothing); endFrame seals the frame with the graphics value submitted so far and emits the [VK_TIMELINE v1] summary at
    // its cadence. A host that presents calls frameBoundary() right after the present: endFrame, then beginFrame of the next frame.
    void beginFrame();
    void endFrame();
    void frameBoundary() { endFrame(); beginFrame(); }
    // The newest sealed frame whose graphics work completed (0 when none within the tracker's ring). Frames complete in order, so everything older is done too.
    uint64_t completedFrame() const {
        poll();
        const uint64_t open = tracker_.frameNo();
        for (uint64_t g = open > 0 ? open - 1 : 0; g > 0 && open - g < CompletionTracker::kFrameRing; --g)
            if (tracker_.isComplete(tracker_.frameTicket(g))) return g;
        return 0;
    }

    // Per-frame transient memory (vertex data, dynamic-offset uniform and storage rows, copy sources): host-visible, written through the returned pointer,
    // valid until the frame that asked for it has completed. `align` 0 = the device uniform / storage offset alignment. An empty result means the
    // allocation failed; the caller declines its draw. Main thread only.
    FrameAlloc frameAlloc(VkDeviceSize size, VkDeviceSize align = 0) { return frameRing_.alloc(*this, size, align ? align : frameAlign_); }
    // The frame the engine loop is recording, the active frame count and each per-frame object's slot (frame % framesInFlight; 0 at N=1).
    // The transient memory ring always rotates through kMaxFramesInFlight regions.
    struct FrameContext { uint64_t frame = 0; uint32_t slot = 0; uint32_t framesInFlight = 1; };
    FrameContext frameContext() const { return {tracker_.frameNo(), framesInFlight_ < 2 ? 0u : static_cast<uint32_t>(tracker_.frameNo() % framesInFlight_), framesInFlight_}; }
    void setFramesInFlight(uint32_t n) { framesInFlight_ = n < 1 ? 1u : n > kMaxFramesInFlight ? kMaxFramesInFlight : n; }
    const FrameRing& frameRing() const { return frameRing_; }
    // GPU -> CPU readbacks that never block (see vk_readback.h).
    ReadbackRing& readbacks() { return readbacks_; }
    // Streaming uploads (see vk_upload.h): stage into the frame ring and queue the copy; it is submitted ahead of the next graphics submit, no CPU wait.
    UploadBatch& uploads() { return uploads_; }
    const UploadBatch& uploads() const { return uploads_; }
    GpuTicket flushUploads() const;   // submitted batch's ticket; empty on failure or no queued uploads
    VkBuffer frameRingBuffer() { return frameRing_.ensureBuffer(*this); }

    // Parallel recording: the next primary command buffer of the CALLING thread's lane (one command pool per participant per frame-in-flight), in its
    // initial state and valid until the frame slot restarts. Any thread while a frame records; VK_NULL_HANDLE when the thread has no lane or the pool
    // failed (record on main instead). The caller keeps the buffers in its own canonical order and appends them to the submit batch from main.
    VkCommandBuffer acquireRecordCommandBuffer();

    // Recorded islands: an island whose inputs are frozen (staged into the frame ring and its descriptor sets on main) hands its recording over as a
    // callback. submitRecorded() reserves the island's place in the deferred submit batch and its timeline value NOW (queue order = call order, as
    // with submit()), and the recording runs later, at the next flush point (poll, wait, waitIdle, endFrame, a submit that carries a fence or a
    // semaphore), on a Render-domain parallel_for: every callback of the batch records into acquireRecordCommandBuffer() of the thread it runs on,
    // and main fills the batch with the finished buffers in reserved order. Without an active executor (MC2_FRAME_JOBS=0, no pool) the callback runs
    // inline on main and submits at once. `rec` runs on any thread: it may touch only what the island captured by value or owns (pipelines,
    // descriptor sets, the frame-ring bytes it was given); it must not submit, poll, wait or queue uploads. `done(ok)` runs on the flushing thread
    // (main in the engine), in reserved order, once the callback returned (ok = recorded; inline mode: and submitted); it must not call back into
    // this context. Main thread only.
    using RecordFn = std::function<bool(VkCommandBuffer)>;
    using RecordDoneFn = std::function<void(bool ok)>;
    struct RecordExecutor {
        bool (*active)() = nullptr;                                               // true while worker threads exist
        void (*run)(int count, const std::function<void(int)>& body) = nullptr;   // body(i) for i in [0,count), any order, joined before return
    };
    void setRecordExecutor(const RecordExecutor& ex) { recordExecutor_ = ex; }
    VkResult submitRecorded(RecordFn rec, RecordDoneFn done, uint64_t* outValue);
    // Render thread (a no-op on the present worker): record everything reserved so far (does not submit). Called before a per-frame slot closes (pass timer, GPU profiler).
    void resolveRecorded();
    VkDeviceSize frameAlignment() const { return frameAlign_; }
    VkDeviceSize frameRingRange() const { return frameRing_.bindRange(); }

    // Parks an owner (a buffer, an image, a container of them) until the GPU has finished the frame that may use it, then destroys it: the replacement
    // for "waitIdle, then reset". Render owner only (Main or the unsealed present worker). Owners must be move-only types or movable values.
    template <class Owner>
    void retire(Owner&& owner, uint64_t bytes = 0) {
        mc2::overlap::requireOwner(mc2::overlap::Owner::Render, "VulkanContext::retire");
        infra::retiring(owner);
        deferred_.retire(tracker_, std::forward<Owner>(owner), bytes);
    }
    // Mission unload, swapchain recreate, teardown: idles the device and destroys everything parked. The only sanctioned device-idle besides init failure.
    void drainDeferred(std::string_view why);
    const DeferredDestroy& deferred() const { return deferred_; }

    // Waits for the device to drain before the caller drops objects that may be in flight, and logs a failed wait (a lost device). A no-op without a device.
    void waitIdle(std::string_view why) const;

    // Emit the [VK_ENGINE_CONTEXT] health line (device_name / validation_errors
    // / created / fallback_reason) to stderr for the T5 device-birth proof.
    void logHealth() const;

private:
    bool             initedVolk_        = false;
    VkInstance       instance_          = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice phys_              = VK_NULL_HANDLE;
    VkDevice         device_            = VK_NULL_HANDLE;
    bool swapchainColorspace_ = false;
    bool hdrMetadata_ = false;
    VkQueue          gfxQueue_          = VK_NULL_HANDLE;
    VkQueue          presentQueue_      = VK_NULL_HANDLE;
    uint32_t         gfxFamily_         = UINT32_MAX;
    VmaAllocator     allocator_         = nullptr;
    profile::Evaluation profile_ = [] { profile::Evaluation e; e.accepted = false; return e; }();   // the chosen device's verdict (see profile())
    profile::RtSelection rtSel_;   // the ray-tracing tier (see rt()); owns the extension name strings devExts points at
    profile::RtCaps rtCaps_;
    profile::ExtSelection extSel_;   // the optional extension tier (see ext()); owns the extension name strings devExts points at
    VkDriverId driverId_ = VK_DRIVER_ID_MAX_ENUM;
    VkSemaphore      gfxTimeline_       = VK_NULL_HANDLE;   // the graphics queue's timeline: signalled with CompletionTracker::noteSubmit values
    mutable CompletionTracker tracker_;
    mutable uint64_t hostSyncedGfx_ = 0;   // highest graphics value the host reached through vkWaitSemaphores / vkDeviceWaitIdle (see hostSynced)
    FrameRing        frameRing_;
    RecordPools<VkCommandBuffer> recordPools_;
    std::array<std::array<OwnedCommandPool, kMaxRecordParticipants>, kMaxFramesInFlight> recordCmdPools_;   // [slot][participant]; a lane's pool is touched by its owner thread only
    uint32_t         framesInFlight_ = 1;
    mutable UploadBatch uploads_;
    ReadbackRing     readbacks_;
    VkDeviceSize     frameAlign_ = 256;   // max(uniform, storage offset alignment, 16): set at device birth
    mutable DeferredDestroy deferred_;   // after the allocator's owners are gone: destroy() drains it first
    // timed: main pumps window messages while the present worker holds it in WSI
    mutable std::timed_mutex submitMutex_;
    mutable std::timed_mutex presentMutex_;
    mutable DeferredSubmitBatch<VkCommandBuffer> batch_;   // submitMutex_: the deferred batch in submit order (the submit signals its last entry's value) and the reserved recordings
    RecordExecutor recordExecutor_;
    mutable bool pendingFailed_ = false;                 // submitMutex_: a batched submit failed after its callers were told VK_SUCCESS; the next submit() reports it
    uint8_t          luid_[8]           = {0, 0, 0, 0, 0, 0, 0, 0};
    bool             luidValid_         = false;
    bool             temporalFeatures_  = false;   // see temporalFeaturesEnabled()

    EngineContextHealth health_;
    std::unique_ptr<RenderCore::TextureService> textureServiceOwner_;
    std::unique_ptr<TextureResidency> textures_;
    SharedLightFeed lightFeed_;
    std::array<std::unique_ptr<IslandState>, static_cast<size_t>(IslandSlot::Count)> islands_;
};

// Object names are retained in crash breadcrumbs in every build; debug-utils publication additionally requires a validation run.
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
    std::string full(name);
    if (i0 >= 0) full += "[" + std::to_string(i0) + "]";
    if (i1 >= 0) full += "[" + std::to_string(i1) + "]";
    nameObject(device, handle, full);
}

// Process-wide accessor for the shared engine context. Returns the same object
// on every call. The first call does NOT auto-init -- call init() explicitly at
// engine init (gameosmain) so init order + fail-soft coercion are controlled by
// the caller. Never returns null.
VulkanContext& getOrCreateEngineVulkanContext(std::source_location site = std::source_location::current());

// The same object without the present-seal wait. For a recording callback on a pool worker (VulkanContext::submitRecorded), which must never block on the
// present worker: the render thread holds the submit lock while the callbacks run, and the present worker needs it to seal.
VulkanContext& engineVulkanContextNoWait();

// Convenience: true if the engine context has been born and holds a live device.
bool engineVulkanContextValid(std::source_location site = std::source_location::current());

// Submits on the shared engine context's graphics queue through the completion tracker (VulkanContext::submit). The only way an engine TU submits.
inline VkResult submitGraphics(const VkSubmitInfo& si, VkFence fence) { return getOrCreateEngineVulkanContext().submit(si, fence); }

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
