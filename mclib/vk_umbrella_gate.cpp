// vk_umbrella_gate.cpp -- out-of-line VK umbrella questions (see GameOS/gameos/vk_umbrella_light.h).
#include "../GameOS/gameos/vk_shared_gbuffer_gate.h"
#include "../GameOS/gameos/vk_umbrella_light.h"
#include "Gates/gate_cold.h"   // raw<>()
#include <cstring>

namespace mc2vkgate {

bool umbrellaOn()
{
    // The umbrella is default ON, but only means anything on the Vulkan backend: MC2_RENDER_BACKEND=gl
    // (the temporary opt-out) must leave every VK island off. Mirrors resolveOnce() in render_backend_select.cpp.
    const char* be = mc2::gate::raw<"MC2_RENDER_BACKEND">();
    if (be && (std::strcmp(be, "gl") == 0 || std::strcmp(be, "GL") == 0)) return false;
    return mc2::gate::get<"MC2_VK_SHARED_GBUFFER">();
}

bool vfxTraceOn()
{
    return mc2::gate::get<"MC2_VK_VFX_TRACE">();
}

} // namespace mc2vkgate
