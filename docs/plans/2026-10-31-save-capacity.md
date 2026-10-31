# Save capacity — 2026-10-31

Assignment: A:/Games/mc2-lane4, claude/save-capacity, base c0eba1a0ac1c197594447ea1517e0c208b77d64f.
Budget: <=150 tool calls; batch discovery, reproduce, smallest eligible fix,
one build/test pass per change, requested runtime proofs, local pathspec commits.

## Findings / scope
- Clean assigned branch verified. Existing build is RelWithDebInfo.
- Prior CVE-G receipt: docs/plans/2026-10-29-cxx23-status.md:832-841.
- mclib/mech3d.cpp:323-328 rejects numWeaponNodes > 10; legacy slot copies
  only 10 bytes from each long/float vector, not ten elements.
- code/terrobj.cpp:1728-1732 rejects numCellsCovered >= 162 and copies into
  TerrainObjectData::cellsCovered. This object-class serializer is explicitly
  excluded by the brief; do not modify it or objmgr_io.cpp.
- Eligible weapon fix: remove only the appearance writer STOP. Existing ANIM v3
  uses counted vectors (65536 elements) and restores them after legacy objects.
  Mission::save adds required WNOD v1 only when a saved mech exceeds 10 nodes;
  old readers reject the unknown required section before mission teardown.
  Under-cap legacy bytes and section payloads stay unchanged.
- Terrain limit remains excluded. TerrainObjectData::cellsCovered is short[81],
  and CopyTo copies 2*numCellsCovered shorts; blindly raising 162 is unsafe.

## Receipts / next
- repo_query preflight: clean, branch/root correct; harness extraction ambiguous
  (use canonical explicit commands).
- Isolated install created: E:/mc2-installs/save-capacity. No extra installs.
- Base mc2 and asset viewer rebuilt; deploy initially refused both stale outputs
  separately. Correct rebuilt deployment completed; no provenance bypass.
- Unit: 7 cases / 257 assertions PASS (save sections:*, 67 s incl build),
  including 32 weapon nodes through UnitAnimState and the section container.
- Contracts: 8/8 selected save + five ratchet rows PASS, 40.48 s; no baselines
  raised. This is a partial board, not full contracts.
- git diff --check reports CRLF carriage returns on the eight added lines in
  the already-CRLF code/saveload.cpp; no whitespace-policy bypass applied.
- First reproduction invocation refused duration=60; corrected duration=30
  keeps save tick=1800 (~60 s sim time). No game ran for the refused invocation.
- Next: reproduce CVE-G at tick 1800, REAP3/MagicExpansion largest missions;
  preserve exact abort source/line. Determine eligible weapon fix without
  touching excluded object codecs. Record scope-blocked terrain work honestly.
- Remaining requested proof: capacity unit test; metropolitan save/load +60;
  build, contracts, five unchanged ratchets, SIMID 03/10/24 20s, continuation
  03/10/24, W91/W94 loads, overlap 01/03/10/16/24, validation=0.
