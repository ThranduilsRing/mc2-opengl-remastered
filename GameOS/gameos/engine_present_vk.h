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

struct SDL_Window;

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
    std::string fallback_reason;           // empty == clean
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
