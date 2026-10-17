// vk_shared_gbuffer_gate.h -- the VK umbrella: one gate, MC2_VK_SHARED_GBUFFER, arms the whole
// shared-GBuffer scene family (terrain, prop, mech, water, VFX islands, the native post/composite chain,
// the present tail and the HUD). Gate amnesty wave 1 folded the former per-member Tri rows into it
// (ruling 2026-10-09 #3: their =0/=1 debug overrides were expendable), so every island asks the one
// question umbrellaOn() answers. The model is documented in docs/tier1_env_vars.md "The VK gate model";
// the umbrella is default ON since the 2026-10-17 flip (and reads false under MC2_RENDER_BACKEND=gl).
//
// There is no session fail-soft any more (owner ruling 2026-10-16: a failed VK init shows a message and
// exits), so the umbrella is a plain gate read and can be cached.

#pragma once

#include "Gates/gate.h"
#include "vk_umbrella_light.h"   // umbrellaOn(), vfxTraceOn(): defined in mclib/vk_umbrella_gate.cpp
