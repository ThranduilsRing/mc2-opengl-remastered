# BT2018-PALETTE-PERPART-OFFSET-AUDIT-1 — VERDICT: KILLED

Per-part `off` mismatch hypothesis DISPROVEN. The GPU palette already produces correct per-part skin `AS1·global·off_part·p` for every shared-bone part to machine epsilon (max 2.9e-17).

## Algebra (the reconciliation)
- Import bake (assimp_importer.cpp:609,630): `rm = bake.rest[bone]` (bone REST global). `v_baked = A2S·rest_b·off_part·p` — vertex carries its OWN off_part permanently.
- invMWrest (:975): `(A2S·rest_b)^-1`.
- Palette delta (:1109): `D_b = AS1·global_b·(A2S·rest_b)^-1`.
- Shader `D_b·v_baked` = `AS1·global_b·off_part·p`. The `rest_b` in the vertex cancels `rest_b^-1` in the palette PER PART; `off_part` survives in the VBO. D_b is identical for all parts on bone b and that is CORRECT — each part's verts already carry their distinct off. Matches design comment mech_skel_import.cpp:174-179 and the CPU re-bake (:1166-1191).

Earlier "fly-off" math missed that line 630 uses `rest` (rm), not identity → fake ~0.098 error; real error 1e-17.

## Per-part table (marauder, 34 intact single-joint parts)
8 shared-bone/different-off parts (6× j_Spine2, 2× j_Pelvis) — the documented fly-off suspects — reconcile to ~1e-17. Unshared leg/arm parts likewise ~1e-17. walkerr (‖GPU − intended per-part‖ at atlas_moveCoreWalkFwd f16) ≤ 3.5e-17 for ALL parts. No part diverges.

## Slot budget (option B, now moot): 34 parts, 28 bones, 31 unique (bone,off) pairs; boneIndices[0]=uint8 (≤255) → would fit trivially, but B is unnecessary.

## All static-rig math now exonerated: FK (oracle), skinning scheme, per-part palette. Raw mode (rotation-only=0) math == glTF viewer.

## Remaining live suspects (need on-hardware MOTION, not stills)
1. rotation-only retarget tradeoff (mech_skel_import.cpp:26-86, MC2_MECH_ANIM_ROTATION_ONLY) — channel-suppression policy; OFF removes gait bob, ON is default. Route to mc2-mech-update-geometry-expert; needs hardware A/B vs reference player.
2. Clip retarget magnitude — atlas walk maps ~17 channels onto marauder's 28 bones (11 hold rest, expected); VERIFY the atlas→marauder joint-name retarget isn't dropping a torso/arm channel (sampleActorClip assimp_importer.cpp:1078).
3. Per-frame foot-ground lift (:1112-1116 gpuLift, single world-up axis) — could read as float/sink, cannot tear a joint.

## Recommendation: stop auditing static skinning math. Next = empirical motion render with rotation-only=0 vs the reference glTF player.
