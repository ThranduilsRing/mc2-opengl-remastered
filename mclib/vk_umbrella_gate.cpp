// vk_umbrella_gate.cpp -- out-of-line VK umbrella questions (see GameOS/gameos/vk_umbrella_light.h).
#include "../GameOS/gameos/vk_shared_gbuffer_gate.h"
#include "../GameOS/gameos/vk_umbrella_light.h"

namespace mc2vkgate {

bool umbrellaOn()
{
    return gate::get<"MC2_VK_SHARED_GBUFFER">() && !g_mc2VkSessionForcedGL;
}

bool vfxArmed()
{
    return armed<"MC2_VK_VFX">();
}

bool vfxTraceOn()
{
    return gate::get<"MC2_VK_VFX_TRACE">();
}

} // namespace mc2vkgate
