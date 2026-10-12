// vk_shared_gbuffer_gate.h -- the VK umbrella: one gate, MC2_VK_SHARED_GBUFFER, arms the whole
// shared-GBuffer scene family (terrain, prop, mech, water, VFX islands, the native post/composite chain,
// the present tail and the HUD). Gate amnesty wave 1 folded the former per-member Tri rows into it
// (ruling 2026-10-09 #3: their =0/=1 debug overrides were expendable), so every island asks the one
// question umbrellaOn() answers. The model is documented in docs/tier1_env_vars.md "The VK gate model";
// the umbrella stays default OFF.
//
// The one input the Gates library cannot see is the session fail-soft flag below; umbrellaOn() reads it
// on every call. A reader that must follow a mid-session fail-soft calls umbrellaOn() per use and never
// caches the answer; a region gate that is resolved once per process caches it at its own site.

#pragma once

#include "Gates/gate.h"
#include "vk_umbrella_light.h"   // umbrellaOn(), vfxTraceOn(): defined in mclib/vk_umbrella_gate.cpp

// VK-FAILSOFT-DISARM-1: session-sticky "VK fail-softed to GL" flag, set by mc2::forceGLFallback
// (render_backend_select.cpp), defined in mclib/tgl.cpp (data-tool-link home). When true the umbrella reads
// as OFF so the whole content family disarms: post-ITEM8 there is no per-layer GL fallback, so armed islands
// plus a GL present would make every VK-owned layer vanish.
extern bool g_mc2VkSessionForcedGL;
