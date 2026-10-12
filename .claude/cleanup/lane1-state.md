<!-- doc-status: current | reviewed:2026-10-12 -->
# Lane 1 state (builder on A:/Games/mc2-main, branch claude/nifty-mendeleev). Hand-over file for the next agent; keep current at every report.

HEAD: `git log -1`. Session 2026-10-12 afternoon (lane-1 Fable, 3 h cap): the host-lease root cause + fix, the mc2_16 music ruling
(owner CONFIRMED mid-session: the ABL builtin reports the script's own request, end-of-track on the scenario clock), board doc_links,
the superseded WIP patch deleted, checkpoint. Never pushed. The orchestrator committed handoff updates on this branch between my commits
(76e9d8cf8, bdca22605, c1f9cafcc: docs only). Lane 3 fixed the mc2_15 writer (ABL concat() overflowing a script string, 76204ae18 on
its branch); it merges AFTER this session, so expect a merge of mclib/abl* next, not here.

## Commits this session (oldest first; every C++ commit built + doctest 2745/2745 + 20 harnesses)
- 821690c2b harness: the host engine lease never follows the cursor-guard test switch. ROOT CAUSE of "three mc2.exe at once":
  `host_lease.disabled()` returned True under `MC2_CURSOR_GUARD_OFF=1`, the test-isolation switch every conftest and check-contracts
  set and that lanes export for their smoke shells (lane 2's live command this morning: `export MC2_CURSOR_NO_PARK=1
  MC2_CURSOR_GUARD_OFF=1; py -3 scripts/run_smoke.py --tier tier1 ... --wait-timeout 3600`; every lane-1 and lane-2 manifest of the
  morning carries `env_gates.MC2_CURSOR_GUARD_OFF=1`, lane 3's do not, and at 11:4x lane 2's mc2_17 ran beside lane 3's mc2_15 while
  lane 3 held the lease). The foreign-engine scan (win_input) was inert under the same switch, so the runner was blind too. The lease
  file was already host-global (`A:/Games/mc2-opengl/.host_game_lease.json`); the per-install `.smoke_leases.json` only serialises one
  install. FIX: fixed host-global LEASE_PATH constant; off only for the owner (`MC2_HOST_LEASE=off` + `MC2_OWNER_NO_LEASE=1`, else
  ignored aloud); never consults the cursor guard; pytest tripwire + a private lease file per test process (`MC2_HOST_LEASE_PATH`, set by
  every conftest and run_tools_tests: test isolation = a private lease, never no lease); countdown line via bounded_wait;
  `py -3 -m scripts.smoke_lib.host_lease status|clear-dead` (status exit 0 free / 1 held / 2 dead holder; names holder, cwd, engines);
  `child_registry.engine_processes()` Toolhelp enumerator independent of the cursor guard (win_input delegates); run_smoke takes the host
  lease FIRST (after the exe guard, before the deploy-folder lease; --verify-only/--resolve-only skip it). Tests: 13 in
  scripts/tests/test_host_lease.py incl. the regression (a launch under GUARD_OFF queues behind a live holder) and two lanes as real
  child processes from two worktree copies against one lease file.
- abf0764c1 sim: getCurrentMusicId reports the script's own request on the scenario clock, never the audio device. mclib/script_music.h
  (pure ScriptMusic + wavDurationSeconds), SoundSystem::scriptMusic armed in playABLDigitalMusic from the .wav header length (audio on or
  off), cleared in stopDigitalMusic; audio side untouched. tests/unit/test_script_music.cpp (4 cases, 17 assertions). New board row
  `sim_no_audio_reads` (scripts/check-sim-no-audio-reads.sh): gosAudio_Get* only in mclib/soundsys.cpp and code/gamesound.cpp.
- d0fcc721c board: check-doc-links scans repo docs only (docs/beta is a git-ignored, never-tracked local folder the checker walked from
  disk: that was the red); lane1-wip-after-limit patch deleted.
- 33d38f204 sim: script_music.h include moved out of soundsys.h's `#ifndef TIMING_H` block (the first slot was inside it: TUs that
  already had timing.h did not compile; my background ninja wrapper reported the tail's rc, so the failure was seen one deploy late).
- 2a83a80fb sim: span / string_view / static_cast at the new code (ratchet LG06/LG09/LG17), check-doc-links spawns git through
  console_child (background_launch row). Ratchet OK at this HEAD.

## Lease proof (task 1)
- pytest: 13 passed (two-lane child-process test: B waits with `[wait] host game lease: ... held by lane laneA`, gets it after A releases).
- Live: `run_smoke --tier tier1 --mission mc2_01 --deploy 0.4` from A:/Games/mc2-main and, concurrently, from a shared sparse clone of
  the tip in the session scratchpad with `MC2_CURSOR_GUARD_OFF=1` exported (the regression recipe): B printed `[wait] host game lease:
  the host game lease is held by run_smoke 0.4 (pid 33276, scripts/run_smoke.py --tier tier1, in A:\Games\mc2-main)` and waited;
  `host_lease status` named A as holder. Earlier the same A waited behind lane 3's tier2 (two countdown lines, 30 s apart). Reports: see
  the checkpoint section. (The archived-copy attempt without .git was refused by the exe guard: the second directory must be a git
  checkout; a `git clone --shared` + sparse checkout of scripts/Gates/tests/smoke/tools works.)

## mc2_16 verdict (task 2)
- `pairs-until-fail --mission mc2_16 --max 6 --duration 30 --env MC2_FIXTURE_ORDER_LIVENESS=1` on exe 2a83a80fb: **6 of 6 pairs
  identical** (2621..2638 sim frames per pair, 84-86 s each, host idle: cpu 8-17 %, procs=none; the morning's 3-of-5 divergence at
  frames 1302..2025 is gone). Baseline committed: `tests/smoke/sim_determinism/mc2_16.json` (1000 frames, chain 381fe40d96777ed3, sha
  2a83a80fb7d6, seed 0x1B7EBC0D, env MC2_FIXTURE_ORDER_LIVENESS=1, written from artifacts/2026-10-12T13-37-03/mc2_16.log). NOT yet
  run: `sim_determinism.py check mc2_16` (two more runs) and the checks of mc2_03/10/24 at the new semantics (NEXT 1).
- Ruling in force (owner confirmed 2026-10-12 afternoon): getCurrentMusicId = the script's last request, -1 when none/stopped; a one-shot
  tune ends at scenarioTime + its .wav length (RIFF header), a looping one never; the device is never read by the simulation
  (board row sim_no_audio_reads). The "nothing playing" branch of every stock mission script now fires on the scenario clock.

## Checkpoint (task 5)
- Build: ninja at 2a83a80fb (fingerprint 2a83a80fb7d6 dirty=0), deployed to 0.4 (manifest 877 rows). `build_contract_harnesses.py
  --run --unit`: 20 harnesses PASS, doctest 2745 passed / 1 skipped.
- Board (full run at d0fcc721c, before 2a83a80fb): 163 passed / 4 failed: cxx17_ratchet, background_launch, scripts_tests -> all three
  fixed by 2a83a80fb (ratchet OK, background-launch OK 27 sites, scripts_tests 392 passed at 2a83a80fb); contract_coverage_selftest
  is PRE-EXISTING and not mine: its arms "tools/check_ubo_layout.py un-wired" / "tools/glsl_port_diff.py un-wired" did NOT fire because
  the glsl_port_diff row is gone from check-contracts.sh (queue item 2: "re-add two registry rows" after visualDisplaceFar PORT markers)
  and the tools are covered by manual exemptions; orchestrator/lane 2 territory. doc_links green.
- tier1 proofs at the final exe (2a83a80fb7d6, fingerprint OK): mc2_01 PASS twice, reports 2026-10-12T13-24-49 (from mc2-main,
  8192 frames, 148 fps) and 2026-10-12T13-27-01 (from the clone, 8192 frames, 148 fps): the two concurrent invocations of the lease
  proof, run one after the other by the lease.
- Full tier1 (`run_smoke.py --tier tier1 --deploy 0.4 --continue --wait-timeout 3600`), exe 2a83a80fb7d6, fingerprint OK: report
  **2026-10-12T13-37-55 = 5/5 PASS** (mc2_01 8192 frames 148 fps, mc2_03 8192/84, mc2_10 8192/107, mc2_17 8192/134, mc2_24 8192/126,
  +0 heartbeat misses): the first clean 5-of-5 of the day, with the host lease serialising the lanes.

## NEXT for the next owner, in order
1. `sim_determinism.py check` on mc2_03 / mc2_10 / mc2_24 at the new exe: the music semantics changed what every stock script sees, so
   the three committed SIMID baselines (recorded under the old device-driven id) may differ; if they do, re-record them with this reason
   (ratchet rule), not a code change.
2. contract_coverage_selftest red (above): re-add the glsl_port_diff row once the PORT markers exist, or teach the selftest that an
   exempted tools/ checker cannot be "un-wired".
3. Save round trip (unchanged from the morning state): MechWarriorData / CommanderData / FIT fields differ run to run; baselines not
   committed (ledgered in saveload-fidelity-2026-10-11.md).
4. B0 remainder: alloc_probe into a proof build's gos_Malloc; mc2_15 under ASan after lane 3's writer fix merges.
5. The stale PostToolUse hook `python3 .claude/hooks/shader_deploy.py` in the untracked settings still fires on every Write/Edit in this
   worktree (the cull deleted the script; the owner removed the entry from one settings file, not this one): remove the entry.

## Tool notes from this session
- A background `cmd > log; echo rc >> log; tail log` wrapper reports the TAIL's exit code to the task system: read the log's rc line
  before deploying. Better: run builds in the foreground with a long timeout.
- soundsys.h wraps every include in `#ifndef X_H ... #endif`; a new include pasted after one of them lands inside the guard.
- mclib/soundsys.cpp is CRLF throughout; Git Bash `sed -n | cat -A` hides the CRs. Patch scripts must detect the newline per file.
- The cxx17 ratchet `--check` scans HEAD blobs, not the working tree: a fix shows green only after it is committed.
- `python -c` with a `\\n` inside a Bash tool call is mangled; keep patch scripts in files (Write tool) and run them.
- The exe guard and the fingerprint verify need git in the run directory: a plain tree copy is refused; use `git clone --shared`.
- Lane state of the host lease at any time: `py -3 -m scripts.smoke_lib.host_lease status`.

## Rules (unchanged)
- Commit by explicit pathspec after `git diff --cached --name-only`; `git commit -F <file>`; trailer `Co-Authored-By: Claude
  Fable 5.1 <noreply@anthropic.com>`. Commit BEFORE build + deploy. Launches only through `scripts/run_smoke.py --deploy 0.4` /
  `tools/sim_determinism.py` / `tools/saveload_roundtrip.py`, background-only, never `--no-lease`. Never kill cl/ninja by name. Never
  contort code for a ratchet (rewrite the baseline with the reason). Never touch GameOS/, RenderCore/, shaders/, Gates/ beyond a task's
  need, nor tests/unit/test_gate.cpp, nor gos_static_prop*.
