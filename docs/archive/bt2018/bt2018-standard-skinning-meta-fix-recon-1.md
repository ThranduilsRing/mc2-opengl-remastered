# BT2018 standard-skinning meta-fix — RECON-1

**Verdict: the skinning/FK math is EXONERATED. The on-screen reverse-leg breakage is the rotation-only retarget (and possibly clip mismatch / per-frame foot lift), NOT a skinning-scheme error.**

## Context
User reported the imported marauder's legs break under locomotion clips (knee swings to front, "ankle torn off foot"), while the GLB plays the CORRECT reverse-joint walk in a standard online glTF player. Hypothesis under test: our pipeline's "bake per-part offset + apply joint-global only" scheme diverges from standard glTF skinning under animation. **Disproven below.**

## Proof the skinning is correct (circularity broken)
Prior recons compared joint globals via the harness, which shares `mc2skel` math with the engine → circular. This recon used an INDEPENDENT numpy glTF evaluator (no shared code).

1. **Independent eval matches ours to 5 decimals.** Clip `atlas_moveCoreWalkFwd`, knee−ankle dz (neg = knee behind = correct reverse): independent standard rest = −0.02430, walk f16 = −0.01072; our harness (rotation-only OFF) walk f16 = −0.01072; (rotation-only ON) = −0.01072. A standard player produces the SAME knee geometry. The knee oscillates fwd/back across gait phases (f0 = +0.026) — that is the authored stride, present in a standard player too.
2. **Vertex-exact to standard `animGlobal·IBM·v`.** Intact L-thigh vertex at walk f16: standard `animGlobal[LThigh]·IBM·v` = (0.0217,0.0779,−0.0061); our `clipGlobal·off·v_raw` = (0.0217,0.0779,−0.0061). All three of our paths (import bake `assimp_importer.cpp:630`, CPU re-bake `:1166-1191`, GPU palette `:1104-1117` + `gos_mech_batcher.cpp:1561-1574`) algebraically reduce to standard `animGlobal·IBM·v` (restGlobal terms cancel). `off = bone->mOffsetMatrix` IS the GLB inverse-bind. The "bake offset / joint-global" description is just an algebraic factoring of standard skinning.
3. **Single-bone rigid confirmed.** harness inspect: marauder & atlas max weights/vertex = 1, weight-sum 1.0.

## Why IBM tables looked suspicious (and are fine)
FBX2glTF exports 61 single-joint skins (one per mesh-part). Intact rendered leg parts (e.g. `mad_left_leg_thigh`, skin18) are authored in joint-LOCAL space with an IDENTITY inverse-bind; the dropped `_dmg` twins carry the model-space-IBM variant. So `restGlobal·IBM ≠ I` for those is normal (identity-IBM/local-vertex case), handled correctly and identically to a standard player. IBM IS preserved through import (`BuildSkeleton → bake.invBind`).

## ABI: no change needed — standard rigid path already ships
- Per-vertex bone index already carried by `GpuMechVertex.boneIndices[0]` (loc 3, `gos_mech_batcher.cpp:1379`), populated from `perVertexBone` (`assimp_importer.cpp:980-985`), `boneWeights[0]=255` (rigid). **No TG_TypeVertex ABI break, no new sidecar buffer.**
- Palette placement `F = Msw·D_i` at `gos_mech_batcher.cpp:1561-1574`. Folding IBM into the palette + shipping raw joint-local verts is textbook-standard but algebraically identical (proven vertex-exact) → cosmetic, not a fix. `mech.vert` unchanged. Stock ASE mechs use the `else` branch keyed on `rec.importedGpuType==nullptr` (`:1576-1587`) — untouched.

## What actually differs from the online player (priority order)
1. **ROTATION-ONLY RETARGET** (`MC2_MECH_ANIM_ROTATION_ONLY`, default ON; `mech_skel_import.cpp:26-86`). A standard player does NOT do this. It suppresses spine/pelvis/root TRANSLATION channels, shifting the leg chain by a constant ~(0.001,0.006,−0.011) and removing the gait's vertical bob/weight-shift. Does NOT flip the knee, but is the single biggest behavioral departure. **Test: render with `MC2_MECH_ANIM_ROTATION_ONLY=0`.** (Caveat: it was added to fix UB/LB ball-joint separation — see memory mech-ublb-rotation-only-retarget; turning it off globally may regress UB/LB. A prior selective-fix REGRESSED.)
2. **Clip mismatch** — confirm engine gesture map and the player loop the SAME clip name (idle vs WalkFwd).
3. **Axis / placement / per-frame foot lift** (`MC2_MECH_IMPORT_GPU_AXIS=2`, `mechToMC2Pos` 180° yaw — cannot tear a joint, verified; per-frame Stuff.y lift could read as feet floating/sinking).

The "ankle torn off foot" was NOT reproduced in any joint/vertex math → most likely (1)+(3) or a clip mismatch.

## Recommended next step
Render `MC2_MECH_ANIM_ROTATION_ONLY=0` and compare to the reference player BEFORE writing code. If it matches → fix is a gate-default change (+ resolve the UB/LB tradeoff). Route palette/GpuMechBone to mc2-render-expert and rotation-only policy / per-frame re-bake to mc2-mech-update-geometry-expert. Do NOT re-litigate FK/skinning — exonerated.

## Files
- `mclib/assimp_importer.cpp` (bake :630, CPU re-bake :1166-1191, GPU delta :1104-1117, perVertexBone :966-1003/:980-985)
- `mclib/mech_skel_import.cpp` (FK :88-99,:148-200; rotation-only :26-86; BuildSkeleton IBM preserve)
- `GameOS/gameos/gos_mech_batcher.cpp` (per-vertex bone :1243-1256,:1379; palette :1549-1593)
- `tools/mech_import_harness/mech_import_harness.cpp` (oracle — shares mc2skel, hence prior circularity)
