// vk_shared_gbuffer_gate.h -- the VK gate adapter header: pulls Gates/gate.h and vfxTraceOn() into the Vulkan islands.
// MC2_VK_SHARED_GBUFFER (the former umbrella that armed the shared-GBuffer scene family) is retired: the Vulkan scene path
// is unconditional and the variable is only reported as ignored (render_backend_select.cpp).

#pragma once

#include "Gates/gate.h"
#include "vk_umbrella_light.h"   // vfxTraceOn(): defined in mclib/vk_umbrella_gate.cpp
