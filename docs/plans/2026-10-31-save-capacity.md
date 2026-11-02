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
- Implementation committed: 3c36d6734. All-target checkpoint PASS (root 709s,
  standalone projects 20s, doctest/seam build 3s); normal shipping build PASS.
- All-target direct deploy refused missing msvcp140.dll; normal build64-ninja
  deploy PASS with require-clean and normal provenance. Exe SHA256:
  ea4fe4e5b33e5fa583eb10bcbce65bab5f055f162d5caf1b2fc8cbf133c94057.
- Host domain PASS: Metropolitan and stock 01/03 quicksave/load/resave at 1800/1900,
  201 ticks after load, 60 contiguous identical SIMID ticks. No capacity aborts.
- Late stock 10 diverges +23 (RNG counter only); 24 diverges +39 (mover 725).
  No claim these are pre-existing or caused by this fix. Original failures retained.
- Older reader refuses the actual new stock 03 save with section_version before
  teardown, no crash, then continues 753 ticks. MC2_MECH_RESTORE_TRACE=1 witnessed.
- Headless wrappers remain red for expected GPU/vegetation/heartbeat observations;
  engine save/load witnesses and SIMID comparison are distinct from wrapper verdicts.
- Farm PASS 4/4: job dc673bf0826e425eab7f4e994244b90a, stock 01/03/10/24,
  standard requested fixture points 1000/1400, each 60 contiguous identical SIMID
  ticks. Actual restored ticks 1000/1448/1140/1000 (movie deferrals); post-load
  windows 601/153/461/601 ticks. CLI exits all 0, job returncode 0; fetched and
  private farm tree cleaned. Receipt: farm-continuation/<job>/stock/receipts.json.
- Capacity implementation and requested domain proofs complete; no blanket smoke
  claim: late 10/24 probes remain divergent and host headless wrappers remain red.
  Follow-up: compare those late failures against a verified baseline before any
  attribution or simulation fix; resolve approval blocks for install cleanup.
- Automatic approval review rejected deleting/reseeding E:/mc2-installs/save-capacity
  with only "blocked by policy". Do not retry that reset by another mechanism.
  A separate fresh copy avoids deleting that existing tree; report skipped cleanup.
- Fresh byte-copy install: E:/mc2-installs/save-capacity-copy (11.603 GiB copied).
  Seed verification PASS; subsequent projection cleanup was also blocked by policy.
  Thus inherited seed writable artifacts remain; do not claim lean/clean staging.
- git diff --check flags CR on the changed line in already-CRLF saveload.cpp;
  no whitespace policy override used.
- Removed our hash-matched testgame_resave.ims fixture (1,466,976 bytes); original
  quicksave restored by guarded backups. Install cleanup freed no measured space.
  Installs left: E:/mc2-installs/save-capacity and E:/mc2-installs/save-capacity-copy.
- Tool slowness / avoidable cost: direct all-target deployment lacks staged CRT;
  recovery forced another 266-step shipping build. Stage runtime CRT in that tree.

## Prior run (historical; terrain exclusion superseded)

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

## W96 first-save RED root cause (2026-11-01)
- Cause (a-variant, but avoidable): 3c36d6734 stamped the OBJS section version 2 on EVERY save. The section container is one packet, so the 1-byte version change alone changed the packet digest (same length). Proof: patching the OBJS version byte 2->1 in the gate's saved save1.ims for mc2_01/03/16/24 reproduces the pinned baseline digests exactly (e.g. mc2_03 1d90195eddf49288).
- Fix: saveload.cpp writes OBJS v1 unless some terrain object has numCellsCovered*2 > legacy buffer; only then v2 (W95 refuses it). Stock saves are byte-identical to W95 and readable by it. No baseline changed.
- Quiet-tag red (CULL_PARITY/LIGHT_CONTRACT/PERF_COUNTERS) came from the reverted light candidate, not this lane.
