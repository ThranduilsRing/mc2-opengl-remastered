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
//     VK-UMBRELLA-DIRECT-CHAIN-DEFAULT-1) the 10 direct-chain gates default ON too —
//     i.e. umbrella-ON MEANS the strict/direct-chain no-GL-interop path. The old
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

namespace mc2vkgate {

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

// The umbrella. Presence-gated =1, default OFF.
inline bool umbrellaOn()
{
    static const bool s_on = (envTriState("MC2_VK_SHARED_GBUFFER") == 1);
    return s_on;
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
// The LIVE direct-chain family (keep in lockstep with the strict runner + the
// ladder doc): MC2_VK_PROP_DIRECT_CHAIN, MC2_VK_WATER_DIRECT_CHAIN,
// MC2_VK_POST_COMPOSITE_DIRECT_CHAIN.
// (MC2_VK_OBJECTID_DIRECT_CHAIN is a view-mode-only canary, NOT a default member.)
// RETIRED to parseable no-ops (item-4 hybrid-arm deletions; explicit set prints a
// deprecation note): MC2_VK_MECH_DIRECT_CHAIN, MC2_VK_SHADOW_DIRECT_CHAIN,
// MC2_VK_POST_{FOG,SSAO,SCREENFX,HZB}_DIRECT_CHAIN, and (VK-RUNG8-ITEM4-TERRAIN-
// DELETE-1) MC2_VK_TERRAIN_NATIVE_PREFIX_CLEAR — terrain native first-owner is now
// unconditional when the terrain Tier-1 island is armed; consumers use
// terrainNativeFirstOwnerOn() below instead of the retired gate.
inline bool directChainGate(const char* name)
{
    const int t = envTriState(name);
    if (t == 1) return true;
    if (t == 0) return false;
    return umbrellaOn();
}

// VK-RUNG8-ITEM4-TERRAIN-DELETE-1: "terrain is the native first owner of the shared
// scene color0/normal/depth for the frame". This is what the retired
// MC2_VK_TERRAIN_NATIVE_PREFIX_CLEAR gate used to mean; after the terrain island's
// hybrid GL<->VK semaphore-handoff arm was deleted it is a STRUCTURAL property of
// the armed terrain Tier-1 island (always CLEAR loadOp on color0/normal/depth, no
// GL<->VK semaphore handoff), so it is true exactly when the terrain Tier-1 island
// is armed. Mirrors terrainTier1Requested() in vulkan_terrain_island.cpp (keep in
// lockstep).
inline bool terrainNativeFirstOwnerOn()
{
    return subGate("MC2_VK_TERRAIN_TIER1") && subGate("MC2_VK_SHARED_SCENE_DEPTH");
}

// Shared post-pass direct-chain prerequisite: the scene producers have kept the
// shared color/depth/normal chain Vulkan-owned through water. Individual post
// passes still require their own direct-chain env gate on top of this.
inline bool postDirectChainUpstreamOn()
{
    // Direct-chain members default ON under the umbrella (directChainGate);
    // MC2_VK_WATER_TIER1 is a normal umbrella sub-gate (subGate). Terrain's term
    // is the structural first-owner predicate (retired gate; see above). The
    // deprecated mech gate reference is retained per the a0cba0c4 precedent
    // (resolves ON under the umbrella; only an explicit =0 debug override gates
    // posts, mech itself is unconditionally native).
    return terrainNativeFirstOwnerOn() &&
           directChainGate("MC2_VK_PROP_DIRECT_CHAIN") &&
           directChainGate("MC2_VK_MECH_DIRECT_CHAIN") &&
           subGate("MC2_VK_WATER_TIER1") &&
           directChainGate("MC2_VK_WATER_DIRECT_CHAIN");
}

} // namespace mc2vkgate
