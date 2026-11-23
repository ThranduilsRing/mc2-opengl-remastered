// render_backend_select.cpp
//
// MC2_RENDER_BACKEND: the engine renders on Vulkan only; the variable is read once to report an ignored value.
// No Vulkan headers (this TU also serves the unit tests).

#include "render_backend_select.h"
#include "Gates/gate.h"
#include "Gates/gate_cold.h"   // str<>()

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
#include "mc2fmt.h"

namespace mc2 {

BackendRequest backendRequestFromText(const char* text) {
    if (!text || text[0] == '\0') return BackendRequest::None;
    if (std::strcmp(text, "gl") == 0 || std::strcmp(text, "GL") == 0) return BackendRequest::RemovedGl;
    if (std::strcmp(text, "vulkan") == 0 || std::strcmp(text, "VULKAN") == 0 || std::strcmp(text, "vk") == 0) return BackendRequest::None;
    return BackendRequest::Unrecognized;
}

void noteBackendEnv() {
    if (!mc2::gate::get<"MC2_VK_SHARED_GBUFFER">()) {
        mc2::fmt::print_out("[RENDER_BACKEND] MC2_VK_SHARED_GBUFFER=0 ignored: the Vulkan scene path is unconditional.\n");
        std::fflush(stdout);
    }
    const std::string envText = mc2::gate::str<"MC2_RENDER_BACKEND">();   // "" when unset or empty
    switch (backendRequestFromText(envText.c_str())) {
    case BackendRequest::RemovedGl:
        mc2::fmt::print_out("[RENDER_BACKEND] MC2_RENDER_BACKEND={} ignored: the OpenGL backend was removed; running on Vulkan.\n", mc2::fmt::safe(envText.c_str()));
        break;
    case BackendRequest::Unrecognized:
        mc2::fmt::print_out("[RENDER_BACKEND] MC2_RENDER_BACKEND={} unrecognized; running on Vulkan.\n", mc2::fmt::safe(envText.c_str()));
        break;
    case BackendRequest::None:
        return;
    }
    std::fflush(stdout);
}

namespace {
void (*g_fatalSink)(std::string_view) = nullptr;
}
void setFatalBackendSink(void (*sink)(std::string_view)) { g_fatalSink = sink; }

void fatalBackendFailure(const char* reason) {
    const char* why = reason && reason[0] ? reason : "(unspecified)";
    mc2::fmt::print_out("[RENDER_BACKEND] FATAL: Vulkan could not be started: {}. There is no OpenGL fallback; exiting.\n", mc2::fmt::safe(why));
    std::fflush(stdout);
    std::fflush(stderr);
    // stdout/stderr are NUL in normal play and _Exit leaves no WER entry: this bundle is the only durable record of the exit.
    if (auto* sink = g_fatalSink) { g_fatalSink = nullptr; sink(why); }
#ifdef _WIN32
    // Automated runs (smoke, crash fast-exit) must not block on a modal box.
    const bool automated = mc2::gate::isSet<"MC2_SMOKE_MODE">() || mc2::gate::isSet<"MC2_NO_LAUNCHER">() ||
                           mc2::gate::isSet<"MC2_SMOKE_SEED">() || mc2::gate::isSet<"MC2_HEARTBEAT">() ||
                           mc2::gate::get<"MC2_CRASH_FAST_EXIT">();
    if (!automated) {
        std::string text = "MechCommander 2 could not start the Vulkan renderer:\n\n";
        text += why;
        text += "\n\nUpdate your graphics driver and make sure the GPU supports the Vulkan 1.2 device profile.";
        MessageBoxA(nullptr, text.c_str(), "MechCommander 2", MB_OK | MB_ICONERROR);
    }
#endif
    std::_Exit(3);
}

}  // namespace mc2
