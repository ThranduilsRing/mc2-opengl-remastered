// vk_umbrella_gate.cpp -- out-of-line VK umbrella questions (see GameOS/gameos/vk_umbrella_light.h).
#include "../GameOS/gameos/vk_shared_gbuffer_gate.h"
#include "../GameOS/gameos/vk_umbrella_light.h"
#include "../GameOS/gameos/render_backend_select.h"

namespace mc2vkgate {

bool umbrellaOn()
{
    // The umbrella is default ON, but only means anything on the Vulkan backend: the GL opt-out must leave every
    // VK island off. The backend is immutable once resolved, so ask the one resolver rather than re-parse the string.
    if (mc2::renderBackend() != mc2::RenderBackend::Vulkan) return false;
    return mc2::gate::get<"MC2_VK_SHARED_GBUFFER">();
}

bool vfxTraceOn()
{
    return mc2::gate::get<"MC2_VK_VFX_TRACE">();
}

} // namespace mc2vkgate
