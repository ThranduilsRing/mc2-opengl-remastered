// vk_shared_gbuffer_gate.h -- the VK umbrella adapter over the typed gate registry.
//
// MC2_VK_SHARED_GBUFFER=1 arms the whole shared-GBuffer scene family. Every member is a Tri row in
// Gates/gate_table.cpp that declares inherits("MC2_VK_SHARED_GBUFFER"): an explicit "1"/"0" on the member
// always wins, otherwise it follows the umbrella. The model (the members, the direct-chain gates, the
// opt-ins that deliberately do NOT inherit, the byte-identical default) is documented in
// docs/tier1_env_vars.md "The VK gate model"; the umbrella itself stays default OFF.
//
// What this header adds is the one input the Gates library cannot see: the session fail-soft flag.
// Rules for callers:
//   * Name a member through armed<"MC2_VK_X">() (or one of the composites below), never through
//     gate::effective<"MC2_VK_...">() directly: that would skip the fail-soft veto.
//   * The env text is cached by the registry; the veto is read on every call. A reader that must follow a
//     mid-session fail-soft calls armed<>() per use and never caches the answer. A region gate that is
//     resolved once per process caches it at its own site.

#pragma once

#include "Gates/gate.h"
#include "vk_umbrella_light.h"   // umbrellaOn(), vfxArmed(), vfxTraceOn(): defined in mclib/vk_umbrella_gate.cpp

// VK-FAILSOFT-DISARM-1: session-sticky "VK fail-softed to GL" flag, set by mc2::forceGLFallback
// (render_backend_select.cpp), defined in mclib/tgl.cpp (data-tool-link home). When true the umbrella reads
// as OFF so the whole content family disarms: post-ITEM8 there is no per-layer GL fallback, so armed islands
// plus a GL present would make every VK-owned layer vanish. An explicit per-member "1" still wins (a debug
// knob).
extern bool g_mc2VkSessionForcedGL;

namespace mc2vkgate {

namespace gate = ::mc2::gate;

template <gate::Name n>
inline bool armed()
{
    return gate::effective<n>(g_mc2VkSessionForcedGL);
}

// "Tier-1 island armed AND shared scene depth armed": the pair every VK scene region requires before it
// co-writes into the shared GBuffer.
template <gate::Name n>
inline bool regionTier1Armed()
{
    return armed<n>() && armed<"MC2_VK_SHARED_SCENE_DEPTH">();
}

// Terrain is the native first owner of the shared scene color0/normal/depth for the frame (CLEAR loadOp, no
// GL<->VK semaphore handoff): a structural property of the armed terrain Tier-1 island.
inline bool terrainNativeFirstOwnerOn()
{
    return regionTier1Armed<"MC2_VK_TERRAIN_TIER1">();
}

// Shared post-pass direct-chain prerequisite: the scene producers kept the shared color/depth/normal chain
// Vulkan-owned through water. Mech is unconditionally native and has no term here.
inline bool postDirectChainUpstreamOn()
{
    return terrainNativeFirstOwnerOn() &&
           armed<"MC2_VK_PROP_DIRECT_CHAIN">() &&
           armed<"MC2_VK_WATER_TIER1">() &&
           armed<"MC2_VK_WATER_DIRECT_CHAIN">();
}

// The native composite arms its direct chain: everything upstream plus the composite's own gate.
inline bool compositeDirectChainOn()
{
    return postDirectChainUpstreamOn() && armed<"MC2_VK_POST_COMPOSITE_DIRECT_CHAIN">();
}

} // namespace mc2vkgate
