// vk_shared_gbuffer_gate.h
//
// VK-P14 GATE-CONSOLIDATION-1: the ONE umbrella that arms the full VK shared-GBuffer
// scene path, plus the sub-gate resolver every VK scene region uses.
//
// THE VK GATE MODEL (single source of truth; keep docs/tier1_env_vars.md "the VK
// gate model" section in lockstep):
//
//   * UMBRELLA  MC2_VK_SHARED_GBUFFER=1  — arms the whole shared-GBuffer scene family
//     at once. When ON, every member sub-gate below DEFAULTS to ON, AND (as of
//     VK-UMBRELLA-DIRECT-CHAIN-DEFAULT-1) the direct-chain gates (prop, water, post
//     composite) default ON too — i.e. umbrella-ON MEANS the strict/direct-chain
//     no-GL-interop path. The old
//     hybrid interop-semaphore path is reachable only by an explicit per-gate =0
//     override (item-4's deletion target). The eventual default flip toggles exactly
//     THIS ONE name. Requires MC2_RENDER_BACKEND=vulkan (the shared engine
//     VulkanContext); fail-soft to GL when the context is absent. The umbrella itself
//     stays DEFAULT-OFF — the flip to VK-by-default remains the USER's decision.
//
//   * SUB-GATES (member of the umbrella family): scene-region + region-detail gates.
//     Explicit env ALWAYS wins over the umbrella (override for debugging):
//         env unset            -> umbrella value (ON iff umbrella ON)
//         env "1"              -> ON  (force-on even if umbrella off)
//         env "0"              -> OFF (force-off even if umbrella on)
//     Members: MC2_VK_SHARED_SCENE_DEPTH, MC2_VK_PROP_COLOR_TIER1, MC2_VK_PROP_MATERIAL,
//              MC2_VK_TERRAIN_TIER1, MC2_VK_TERRAIN_LIGHTING, MC2_VK_TERRAIN_DETAIL,
//              MC2_VK_TERRAIN_DYNSHADOW, MC2_VK_MECH_TIER1, MC2_VK_MECH_MATERIAL,
//              MC2_VK_MECH_LIGHTING.
//     VK-FIELD-ENV-CONTENT-STACK-ARM-1 (2026-07-11) added the COMPLETE-CONTENT members —
//     content that would VANISH under VK-only present if left opt-in (no per-layer GL
//     fallback post-ITEM8): MC2_VK_TERRAIN_ALL_LODS, MC2_VK_TERRAIN_SKIRTS,
//              MC2_VK_WATER_TIER1, MC2_VK_POST_FOG_NATIVE, MC2_VK_HUD, MC2_VK_HUD_SKIP_GL
//              (and MC2_VK_HUD_COLOR_PROOF resolves via directChainGate — the native HUD
//              renderer in the present tail). STILL OPT-IN (absence leaves geometry intact):
//              MC2_VK_TERRAIN_VISUALDISPLACE_FAR, MC2_VK_POST_{SSAO,SCREENFX,HZB}, and the
//              shadow island (MC2_VK_SHADOW_INTEROP/_CSM_*_INTEROP — documented parity hazard).
//
//   * SAFETY gates — INDEPENDENT of the umbrella, keep their own defaults:
//         MC2_VK_TERRAIN_HEIGHT_AUDIT (verification-only, default-OFF) and the
//         structural island fail-soft (no env; always-on). Do NOT fold these in — a
//         verification/robustness gate must be togglable orthogonally to the feature.
//
//   * DEBUG / RETIRED gates — NOT in the umbrella, NOT in the flip path:
//         MC2_VK_PROP_MATERIAL_DEBUG, *_HEIGHT_AUDIT_CORRUPT (negative-test knobs).
//
// BYTE-IDENTICAL RULE: umbrella unset/!=1 AND no sub-gate forced -> every member is
// OFF -> pure GL path, byte-identical to today's default. Verified by the near-oracle
// byte-OFF control (near_land_off self-diff 0.000).
//
// Firewall: pure <cstdlib>/<cstring>; no GL/VK/game headers. Safe in every TU.
//
// Each accessor memoizes its first read (env is process-static); the family is read
// exactly once per process. gate-ON smoke MUST allowlist MC2_VK_SHARED_GBUFFER (and
// any sub-gate it forces) in scripts/run_smoke.py or the passthrough drops it.

#pragma once

#include <cstdlib>
#include <cstring>

#include "Gates/gate.h"

// VK-FAILSOFT-DISARM-1: session-sticky "VK fail-softed to GL" flag, set by
// mc2::forceGLFallback (render_backend_select.cpp), defined in mclib/tgl.cpp
// (data-tool-link home). When true the umbrella reads as OFF so the whole
// content family disarms — post-ITEM8 there is no per-layer GL fallback, so
// armed islands + a GL present = every VK-owned layer VANISHES (torn grey
// scene with only residual GL-drawn VFX/decals over the wash).
extern bool g_mc2VkSessionForcedGL;

namespace mc2vkgate {

namespace gate = ::mc2::gate;

// Raw tri-state read of an env var: -1 unset, 0 explicit "0", 1 explicit "1".
// Anything else (unset / other value) is treated as unset (-1) so only the exact
// "1"/"0" tokens force — matches the historical `v[0]=='1' && v[1]=='\0'` idiom.
inline int envTriState(const char* name)
{
    const char* v = std::getenv(name);
    if (!v || v[0] == '\0' || v[1] != '\0') return -1;
    if (v[0] == '1') return 1;
    if (v[0] == '0') return 0;
    return -1;
}

// The umbrella. Presence-gated =1, default OFF. Reads the fail-soft disarm
// flag PER CALL (not memoized): once the session force-falls back to GL the
// whole family must read OFF, or armed islands keep claiming content the GL
// present can never show (VK-FAILSOFT-DISARM-1). Explicit per-sub-gate "1"
// overrides still win in subGate()/directChainGate() — those are debug knobs.
inline bool umbrellaOn()
{
    return gate::get<"MC2_VK_SHARED_GBUFFER">() && !g_mc2VkSessionForcedGL;
}

// The typed form of subGate()/directChainGate(): a Tri row of Gates/gate_table.cpp that declares
// inherits("MC2_VK_SHARED_GBUFFER"). The row's explicit "1"/"0" wins; else on iff the umbrella is on
// and the session has not fail-softed to GL. This is the only place the fail-soft flag enters, so a
// caller never calls gate::effective<"MC2_VK_...">() directly (that would skip the veto).
//
// The env text is cached by the gate registry on first read; the veto is read on EVERY call. A caller
// that must follow a mid-session fail-soft calls armed<>() per use and never caches the answer; a
// region gate that is resolved once per process caches it at its own site, as before.
template <gate::Name n>
inline bool armed()
{
    return gate::effective<n>(g_mc2VkSessionForcedGL);
}

// Resolve a sub-gate that is a MEMBER of the umbrella family:
//   explicit env "1"/"0" wins; else the umbrella value (ON iff umbrella ON).
inline bool subGate(const char* name)
{
    const int t = envTriState(name);
    if (t == 1) return true;
    if (t == 0) return false;
    return umbrellaOn();
}

// Resolve a DIRECT-CHAIN gate — a member of the strict/direct-chain family that
// arms the no-GL-interop path (each scene producer + post pass keeps the shared
// color/depth/normal chain Vulkan-owned and skips its GL<->VK semaphore handoff).
//
// VK-RUNG8 item-4 PREREQUISITE (VK-UMBRELLA-DIRECT-CHAIN-DEFAULT-1): the umbrella
// MC2_VK_SHARED_GBUFFER=1 now arms these by DEFAULT (strict/direct-chain is the
// umbrella-ON meaning; the old hybrid interop-semaphore path is reachable ONLY by
// an explicit per-gate =0 override — it is item-4's deletion target). Precedence is
// identical to subGate: explicit env "1"/"0" wins; else the umbrella value.
//
// The direct-chain family (keep in lockstep with the strict runner + the ladder doc):
// MC2_VK_PROP_DIRECT_CHAIN, MC2_VK_WATER_DIRECT_CHAIN, MC2_VK_POST_COMPOSITE_DIRECT_CHAIN.
// (MC2_VK_OBJECTID_DIRECT_CHAIN is a view-mode-only canary, NOT a default member.)
inline bool directChainGate(const char* name)
{
    const int t = envTriState(name);
    if (t == 1) return true;
    if (t == 0) return false;
    return umbrellaOn();
}

// "Terrain is the native first owner of the shared scene color0/normal/depth for the
// frame": a STRUCTURAL property of the armed terrain Tier-1 island (always CLEAR loadOp
// on color0/normal/depth, no GL<->VK semaphore handoff), so it is true exactly when the
// terrain Tier-1 island is armed. Mirrors terrainTier1Requested() in
// vulkan_terrain_island.cpp (keep in lockstep).
// "tier-1 island armed AND shared scene depth armed": the pair every VK scene region requires before it
// co-writes into the shared GBuffer.
template <gate::Name n>
inline bool regionTier1Armed()
{
    return armed<n>() && armed<"MC2_VK_SHARED_SCENE_DEPTH">();
}

inline bool terrainNativeFirstOwnerOn()
{
    return regionTier1Armed<"MC2_VK_TERRAIN_TIER1">();
}

// Shared post-pass direct-chain prerequisite: the scene producers have kept the
// shared color/depth/normal chain Vulkan-owned through water. The post passes need
// only this plus the post-composite direct-chain gate (the shared-scene resolver in
// gos_postprocess.cpp). Mech is unconditionally native and has no term here.
inline bool postDirectChainUpstreamOn()
{
    return terrainNativeFirstOwnerOn() &&
           armed<"MC2_VK_PROP_DIRECT_CHAIN">() &&
           armed<"MC2_VK_WATER_TIER1">() &&
           armed<"MC2_VK_WATER_DIRECT_CHAIN">();
}

} // namespace mc2vkgate
