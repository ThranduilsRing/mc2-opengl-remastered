# Review: lane 1 session 5 cleanup (crater, B5.1c, B5.2a/b, baselines)

Range 394ea47dc..94568c316; lane-1 code commits: 7e8d96bea, 94321ef0a, ef02d3a14 (+3b87c4835), 2f4bd7996, a73817ce7, 81fee8f23, b0674c15b, e9b706cfa. Static read only; no builds.

## Verdict
ACCEPT with should-fixes. No must-fix found.

## Must-fix
None.

## Should-fix
1. Partial-failure state in `TG_AnimateShape::LoadBinaryCopy` (msl_anim.cpp ~128): when animation i fails, `destroy()` clears the vector and `listOfAnimation`, but `count` stays at the file's value. A caller that skips the `-1` and reads `count` and `listOfAnimation[i]` gets a null deref. The ASE caller (LoadTGMultiShapeAnimationFromASE, line ~245) forwards the -1, but the actor type loaders (mech3d_type, gvactor, genactor, bdactor_type) discard the return value. Set `count = 0` inside the failure path, as the count<0/>4096 branch already does, and check that the actors cope with a zero-count animation.
2. `MC2_LOG_ONCE` makes [ANIM_TRACK_BAD] log one line per call site for the whole run, so a second corrupt mod file is silent. "One line per rejection" would need a keyed log. Acceptable, but the commit text implies per-file logging.
3. Baseline commit b0674c15b gives the reason (WeaponBolt handle bytes) but not the digests. The confirmation was one check run on mc2_03, mc2_16 and mc2_24, and two on mc2_01. 81fee8f23 (mc2_10) does state the digest and two checks. State the digests and run counts for the four.
4. LG05 +2: named in the e9b706cfa subject and in the b5 notes, but the commit body is empty. Fine, but the body should say that the two rises are the new `return -1` refusals.
5. Destruction-order hazard (theoretical): `ManagerTextureRelease` goes through the global `mcTextureManager`, which is only nulled after `delete`. Any TextureRef member released during `~MC_TextureManager` would use the half-destroyed manager. Not hit today (see Checked-OK 4).

## Nits
- `count = 0; return -1;` in the count>4096 branch: the `count < 0` message says "animations=" for negative values; harmless.

## Checked-OK
1. CraterManager: both creation sites (mission_init.cpp:710, saveload_load.cpp:51) placement-new into fresh `missionHeap->Malloc` memory. The only destroy is Mission::destroy (mission.cpp:1110), which calls `~CraterManager` once, frees the memory and nulls the pointer, so a later site always constructs on new storage and does not leak. No static-duration owner.
2. TG pools: `make_unique_for_overwrite` matches the old unzeroed Malloc. The element types have no constructors. `*Store` members are added next to the views. The pools are per-frame scratch, so nothing is saved to disk or hashed by the determinism trace. `destroy()` resets the store.
3. Loader: the count (<=4096), frame count (<=100000) and exact track bytes are checked before `resize`. There is one log line and a false/-1 return. The ASE caller propagates the failure; see should-fix 1 for the actors.
4. TextureRef: `terrainTextures2` is deleted in `Terrain::destroy` (terrain.cpp:630), which runs from the mission teardown path before the texture manager is deleted at app exit (mechcmd2_frame.cpp:240). `releaseThrough` is null-safe, and the arena struct and cellip handle keep plain indices.
5. `releaseNode` rejects a node with no counted reference (logged once), so the new counted release cannot underflow.
6. Comments are WHAT+WHY; no journal comments seen in the diffs.
7. Saveload baselines: the cause is stated for all five.
