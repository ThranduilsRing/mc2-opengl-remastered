# Lane 1 cleanup review: render/camera coupling fixes (2026-10-15)

Pinned commits: a08dbd0e1 059366c0e 1ba8e3960 732b0635f 0062d6f98 a182b11c3 1ec341af1 47522b3fa (branch claude/nifty-mendeleev).
Read-only review: nothing was built or launched. **0 blockers, 3 should-fix, 6 nits.** No CRLF in any code hunk (0 CR bytes).

## updateSimPose (1ba8e3960)
- Same math as the visible path minus the draw work: CONFIRMED. It runs the same steps as the updateGeometry AnimPose stage on
  mechShape: setAnimation, SetFrameNum (skipped when DESTROYED, as in updateGeometry), and the joint_torso rotation. It then
  calls TransformMultiShape_BuildRecipe, which is the function TransformMultiShape_HierarchyOnly already uses for the on-screen
  GPU mech body. That fills listOfShapes[].shapeToWorld, and TG_MultiShape::GetTransformedNodePosition (msl_shape.cpp:270)
  reads only that matrix, so the transform is needed and is the right one. It skips the shadow shape, lights and leaf work.
- Once per sim step and no double transform: CONFIRMED. `if (...) updateGeometry(); else updateSimPose();` in
  Mech3DAppearance::update (mech3d_update.cpp:460) runs exactly one of the two per update() call.
- **S1 (should-fix): the camera still reaches imported (BT2018) mechs.** getWeaponNodePosition resolves an imported mech's
  firepoint from mc2mechanim's live clip globals (GetImportedNodeWorld). That clip advances only inside updateGeometry (the
  BT2018-SKEL-ENGINE-1B block at mech3d_geometry.cpp:124). updateSimPose does not advance it, so an off-screen imported mech
  fires from a frozen clip. No stock mission is affected, so this is not a blocker. Either advance the clip in
  updateSimPose or record the gap in BUGS row 38.
- **S2 (should-fix): the cost is not stated.** Neither the commit, BUGS row 38 nor the code says what one more hierarchy walk per
  off-screen mech per sim step costs. A Tracy zone exists (GameLogic.Mech3D.UpdateSimPose). Record one figure, for example
  the zone mean on mc2_24 at 1x and 4x, next to row 38.

## The six node queries (getHitNodeLeft/Right, getWeaponNodePosition, getNodeNamePosition, getNodeIdPosition, getNodePosition)
- Each one only lost its visibility early-return. The rest of each function is unchanged, so a visible mech returns the same value
  as before. For a mech out of view, the query now reads the shapeToWorld that updateSimPose wrote in this step's update(),
  where before it returned the origin. When a query runs before the mech's own update() in the same step, it reads the previous
  step's pose. The visible path has always worked that way, so the order is unchanged. No query reads a transform older than one
  step, unless S1 applies.
- Removing s_lcSkipCount3d / LC3D_TRACE in mech3d_type.cpp left no dangling use.

## damageThisFrame (0062d6f98, 47522b3fa)
- The reset now happens inside the only consumer (handleWeaponHit, mech_damage.cpp:626), stamped with the global `turn`, so it
  happens once per sim turn, before that turn's first accumulation. At 1x with one render per turn, this is the same sum as
  before: the old reset ran after the turn's hits, in render().
  One behaviour change is deliberate and not stated: if the object manager skips render() for an off-screen mech
  (not verified here), that mech used to sum damage across turns at 1x too. That would be part of why mc2_24 changed at 1x,
  so the commit should say so (nit N1).
- `damageTurn` is not saved. It defaults to -1 and a load builds new objects, so the first hit after a load resets the sum. Safe.
- No other reader exists (grep of code/ and mclib/: mech.cpp CopyTo/Load, mech_init.cpp:38, mech_damage.cpp). Load is unchanged
  and reads the 0 that is now saved.

## windowsVisible (a08dbd0e1, 732b0635f)
- The save writes 0, and GameObject::Load already zeroes the field (gameobj.cpp:1536), so the load path is unchanged. The
  readers are light.cpp:141 (`windowsVisible == turn`) and the getter at gameobj.h:1078. The writers stamp it again before it
  is read, and both readers treat 0 as "not seen this turn". Tolerant: confirmed.

## Mission map refusal (059366c0e)
- MissionMap::init returns 0 with exactly one log line for each reject path: the readExact line, or the dimension line. The
  `||` chain stops at the first failure, so no second line is printed.
- **S3 (should-fix, ruling): Fatal is the wrong level for corrupt mod data.** Under the readers rule a malformed pak is
  content, not an engine invariant: the mission load should be refused with one log line, and the game should not end.
  Fatal is acceptable for now only because MOVE_readData's callers (mission_init.cpp:820, saveload_load.cpp:134) have no
  path to refuse a load, and the function already uses Fatal for its other read failures (move.cpp:620, 641). Route a
  follow-up: MOVE_readData returns a refusal, and mission_init / saveload_load abort the load and return to the menu. This
  touches the Codex-claimed saveload_load.cpp, so it needs the claim first. Note also that the planet field can be
  overwritten before a dimension rejection. That does not matter while the load is refused.

## mc2::pak::readExact + doctest
- It is correct. It checks the index range, which avoids seekPacket's STOP, a seek error, the size, and the decoded length.
  The header comment says `dst` may be partly written when decoding fails. It is WHAT+WHY.
- N2: the doctest has no compressed-packet or decode-failure case, which is the one branch with partial writes.
- N3: if a REQUIRE fails partway through, the test leaves g_mc2TestStopFatal=false, the stderr override and the "*:info" spec
  set. Restore them in a guard. The scratch .pak goes to the current directory; use a temp path.

## mc2_24 SIMID re-record (1ec341af1)
- The reason is sound and specific. The two sim-changing commits are named, the old and new visible chains are given and
  reproduced on three exes, and the other three baselines are shown unchanged over 1000 frames on the exe of 37f2241a3.
  The recording exe (a182b11c3) does not include 47522b3fa, which changes only the save, so the SIMID is unaffected.

## Comments
- The comments are WHAT+WHY, and the "C3:" journal comments are removed. Nits: N4 mech_damage.cpp:624 "the reset used to live in
  render()", N5 mech.cpp:887 "(what it carried while render() reset it)", and N6 mech3d_type.cpp "a weapon node that fell back to
  the mech origin off screen made ..." are history. Say the current invariant instead (for example "one sim turn's damage; it does
  not depend on how often the frame is drawn").

## Ruling input: five saveload baseline failures (exe 47522b3fa vs baselines of 55f62bc4d / afe0e21a7)
Both groups of commits plausibly move these bytes, but the lane's own commits are enough to explain the MechData diffs.
a08dbd0e1 changes MechData on every mission, because MechData embeds GameObjectData: before, a mech the camera had seen at
1x saved `windowsVisible = turn`, and now it saves 0. 1ba8e3960 and 0062d6f98 change the simulation by save time, through
poses, shot origins and flinches. That is shown on mc2_24, and can happen on any mission where an off-screen mech fires or
is hit before the save frame. 47522b3fa did not move the digest, which fits damageThisFrame already being 0 at that save
frame. mc2_16 is different: its SIMID is unchanged over 1000 frames and its diff is in data packets 3..258 (several object
classes), which points to windowsVisible on non-mech objects (buildings and terrain objects stamp it too) or to the merged
sim-s4b2b route wiring (a0614d7d2), not to the mech fixes. Before re-recording, confirm with save_inspect that the differing
offsets map to windowsVisible or to sim-state fields, and not to new junk such as pointers or padding (rows 30/31 were
that class). With that attribution, plus a same-exe stability pair on all five (mc2_10 also at 4x), a re-record on the final
exe is sound. Without the attribution, a re-record could hide a sim-s4b2b save defect behind the planned changes.
