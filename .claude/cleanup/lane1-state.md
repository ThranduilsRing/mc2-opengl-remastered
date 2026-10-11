<!-- doc-status: current | reviewed:2026-10-11 -->
# Lane 1 state (builder on A:/Games/mc2-main, branch claude/nifty-mendeleev). Keep current at every report.

HEAD 491051f52 (2026-10-11). Board 166/166, tier1 5/5 at 491051f52, doctest 1351/1351 (635 printf-corpus cases), ratchet LG01 693, LG02 691, LG12a 1295, LG12b 625.

## Done (newest first)
- Step 3 LG12b: B0 mc2fmt.h + census + corpus (57f51d61d, de45cb4b5); B1 GuiRuntime/GameAdapters/GameOS (d67d82579); B2 mclib (fd9f2e5fd); B3 code/ (37ad605d9); B3a compiler-judged array sites (0f89799d1); B3b hand fixes, one commit per site (94e4cd2da..5002c6a47); B3c PATH_SEPARATOR (dbf53a8e1). Ledger of what stays: `.claude/cleanup/step3-lg12b-ledger.md`. Plan: `cxx23-step3-logging-plan.md`.
- Frustum-plane repair: Camera::init now initialises its matrices (47a9623ed); log_identity noise list is strict again (8b9f81232).
- PCH (395a37af0, skip list cmake/pch/pch_skip.cmake by tools/pch_audit.py), header phase, typedef-aggregates + null-tokens rewriters, lock/kill tools, lease, --rows presets.
- sqrt overload-visibility finding is in legacy-subsystem-audit.md section D (50 TUs).

## In flight
Nothing uncommitted. Worktree clean except untracked scratch.

## Next steps, in order
1. `tidy_batch.py --proof compile --corpus` (one commit) so B4-B6 run through the driver.
2. B4: untagged LG12a (fprintf(stderr|stdout)/printf/puts with no [TAG vN]) in GameOS, mclib, code -> `mc2::log::print` (raw sink over `mc2::log::writeRaw`, no level gate, no added newline; mclib reaches it header-only - plan decision 4 open). Same proofs: compile, corpus, census unchanged, log_identity on mc2_03/mc2_24, board.
3. B5: tagged lines with no Python consumer -> MC2_EVENT. B6: parsed tags one at a time behind a golden harness; a tag failing its golden stays on printf (allow-list).
4. After lane 2 merges its step 2 batches: rerun printf-buffers on the lane-2 files in the ledger; `logging.h`/logparse.py:36 pair changes after B6.
5. Header tidy remainder (LG01 693, LG02 691) is heavy headers: leave. Tier1 every sixth batch (last at 491051f52).

## Rules
- Commit by explicit pathspec; message ends `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`. Never push, never touch lane 2 (A:/Games/mc2-lane2, claude/lane2-s3) or the golden mc2-win64-v0.4c.
- Launches background-only: `scripts/run_smoke.py ... --deploy 0.4`; build with `scripts/ninja_build.py`, deploy with `scripts/deploy_payload.py --target game --ninja` (NOT without --ninja: it deploys the stale build64 exe). The exe must be built from a COMMITTED tree (fingerprint).
- One tidy tool per tree (`build-alltargets/.tidy.lock`); never kill cl/ninja by image name (use tools/kill_build_tree.py); run loops in one tool call, stop with --stop-after-batch.
- Lane-2 files (git diff merge-base..claude/lane2-s3) are excluded automatically (--exclude-lane).
- Every tidy/printf batch: full `tools/build_all_targets.py` (standalone harnesses fail on include dirs), doctest tier, board, then build+deploy+smoke and `tools/log_identity.py before after`; save the 'before' artifact dir first. Board after every 2 batches, tier1 every 6; log mc2 heartbeat_freeze_play flakes in tier1-flake-log.md, do not hunt them.
- Comments say WHAT and WHY, never HOW. No emojis.

## Tool quirks learned
- Heredocs and `python -` here mangle backslashes (`\\` becomes `\`, `\n` inside strings becomes a real newline). Any script containing a backslash goes through the Write tool to a file, then `python file`. Shell `sed` with backslashes fails the same way.
- Source-text checkers read the committed tree by default: `cxx17_ratchet --check` needs `--worktree` to see uncommitted edits; check-beauty-sidecars and extract_gl_shader_variants parse C++ text, so a printf-to-format conversion can break them (both fixed; expect the same for any checker that greps `sprintf`/`"%s"` literals). Board rows are the guard - run it before the batch commit, not after.
- `run_in_background` + `... &` inside a command detaches a process that outlives the call; use run_in_background alone and block on the lock file or the output file.
- Monitor/Bash waits cap at 10 min: loop `for i in $(seq 1 55); do ...; sleep 10; done`.
- `grep`/`awk` on the ratchet TOTAL row misaligns columns; read counts from tools/cxx17_ratchet/baseline.json (`counts` per dir).
- objhash cannot prove printf conversions (output changes by design): proof = compile + corpus doctest + census + log_identity.
- tests/unit uses build64-tests; `tools/build_contract_harnesses.py` hides compile errors, call cmake --build directly to see them.
- Ledger/state locations: `.claude/cleanup/step3-lg12b-ledger.md`, `cxx23-step3-logging-plan.md`, `tier1-flake-log.md`, `legacy-subsystem-audit.md`, `lane1-state.md` (this file).
