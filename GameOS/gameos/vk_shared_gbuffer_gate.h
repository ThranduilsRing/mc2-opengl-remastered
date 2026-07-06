// vk_shared_gbuffer_gate.h
//
// VK-P14 GATE-CONSOLIDATION-1: the ONE umbrella that arms the full VK shared-GBuffer
// scene path, plus the sub-gate resolver every VK scene region uses.
//
// THE VK GATE MODEL (single source of truth; keep docs/tier1_env_vars.md "the VK
// gate model" section in lockstep):
//
//   * UMBRELLA  MC2_VK_SHARED_GBUFFER=1  — arms the whole shared-GBuffer scene family
//     at once. When ON, every member sub-gate below DEFAULTS to ON. The eventual
//     default flip toggles exactly THIS ONE name. Requires MC2_RENDER_BACKEND=vulkan
//     (the shared engine VulkanContext); fail-soft to GL when the context is absent.
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
//
//   * SAFETY gates — INDEPENDENT of the umbrella, keep their own defaults:
//         MC2_VK_TERRAIN_HEIGHT_AUDIT (verification-only, default-OFF) and the
//         structural island fail-soft (no env; always-on). Do NOT fold these in — a
//         verification/robustness gate must be togglable orthogonally to the feature.
//
//   * DEBUG / RETIRED gates — NOT in the umbrella, NOT in the flip path:
//         MC2_VK_PROP_COLOR_INTEROP (Tier-0 side-image+composite proof, SUPERSEDED by
//         the Tier-1 in-place co-write; parked behind its own gate, see registry),
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

} // namespace mc2vkgate
