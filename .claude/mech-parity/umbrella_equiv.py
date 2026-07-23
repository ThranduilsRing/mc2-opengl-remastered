#!/usr/bin/env python3
"""VK-P14 GATE-CONSOLIDATION-1 verification.

Proves MC2_VK_SHARED_GBUFFER umbrella reproduces the explicit all-sub-gates-ON
state, and that umbrella-OFF is pure GL. Reuses terrain_near_oracle's run/diff.

  A = EXPLICIT: every sub-gate set =1 (the pre-consolidation all-gates-on config)
  B = UMBRELLA: only MC2_VK_SHARED_GBUFFER=1 (sub-gates default ON via subGate)
  C = GL      : umbrella OFF, MC2_RENDER_BACKEND=gl (byte-OFF control)

Acceptance:
  (1) A vs B == 0.000 (umbrella == explicit)            [equivalence]
  (2) A/B self-diff == 0.000 x2 (determinism unbroken)  [no new nondeterminism]
  (3) C self-diff == 0.000 (GL control)                 [byte-OFF]
  (4) B vs C nonzero coverage (umbrella actually arms VK co-write)
"""
import sys
sys.path.insert(0, r"A:/Games/mc2-vk-interop-bridge/.claude/mech-parity")
import terrain_near_oracle as O

# Full all-regions-ON (matches the 58.97/5.01 whole-frame baseline: terrain+detail
# +dynshadow, prop color+material, mech tier1+material+lighting, shared depth).
SUBGATES = {
    "MC2_VK_SHARED_SCENE_DEPTH":"1",
    "MC2_VK_TERRAIN_TIER1":"1","MC2_VK_TERRAIN_LIGHTING":"1",
    "MC2_VK_TERRAIN_DETAIL":"1","MC2_VK_TERRAIN_DYNSHADOW":"1",
    "MC2_VK_PROP_COLOR_TIER1":"1","MC2_VK_PROP_MATERIAL":"1",
    "MC2_VK_MECH_TIER1":"1","MC2_VK_MECH_MATERIAL":"1","MC2_VK_MECH_LIGHTING":"1",
}
A_EXPLICIT = dict({"MC2_RENDER_BACKEND":"vulkan"}, **SUBGATES)
B_UMBRELLA = {"MC2_RENDER_BACKEND":"vulkan","MC2_VK_SHARED_GBUFFER":"1"}
C_GL       = {"MC2_RENDER_BACKEND":"gl"}

if __name__=="__main__":
    a0=O.run("eq_A_explicit_0", A_EXPLICIT)[0]
    a1=O.run("eq_A_explicit_1", A_EXPLICIT)[0]
    b0=O.run("eq_B_umbrella_0", B_UMBRELLA)[0]
    b1=O.run("eq_B_umbrella_1", B_UMBRELLA)[0]
    c0=O.run("eq_C_gl_0", C_GL)[0]
    c1=O.run("eq_C_gl_1", C_GL)[0]
    # unmasked whole-frame (settle default 30 here; determinism of terrain is the point)
    O.pair_report("(1) A(explicit) vs B(umbrella) -- MUST be 0.000 [EQUIVALENCE]", a0, b0)
    O.pair_report("(2) A self-diff -- MUST be 0.000", a0, a1)
    O.pair_report("(2) B self-diff -- MUST be 0.000", b0, b1)
    O.pair_report("(3) C(GL) self-diff -- MUST be 0.000 [byte-OFF]", c0, c1)
    O.pair_report("(4) B(umbrella) vs C(GL) -- MUST be nonzero [VK armed]", b0, c0)
