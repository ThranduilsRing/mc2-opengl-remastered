# BT2018-WEAPON-NODE-AUTHORING-RECON-1

Read-only recon. Close the weapon-fire-point gap for imported BattleTech 2018 mechs (all-53
import) so weapons fire from arm/torso/head instead of body origin.

Status: RECON COMPLETE. Modder premise PARTIALLY CORRECTED. The hardpointdatadef JSON files do
NOT contain fire-point transforms. They define WHICH weapon categories may slot per location
(loadout combinatorics), not WHERE the muzzle is in 3D. Fire-point positions must be synthesized
from the rig (bones), not read from JSON.

## 1. hardpointdatadef / chassis_def schema

TextAsset/hardpointdatadef_<mech>.json (16 files: annihilator, archer, assassin, bullshark, crab,
cyclops, flea, gallant, hatchetman, javelin, packrat, phoenixhawk, raven, rifleman, rotunda,
vulcan). Shape:
  ID: "hardpointdatadef_archer"
  HardpointData[]:
    location: centertorso|leftarm|rightarm|lefttorso|righttorso|head
    weapons: [ [slot0 candidate weapon-prefab IDs], [slot1 candidates] ]
    blanks: []          EMPTY in every file
    mountingPoints: []  EMPTY in every file  <-- KEY: NO transforms here

- location = lowercased BattleTech location name (archer.json:5 centertorso, :32 leftarm).
- weapons[slot][] = candidate weapon PREFAB VARIANT IDs, form
  chrPrfWeap_<mech>_<location>_<weapontype>_<class><index> (archer.json:9-16). Trailing token =
  visual rep slot: eh1/eh2 energy, mh1/mh2 missile, ah1 AMS (archer.json:163). ART-PREFAB names,
  NOT skeleton nodes.
- mountingPoints and blanks are EMPTY in ALL 16 files (verified annihilator, archer). Zero
  position/transform/attach-bone data.

TextAsset/chassisdef_<mech>_<variant>.json (gameplay record):
  HardpointDataDefID: "hardpointdatadef_archer"   chassis -> hardpoint table
  PrefabIdentifier: "chrPrfMech_archerBase-001"   chassis -> Unity art prefab (FBX dir)
  Locations[]: { Location, Hardpoints[]: { WeaponMount, Omni }, InventorySlots, MaxArmor, ... }
- WeaponMount enum (all chassisdef_*.json): Energy | Ballistic | Missile | AntiPersonnel. The
  gameplay slot category. chassisdef ALSO has NO 3D transforms (tonnage/armor/structure only).

Where transforms REALLY are: MonoBehaviour/MechRepresentation*.json (Unity component dump):
- LeftArmAttach, RightArmAttach, TorsoAttach, LeftLegAttach, RightLegAttach (json:81-97)
- vfxCenterTorso/LeftTorso/RightTorso/Head/LeftArm/RightArm/LeftLeg/RightLeg/LeftShoulder/
  RightShoulder Transform (json:101-137); leftFootTransform/rightFootTransform (json:158-162)
- WeaponRepresentation.json vfxTransforms[] (json:46+).
BUT each is a Unity {m_FileID, m_PathID} ref into the serialized scene graph. The dump does NOT
include resolved Transform components (localPosition/localRotation) -> muzzle offsets NOT
recoverable. NestedPrefabAnchor*.json carry identity local transforms (json:21-39); real offset
lives in the parent GameObject Transform, unresolved here.

CONCLUSION #1: No usable fire-point transform exists in the dumped JSON. hardpointdatadef gives
the location list per mech + slot count per location -- enough to author WeaponNode names and
weapon-type roles, but positions must come from the skeleton (#3).

## 2. JSON -> MC2 [WeaponNode%d] mapping rule

MC2 INI weapon-node format (parser mech3d.cpp:684-705):
  NumWeapon  <N>
  [WeaponNode0]
  WeaponNodeName <node-id-string>   matched at runtime vs TG sub-shape getNodeId()
  WeaponType     <0..5>
At fire time the engine calls TG_MultiShape::GetTransformedNodePosition(pos,rot,nodeId)
(msl.cpp:1334-1368): scans listOfShapes[] for a sub-shape whose getNodeId() matches the string
(case-insensitive, msl.cpp:1357) and returns its world translation. A WeaponNode fires from
wherever a TG sub-shape of that name sits.

WeaponType enum (MC2, 0-5): JSON WeaponMount categories map to MC2 WeaponType roles. Exact ints
must be confirmed against MECH3D_WEAPONTYPE_* in mech3d.h before generating (recon did not pin
every int; sentinel MECH3D_WEAPONTYPE_NONE used for jump/foot/smoke, mech3d.cpp:704,722,740).
Stock mech INIs are ground truth -- copy their WeaponType integers per category.

Deterministic generation rule (CAN be automated), per chassis variant:
1. Read HardpointDataDefID -> open the hardpointdatadef.
2. For each HardpointData[] entry emit ONE WeaponNode per slot (inner array of weapons[]; archer
   centertorso = 2 slots -> 2 nodes). Equivalently use chassisdef Locations[].Hardpoints[]
   (identical counts AND the WeaponMount category for WeaponType).
3. WeaponNodeName = synthesized name we control, one per (location, slot), e.g. weap_leftarm_0.
   Engine only needs the INI string to match a TG sub-shape name (or bone alias) we emit.
4. WeaponType = map WeaponMount -> MC2 int (Energy/Ballistic/Missile/AntiPersonnel).
5. NumWeapon = total slot count.
6. The cockpit node MC2 expects (literal name cockpit) -> head location -> anchor j_COCKPIT.
Deterministic from JSON. What JSON does NOT give = muzzle offset, so each node anchors to a bone,
accepting the bone origin as the fire point.

## 3. Anchor strategy: bone vs baked-offset vs locator

Rig facts (verified from chrPrfMech_archerBase-001.fbx joint scan) -- clean per-location joints
that survive Assimp import:
- Head/cockpit: j_Head, j_COCKPIT, j_Neck
- Left arm: j_LClavicle, j_LUpperArm, j_LForearm, j_LHand, j_LHandNub
- Right arm: j_RClavicle, j_RUpperArm, j_RForearm, j_RHand, j_RHandNub
- Torso: j_Spine, j_Spine1, j_Spine2, j_Pitch, j_Pelvis
- Legs/feet: j_L/RThigh, j_L/RCalf, j_L/RFoot, j_L/RToe0
There are NO chrPrfWeap_*, attach_*, mount_*, muzzle_*, socket_* nodes in the FBX (scan empty).
So the modder note "may have to add locators" is the CORRECT read: BT2018 parents weapon
GameObjects to these bones at runtime (the *Attach / vfx*Transform refs); those attach objects are
NOT in the geometry FBX.

RECOMMENDED anchor: BONE-ORIGIN (no baked offset, no authored locator)
  head/cockpit -> j_COCKPIT (or j_Head)   satisfies literal cockpit node MC2 expects
  leftarm      -> j_LHand (or j_LForearm)  hand = closest to muzzle for arm weapons
  rightarm     -> j_RHand (or j_RForearm)
  lefttorso    -> j_Spine2 / j_LClavicle   clavicle shoulder-mount, good for SRM/LRM racks
  righttorso   -> j_Spine2 / j_RClavicle
  centertorso  -> j_Spine1
Rationale:
- Rig already gives one well-placed joint per location; bone origin is a good-enough fire point
  (MC2 stock muzzle nodes are themselves just named pivot nodes, not barrel tips).
- Baked-offset is NOT available -- no muzzle offset in the dump (lived in unresolved Unity
  Transforms), nothing to bake. Finer placement later = small hand-authored per-location offset
  table (forward+up nudge along the bone), NOT JSON-derived.
- Authored locators in source art = cleanest long-term but need FBX re-export with added empties
  -- out of scope, unnecessary for first fire.
Interaction with merged-import rig rule (memory bt2018-skel-1a): import bakes each part offset into
vertices; runtime matrices are joint-GLOBAL only (skin = restGlobal(bone) . offset_part . v).
Weapon-node anchors carry NO geometry so they use restGlobal(bone) ONLY -- no part offset. Keeps
muzzles tracking the animated arm/torso when 1B animation lands.

## 4. OPEN QUESTION (needs build+smoke / mc2-render-expert sign-off): empty-geometry TG nodes

To expose per-location fire points VIA THE STOCK PATH, the importer would emit zero-geometry named
TG sub-shapes (one per WeaponNode name) on the merged shape, so GetTransformedNodePosition
(msl.cpp:1357) finds them by name and Mech3DAppearance wiring (mech3d.cpp:684-705) resolves.

RISK: the current merged import deliberately produces ONE shape ("imported_mech",
assimp_importer.cpp:677) BECAUSE per-mesh empties were found to crash the GPU batcher draw path
(memory bt2018-skel-1a: "per-mesh empties crash GPU path"). Re-introducing empty named nodes --
even zero-triangle ones -- re-enters exactly that failure mode.

This is the one item this recon does NOT resolve. It requires:
- Confirm a TG sub-shape with 0 verts/0 tris can exist purely as a transform carrier WITHOUT being
  enqueued into static-prop/mech GPU batch draw records (batcher must skip zero-tri shapes, not
  emit a degenerate draw).
- A build + tier1 smoke (mc2_24 as the imported mech) with empty nodes present, watching for the
  static_prop_overflow / degenerate-draw crash that motivated the single-shape merge.
- mc2-render-expert sign-off on batcher handling of zero-geometry shapes (draw-record emission,
  cull gate, vertex-pool allocation for empties).

SAFER ALTERNATIVE (RECOMMENDED FIRST): do NOT add sub-shapes. Resolve fire points in
Mech3DAppearance by evaluating the BONE global transform directly (import already computes
restGlobal(bone)), bypassing the GetTransformedNodePosition sub-shape scan. Keeps the merged
single-shape invariant intact and sidesteps the GPU-batch risk entirely -- lower-risk than empty
TG nodes and needs no batcher change.

## 5. Recommended implementation plan

1. Pin the enum -- read MECH3D_WEAPONTYPE_* in mech3d.h + one stock data/tgl/*.ini WeaponNode
   block; build WeaponMount -> WeaponType int table. (no build)
2. INI generator (offline tool) -- from chassisdef + hardpointdatadef, emit per-variant
   [WeaponNode%d] blocks with synthesized names (weap_<loc>_<slot>) + cockpit, deterministic.
   Names are ours; positions deferred to runtime bone lookup. (no engine change)
3. Anchor map (data) -- small static table: MC2 location/synthesized-name -> BT joint (table #3).
4. Fire-point resolution -- PREFERRED low-risk path: in Mech3DAppearance resolve each WeaponNode
   world position from the BONE (restGlobal(joint), animated via 1B clip when available) using the
   anchor map, NOT from a TG sub-shape. No empty nodes, single-shape invariant preserved, no
   GPU-batch risk. Build + tier1 smoke (mc2_24 imported mech) to confirm fire points track
   arms/torso and nothing crashes.
5. Empty-TG-node path -- ONLY if #4 insufficient (some engine system requires a real sub-shape).
   Gate behind mc2-render-expert sign-off + the GPU-safety smoke in #4.
6. Optional later polish: per-location forward/up muzzle offsets (hand-authored, not JSON).

What this unblocks: with #1-#4, all 53 imported mechs get deterministic per-location fire points
from the rig with NO hand-authored positions and NO geometry re-export -- closing the "all weapons
fire from body origin" gap. The only build-gated risk (empty TG nodes) is avoidable via the
bone-direct path.

## Citations
- hardpointdatadef: TextAsset/hardpointdatadef_archer.json:1-180 (mountingPoints/blanks empty
  :17-19,:57-59; weapon-id form :9-16; AMS slot :161-164). 16 files total.
- chassisdef: chassisdef_archer_ARC-2R.json:15-16 (HardpointDataDefID, PrefabIdentifier), :47-118
  (Locations[].Hardpoints[].WeaponMount). Enum Energy/Ballistic/Missile/AntiPersonnel (grep all
  chassisdef_*.json).
- unresolved Unity transforms: MonoBehaviour/MechRepresentation.json:81-162 (*Attach +
  vfx*Transform PathID refs), WeaponRepresentation.json:46+ (vfxTransforms),
  NestedPrefabAnchor.json:21-39 (identity local transform).
- rig joints (no weapon locators): chrPrfMech_archerBase-001.fbx joint scan -> j_COCKPIT, j_LHand,
  j_RHand, j_L/RClavicle, j_Spine/1/2, j_Pitch (no chrPrfWeap_/attach_/muzzle_).
- MC2 weapon-node parse: mech3d.cpp:684-705 (WeaponNodeName/WeaponType), :866-877 (reset).
- runtime fire-point lookup: msl.cpp:1334-1368 (GetTransformedNodePosition by name).
- merged single-shape + GPU-batch risk: assimp_importer.cpp:677 ("imported_mech"); memory
  bt2018-skel-1a-bindpose ("per-mesh empties crash GPU path").
