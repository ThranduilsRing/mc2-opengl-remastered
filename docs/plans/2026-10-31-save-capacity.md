# Save capacity — 2026-10-31

## Continuation after W95
- Rebased onto e5cf6ba9c; preserved main's packet F code and both OBJS/WNOD registrations.
- Rebase build PASS (mc2 + mc2_asset_viewer, -j6). Old reader preserved in proof root.
- Root cause: native coverage has 81 shorts (40 complete coordinate pairs), not 162 cells.
  Main already has counted BuildingRecord coverage; TerrainRecord remains fixed.
- Implemented counted TerrainRecord writer/reader, safe CopyTo bound, required OBJS v2.
- Unit PASS: 58 cases / 3063 assertions (56s), covering 0/40/41/81/161/162/255 cells,
  malformed/truncated counts, real ObjectSlot fixtures, and section version rejection.
- Selected contracts PASS: 8/8 save and ratchet rows; axis scan took 51s.
- Next: commit, one all-target checkpoint, explicit copy-install deploy, stock then
  CVE-G Metropolitan quicksave/quickload + 60 contiguous identical SIMID ticks,
  actual new save refusal with the preserved older reader.
- Automatic approval review rejected deleting/reseeding E:/mc2-installs/save-capacity
  with only "blocked by policy". Do not retry that reset by another mechanism.
  A separate fresh copy avoids deleting that existing tree; report skipped cleanup.
- Fresh byte-copy install: E:/mc2-installs/save-capacity-copy (11.603 GiB copied).
  Seed verification PASS; subsequent projection cleanup was also blocked by policy.
  Thus inherited seed writable artifacts remain; do not claim lean/clean staging.
- git diff --check flags CR on the changed line in already-CRLF saveload.cpp;
  no whitespace policy override used.

Assignment: A:/Games/mc2-lane4, claude/save-capacity, base c0eba1a0ac1c197594447ea1517e0c208b77d64f.
Budget: <=150 tool calls; batch discovery, reproduce, smallest eligible fix,
one build/test pass per change, requested runtime proofs, local pathspec commits.

## Findings / scope
- Clean assigned branch verified. Existing build is RelWithDebInfo.
- Prior CVE-G receipt: docs/plans/2026-10-29-cxx23-status.md:832-841.
- Base mclib/mech3d.cpp:323-328 rejected numWeaponNodes > 10; legacy slot copies
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
- Local implementation commit: a2193d082. Build of mc2 + asset viewer PASS;
  shared header invalidated 266 steps under four simultaneous lane builds,
  about 16 minutes. Exe SHA256 5f8e9d87d7bf69a548afe7214606f2bf12ebe543770100def2273395fabcbd80.
  Tool slowness: well beyond the two-minute one-file proof target.
- CVE-G base reproduced at tick 1800 (~60 s), cve-base-30/result.json:
  "Mech Has too many weapon nodes to save 12" (three instances), then
  "Object 125 covers too many cells in Save/Load!!". No quickload starts.
- Proof root: E:/mc2-archive-2026-10/proofs/save-capacity. Base executable copy,
  reproduction script, logs, and old-save fixtures are there (not in git).
- Largest mission selection by uncompressed PAK bytes: REAP3 reap3m02
  (12,968,262), MagicExpansion mc2_14 (806,927); mission-sizes.json.
- Automatic approval review rejected the bulk delete/reseed command with only
  "blocked by policy". That reset sequence was skipped; no alternate deletion
  mechanism used. REAP3/MagicExpansion runtime save trials remain unproven.
- Fixed CVE-G reproduced at tick 1800: weapon aborts gone; same fatal
  "Object 125 covers too many cells in Save/Load!!" remains. No load/continuation.
  Receipt: cve-fixed/result.json. Deployment succeeded with normal provenance.
- The initial dependent jobs watched Ninja, which exits before the wrapper's
  final relink. Farm found no exe and deploy refused unreadable provenance;
  both were rerun only after the wrapper finished, without bypassing checks.
- Farm job 6858685ff954408eb8b3370de3f98cc0 finished and fetched; its private
  install/stage were cleaned by host.py fetch. Elapsed 851 seconds (14.2 min).
- SIMID 03/10/24 PASS: 1001/801/1001 paired frames; unchanged baseline chains
  39a6ef01d03af2d1 / 0edf8f29798d6297 / 7a3a56e750289c6f. Duration=20 s,
  headless-both. No baseline/mask/ratchet changes.
- Continuation 03/10/24 PASS: all 60 post-load digests identical to pre-save.
- Overlap audit ON: zero VIOLATION lines across all farm logs, covering
  01/03/10/16/24. Headless 01/16 emit engine summary=pass; their smoke wrappers
  fail the expected render/heartbeat checks. Do not call those wrappers green.
- W94 03/10/24 and W91 03/10 load PASS. W91 mc2_24 crashes with 0xC0000005
  after load; no claim that this is pre-existing or caused by this fix.
  Evidence: farm/<job>/stock/load-w91-mc2_24/run/mc2_24.log and runner.txt.
  A base comparison was attempted, but host.py refused the archived base exe:
  "--exe must be in its source worktree/build tree". Skipped; no relocation or
  provenance bypass. Baseline comparison remains an exact next step.
- Actual base exe refuses a synthetic required WNOD section with
  reason=unknown_required_section before phase=post_destroy, then completes
  tick budget 2200. This is a marker refusal proof, not an oversized mod save.
- Farm Vulkan validation unavailable (layer_unavailable). Host metropolitan
  validation rerun: RTX 2070 SUPER, errors=0 warnings=0, 2029 frames; wrapper
  remains red for seven content errors. Receipt: host-validation.log.
- Current proofs are partial acceptance: the terrain abort, deferred big-mod
  trials, and W91 mc2_24 load failure remain open. No new default-OFF gates.
- Disk: 0 GB deliberately freed locally. Only E:/mc2-installs/save-capacity
  left behind; no additional local installs created. Farm private job cleaned.
- Metropolitan full round trip cannot pass while excluded TerrainObject::CopyTo
  aborts. Object-owner lane must replace its fixed coverage representation with
  counted coordinate pairs, add loader support and an old-reader required tag,
  test >old cell count, then run metropolitan +60 and the deferred mod trials.
