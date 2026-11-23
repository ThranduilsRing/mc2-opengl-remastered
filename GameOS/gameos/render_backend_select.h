// render_backend_select.h
//
// MC2_RENDER_BACKEND seam. The engine renders on Vulkan only (the GL opt-out was removed); this header reports an ignored
// MC2_RENDER_BACKEND value and holds the fatal-failure path for a Vulkan that cannot start.
//
// No Vulkan headers and no MC2_VULKAN dependency.

#ifndef MC2_RENDER_BACKEND_SELECT_H
#define MC2_RENDER_BACKEND_SELECT_H

#include <string_view>

namespace mc2 {

// What MC2_RENDER_BACKEND text asked for. The engine has ONE backend (Vulkan); the variable is only read to tell the user
// that an old value is ignored.
enum class BackendRequest {
    None,         // unset / empty / "vulkan" / "VULKAN" / "vk"
    RemovedGl,    // "gl" / "GL": the OpenGL backend no longer exists
    Unrecognized, // anything else
};
BackendRequest backendRequestFromText(const char* text);

// Once at engine init: logs a [RENDER_BACKEND] line when MC2_RENDER_BACKEND names the removed GL backend or is unrecognized,
// or when the retired MC2_VK_SHARED_GBUFFER is set to 0 (the run continues on Vulkan either way). Silent otherwise.
void noteBackendEnv();

// Fatal backend failure (there is no GL fallback). Called when Vulkan cannot come up (device /
// extensions / validation layer failure, no MC2_VULKAN in the build, or a present
// path that cannot be VK-owned). Logs "[RENDER_BACKEND] FATAL: <reason>", shows a
// message box unless the run is automated (smoke / crash-fast-exit), and ends the
// process with exit code 3. Does not return.
[[noreturn]] void fatalBackendFailure(const char* reason);

// The crash bundle registers here at init so a fatal backend exit (a plain _Exit: no SEH, no WER entry) still leaves crash.txt,
// the instrumentation ring and a minidump. Called once from fatalBackendFailure, before the message box.
void setFatalBackendSink(void (*sink)(std::string_view reason));

}  // namespace mc2

#endif  // MC2_RENDER_BACKEND_SELECT_H
