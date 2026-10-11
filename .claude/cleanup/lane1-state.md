<!-- doc-status: current | reviewed:2026-10-11 -->
# Lane 1 state (builder on A:/Games/mc2-main, branch claude/nifty-mendeleev). Hand-over file for the next agent; keep current at every report.

HEAD: `git log -1`. Board 166/166, tier1 5/5 after the B6 loop. Ratchet: LG01 693, LG02 691, LG12a 593 (from 1295), LG12b 625 (from 1497). Nothing uncommitted. Never pushed.

## Done (newest first)
- Step 3 LG12a: B4 untagged, B5 tags no parser mentions, B6 parsed tags one run each (ASSET_SCALE, TEXLOAD, PIPELINE_BIND, MOVER, RENDER_SNAPSHOT, VK_PRESENT, INSTR, GPUPROPS, SOAK, SMOKE, TIMING, PERF).
- Step 3 LG12b B0-B3c; frustum-plane repair 47a9623ed; PCH 395a37af0; header phase; typedef-aggregates / null-tokens / printf rewriters; lock, kill_build_tree, host lease tools.

## What remains, exactly (ledger: `.claude/cleanup/step3-lg12b-ledger.md`; plan: `cxx23-step3-logging-plan.md`)
LG12a 593: (1) 88 calls whose value is used; (2) SPEW ~180, OutputDebugString ~19, puts/fputs: other entry points (facade P3); (3) 16 headers with no #include for mc2fmt.h; (4) 11 `%.Nd` in gos_render.cpp, 4 `%t`, 3 `%*d`, 1 `%S`, 4 runtime formats (gameosmain_frame.cpp); (5) crash/assert/allocator files excluded on purpose (gos_crashbundle, gameos_debugging, memorymanager/heap, err); (6) lane-2 files: terrain_geometry.cpp (parsed tags FASTPATH_DROP, QUADSETUP_SKIP), projectz_trace.cpp, terrtxm2_internal.h.
LG12b 625: (1) 53+10 in lane-2 files (mechlopedia, logisticsdialog, pilotreviewarea, mpparameterscreen, mpstats, mainmenu, tacordr, ...); (2) ~37 runtime-supplied formats (stay on snprintf/vsnprintf); (3) 27 ABL-debugger `char* dest` buffers (abldbug.cpp, abldbug_print.cpp); (4) 5 in warrior_target.cpp (needs `printWeaponsStatus(char*, size_t)` in warrior.h, a lane-2 file; only caller mover_diag.cpp has `char ts[50]`); (5) 8 headers without an include; (6) 4 `_snprintf_s` + 1 `_snprintf` with the result used (MC2Strings.cpp, gos_crashbundle.cpp); (7) 2 `%.Nd`, 1 `%*d`, 1 `%p` with modifiers.

## Next steps, in order
1. NEXT COMMIT: the `GameOS/gameos/utils/logging.h` + `scripts/smoke_lib/logparse.py:36` pair, one commit with both sides. logmsg prefixes `file(line): ERROR` and logparse.py:36 matches `shader_builder\.cpp\(\d+\)\s*:\s*ERROR` as a shader-failure bucket; migrate the 56 `log_*` calls in 6 files to the facade and change producer and pattern together, proof = logparse tests + log_identity + board.
2. After lane 2 merges its step 2 batches: rerun the rewriters on the lane-2 files (`printf-buffers`; `printf-log`; `MC2_PRINTF_LOG_TAG=FASTPATH_DROP` then `QUADSETUP_SKIP` with `--rewrite printf-log-parsed`, dir mclib).
3. Hand work from the ledger: the lists above (value-used calls, ABL debugger capacities, headers, `%.Nd`).
4. Facade P3: SPEW shim, OutputDebugString sink. Header tidy remainder (LG01/LG02: heavy headers) stays. Tier1 every sixth batch.

## The printf pipeline (tools/tidy/tidy_batch.py)
`py -3 tools/tidy/tidy_batch.py --dir <d> [--dir ...] --count 50 --batches N --first-number K --rewrite printf-buffers|printf-log|printf-log-tagged|printf-log-parsed --corpus --log-identity --done <ledger file>`
- `--proof auto|objhash|compile` (auto = compile for the printf rewriters), `--corpus` records the converted format pairs and regenerates the doctest, `--log-identity` builds+deploys HEAD and compares mc2_03/mc2_24 logs before and after by line shape. MC2_PRINTF_LOG_TAG=NAME limits printf-log-parsed to one tag. It refuses a dirty tracked worktree and fails a batch whose ratchet tighten is refused.
- A failed batch leaves its edits in the worktree: `git checkout -- .`, fix the cause, and drop that batch's files from the `--done` ledger (the driver has no --resume: `head -n -<picked> ledger > tmp && mv tmp ledger`).
- The failure detail shows only the last lines of the build output: rerun `py -3 tools/build_all_targets.py --keep-going -j6` (or `cmake --build build64-tests ...`) to see the real compiler error.

## Rules
- Commit by explicit pathspec; message ends `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`. Never push; never touch lane 2 (A:/Games/mc2-lane2, claude/lane2-s3) or the golden mc2-win64-v0.4c.
- Launches background-only: `scripts/run_smoke.py ... --deploy 0.4`; build `scripts/ninja_build.py`, deploy `scripts/deploy_payload.py --target game --ninja` (without --ninja it deploys the stale build64 exe). The exe must be built from a COMMITTED tree (fingerprint).
- One tidy tool per tree (build-alltargets/.tidy.lock); never kill cl/ninja by image name (tools/kill_build_tree.py); lane-2 files are excluded automatically (--exclude-lane). Comments say WHAT and WHY, never HOW. No emojis.

## Tool quirks
- Heredocs and `python -` mangle backslashes (`\\` becomes `\`, `\n` in a string becomes a real newline): any script with a backslash goes through the Write tool to a file, then `python file`. Same for `sed` expressions with backslashes.
- A bare `bash` outside the Bash tool can resolve to WSL bash; use the Bash tool (Git Bash) or C:/Program Files/Git/bin/bash.exe. Windows paths work in python; `/tmp` is not the same place for bash and python.
- Source-text board checkers read the COMMITTED tree and have no --worktree mode (`cxx17_ratchet` does: pass `--worktree`): check-beauty-sidecars, extract_gl_shader_variants / check-gl-shader-variants, check-gate-echo-reads, log_tag_census parse C++ text, so a printf conversion can break them after the commit; expect that for any new spelling and run the board before trusting a batch. Regenerate the variants JSON with `py -3 scripts/extract_gl_shader_variants.py` (only source line numbers move).
- `.gitignore` has `*.cmake`: a new cmake file (cmake/pch/pch_skip.cmake) needs `git add -f`, and a plain pathspec `git add` of a list that contains one aborts the whole add.
- `run_in_background` plus `&` inside the command detaches a process that outlives the call; use run_in_background alone and block on the output or lock file. Bash waits cap at 10 min: loop `for i in $(seq 1 55); do ...; sleep 10; done`.
- Ratchet TOTAL row columns misalign under awk: read `counts` per dir from tools/cxx17_ratchet/baseline.json. objhash cannot prove printf conversions (proof = compile + corpus doctest + census + log_identity). tools/build_contract_harnesses.py hides compile errors.
- Converters rebuild calls with `Call.rebuild` (original line layout survives; a collapsed call once put two std::max on a line and tripped LG10). New rewriter checklist: module in tools/tidy, entry in run_tidy_safe.REWRITERS (+ COMPILE_ONLY), test file, board checkers taught the new spelling. The corpus .inc is compiled with /bigobj; its titles carry no backslash or quote.
- Locations: step3-lg12b-ledger.md, cxx23-step3-logging-plan.md, tier1-flake-log.md, legacy-subsystem-audit.md (section D: 50 TUs where math overload visibility changes arithmetic), this file.
