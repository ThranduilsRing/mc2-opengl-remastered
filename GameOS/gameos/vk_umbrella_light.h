// vk_umbrella_light.h -- the three VK umbrella questions the code outside the VK islands asks, declared
// without pulling the gate registry's compile-time index into the translation unit.
//
// vk_shared_gbuffer_gate.h is the full adapter (armed<>, regionTier1Armed<>, the composites) and includes
// Gates/gate.h; a translation unit that asks only these questions includes this header instead and calls
// out of line. Defined in mclib/vk_umbrella_gate.cpp (mclib, so data tools that link only mclib still
// resolve them next to g_mc2VkSessionForcedGL).
#pragma once

namespace mc2vkgate {

// MC2_VK_SHARED_GBUFFER is on and the session has not fail-softed to GL.
bool umbrellaOn();

// armed<"MC2_VK_VFX">(): the gosFX HUD-tail route. Re-reads the fail-soft veto on every call.
bool vfxArmed();

// MC2_VK_VFX_TRACE: the weapon-fire pipeline trace.
bool vfxTraceOn();

} // namespace mc2vkgate
