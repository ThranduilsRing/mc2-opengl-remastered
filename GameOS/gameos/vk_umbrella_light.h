// vk_umbrella_light.h -- the VK trace question the code outside the VK islands asks, declared without pulling the gate
// registry's compile-time index into the translation unit.
//
// vk_shared_gbuffer_gate.h includes Gates/gate.h; a translation unit that asks only this question includes this header
// instead and calls out of line. Defined in mclib/vk_umbrella_gate.cpp (mclib, so data tools that link only mclib still
// resolve it).
#pragma once

namespace mc2vkgate {

// MC2_VK_VFX_TRACE: the weapon-fire pipeline trace.
bool vfxTraceOn();

} // namespace mc2vkgate
