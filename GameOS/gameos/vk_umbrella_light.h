// vk_umbrella_light.h -- the two VK umbrella questions the code outside the VK islands asks, declared
// without pulling the gate registry's compile-time index into the translation unit.
//
// vk_shared_gbuffer_gate.h includes Gates/gate.h; a translation unit
// that asks only these questions includes this header instead and calls out of line. Defined in
// mclib/vk_umbrella_gate.cpp (mclib, so data tools that link only mclib still resolve them).
#pragma once

namespace mc2vkgate {

// MC2_VK_SHARED_GBUFFER is on; this is also the gosFX HUD-tail route gate.
bool umbrellaOn();

// MC2_VK_VFX_TRACE: the weapon-fire pipeline trace.
bool vfxTraceOn();

} // namespace mc2vkgate
