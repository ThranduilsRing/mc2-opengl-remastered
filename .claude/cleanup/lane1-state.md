<!-- doc-status: current | reviewed:2026-10-12 -->
# Lane 1 state (builder on A:/Games/mc2-main, branch claude/nifty-mendeleev). Hand-over file for the next agent; keep current at every report.

Session 2026-10-12 evening (lane-1 Sonnet, 15:59 -> 17:50 CDT, cap 6 h, stopped early: every assigned task is done). Never pushed. Install `mc2-win64-v0.4`.
Previous session (host lease fix, mc2_16 music ruling, board doc_links): `git show e81e3d8b0:.claude/cleanup/lane1-state.md`.

## Commits this session (oldest first)
- c8f6fa3df board: contract_coverage selftest. The un-wire arms for `tools/check_ubo_layout.py` / `tools/glsl_port_diff.py` could not fire because
  `tests/callers.json` (the Codex `tools/callers.py` inventory of who mentions each script) was credited as an external CALLER of every checker.
  Both rows exist (`check-contracts.sh:960-961`); the scan now skips that one generated file. Selftest and live scan green.
- 180f6f4f9 abl: bounded script-string writes. `ABLi_popCharBuf` (span sized by the declared element count); `execStdGetModule` goes through
  `abl::copyBounded` and logs `[ABL v1] event=getmodule_truncated module= line= capacity= dropped=` (shared `reportTruncated` with concat, 16 lines per
  routine); `execGetRelativePositionToObject` writes the zero vector for a non-mover / unknown id (`mclib/abl_out_params.h`); `abl::appendBounded` is
  `std::span<char>` + `std::string_view` (`abl::textOf` is the one `const char*` boundary). cxx17 ratchet re-tightened: LG09 2190 -> 2188 (ablxstd_std.cpp
  27 -> 25), LG06 unchanged at 670 (the `textOf(const char*)` site is kept on purpose). Log tag census unchanged (event name is a format argument).
  Doctest 2750/2750 + 20 harnesses.
- f4ab40f30 docs: `abl-string-audit.md` and `cxx23-step4-b1-notes.md`.
- 7dbcb45f0 board: lane 3's `.claude/cleanup/lane3-diag/strhunt_patch.py` revert() spawned git without `console_child.child_kwargs()` -> the only two board reds
  after the merge (background_launch and its pytest twin in scripts_tests).

## Task results
1. Determinism under the new music semantics (abf0764c1): `sim_determinism.py check` against the committed baselines, NO digest differs, no re-record.
   At 642019a64 (clean HEAD build, deployed): mc2_10 PASS (2927 frames identical, baseline chain fe69b943f4bc71ee), mc2_24 PASS (3769, e34f04954e5a949c),
   mc2_16 with `MC2_FIXTURE_ORDER_LIVENESS=1` PASS (2634, 381fe40d96777ed3), mc2_03 PASS at `--duration 30` (1871 frames identical, baseline 3bfc7e1947c403dc;
   the first attempt at the default 20 s reached only 377 of the 1000 baseline frames because the host was loaded: use `--duration 30`, the runner caps
   at 30). Repeated at the final ABL exe (180f6f4f9 + docs, 7dbcb45f0 build) with the same four baseline chains, all PASS (mc2_03 2232 frames, mc2_10 2788,
   mc2_24 3788, mc2_16 2634): the ABL changes (relpos zero vector, bounded getmodule) do not change a stock mission's digests.
2. contract_coverage_selftest green (c8f6fa3df).
3. ABL string safety: (a) getModule bounded + the grep result that it was the only builtin writing text into a script array; (b) relpos defined output;
   (c) span/string_view appendBounded + ratchet re-tightened; (d) audit in `abl-string-audit.md`: tier2 mc2_01 / mc2_15 / mc2_17 PASS (reports
   2026-10-12T16-46-44, 16-44-31, 16-48-57) plus mc2_07 (two heartbeat_freeze_play verdicts, host at 100 % CPU, 16-39-28 and 16-52-09): ZERO `*_truncated`
   lines in any of them at HEAD (expected: the overflow needed the garbage positions fixed by the zeroing commits). Static scan of the 994 stock scripts:
   121 concat chains, 8 tight debug strings (corebrain.abx:538 and :302 char[50]; hal.abl:181 and the five mc2_07_* brains :183 char[25]) = content bugs for
   a later data pass. OPEN CLASS: int/real out-arrays (`getContacts`, `getWeaponsReady/Locked/InRange`, `getAttackers`, `getAlarmTriggers`) are written
   without a bound (section 1 of the audit): queue as its own slice (`ABLi_popIntBuf` / `ABLi_popRealBuf` + clamp + `<name>_truncated`).
4. B1 notes: `.claude/cleanup/cxx23-step4-b1-notes.md`, ready for the Opus before-review. Headline: 67 files outside the gui widgets (code 48, editor 18,
   mclib/tgainfo.cpp 1; the plan said 46/16/3), 8 batches (B1.0 bridge + pins 6 files + tests; B1.1 texture-cache global 3; B1.2 logistics data model 19;
   B1.3 objectives 2 (+6 includers); B1.4 front-end screens 14; B1.5 multiplayer screens 11; B1.6 editor objectives 10; B1.7 editor data 8; B1.8 tests +
   class stays for the widgets), none above 19. The notes' main finding: EString is NOT a plain string. Confirmed by a throwaway doctest on the real class:
   empty `Data()` is NULL, `==` / `<` / `Compare` are effectively case-insensitive (memcmp mismatch falls through to `lstrcmpi`), `MakeUpper` is ANSI
   code-page aware; plus COW by-value returns (`Title()`, `Description()`), inclusive-end index API, unchecked printf `Format` (103 sites). `/analyze`
   (C26815/C26816) is NOT wired in the repo yet and must be proven by a seeded negative test in B1.0 before it is trusted. 6 questions for the reviewer
   at the end of the notes.
5. Checkpoint (exe `7dbcb45f0c49`, fingerprint OK, deployed to 0.4): ninja build rc 0; `build_contract_harnesses.py --run --unit` 20 harnesses PASS, doctest
   2750 passed / 1 skipped; board `check-contracts.sh` at f4ab40f30: 167 passed / 2 failed (both the strhunt spawn, fixed by 7dbcb45f0; re-run of
   `check-background-launch.py` and `test_check_background_launch.py` green; full board re-run at 7dbcb45f0: 168 passed / 1 failed = `script_callers` (stale `tests/callers.json` after the strhunt edit), fixed by regenerating it in 2a1007f56; `callers.py --check` and the coverage selftest green after; no full board run after that last data commit); tier1
   `2026-10-12T17-05-34` = 4/5 (mc2_01 149 fps, mc2_03 68, mc2_17 134, mc2_24 81 PASS; mc2_10 heartbeat_freeze_play, p1 2 fps with the host saturated) and
   mc2_10 alone `2026-10-12T17-30-29` PASS (108 fps) = 5/5 with the single host-freeze rerun (same pattern as lane 3's mc2_03 rerun). Host note: from about
   16:30 the host sat at 100 % CPU with no build of mine running (other lanes), so mc2_07 / mc2_10 freezes are load, not code: mc2_07 passed tier2 at 138 fps
   in April; its two freezes today are unexplained beyond load and worth one rerun on an idle host.

## NEXT for the next owner, in order
1. Opus before-review of `cxx23-step4-b1-notes.md` (6 questions at its end); B1 code only after it. B1.0 first: bridge + characterization tests of H1/H2/H3/H6/H7/H8/H12
   + `/analyze` recipe with the seeded negative + C4840 into `/we`.
2. ABL int/real out-array slice (audit section 1) and the data pass for the 8 tight debug strings (needs the owner to say who edits `.abl` content).
3. mc2_07 tier2 on an idle host (two heartbeat_freeze_play today at 3-5k frames, p1 2-3 fps).
4. Save round trip: MechWarriorData / CommanderData / FIT fields differ run to run; no `tests/saveload/baselines/` yet (ledgered in saveload-fidelity-2026-10-11.md);
   B1.2 / B1.4 rely on `saveload_roundtrip.py resave` byte identity until it exists.
5. B0 remainder: alloc_probe into a proof build's gos_Malloc; mc2_15 under ASan (the writer fix has merged, unblocked).

## Tool notes from this session
- `grep -c $'\r' file` in this Bash tool matches EVERY line (the escape is lost): use Python (`b.count(b"\r\n")`) or `git ls-files --eol`. The index stores LF for
  most mclib/code files (`i/lf w/lf`); only some (abl.h, soundsys.cpp) are CRLF. `tools/dev/apply_edit.py` keeps each file's own ending; a file written fresh by
  the Write tool is CRLF on this host and git warns "CRLF will be replaced" (harmless, normalised at commit; convert LF first when overwriting an LF file).
- `sim_determinism.py` / `run_smoke.py` cap `--duration` at 30 s; mc2_03 needs the full 30 s on a loaded host to pass the 1000-frame baseline.
- `run_smoke.py --keep-logs` keeps `tests/smoke/artifacts/<id>/<mission>.log` on a PASS (without it a passing run leaves only manifest.json).
- A foreground `until grep -q "^rc=" log; do sleep N; done` works for waiting (a bare `sleep N` is blocked); the tool moves it to the background after 600 s
  and still notifies.
- Fingerprint: committing ANY file (even a doc) after the build makes the deployed exe stale against HEAD (`DEPLOY_FINGERPRINT`); rebuild (a relink) + deploy
  before the smoke.
- tier1 on a loaded host: a heartbeat_freeze_play with p1 2-3 fps is the host; rerun that mission alone and record both report ids.

## Rules (unchanged)
- Commit by explicit pathspec after `git diff --cached --name-only`; `git commit -F <file>`; trailer `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`.
  Commit BEFORE build + deploy. Launches only through `scripts/run_smoke.py --deploy 0.4` / `tools/sim_determinism.py` / `tools/saveload_roundtrip.py`,
  background-only, never `--no-lease`. Never kill cl/ninja by name (`tools/kill_build_tree.py`). Never contort code for a ratchet (rewrite the baseline with
  the reason). Never touch GameOS/, RenderCore/, shaders/, Gates/ beyond a task's need, nor tests/unit/test_gate.cpp, nor gos_static_prop*, nor code/multplyr*.
