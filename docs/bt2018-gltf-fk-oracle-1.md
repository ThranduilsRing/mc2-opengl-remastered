# BT2018-GLTF-FK-ORACLE-1 — results

**VERDICT 2: runtime FK matches a trusted spec-pure oracle. The FK math is NOT the bug.**

## Oracle
`scripts/gltf_fk_oracle.py` — pure python (manual GLB parse + numpy), ZERO mc2skel/Assimp/pygltflib. glTF 2.0 semantics: channel REPLACES node-local TRS component at time t; missing component falls back to node bind TRS; LINEAR interp; quat normalize + hemisphere-correct nlerp; local = T*R*S; global = parentGlobal*local from scene roots; no axis conversion in FK.

## Oracle validated against the viewer claim (gate PASSED)
Marauder reverse-joint, knee(j_LCalf)−ankle(j_LFoot) global, forward axis = Z:
- f0 dZ = +0.0259 (knee AHEAD — swing phase), f11 +0.0219, **plant f14–19 dZ NEGATIVE** (f16 = −0.01072 — knee BEHIND, correct reverse stance), recovers + by f23.
- f16 dZ = −0.01072 matches the prior independent eval exactly. **The "knee in front" stills were swing-phase, NOT a bug.**

## Diff: oracle vs mc2skel raw FK (MC2_MECH_ANIM_ROTATION_ONLY=0)
`gpu-bones` emits per-bone GLOBAL row-major 4×4 + parent idx; honors the rotation-only env. Frames 0/8/11/16, bones j_Pelvis(root)/j_LHip/j_LThigh/j_LCalf/j_LFoot/j_Spine2/j_LUpperArm/j_LForearm/j_LClavicle: full-matrix max|Δ| = **2.3e-5** (translation ≤1e-6). float32 + slerp-vs-nlerp noise. **No first divergent bone — FK is byte-equivalent to a standard glTF player.** (Rig has no single j_Spine/j_LHand skinned bone; torso=j_Spine2, fingers=j_LIndex1/Pinky1 — 28 skinned bones, oracle+engine agree.)

## rotation-only ON vs OFF
rotation-only=1 shifts the whole leg chain by a near-constant (+0.004,+0.0077,−0.0109)@f0 and the spine by a different (+0.005,−0.017,−0.006) — discards spine/pelvis/root translation, removes gait bob. Does NOT flip the knee. A standard viewer applies those channels.

## Conclusion
FK exonerated (3rd independent confirmation). The on-screen problem is the rotation-only TRADEOFF + a downstream SKINNING/palette issue:
- rotation-only ON → legs distorted (needed leg translation dropped).
- rotation-only OFF → upper-body/arm parts fly off center (original documented reason it was added).
- Viewer does neither → a real per-part palette bug on the fly-off parts.

## Next (the last unknown) — BT2018-PALETTE-PERPART-OFFSET-AUDIT-1
Per render-ABI recon: per-part `off` (mOffsetMatrix) is baked into VBO verts (assimp_importer.cpp:630) but the per-BONE palette delta D_i = AS1·globals·invMWrest (invMWrest = (A2S·rest)^-1, NO off; :1109,:975). MULTIPLE torso/arm parts share ONE bone with DIFFERENT off (render recon: 6 parts j_Spine2, 2 j_Pelvis, distinct IBMs). A single per-bone palette matrix cannot un-bake distinct per-part offsets under animation → those parts fly off when spine/pelvis translation is applied (raw mode); rotation-only masks it by killing that translation, at the cost of leg gait. Audit: enumerate intact-part off matrices + bone-sharing; prove fly-off cause; propose smallest fix (per-part palette entry, or correct per-part IBM in palette) so raw mode is correct everywhere and rotation-only retires.
