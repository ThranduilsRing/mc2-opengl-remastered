// vk_umbrella_gate.cpp -- out-of-line VK umbrella questions (see GameOS/gameos/vk_umbrella_light.h).
#include "../GameOS/gameos/vk_shared_gbuffer_gate.h"
#include "../GameOS/gameos/vk_umbrella_light.h"
#include "Gates/gate_cold.h"   // str<>()
#include <string>

namespace mc2vkgate {

bool umbrellaOn()
{
    // The umbrella is default ON, but only means anything on the Vulkan backend: MC2_RENDER_BACKEND=gl
    // (the temporary opt-out) must leave every VK island off. Mirrors resolveOnce() in render_backend_select.cpp.
    static const bool glOptOut = [] { const std::string be = mc2::gate::str<"MC2_RENDER_BACKEND">(); return be == "gl" || be == "GL"; }();
    if (glOptOut) return false;
    return mc2::gate::get<"MC2_VK_SHARED_GBUFFER">();
}

bool vfxTraceOn()
{
    return mc2::gate::get<"MC2_VK_VFX_TRACE">();
}

} // namespace mc2vkgate
