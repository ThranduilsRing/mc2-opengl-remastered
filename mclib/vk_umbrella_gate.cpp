// vk_umbrella_gate.cpp -- out-of-line VK umbrella questions (see GameOS/gameos/vk_umbrella_light.h).
#include "../GameOS/gameos/vk_shared_gbuffer_gate.h"
#include "../GameOS/gameos/vk_umbrella_light.h"

namespace mc2vkgate {

bool umbrellaOn()
{
    return mc2::gate::get<"MC2_VK_SHARED_GBUFFER">();
}

bool vfxTraceOn()
{
    return mc2::gate::get<"MC2_VK_VFX_TRACE">();
}

} // namespace mc2vkgate
