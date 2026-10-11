<!-- doc-status: current | reviewed:2026-10-11 -->
# Lane 1 state (builder on A:/Games/mc2-main, branch claude/nifty-mendeleev). Hand-over file for the next agent; keep current at every report.

HEAD: `git log -1`. Ratchet at efa0a7732: LG12a 409 (from 593 at the start of this session; +2 = the BUILD_FINGERPRINT printf banner, a binary contract), LG12b 624, LG03 2397, LG13 1758. Nothing uncommitted except this file and the step 4 plan. Never pushed. Session cap: stop by about 13:00 on 2026-10-11.

## Done this session (newest first)
- BUILD_FINGERPRINT banner restored to printf (capture_provenance went red: exe_fingerprint.py reads the literal from the exe image; the rewriter now refuses it). `lane_merge` verify builds in `build-merge/` + `build-merge-tests/` (MC2_NINJA_TREE, MC2_UNIT_BUILD_DIR).
- Step 3.5 (a)-(e): `tools/sim_determinism.py` + row `sim_determinism` (SIMID chain in report.json); `MC2_SMOKE_FREEZE_ANIM` (capture_ab sets it); `tools/saveload_roundtrip.py` + row; `<cmath>` first in the 50 overload TUs; `lane_merge` builds and doctests (exit 7); `cursor_guard.py` deleted for `smoke_lib/win_input.py`.
- Step 3 review fixes F1-F8, apply_edit (`tools/dev/apply_edit.py`), logging.h retired (59 calls, `SHADER_LOG_ERROR`, logparse pair), `print` returns printf's value, `debug_out`, 40 SPEW sites to `MC2_LOG(cat, Debug)`, tidy batches 15-18 (value-used, tagged, parsed tags), unsigned `%.Nx` rule.

## FINDINGS the next owner of each area must know (both have a spawned task chip)
1. mc2_10 is NOT run-to-run deterministic: `py -3 tools/sim_determinism.py run mc2_10 --duration 30`. First divergence at sim frame about 1085 (t 36 s), mover 710 gets `target=84159` in one run and stays `-1` in the other; not animation (FREEZE_ANIM on), not jobs (MC2_FRAME_JOBS=0). mc2_03, mc2_16 (with `MC2_FIXTURE_ORDER_LIVENESS=1`), mc2_24 are identical.
2. Save/load is lossy: `py -3 tools/saveload_roundtrip.py run`. After a quickload moving units lose gesture (walk becomes stand) and path step in frame 0, and the digests drift apart after about 60 frames (11 movers differ in position by frame 300). Quicksave fixture: `MC2_BOOT_QUICKSAVE_AT/LOAD_AT` are RENDER frames; mc2_03 never fires them (intro), use mc2_16 or mc2_01.
3. `<cmath>` in the 50 TUs: graded and visual digests unchanged on 1755 frames of mc2_03, strict digest differs from frame 1396 (float bits only). `sim_determinism --baseline` compares the visible chain; strict is a note.

4. Tier2 (24 missions, 120 s each, exe cc4aaf6a9): 17 PASS. mc2_11/12/13/16/22/24 fail asset_load_fail (the 0.4 install lacks data/tgl/jaegermech.ase and similar: data, not code). mc2_15 fails crash_silent EVERY time (log just ends after frame 1200+, no crash bundle; bisect needed). Also: quickloading an old save crashed in GameObjectManager::Load (objmgr_io.cpp:1143; crash bundle copied to the scratchpad `l1_crash_stale_save`).
Chips spawned for 1, 2 and 4 (determinism, save/load fidelity, mc2_15 + old-save crash).

## Next steps, in order
1. Tier3 (9 mod missions) is BLOCKED: the 0.4 install has no mods (TangoMaster, DarkRain, MCO-ClanEagle dirs missing; 7 env_mission_not_found, 2 mission_load_fail). Install the mod set (tools/mod_install, `project-mod-compat-targets`) into a smoke install, then `py -3 scripts/run_smoke.py --tier tier3 --deploy <name> --continue` before step 4 B3.
2. Step 4 plan awaits the Opus design review (`cxx23-step4-ownership-plan.md`, section 10 = meta-review E state). B0 needs finding 1 and 2 closed.
3. Lane-2 files after its next merge: `MC2_PRINTF_LOG_TAG=FASTPATH_DROP` and `QUADSETUP_SKIP` through `--rewrite printf-log-parsed` on mclib; mclib SPEW (98) and `SPEWALWAYS` (3) wait for mc2log to be a library the standalone tools link (plan section 9.4).
4. Remaining LG12a: puts/fputs, mclib SPEW, `%t`/`%*d`/runtime formats; LG12b ledger items in `step3-lg12b-ledger.md`.

## The printf pipeline (tools/tidy/tidy_batch.py)
`py -3 tools/tidy/tidy_batch.py --dir <d> [--dir ...] --count 50 --batches N --first-number K --rewrite printf-buffers|printf-log|printf-log-tagged|printf-log-parsed --corpus --log-identity --done <ledger file>`
- `MC2_PRINTF_LOG_TAG=A,B` limits printf-log-parsed to those tags. `--log-identity` builds+deploys HEAD and compares mc2_03/mc2_24 logs by line SHAPE (numbers masked; the corpus proves numbers). A failed batch leaves edits: `git checkout -- .`, drop its files from the ledger.
- Census/consumers: `tools/log_tag_census.py` (`consumed_tags()` reads the syntax tree; `plain_tags` for VEG_SUMMARY etc.). The MC2_LOG family is in the census as stream `log`.

## Rules
- Commit by explicit pathspec; message ends `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`. The Bash hook blocks any command line containing `git commit` followed LATER by a token like `--duration` or `-nan` (it reads it as `-a`): keep `git commit` in its own call and keep option-like words out of messages (use `-F file`).
- Commit BEFORE build+deploy (fingerprint). Launches only `scripts/run_smoke.py ... --deploy 0.4` or the tools above; run_smoke caps `--duration` at 30 (tier2 uses 120). Never kill cl/ninja by name (`tools/kill_build_tree.py`). Never touch lane 2 or `code/multplyr*`.
- Never contort code for a ratchet: rewrite the baseline with the reason.
- The source-text board checkers read the COMMITTED tree: after a code commit run the full board; `scripts/extract_gl_shader_variants.py` regenerates `gl_shader_variants.json` when line numbers move.

## Tool quirks
- Heredocs and `python -` mangle backslashes (`\\n` arrives as `\n`, a real newline): write scripts and JSON with the Write tool, or use `tools/dev/apply_edit.py` with a JSON file written by Write. `/tmp` differs between bash and python; use the scratchpad.
- `git add` of a path ignored by `.gitignore` (`*.cmake`) needs `-f`. A pathspec commit cannot name a staged deletion; commit with an exact index instead.
- Adding a gate: edit `Gates/gate_table.cpp` (doc <= 160 chars), `py -3 tools/gate_gen.py --sync-verified`.
