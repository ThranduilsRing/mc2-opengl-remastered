// render_backend_select.cpp
//
// VK-P13-SELECTOR-1 / T1 -- implementation of the memoized MC2_RENDER_BACKEND
// selector. GL-safe (no Vulkan headers); the whole TU compiles in the shipping
// GL-only build. The selector adds only a single memoized getenv on the =gl /
// unset path, so GL behavior is byte-identical.

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

namespace {

// Cached backend. -1 = not yet resolved; 0 = GL; 1 = Vulkan.
int s_cached = -1;

RenderBackend resolveOnce() {
    const std::string envText = mc2::gate::str<"MC2_RENDER_BACKEND">();   // "" when unset or empty
    const char* env = envText.c_str();
    if (env[0] == '\0') {
        // Unset -> GL. Silent (this is the default, byte-identical path).
        return RenderBackend::GL;
    }
    if (std::strcmp(env, "gl") == 0 || std::strcmp(env, "GL") == 0) {
        return RenderBackend::GL;
    }
    if (std::strcmp(env, "vulkan") == 0 || std::strcmp(env, "VULKAN") == 0 ||
        std::strcmp(env, "vk") == 0) {
        mc2::fmt::print_out("[RENDER_BACKEND] MC2_RENDER_BACKEND={} -> Vulkan backend selected.\n", mc2::fmt::safe(env));
        std::fflush(stdout);
        return RenderBackend::Vulkan;
    }
    // Unrecognized -> GL, log the coercion once.
    mc2::fmt::print_out("[RENDER_BACKEND] MC2_RENDER_BACKEND={} unrecognized (expected gl|vulkan); coercing to GL.\n", mc2::fmt::safe(env));
    std::fflush(stdout);
    return RenderBackend::GL;
}

}  // namespace

RenderBackend renderBackend() {
    if (s_cached < 0) {
        s_cached = static_cast<int>(resolveOnce());
    }
    return static_cast<RenderBackend>(s_cached);
}

const char* renderBackendName(RenderBackend b) {
    return b == RenderBackend::Vulkan ? "vulkan" : "gl";
}

void fatalBackendFailure(const char* reason) {
    const char* why = reason && reason[0] ? reason : "(unspecified)";
    mc2::fmt::print_out("[RENDER_BACKEND] FATAL: Vulkan could not be started: {}. There is no OpenGL fallback; exiting.
", mc2::fmt::safe(why));
    std::fflush(stdout);
    std::fflush(stderr);
#ifdef _WIN32
    // Automated runs (smoke, crash fast-exit) must not block on a modal box.
    const bool automated = mc2::gate::isSet<"MC2_SMOKE_MODE">() || mc2::gate::isSet<"MC2_NO_LAUNCHER">() ||
                           mc2::gate::isSet<"MC2_SMOKE_SEED">() || mc2::gate::isSet<"MC2_HEARTBEAT">() ||
                           mc2::gate::get<"MC2_CRASH_FAST_EXIT">();
    if (!automated) {
        std::string text = "MechCommander 2 could not start the Vulkan renderer:

";
        text += why;
        text += "

Update your graphics driver and make sure the GPU supports Vulkan 1.3.";
        MessageBoxA(nullptr, text.c_str(), "MechCommander 2", MB_OK | MB_ICONERROR);
    }
#endif
    std::_Exit(3);
}

}  // namespace mc2
