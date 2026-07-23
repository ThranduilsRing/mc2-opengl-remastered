// render_backend_select.h
//
// VK-P13-SELECTOR-1 / T1 -- the top-level MC2_RENDER_BACKEND selector seam.
//
// A single memoized accessor that reads MC2_RENDER_BACKEND once at first call
// and caches the result for the process lifetime. This is the ONE place the
// engine (gos_render/gameosmain) and future Vulkan consumers (P6/P8/P9/P14)
// ask "which render backend did the user select?".
//
// DISTINCT from MC2_RENDER_BACKEND_IFACE (gos_postprocess.cpp) -- that is a
// per-region post-process interface toggle; this is the top-level GL-vs-Vulkan
// backend selector. Do not conflate them.
//
// This header is GL-safe: it pulls in NO Vulkan headers and has NO #ifdef
// MC2_VULKAN dependency, so it compiles identically in the shipping GL-only
// build. On =gl / unset / unrecognized the accessor adds only one memoized
// getenv and returns GL -- byte-identical behavior, no code path changes.

#ifndef MC2_RENDER_BACKEND_SELECT_H
#define MC2_RENDER_BACKEND_SELECT_H

namespace mc2 {

enum class RenderBackend {
    GL = 0,      // default / unset / unrecognized MC2_RENDER_BACKEND
    Vulkan = 1,  // MC2_RENDER_BACKEND=vulkan
};

// Memoized accessor. Reads MC2_RENDER_BACKEND once (values "gl" | "vulkan";
// default GL; anything unrecognized -> GL, coercion logged once). Idempotent
// and cheap after the first call. Thread-safety: intended to be first called
// during single-threaded engine init (gameosmain), after which it is a pure
// read of a cached value.
RenderBackend renderBackend();

// Human-readable name for logs ("gl" | "vulkan").
const char* renderBackendName(RenderBackend b);

// Fail-soft coercion: force the session back to GL for the remainder of the
// process. Called by T2 when the engine VulkanContext fails to be born (bad
// device / missing exts / validation-layer failure). After this, renderBackend()
// returns GL so every downstream consumer takes the GL path. Session-sticky;
// there is no un-force. Logs the reason once.
void forceGLFallback(const char* reason);

}  // namespace mc2

#endif  // MC2_RENDER_BACKEND_SELECT_H
