# PC mc2_10 stall investigation checkpoint

Assignment: slot07, claude/pc-mc2-10-stall, runtime baseline
d63c403c9715b6a664a3513b445430e722c94c10. No production changes.
This investigation is INCOMPLETE; no root cause or mitigation is proved.

Evidence root: E:/mc2-archive-2026-10/proofs/pc-mc2-10-stall/
The state.md there contains chronology; logs/dumps stay outside Git.
Retain the single verified private install E:/mc2-installs/pc-mc2-10-stall
for continuation. Windows game/viewer compile and deployment passed; PDB
hash and 906-row deployed manifest match the baseline. Do not rebuild it.
Box build and both smoke-lite maps passed at the same baseline. Box units
failed in the inherited VFX frozen-driver parent: 3905/3906 cases passed.
See ../box/mc2-slot07/d63c403c/{rows.tsv,.tails/unit.log} under proofs.

## Observations, not conclusions

- pin1: actual mask 0x3, PASS, load 89278ms, first presentation at 89.604s
  of the observed process, 1082 frames, zero content errors. Last startup
  phase: first_frame_presented. Wrapper wall 140.0s includes preflight/lease.
  Main thread at the load-time dump executes vegetation hashf/sinf through
  Vegetation::missionLoaded and Mission::initFinalizeGeometry. At 65s the
  main thread had 6.65625s user + 2.5625s kernel CPU. This is not a stall stack.
- pin2: actual mask 0x3, FAIL timeout, zero frames, runner cap verdict at
  140.7s. Last mission phase terrain_ready (observed at 110.356s), last
  live line a content-model cache hit at 115.867s. Wrapper wall 441.7s
  includes a long foreign lease wait. Do not count wrapper time as startup.
- These two runs used live noninvasive debugger attachment at 65/105s.
  Attachment can suspend the game; pin2 cannot establish an unperturbed
  reproduction. The first dump was created; symbol/parameter formatting
  exceeded the observer's 90s bound. The 105s attachment failed after the
  game exited. Preserve both CPU observations and the first dump.
- Offline plain-stack extraction subsequently succeeded for pin2; see
  pin2-offline-plain-stacks.log. Main thread is in memmove through
  vector<unsigned long>::assign -> TerrainColorMap::init -> Terrain::init
  -> Mission::initTerrain. Audio and AMD helper threads wait normally in
  that snapshot. This is a load-time snapshot, not proof of where the game
  was at its eventual deadline. No main-thread lock, audio or GPU-fence
  wait was captured. Do not infer continuous spinning from one CPU sample.
- The first observer matched the wrong phase-tag spelling; reconstruct
  phases from the timestamped *-live.log files. The source was corrected
  for subsequent runs. First two observers also lacked scripts/ on sys.path,
  causing the deploy-staleness helper import to skip. Fingerprint and
  deployed-manifest coherence passed; do not call all preflights green.
- pin2 was observed running at Windows Idle priority. nofocus.spawn passes
  startup/window kwargs to Popen without an explicit game priority; an Idle
  harness therefore produces an Idle game. Rules require BelowNormal games.
  This is a concrete launch mismatch, NOT proof of the intermittent stall.
- MC2_NO_CPU_PIN=1 is not unrestricted on this baseline: affinity_mask
  selects its legacy fallback, then apply_affinity clamps to game_cores().
  Record actual masks for the three requested environment-variable runs.
- The verified seed creation took roughly 18 minutes under host load.
  Repeated setup-process stacks first showed NtCreateFile on a cityblock
  asset, later progressed through Python and copying. No persistent disk
  deadlock or missing Defender exclusion was established.

## Exact continuation

1. Keep this runtime baseline and existing install. Do not rerun green
   Windows/box compile or box smoke on the unchanged engine tree.
2. DO NOT use probe.py's existing timed live attachments as a clean matrix.
   Replace them with CPU/log observations only, and an external
   MiniDumpWriteDump at the runner's timeout boundary BEFORE terminating
   only its own game. Resolve that dump offline. This avoids perturbing the
   startup being measured. stacks.cpp supports --dump INSTALL DUMPFILE;
   use plain ~* k, not expensive parameter formatting (~* kp).
3. Complete a clean 5 default-pin + 3 MC2_NO_CPU_PIN=1 mc2_10 matrix.
   Preserve phase timestamps, actual masks AND priorities, per-thread CPU,
   and PC lease waits. Review VK/pipeline, audio and worker logs at a stall.
4. Confirm whether Idle inheritance causes the observed failure with a
   controlled BelowNormal comparison. If it does, fix nofocus.spawn to set
   the mandated priority and add an inheritance regression (existing
   scripts/tests/test_nofocus_env.py is the likely home). Do not silently
   equate the flag comparison with fully unrestricted CPU affinity.
5. Only after causal evidence: make the smallest code fix or document the
   demonstrated host cause and mitigation. Run requested covering rows,
   tests/lint and final proof: 8/8 PC night + 2 day passes. If engine changes,
   new box build/unit/smoke and final Windows game/viewer compile are owed.
6. Preserve crash evidence before any install cleanup. Delete dumps over
   200MB after extraction; current small dumps are far below the limit.

No pin3 game started: its waiting observer and matrix parent were stopped
between games to hand back within the call budget. No unrestricted runs,
post-fix acceptance or root-cause covering tests have been completed.
