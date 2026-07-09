// engine_present_vk.h
//
// VK-P13-SELECTOR-PLAN / T3 -- the whole-frame GL->VK present intercept.
//
// When MC2_RENDER_BACKEND=vulkan AND the shared engine VulkanContext (T2) is up,
// this object presents the FINISHED GL frame (FBO-0, after HUD + composite +
// FORCE-43 pillarbox) through a Vulkan swapchain on a HIDDEN SDL_WINDOW_VULKAN.
//
// MECHANISM (plan v2 §3, NOT zero-copy -- 2 copies/frame):
//   copy-1 (GL):  glBlitFramebuffer FBO-0 -> an exportable RGBA8 VkImageResource's
//                 GL texture (GlInteropView), sized to the GL drawable (incl.
//                 pillarbox). Then glInteropSignalToVk hands it to VK.
//   copy-2 (VK):  vkCmdBlitImage shared image -> acquired swapchain image, waiting
//                 the GL->VK semaphore; then vkQueuePresentKHR.
//
// OWNERSHIP: this object DOES NOT create a VkInstance/VkDevice. It borrows the
// shared engine device/instance/queue from the T2 VulkanContext (the T2
// acceptance criterion: one engine device, one volkLoadDevice, no R2 collision).
// It owns ONLY the window-specific surface + swapchain + per-frame sync + the
// shared interop image. That keeps the present path on the SAME global volk
// dispatch table the engine context loaded.
//
// FAIL-SOFT: any init/runtime failure logs a reason and returns false; the caller
// (gameosmain) then forces the session back to GL (SDL_GL_SwapWindow). Never
// crashes, never breaks the GL path.
//
// Whole header is inert unless MC2_VULKAN is defined (matches the TU). The
// interop primitives it uses are further #ifdef MC2_VULKAN_ISLAND; the .cpp
// hard-requires both (build64_island defines both).

#ifndef MC2_ENGINE_PRESENT_VK_H
#define MC2_ENGINE_PRESENT_VK_H

#ifdef MC2_VULKAN

#include <cstdint>
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
    uint32_t    direct_fbo_requests = 0;   // presentDirectFbo() calls from endScene
    uint32_t    direct_fbo_grants   = 0;   // calls that returned the shared FBO
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
    unsigned int last_direct_fbo    = 0;   // last shared FBO id granted
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
    uint32_t glTextureId = 0;
    uint32_t textureWidth = 0;
    uint32_t textureHeight = 0;
    uint32_t textureFormat = 0;
    uint32_t textureSerial = 0;
    uint32_t alphaMode = 0;
    uint32_t textureAddress = 0;
    uint32_t filter = 0;
    uint32_t foregroundColor = 0;
    std::vector<uint8_t> textureRgba;
    std::vector<VkHudVertex> vertices;
};

// The engine VK present object. Reuses the T2 shared VulkanContext device;
// owns the hidden present window + surface + swapchain + interop image.
class EnginePresentVk {
public:
    EnginePresentVk() = default;
    ~EnginePresentVk() { destroy(); }

    EnginePresentVk(const EnginePresentVk&) = delete;
    EnginePresentVk& operator=(const EnginePresentVk&) = delete;

    // Stand up the present surface + swapchain + interop image on the EXISTING
    // visible GL game window (P13b: same-HWND raw vkCreateWin32SurfaceKHR, no
    // second window), sized to (w,h) == the GL drawable extent. `glWindow` is the
    // game's SDL_WINDOW_OPENGL window (g_sdl_window) whose HWND we present to; it
    // stays visible and owns all input. `ctx` MUST be a valid, present-capable
    // (init(wantPresent=true)) engine context. Returns false + sets
    // health.fallback_reason on any error (fail-soft: partial state torn down).
    bool init(VulkanContext& ctx, SDL_Window* glWindow, uint32_t w, uint32_t h);

    // Present ONE finished GL frame. `glFbo0DrawableW/H` is the current GL
    // drawable size (for resize detection). Runs copy-1 (GL blit FBO-0 -> shared
    // GL texture), the GL->VK signal, then copy-2 (VK blit -> swapchain) +
    // present. Rebuilds the swapchain + shared image on drawable resize.
    // Returns false (fail-soft) on a hard error -- caller then forces GL.
    // `verifyReadback` (T5): if true, reads back the shared image once this frame
    // and folds a cheap checksum into health (proves real, changing content).
    bool presentGLFrame(uint32_t glDrawableW, uint32_t glDrawableH,
                        bool verifyReadback);

    void destroy();

    bool valid() const { return created_; }

    // VK-P14b (MC2_VK_PRESENT_DIRECT_FBO, copy-1 elimination): the GL FBO id that
    // wraps the shared exportable present image (COLOR_ATTACHMENT, FRAMEBUFFER_
    // COMPLETE). When the direct-composite gate is ON and the present path is up,
    // endScene binds THIS instead of FBO-0 so the whole composite+pillarbox+HUD+
    // ImGui renders straight into the shared image, and presentGLFrame then SKIPS
    // copy-1 (the FBO-0->shared blit). Returns 0 when: the present path isn't
    // valid, the gate is OFF, or the FBO isn't available -> caller falls back to
    // the normal FBO-0 bind + copy-1 (fail-soft, no crash). Reading this also
    // arms presentGLFrame's copy-1 skip for the frame (single source of truth:
    // whichever branch endScene took, presentGLFrame mirrors it via directFbo_).
    // Return type is a bare `unsigned int` (== GLuint) to keep this header free of
    // any GL include; the impl casts to/from GLuint at the .cpp boundary.
    unsigned int presentDirectFbo();

    // Rung 8.6b seam: true only after this frame's endScene() successfully armed
    // the direct-present shared FBO. Future native HUD/ImGui tail work must use
    // this as its precondition; it is not permission to begin a second VK write
    // by itself, because presentGLFrame() still owns the GL->VK semaphore handoff.
    bool directPresentFrameArmed() const { return created_ && directFbo_ != 0; }

    // Rung 8.6 native ImGui renderer seam. These stay VK-header-free so
    // GuiRuntime can hand draw data to the present owner without learning Vulkan.
    bool prepareImguiVulkanFrame();
    bool renderImguiVulkanDrawData(ImDrawData* drawData);
    bool imguiVulkanReady() const;
    void shutdownImguiVulkanRenderer();

    // Rung 8.6f HUD seam: queue an owned, already-scaled GameOS HUD batch for
    // present-tail native replay. Until the native renderer is complete, GL HUD
    // replay remains the source of truth; this API proves ownership of the data
    // shape and gives the final consumer an ordering-preserving handoff.
    bool queueHudVulkanFrame(std::vector<VkHudDraw>&& draws);

    // Rung 8.5b first runtime slice: render the Visual-mode postprocess composite
    // from the shared scene color image into the shared present image, then hand
    // that image back to GL so legacy HUD can draw on it. Conservative: returns
    // false unless the direct-present shared FBO is live. The caller supplies the
    // composite viewport; narrower FORCE-43 viewports are drawn over a VK black clear.
    bool renderNativeCompositeVisual(float exposure,
                                     bool fxaaEnabled,
                                     float fxaaSubpix,
                                     float fxaaEdgeThreshold,
                                     float fxaaEdgeThresholdMin,
                                     int viewMode,
                                     int engineIdxBase,
                                     float lowLightGain,
                                     const float lowLightTint[3],
                                     int viewportX, int viewportY,
                                     int viewportW, int viewportH);

    // Diagnostic-only native overlay for the CSM dynamic shadow debug view.
    // `csmArrayView` is the same-frame VK image view from the shadow island.
    bool renderNativeCsmShadowDebugOverlay(void* csmArrayView, int layer);

    // P13b: the visible GL game window we present onto (borrowed, not owned).
    SDL_Window* window() const { return glWindow_; }
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

    Impl* impl_ = nullptr;

    bool created_ = false;
    // VK-P14b: last FBO id handed out by presentDirectFbo() this frame (0 == none
    // -> presentGLFrame runs copy-1 as usual; non-0 -> endScene rendered straight
    // into the shared image so presentGLFrame SKIPS copy-1 and Y-flips in copy-2).
    unsigned int directFbo_ = 0;
    // True only when a native VK path wrote the shared present image after the
    // current frame armed directFbo_. Cleared at each presentDirectFbo() grant.
    bool presentImageVkOwned_ = false;
    // P13b: the existing visible GL game window we present onto via its HWND.
    // BORROWED (owned by gos_render / g_sdl_window) -- never destroyed here.
    SDL_Window* glWindow_ = nullptr;
    EnginePresentHealth health_;
};

// Process-wide accessor for the single engine present object (parallels the
// engine context accessor). Does NOT auto-init.
EnginePresentVk& getOrCreateEnginePresentVk();

}  // namespace mc2vk

#endif  // MC2_VULKAN
#endif  // MC2_ENGINE_PRESENT_VK_H
