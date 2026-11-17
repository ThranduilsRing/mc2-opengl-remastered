// Native Vulkan presentation on the game window.
// Borrows the engine device and owns the swapchain and frame synchronization.

#ifndef MC2_ENGINE_PRESENT_VK_H
#define MC2_ENGINE_PRESENT_VK_H

#ifdef MC2_VULKAN

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct SDL_Window;
struct ImDrawData;

namespace mc2vk {

class VulkanContext;

// Health snapshot for the [VK_PRESENT] diagnostic line (T5 proof).
struct EnginePresentHealth {
    bool        created           = false; // present path stood up successfully
    bool        attempted         = false;
    uint32_t    present_frames    = 0;     // frames handed to vkQueuePresentKHR OK
    uint32_t    resize_count      = 0;
    // Swapchain resize/recreate recovery receipts (VK-SWAPCHAIN-RESIZE-HARDEN-1).
    // resize_recreate_ok   : successful swapchain (re)creates (first-try or retry).
    // resize_recreate_retries : extra create attempts beyond the first that a
    //                           single recreate needed (transient window states /
    //                           the old-chain-alive NATIVE_WINDOW_IN_USE retry).
    // resize_recreate_failsoft : recreates that exhausted all attempts and fell
    //                            back to GL. Nonzero == the hardening did not fully
    //                            cover some window state; investigate.
    uint32_t    resize_recreate_ok       = 0;
    uint32_t    resize_recreate_retries  = 0;
    uint32_t    resize_recreate_failsoft = 0;
    uint32_t    validation_errors = 0;     // pulled from the engine ctx messenger
    uint32_t    extent_w          = 0;
    uint32_t    extent_h          = 0;
    int         present_mode      = 0;     // VkPresentModeKHR as int (for logging)
    std::string present_mode_name;         // "FIFO" | "MAILBOX" | "IMMEDIATE" ...
    // Content-verification: rolling checksum of copy-1 readback (T5 "real frame"
    // proof). last != prev across frames => the presented content is live.
    unsigned long long last_readback_checksum = 0;
    unsigned long long prev_readback_checksum = 0;
    bool        readback_changed  = false; // last != prev at least once
    uint32_t    native_composite_requests = 0;   // native composite attempts
    uint32_t    native_composite_grants   = 0;   // attempts accepted by the native owner
    uint32_t    copy1_blit_frames   = 0;   // legacy FBO-0 -> shared-image blit
    uint32_t    copy1_skip_frames   = 0;   // direct shared-FBO frames
    uint32_t    gl_handoff_skip_frames = 0;// explicit VK-owned present-image frames
    uint32_t    tail_hook_frames    = 0;   // gated VK final-tail hook executions
    uint32_t    imgui_vk_frames     = 0;   // native ImGui draw-data frames recorded
    uint32_t    composite_vk_frames = 0;   // native composite frames recorded
    uint32_t    shadow_debug_vk_frames = 0;// native CSM shadow-debug overlay frames
    uint32_t    hud_vk_queued_frames = 0;  // HUD batches queued for native tail replay
    uint32_t    hud_vk_consumed_frames = 0;// queued HUD batches observed by the tail hook
    uint32_t    hud_vk_draws_last = 0;     // draw records in the last queued HUD batch
    uint32_t    hud_vk_textured_last = 0;  // draw records with texture metadata
    uint32_t    hud_vk_serialized_last = 0;// textured draw records with content serials
    uint32_t    hud_vk_color_frames = 0;   // proof-only native untextured HUD frames
    uint32_t    hud_vk_color_draws_last = 0;
    uint32_t    hud_vk_textured_frames = 0;
    uint32_t    hud_vk_textured_draws_last = 0;
    uint32_t    hud_vk_texture_uploads = 0;
    bool        imgui_vk_ready      = false;
    bool native_composite_armed = false;   // most recent native composite readiness
    // VK-QWI-TO-FENCE-1 / VK-PRESENT-SINGLESUBMIT-1: water-style us_fence_wait
    // telemetry around the present path's ONLY CPU wait. History: QWI drain after
    // the composite submit -> per-submit compositeFence wait -> (single-submit) the
    // inFlight wait for the PREVIOUS frame's {composite, tail+blit} submission,
    // taken at composite-record time. Same [QWI-WAIT] receipt tag throughout so
    // before/after greps line up.
    double      composite_wait_us_last = 0.0;
    double      composite_wait_us_accum = 0.0;
    uint32_t    composite_wait_samples = 0;
    std::string fallback_reason;           // empty == clean
};

struct VkHudVertex {
    float x = 0.0f, y = 0.0f, z = 0.0f, rhw = 1.0f;
    uint32_t argb = 0xffffffffu;
    uint32_t frgb = 0xffffffffu;
    float u = 0.0f, v = 0.0f;
};

struct VkHudDraw {
    uint32_t kind = 0;
    uint32_t textureHandle = 0;
    uint32_t textureWidth = 0;
    uint32_t textureHeight = 0;
    uint32_t textureFormat = 0;
    uint32_t textureSerial = 0;
    uint32_t alphaMode = 0;
    uint32_t textureAddress = 0;
    uint32_t filter = 0;
    uint32_t foregroundColor = 0;
    // VK-HUD-MIRRORCOPY-1: shared, immutable view of the gosTexture CPU rgba
    // mirror (refcount bump instead of the former per-draw full-atlas deep copy —
    // several MB memcpy + ~80 heap alloc/free pairs per frame, >99.9% never read;
    // the consumer only touches bytes on a (handle,serial) change). The pointee
    // is never mutated after publication: gosTexture::setRgbaMirror() swaps in a
    // FRESH vector on every content change, so draws queued against an older
    // serial keep their exact bytes even across a mid-frame re-bake or
    // gos_DestroyTexture/txmmgr cache-out. null == no mirror (was: empty()).
    std::shared_ptr<const std::vector<uint8_t>> textureRgba;
    std::vector<VkHudVertex> vertices;
};

// The engine VK present object. Reuses the T2 shared VulkanContext device;
// owns the surface and swapchain on the visible SDL game window.
class EnginePresentVk {
public:
    EnginePresentVk() = default;
    ~EnginePresentVk() { destroy(); }

    EnginePresentVk(const EnginePresentVk&) = delete;
    EnginePresentVk& operator=(const EnginePresentVk&) = delete;

    // Create the surface and swapchain on the visible SDL_WINDOW_VULKAN game
    // window, sized to its drawable extent. ctx must be present-capable.
    // Returns false and records fallback_reason on failure.
    bool init(VulkanContext& ctx, SDL_Window* window, uint32_t w, uint32_t h);

    // Present the native Vulkan frame; rebuild the swapchain on drawable resize.
    // verifyReadback folds the frame's readback checksum into health.
    // Returns false on a hard error.
    //
    // The present IS the frame boundary: every exit of this function (presented, skipped for a zero extent or a swapchain rebuild, declined, or
    // failed) ends with the context's frameBoundary(), so any host that presents through it (the game, the editor viewport) advances the frame
    // tracker, the frame ring and the deferred-destroy queue without a separate call.
    bool presentFrame(uint32_t drawableW, uint32_t drawableH,
                        bool verifyReadback);
    void setCaptureFrame(uint32_t renderFrame, uint32_t fireFrame);

    void destroy();

    bool valid() const { return created_; }

    // Start this frame's native composite only when the present image exists.
    bool beginNativeComposite();
    bool directPresentFrameArmed() const { return created_ && nativeCompositeArmed_; }

    // Rung 8.6 native ImGui renderer seam. These stay VK-header-free so
    // GuiRuntime can hand draw data to the present owner without learning Vulkan.
    bool prepareImguiVulkanFrame();
    bool prewarmPipelines();
    bool prepareHudTexture(const VkHudDraw& draw);
    void releaseHudTextures(uint32_t handle = 0); // zero releases the mission's entire HUD cache
    bool renderImguiVulkanDrawData(ImDrawData* drawData);
    bool imguiVulkanReady() const;
    void shutdownImguiVulkanRenderer();

    // Rung 8.6f HUD seam: queue an owned, already-scaled GameOS HUD batch for
    // present-tail native replay. Until the native renderer is complete, GL HUD
    // replay remains the source of truth; this API proves ownership of the data
    // shape and gives the final consumer an ordering-preserving handoff.
    // canvasW/H: the pixel space the vertices were authored in (logical game res,
    // gosRenderer width_/height_). The native tail divides by THIS, not the present
    // image size, so HUD placement is correct at any resolution/DPI (HUD-CANVAS-
    // EXTENT-FIX). Pass 0 to fall back to the present image extent.
    bool queueHudVulkanFrame(std::vector<VkHudDraw>&& draws,
                             uint32_t canvasW = 0, uint32_t canvasH = 0);

    // Rung 8.5b first runtime slice: render the Visual-mode postprocess composite
    // from the shared scene color image into the shared present image, then hand
    // that image back to GL so legacy HUD can draw on it. Conservative: returns
    // false unless the direct-present shared FBO is live. The caller supplies the
    // composite viewport; narrower FORCE-43 viewports are drawn over a VK black clear.
    bool renderNativeCompositeVisual(float exposure,
                                     int viewMode,
                                     int engineIdxBase,
                                     float lowLightGain,
                                     const float lowLightTint[3],
                                     bool casEnabled, float casSharpness, int tonemapMode,
                                     int viewportX, int viewportY,
                                     int viewportW, int viewportH);

    // Diagnostic-only native overlay for the CSM dynamic shadow debug view.
    // `csmArrayView` is the same-frame VK image view from the shadow island.
    bool renderNativeCsmShadowDebugOverlay(void* csmArrayView, int layer);
    // Debug inset of the camera motion-vector target (docs/plans/2026-11-06-temporal-aa.md).
    void setMotionVectorsDebug(bool on);

    // P13b: the visible GL game window we present onto (borrowed, not owned).
    SDL_Window* window() const { return window_; }
    const EnginePresentHealth& health() const { return health_; }

    // Emit the [VK_PRESENT] health line for the T5 proof.
    void logHealth() const;

    // Opaque impl (defined in the .cpp). Public only so the .cpp's file-local
    // helpers can name it; it holds no accessible members outside the TU (the
    // full definition lives in engine_present_vk.cpp).
    struct Impl;

private:
    // T5 content proof: read back the shared image, fold a checksum into health.
    void verifyReadbackContent();
    bool finishFrame();

    Impl* impl_ = nullptr;

    bool presentFrameBody(uint32_t drawableW, uint32_t drawableH, bool verifyReadback);
    // Drains the outstanding presented-image readback (if any) before the shared image it copies is destroyed.
    void drainReadbackBeforeImageDestroy();

    bool created_ = false;
    bool nativeCompositeArmed_ = false;
    // The composite/HUD tail must actually write the image before presentation.
    bool presentImageVkOwned_ = false;
    // VK-RUNG8-ITEM8: consecutive presented-frame attempts where the present image
    // was NOT VK-owned (native composite declined). Reset to 0 on any owned frame.
    // Bounded warmup budget: while armed and under budget we skip+retry (no stale
    // present, no GL drop); past the budget the config plainly cannot produce a VK
    // present image -> fail-soft the session to GL (copy-1 no longer rescues it).
    unsigned int notOwnedStreak_ = 0;
    // P13b: the existing visible GL game window we present onto via its HWND.
    // BORROWED (owned by gos_render / g_sdl_window) -- never destroyed here.
    SDL_Window* window_ = nullptr;
    EnginePresentHealth health_;
};

// Process-wide accessor for the single engine present object (parallels the
// engine context accessor). Does NOT auto-init.
EnginePresentVk& getOrCreateEnginePresentVk();

}  // namespace mc2vk

#endif  // MC2_VULKAN
#endif  // MC2_ENGINE_PRESENT_VK_H
