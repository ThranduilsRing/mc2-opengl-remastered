# BT2018 reverse-jointed leg LOCOMOTION FK -- RECON-1 (read-only)

Status: RECON (no edits, no build). Harness numbers only; engine FK already matches the
harness per the mech-ublb-rotation-only-retarget memory; no mech_shot needed (numeric
proof is byte-decisive).

Confirmed framing (user): imported marauder BAKED/REST pose is CORRECT (reverse knee at
rear). The knee-at-front/backwards problem appears ONLY under the WALK/RUN clip. Humanoid
mechs (atlas) animate fine. So this is an ANIMATION-FK issue specific to reverse-joint
(digitigrade) legs.

Harness: build64-harness/RelWithDebInfo/mech_import_harness.exe (nifty worktree).
GLBs: BattleTech_2018_Dump/MadCat/marauder_fbx2gltf.glb, Atlas/atlas_fbx2gltf.glb.

## ROOT CAUSE = (a) the CLIP DATA is humanoid-authored and SHARED across all mechs

NOT (b) rotation-only dropping leg translation, and NOT (c) an FK composition-order bug.
Every BT2018 mech GLB ships the SAME atlas_moveCore* locomotion clip set, authored
against the ATLAS (humanoid, knee-forward) rig. The clip channels carry ABSOLUTE local
joint rotations (not deltas), so once the walk clip drives j_LThigh/j_LCalf the marauder
reverse-jointed BIND chirality is fully overwritten and the leg snaps into the humanoid
(knee-forward) configuration.

### Discriminator: frame 0 already flips the knee (not only-later-frames)

knee-minus-ankle Z (j_LCalf.z - j_LFoot.z); BT space z = fwd/back; NEGATIVE = knee BEHIND
ankle = correct reverse-joint:

    rest               dz = -0.02430   BEHIND   (correct reverse-joint bind)
    idle0   (clip f0)  dz = +0.00341   AHEAD    (already flipped, idle too)
    walk0   (rotOnly)  dz = +0.02594   AHEAD    (flipped at frame 0)
    walk8   (rotOnly)  dz = +0.03371   AHEAD
    walk16  (rotOnly)  dz = -0.01072   BEHIND   (swings back mid-stride)
    walk24  (rotOnly)  dz = +0.00267   AHEAD

Frame 0 already moves the knee from BEHIND to AHEAD. By the discriminator this rules out
clip rotations applied to a wrong-handed chain only at later frames; the authored pose at
every frame (including idle f0) places the knee forward. The knee oscillates front/back
through the stride (the gait), but its NEUTRAL is forward = humanoid.

### Clincher: marauder == atlas under the SAME clip (clip dominates the bind)

knee-minus-ankle Z, same clip:

    MARAUDER walk0  = +0.02594        ATLAS walk0  = +0.02594   (identical to 5dp)
    MARAUDER walk16 = -0.01072        ATLAS walk16 = -0.01072   (identical to 5dp)

absolute leg joint GLOBALS, walk frame 0:

    j_LThigh  mar=(0.0283,0.0863,-0.0034)  atl=(0.0283,0.0867,-0.0034)
    j_LCalf   mar=(0.0317,0.0463,-0.0007)  atl=(0.0317,0.0467,-0.0007)
    j_LFoot   mar=(0.0324,0.0216,-0.0267)  atl=(0.0324,0.0220,-0.0267)

Match to ~0.0004 (tiny Y delta = slightly different bind bone lengths). The walk clip
produces the SAME leg pose on both rigs. The marauder reverse-jointed bind (rest
dz=-0.0243) contributes NOTHING once the clip absolute channels overwrite the local
transforms. The marauder ships only atlas_moveCore* clips (all 64 atlas-prefixed).

### rotation-only retarget is NOT the cause (rules out hypothesis b)

MC2_MECH_ANIM_ROTATION_ONLY=0 (raw channels) vs default ON gives an IDENTICAL leg knee:

    walk0  rotOnly +0.02594   |  walk0  RAW +0.02594
    walk16 rotOnly -0.01072   |  walk16 RAW -0.01072

The UB/LB rotation-only fix (waist/spine translation suppression) does not touch leg
chirality and is not implicated. The leg flip is present with or without it.

## EXACT CODE SITE

There is NO bug to fix in mech_skel_import.cpp. The FK is faithful:
- sampleChannel mclib/mech_skel_import.cpp:35-86 -- takes the channel rotation (the
  absolute local rotation authored in the clip). For legs this is the humanoid rotation;
  applying it is correct FK, wrong DATA.
- computeGlobals mclib/mech_skel_import.cpp:88-99 -- global = parent * local; legs are
  composed identically to arms/spine. No knee/bend assumption, no leg special-case.
- EvaluateClipGpuBones mclib/mech_skel_import.cpp:148-186 -- returns joint globals;
  composition order verified correct (prior recon: non-animated bones had localDelta
  exactly 0.0 rest-vs-clip0).

The problem is SOURCE DATA: the marauder GLB carries the atlas humanoid leg clip
(authored for forward knees). The only way to make reverse-joint legs look right under
locomotion is to RETARGET the leg sub-chain, not to patch FK math.

## PROPOSED FIX

DATA/RETARGET problem. A leg-axis mirror of the REST/import path is FORBIDDEN (corrupts
the correct reverse-joint rest and breaks humanoid mechs; see RECON-1).

### Option 1 (recommended, generalizes to all 53): reflect the leg CLIP rotation about
the knee-bend (hinge) axis, ONLY for reverse-joint legs, gated by the rest dot tell.

Reverse-joint and humanoid legs are mirror images about the lateral axis in the sagittal
plane. The walk clip per-frame knee BEND angle is correct in magnitude but authored for
the opposite chirality. For a reverse-joint leg, negate the thigh/calf pitch (rotation
about the lateral hinge axis) of the clip local leg rotations (reflect the sagittal
swing), leaving yaw/roll and all non-leg chains untouched.

- Gate behind per-mech leg-class detection (NOT universal): reuse the dot-product TELL
  from bt2018-reverse-leg-detection-recon-1.md (section THE DATA TELL):
  kneeForward = dot(C-A, fwd) on rest-pose FK globals; < -eps => REVERSE-JOINT.
  Corroborate with foot-child names (Talon* = reverse; single Toe0 = humanoid).
- Implement ClassifyLegMorphology(scene, boneNames) next to BuildSkeleton in
  mclib/mech_skel_import.cpp; pure function of the GLB, zero per-mech data.
- In sampleChannel/computeGlobals, when the node is a leg-chain node (j_*Thigh/j_*Calf,
  possibly j_*Foot) AND the mech is REVERSE-JOINT, reflect the local rotation sagittal
  pitch before composing. Humanoid mechs (atlas) are unaffected. The UB/LB rotation-only
  path is untouched (legs are not waist-chain nodes; isWaistChainNode excludes them).
- WHY per-mech detection is required: the SAME clip drives both rig types, so the engine
  cannot tell from the clip alone which chirality the legs want. The bind-pose dot tell
  is the only signal. Correct for all 53 without per-mech tuning.

CAVEAT to validate first: the reflection axis must be the bind leg lateral hinge axis in
the leg LOCAL frame, not world X. Derive it from rest-pose thigh->calf and calf->foot
vectors (cross product ~= the knee hinge axis). Verify in the harness that post-reflection
marauder walk0 knee dz goes NEGATIVE (behind, matching its rest chirality), the foot
plant still tracks the gait, and atlas stays byte-identical to today.

### Option 2 (fallback, no engine math): author/retarget marauder-specific leg clips
offline (DCC retarget of atlas walk onto the reverse rig) shipped in the marauder GLB.
Robust but needs per-mech art for every reverse-joint chassis -- rejected for all-53.

### NOT acceptable
- Mirroring/negating a leg axis in the IMPORT (rest) path -- corrupts the correct bind.
- Blanket leg reflection for ALL mechs -- breaks atlas/humanoid (their legs are correct).

## Does the fix need per-mech leg-class detection? YES.

Universal leg reflection would break humanoid mechs. The fix MUST be gated on the
rest-pose dot-product leg-morphology classifier (reverse vs humanoid), computed per mech
at import from the GLB it already loads. Reverse-joint mechs get the sagittal-pitch
reflection on the leg sub-chain; humanoid mechs stay byte-identical. This gates the
all-53 push: classify every *_fbx2gltf.glb, spot-check marauder/catapult (reverse) vs
atlas/banshee (humanoid), then enable the leg retarget only for the reverse set.

## File:line references
- FK/clip eval (faithful, not the bug): mclib/mech_skel_import.cpp:35-86 (sampleChannel),
  :88-99 (computeGlobals), :148-186 (EvaluateClipGpuBones).
- Rest FK (correct reverse bind): mclib/mech_skel_import.cpp:188-200.
- Leg-morphology dot TELL to reuse: docs/bt2018-reverse-leg-detection-recon-1.md.
- Marauder ships atlas clips only: harness inspect marauder_fbx2gltf.glb (64 clips, all atlas_moveCore*).
- UB/LB rotation-only (NOT implicated; legs are not waist-chain): mclib/mech_skel_import.cpp:65-84.
