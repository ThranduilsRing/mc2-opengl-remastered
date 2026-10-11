<!-- doc-status: current | reviewed:2026-10-11 -->
# Lane 1 state (builder on A:/Games/mc2-main, branch claude/nifty-mendeleev). Keep current at every report.

HEAD: see `git log -1` (2026-10-11 evening). Board 166/166, tier1 5/5 (last run after the B6 loop), ratchet LG01 693, LG02 691, LG12a 593 (from 1295), LG12b 625 (from 1497).

## Done (newest first)
- Step 3 LG12a: B4 untagged (GameOS/mclib/code), B5 tags nobody parses, B6 parsed tags one at a time (ASSET_SCALE, TEXLOAD, PIPELINE_BIND, MOVER, RENDER_SNAPSHOT, VK_PRESENT, INSTR, GPUPROPS, SOAK, SMOKE, TIMING, PERF) - all through `tools/tidy/tidy_batch.py --rewrite printf-log[-tagged|-parsed] --corpus --log-identity`. Sink is header-only `mc2::fmt::print/print_err/print_out` (one fwrite, same FILE*). What stays and why: `.claude/cleanup/step3-lg12b-ledger.md` (LG12a section at the end).
- Step 3 LG12b (B0-B3c) and the ledger; frustum-plane repair (47a9623ed); PCH (395a37af0); header phase; typedef-aggregates / null-tokens / printf rewriters; lock/kill/lease tools.
- Driver: tidy_batch.py has --proof, --corpus, --log-identity; it refuses a dirty worktree and fails on a refused ratchet tighten.

## In flight
Nothing uncommitted except this file's edit being committed with the report.

## Next steps, in order
1. Wait for lane 2's step 2 batches; then rerun the rewriters on the lane-2 files listed in the ledger: `printf-buffers` (53+10 LG12b sites), `printf-log` (projectz_trace, terrtxm2_internal.h), and the two parsed tags waiting in mclib/terrain_geometry.cpp: `MC2_PRINTF_LOG_TAG=FASTPATH_DROP` and `QUADSETUP_SKIP` with `--rewrite printf-log-parsed`.
2. Hand work in the ledger: `printWeaponsStatus(char*, size_t)` (warrior.h, lane 2), ABL debugger buffers, 88 value-used printf calls, 16 headers needing an include, `%.Nd` in gos_render.cpp.
3. Facade phases still open: SPEW shim and OutputDebugString sink (P3), MC2_EVENT/category adoption (optional now that print is byte-identical), the `logging.h` + logparse.py:36 pair (one commit, both sides).
4. Header tidy remainder (LG01 693, LG02 691): heavy headers, leave. Tier1 every sixth batch.

## Rules
- Commit by explicit pathspec; message ends `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`. Never push, never touch lane 2 (A:/Games/mc2-lane2, claude/lane2-s3) or the golden mc2-win64-v0.4c.
- Launches background-only: `scripts/run_smoke.py ... --deploy 0.4`; build with `scripts/ninja_build.py`, deploy with `scripts/deploy_payload.py --target game --ninja` (NOT without --ninja: it deploys the stale build64 exe). The exe must be built from a COMMITTED tree (fingerprint).
- One tidy tool per tree (`build-alltargets/.tidy.lock`); never kill cl/ninja by image name (use tools/kill_build_tree.py); run loops in one tool call, stop with --stop-after-batch.
- Lane-2 files (git diff merge-base..claude/lane2-s3) are excluded automatically (--exclude-lane).
- Printf batches run through `tidy_batch.py ... --corpus --log-identity` (it does the full build, doctest, census, before/after logs); a failed batch leaves the worktree dirty - `git checkout -- .` and fix the tool, never commit a half batch. Board after every 2 batches, tier1 every 6; log heartbeat_freeze_play flakes in tier1-flake-log.md, do not hunt them.
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
- Converters rebuild calls with `Call.rebuild` so the original line layout survives (a collapsed call once put two std::max on a line and tripped LG10). A new rewriter needs: module in tools/tidy, entry in run_tidy_safe.REWRITERS (+ COMPILE_ONLY if the output changes), a test file, and board checks that parse source text taught the new spelling (gate_echo_reads, beauty sidecars, shader variants JSON: regenerate with scripts/extract_gl_shader_variants.py).
- The generated corpus (tests/unit/printf_corpus_gen.inc) is compiled with /bigobj; titles carry no backslash or quote.
