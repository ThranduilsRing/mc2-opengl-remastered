# BT2018 reverse-jointed leg detection — RECON-1 (read-only)

Status: RECON (no edits, no build). Determines why the imported MARAUDER legs look
reverse-jointed-but-backwards, and whether BT2018 data has a tell to auto-classify
reverse-joint vs humanoid across all 53 mechs.

Harness: build64-harness/RelWithDebInfo/mech_import_harness.exe (gpu-bones --rest).
GLBs: BattleTech_2018_Dump/MadCat/marauder_fbx2gltf.glb and Atlas/atlas_fbx2gltf.glb.

## VERDICT (root cause)

The backwards appearance is NOT an import bug. The marauder IS genuinely
reverse-jointed (digitigrade / chicken-walker) in the BT2018 source rig, and we
import it faithfully and self-consistently. The axis remap does NOT flip the bend.

Candidates tested and resolved:

1. AXIS flip (mechToMC2Pos) — RULED OUT. assimp_importer.cpp:172-177 :
   mechToMC2Pos = (x,y,z) -> (-x, +y, -z). Negating X and Z with Y/up unchanged is a
   180 deg rotation about the up axis (even parity, winding preserved; comment
   assimp_importer.cpp:168-170). A 180 deg yaw is a rigid-body rotation and CANNOT
   mirror the knee relative to the body; thigh->calf->foot->toe relations are
   invariant. So it applies identically to both mechs and flips neither bend.

2. Bind-pose geometry corruption — RULED OUT. Marauder bind pose is internally
   consistent and anatomically correct for a bird foot (evidence below).

3. Animation / FK composition — RULED OUT for bind pose. Numbers below are --rest
   (pure FK of node defaults, no clip). Reverse geometry exists in the bind pose
   itself; rotation-only retarget (mech-ublb-rotation-only-retarget) does not touch
   leg chirality.

Why marauder looks wrong but atlas looks fine under the SAME transform: both pass
the identical 180 deg yaw, which preserves each rig own bend. Atlas is humanoid
(knee forward) and reads normal; marauder is reverse-jointed (knee back) and reads
as a chicken-walker (correct for a marauder, surprising to an eye expecting a
humanoid knee). No per-mech bug; a per-mech morphology the importer does not yet
distinguish.

## EVIDENCE (gpu-bones --rest, Assimp Y-up space, translation column of FK global)

BT FBX/GLB space: Y = up, Z = body forward/back, X = lateral. t=(x, y, z); z is the
front/back axis. Axis remap is applied later at vertex bake (assimp_importer.cpp:638),
so these joint globals are pre-remap.

MARAUDER (reverse-joint) left leg:
    j_Pelvis     t=(+0.00000,+0.08593,-0.00082)
    j_LThigh     t=(+0.02308,+0.08593,-0.00082)
    j_LCalf      t=(+0.02308,+0.06842,-0.02577)   KNEE Z=-0.0258 (BEHIND)
    j_LFoot      t=(+0.02308,+0.00897,-0.00146)   ankle ~0
    j_LTalon1    t=(+0.02308,+0.00526,-0.01098)   rear dewclaw at -Z
    j_LIndex1    t=(+0.01382,+0.00535,+0.00979)   front toe at +Z
    j_LPinky1    t=(+0.03206,+0.00535,+0.00979)   front toe at +Z

ATLAS (humanoid) left leg:
    j_Pelvis     t=(-0.00000,+0.08632,-0.00000)
    j_LThigh     t=(+0.02849,+0.08630,+0.00000)
    j_LCalf      t=(+0.02849,+0.04622,+0.00294)   KNEE Z=+0.0029 (FORWARD)
    j_LFoot      t=(+0.02849,+0.01179,-0.00698)   ankle
    j_LToe0      t=(+0.02841,+0.00245,+0.01611)   single toe at +Z (forward)

Foot fan confirms bird foot intact: j_LTalon1/2 at -Z (rear dewclaw behind),
j_LIndex1/2 and j_LPinky1/2 at +Z (front toes). Skeleton morphology also differs
(harness inspect): marauder foot children = Talon1/2,Index1/2,Pinky1/2; atlas foot
child = single Toe0.

Knee projected onto foot-forward direction (forward = sign of toeZ minus ankleZ):
    MARAUDER: knee-ahead-of-ankle = -0.02430  -> REVERSE (knee behind)
    ATLAS:    knee-ahead-of-ankle = +0.00991  -> FORWARD (humanoid)
Clean opposite-sign separation.

## THE DATA TELL

NO explicit reverse-joint field exists in BT2018 data. Searched every
chassisdef_*.json in BattleTech_2018_Dump/TextAsset/ (35 present): no
reverse/chicken/digiti/leg key. Only skeleton-ish field is PrefabIdentifier /
PrefabBase (e.g. chrPrfMech_annihilatorBase-001), naming the art prefab; using it
needs a hardcoded per-mech allow-list (NOT generalizable; marauder/atlas/catapult/
banshee chassisdefs are not even in this dump). Reject the explicit-field path.

Reverse-joint is encoded ONLY implicitly in bind-pose geometry. The robust
data-driven tell is geometric, from the GLB the importer already loads:

TELL (per leg, from rest-pose FK joint globals):
  A = ankle (j_LFoot/j_RFoot), C = knee (j_LCalf/j_RCalf),
  T = front toe (j_L*Toe0 humanoid, else j_L*Index1 bird foot).
  fwd = normalize of horizontal component of (T - A)   [drop Y/up]
  kneeForward = dot(C - A, fwd)
  kneeForward < -eps  => REVERSE-JOINT (digitigrade)
  kneeForward > +eps  => HUMANOID
Margins: marauder -0.0243 vs atlas +0.0099 per normalized unit; eps ~ 0.002.
Compute both legs, require agreement/majority. Corroborating signal: a Talon* foot
child = strong reverse marker; a single Toe0 = humanoid. Dot-product is primary.

## CORRECTION PROPOSAL (auto across all 53, no per-mech tuning)

First CONFIRM the complaint. Two cases, different fixes:

- A: marauder SHOULD be a chicken-walker and it is. NO correction needed; the import
  is already correct. Marauder/catapult are reverse-jointed in canon. Document and close.
- B: the bend is wrong for the mech FACING (knee toward camera-front while the mech
  faces away). That is a FACING issue (the 180 deg yaw), NOT per-leg chirality, and
  it would affect arms/torso/cockpit too. If only legs look off while torso/arms
  face correctly, it is case A.

If a per-morphology behavior is genuinely wanted, wire the detector, do NOT hand-tune:
  1. Add ClassifyLegMorphology(scene, boneNames) in mclib/mech_skel_import.cpp next
     to BuildSkeleton, returning ReverseJoint/Humanoid/Unknown via the TELL dot test
     on rest-pose FK globals (reuse EvaluateRestGpuBones). Find ankle/knee/toe by
     name suffix; Unknown if missing.
  2. Pure function of the GLB -> generalizes to all 53 with zero per-mech data.
     Validate over every *_fbx2gltf.glb, spot-check marauder/catapult (reverse) vs
     atlas/banshee (humanoid).
  3. Only THEN key new behavior off the flag. The FK/import is morphology-agnostic
     and correct; the flag drives NEW behavior, it does not "correct" the faithful bend.

Do NOT mirror/negate any leg-chain axis to "fix" the bend; that corrupts the correct
reverse-joint geometry and breaks humanoid mechs.

## File:line references
- Axis remap (180 deg yaw, even parity): mclib/assimp_importer.cpp:168-177 (mechToMC2Pos).
- Vertex bake applies remap: mclib/assimp_importer.cpp:638-639.
- Shared FK: mclib/mech_skel_import.cpp:188-200 (EvaluateRestGpuBones), :88-99 (computeGlobals).
- Rotation-only retarget: mclib/mech_skel_import.cpp:15-86.
- BT data has no reverse field: BattleTech_2018_Dump/TextAsset/chassisdef_*.json
  (only PrefabIdentifier/PrefabBase).
