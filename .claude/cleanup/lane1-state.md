<!-- doc-status: current | reviewed:2026-10-12 -->
# Lane 1 state (builder on A:/Games/mc2-main, branch claude/nifty-mendeleev). Hand-over file for the next agent; keep current at every report.

HEAD: `git log -1`. Session 2026-10-12 (lane-1 Fable, 08:35 start, cap 14:35): T1 + T3 of `program-review-2026-10-12.md` section 8 from the
saved WIP patch (`lane1-wip-after-limit-2026-10-12.patch`, now fully landed or superseded; the patch file can go). Never pushed. Lane 4's
cull wave 1 was merged into this worktree by the orchestrator mid-session (e07759326 + 6c2e2e2fe); nothing of it touched C++ or tests/unit.
Task 7 (hierarchical-brain / modder ABL status audit) was DROPPED from this lane by the coordinator: an external worker owns it.

## Commits this session (oldest first; every C++ commit built + doctest 2743)
- 38abacf37 sim_determinism.py: per-mission baselines under `tests/smoke/sim_determinism/<m>.json` pin the install data fingerprint + mission
  seed; `check <m>` applies the recorded fixture env; STARVED pairs (host too busy) are retried, not counted; old single baseline file removed.
- e81d237bf `MC2_MULTIPLAYER` CMake option default OFF: multplyr*.cpp off the target; `code/multplyr_null.cpp` = null MPlayer + statics +
  42 generated no-op members (`tools/dev/gen_multplyr_null.py --from-link-log`); `LogisticsData::startMultiPlayer` compiled out (per-source
  define, so toggling recompiles 4 TUs). ON and OFF both link; default build.ninja carries only multplyr_null.cpp.obj.
- 5e0341d37 SIMID line gains `nobj= objects= nabl= abl= rng=sim/app/wx`: buildings + turrets digest (sorted by partId), ABL instances in
  registry order (handle, FSM state/prev, init flag, FNV of static vars), per-stream draw counts (`SimRandomDraws()` in mclib/routines.cpp,
  reset at the mission seed; `Weather::drawCount()`); `ABLi_getNumModuleInstances/ABLi_getModuleInstance` added. Radio style/callsign rolls
  and the terrain texture fractal noise moved to the appearance stream. The four `projectForScreenXY` sites the review named are in
  `BattleMech::render` / `GroundVehicle::render` already (not update): nothing to move.
- fd47cde46 `MC2_SIM_TRACE` compile-time option (default OFF): mclib/sim_trace.{h,cpp}, docs/sim-trace.md, trace points (RNGF, RNGCALL with
  std::stacktrace, HIT/INJURE/DESTROYLOC/CTBRANCH, BOLT, REQ/CALC/CALCDONE, SETTGT, SADD/SREM, WX, ABL, SETPOS/SCROLL); determinism-hunt/
  deleted. 8713e228e: ABL tag behind `MC2_SIM_TRACE_ABL` (thousands of lines per frame). 8cf860cc6: ABLRAND tag (module + script line
  of every script `random()`).
- 62a605c43 B0 harness: `MC2_ASAN` in cmake/Mc2Standalone.cmake + `build_contract_harnesses.py --asan` (doctest under ASan: 56 s build+run
  beside three other lanes, native 10 s); mclib/alloc_probe.{h,cpp} (portable size seam, in mclib, nothing calls it in a default build) +
  tests/unit/alloc_probe_new.cpp (mc2_tests' global new/delete through the probe) + `ScopedLeakCheck` + 7 doctest cases (self, std owners,
  EString); `tools/own_inventory.py` (691 types) + `tools/pointer_order_scan.py` (14 sites / 12 files) with baselines under tests/unit and
  four check-contracts rows (check + selftest each).
- 1209d66cb / 27ba3cd99 static_cast in the new code; 0a082e1fb + 604a75a28 cxx17 ratchet baseline rewritten (reasons in the messages:
  multiplayer stubs mirror the frozen header LG05/06/17; digests share digestWorld's (records, n) shape LG17; alloc_probe size header LG18)
  then tightened; tier1 flake-log row.
- 6eef8e2b2 dup_lines back under baseline (compareQuantized, sortTotal<>, foldMoverVisual/Intent shared) + split-map row (coordinator's two
  reds at the merged HEAD). 81d82022b SIMID baselines mc2_03 / mc2_10 / mc2_24. d821a03c4 saveload ledger.

## mc2_24 verdict (task 2)
- `pairs-until-fail --mission mc2_24 --max 12 --duration 30` on exe 0a082e1fb (C++ identical to the merged HEAD): **12 of 12 counted pairs
  identical**, 15 run, 3 STARVED (host at 100 % from other lanes' builds and up to three games at once: the lease is NOT serializing the
  other lanes' runs, `procs=mc2=3` in pairs.log). Frames per pair 310..3471. Camera-park pair (`--env-a/--env-b MC2_CAMERA_PARK=...`) on
  6eef8e2b2: identical over 3285 sim frames (first attempt starved at 284). mc2_24 is CLOSED structurally (b6c154f59 + 5e0341d37).
- Baselines committed: mc2_03 (pair 2580 frames), mc2_10 (1620; the plan listed it as diverging near frame 1085, it is identical now),
  mc2_24 (3444), 1000 frames each, sha 6eef8e2b2, seed + data fingerprint `17051a2e90619f97` of the 0.4 install.
- mc2_16 under `MC2_FIXTURE_ORDER_LIVENESS=1`: NOT recorded. 3 of 5 pairs DIVERGED (sim frames 2025, 1393, 1327, 1302): at the first
  differing frame every digest (movers, objects, ABL) is still equal and ONLY the simulation-stream draw count is one apart (`rng=903/325/0`
  vs `902/325/0`), the new counter naming the consumer class before any state moved. The [DT] trace (MC2_SIM_TRACE exe) caught the extra
  draw in run b at frame 1302: stack `RandomNumber <- execStdRandom <- execStandardRoutineCall <- execFactor .. execIfStatement`: a brain
  script's `random()` inside an `if` that only one run reached, with identical ABL static state at that frame, so the condition reads a
  builtin whose answer is not simulation state. ABLRAND (8cf860cc6) named it: `mod=Miss ln=422 n=100` = the mission script
  `missions/mc2_16.abl` line 422 `if (random(100) < 1)`, inside line 421 `if (getSensorsActive and ((getCurrentMusicId == -1) or
  (playingCombatTune and not getPlayerInCombat) or (playingAmbientTune)))`. `getCurrentMusicId` returns `soundSystem->currentMusicId`,
  which mclib/soundsys.cpp:676 sets to -1 when both music streams report `gosAudio_Stop`: the audio device's playback position, wall
  clock, not simulation state. ROOT CAUSE: audio playback state reaches the simulation through an ABL builtin and the script then draws
  from the simulation stream. (`getSensorsActive` = `SensorSystemManager::enemyInLOS`, sim; `getPlayerInCombat` = sim.) Every stock
  mission script has this music block, so the leak class is in all 24 missions; it fires when a tune ends during the run.
  FIX NEEDS A RULING (not patched here, two designs change what scripts see): (a) the sound system reports end-of-track on the
  scenario clock (track duration from the stream, deterministic, music semantics unchanged) or (b) `getCurrentMusicId` returns the
  last id a script requested until a script changes it (auto-advance moves to the audio side; the "nothing playing" branches of
  every mission script change behaviour). A per-script RNG stream does NOT fix it (gameplay random() after the branch would shift).
  Symbolizing a stack: `llvm-pdbutil dump --publics build-simtrace/mc2.pdb` for RandomNumber's RVA (section 1 + 0x1000), base = leaf
  return address - RVA rounded to 64 K, then `llvm-symbolizer --obj=build-simtrace/mc2.exe --relative-address` on addr - base (TRACEBASE
  prints an ILT thunk, not the function, so it does not give the base directly: fix = print `&line` through a volatile function pointer
  or print GetModuleHandle(nullptr)).

## Save round trip (task 5)
- `saveload_roundtrip.py baseline` wrote mc2_01 (digest 41c85ecc769a0927) and mc2_16 (e7732b406a66ec67), but `check` FAILS on both: the
  round-trip save is not stable run to run, so NO baseline is committed (tests/saveload/ left untracked and removed). Packet classes that
  differ (save_inspect diff): mc2_01 48 packets (MechWarriorData 38 / 24405 bytes, MechData 6, CommanderData 2, TeamData 1, FIT text
  packet 2 length 34166 vs 34238); mc2_16 123 (TerrainObjectData 77 / 77 bytes, MechWarriorData 36, MechData 8, CommanderData 2). Ledgered
  in `saveload-fidelity-2026-10-11.md` (new section). The 1159 not-restored packets of the earlier resave work stay ledgered there; no Load
  patches (one-schema design).

## B0 rows added (task 6)
own_inventory, own_inventory_selftest, pointer_order_scan, pointer_order_scan_selftest (scripts/check-contracts.sh); ASan tier via
`py -3 tools/build_contract_harnesses.py --asan --run` (not a board row: 56 s). Reload-delta size seam = mclib/alloc_probe (opt-in, no game
wiring yet). SIMID digest extended; baselines re-recorded once (81d82022b).

## Checkpoint (task 8), HEAD 1daa78a43 (+ this state commit)
- `tools/build_all_targets.py` (MC2_MULTIPLAYER OFF, the default): every stage built, 16/16 harnesses. `build_contract_harnesses.py --run
  --unit`: 20 harnesses PASS, doctest 2743 passed (native 2-10 s; ASan 56 s). cxx17 ratchet OK at HEAD (LG19 3 / LG20 136 unchanged,
  totals only down: dedupe + span + static_cast); dup_lines 57 under baseline; own_inventory 455 types; pointer_order 14 sites / 12 files;
  log_tag_census re-frozen; gl_shader_variants regenerated; split_map OK; tools_tests rc=0 after the two manifest re-freezes.
- Full board (10:51, at 1c4f5f1d1): reds were cxx17_ratchet, own_inventory, log_tag_census, gl_shader_variants, tools_tests (all fixed in
  1daa78a43, rows re-run green) and doc_links: `docs/beta/stable_surfaces.md:50 -> docs/load_bearing_pointers.md` broken by lane 4's cull
  (not mine, still red). The test-tier-coverage lines in the board log are that row's own selftest arms (expected failures), not findings.
- tier1 on the 0.4 install, exe 1daa78a43 (fingerprint OK): run 2026-10-12T11-13-42 = 3/5 (mc2_03, mc2_17, mc2_24 PASS; mc2_01 and
  mc2_10 heartbeat_freeze_play at the END of the run, last heartbeat 127.8 / 127.4 s wallclock after the 120 s duration, mc2_10 at the
  8192-frame cap), then mc2_01 alone PASS (8192 frames, 100 fps) and mc2_10 alone PASS (6848, 57 fps). Earlier at 1c4f5f1d1: 4/5 with
  mc2_24 load-timeout (71.9 s load), mc2_24 alone PASS (8094, 68 fps); at d68203bf9 (first proof of b6c154f59): 4/5, mc2_24 PASS with no
  `[ASSET_LOAD v1]` line, mc2_01 timeout then heartbeat under load. Every mission passed on every exe of the day; no single 5-mission run
  was 5/5 because three other lanes ran builds and up to three games at once (flake-log rows). NOT a code regression: all three
  failure buckets are load timeouts or the end-of-run heartbeat with a normal shutdown in the log.
- Install state: 0.4 = 1daa78a43 default (multiplayer OFF, trace OFF). The SIM_TRACE hunt exe is in `build-simtrace/` (deploy with
  `--build-dir A:/Games/mc2-main/build-simtrace --build64-root A:/Games/mc2-main/build-simtrace`, redeploy the default after).

## NEXT for the next owner, in order
1. mc2_16 liveness: owner ruling on the music-end clock (above), implement, 6 pairs, then `write-baseline` for mc2_16
   (`--env MC2_FIXTURE_ORDER_LIVENESS=1`) and `check` on all four. The hunt exe + logs: build-simtrace/, scratchpad chain3/pairs2_mc2_16.
2. Save round trip: name the MechWarriorData / CommanderData / FIT fields that differ run to run (per-offset tallies over the two
   save1.ims kept under the scratchpad chain2/rt_base_* and rt_check_*; `save_inspect.py diff` gives packet classes only), mask or fix
   them, then commit `tests/saveload/baselines/{mc2_01,mc2_16}.json` once `check` passes twice.
3. B0 remainder: wire mclib/alloc_probe into a proof build's gos_Malloc for the mission-reload delta; mc2_15 once under ASan; the B1 batch
   notes for RB1 (file lists, own_inventory rows, free sites, Description()/COW sites) in this file.
4. Lease discipline: three lanes' games ran at once on the host this morning (`procs=mc2=3`); ask the orchestrator whether the other lanes
   pass `--no-lease` or use another lease name. Pairs under that load starve (STARVED is retried, but a FAIL under load is still a FAIL).
5. `.claude/cleanup/lane1-wip-after-limit-2026-10-12.patch` is fully landed: delete it.

## Tool notes from this session
- Heredocs mangle backslashes (`\\n` arrives as a newline): write edit scripts with the Write tool and run them; `sed -i` rewrites CRLF files.
- `deploy_payload.py --build-dir <tree>` needs `--build64-root <tree>` too for a non-standard Ninja tree (the launcher lookup), and the tree
  must have built `mc2_launcher` (`scripts/ninja_build.py` with no target builds both).
- A code commit after the deployed exe makes every later smoke a fingerprint verdict (the SIMID streams are still compared, but tier1 fails);
  commit, then build, then deploy, then run; doc-only commits are fine.
- The smoke fingerprint accepted the merge: the cull changed CMakeLists.txt (a build input), so the chain was split and the merged tree
  rebuilt + redeployed between the pairs and the baselines.
- `own_inventory.py` skips `operator new`/`operator delete` tokens (allocator definitions and calls to the global ones).
- `lane_merge.py <sha> --dry-run` reports "already contained" when the orchestrator merged first; check `git log -3` before merging.

## Rules (unchanged)
- Commit by explicit pathspec after `git diff --cached --name-only`; `git commit -F <file>` for messages; trailer `Co-Authored-By: Claude
  Fable 5.1 <noreply@anthropic.com>`. Commit BEFORE build + deploy. Launches only through `scripts/run_smoke.py --deploy 0.4` /
  `tools/sim_determinism.py` / `tools/saveload_roundtrip.py`, background-only, never `--no-lease`. Never kill cl/ninja by name. Never
  contort code for a ratchet (rewrite the baseline with the reason). Never touch GameOS/, RenderCore/, shaders/, Gates/ beyond a task's
  need, nor tests/unit/test_gate.cpp, nor gos_static_prop*.
