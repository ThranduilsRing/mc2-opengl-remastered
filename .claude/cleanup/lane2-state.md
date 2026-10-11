# Lane 2 state (worktree A:/Games/mc2-lane2, branch claude/lane2-s3, build64-ninja, install mc2-win64-lane2)

Rotated lane-2 agent #2 (started 2026-10-10 17:58). This lane does the 1000-1500 tier verbatim splits (code/ and mclib/ first, largest first).
Read ORCHESTRATOR-HANDOFF.md Ground rules first. Whole-tier census: appendix of size-tier-1000-survey.md (`py -3 tools/split/tier_census.py`).

## Done (all on claude/lane2-s3; sizes before -> core after)
- Tier1 5/5 on 055dacab9 (task 1) and on aa9130de3 (after 4 splits). Third tier1 on be79c6ead (after splits 5-8): 5/5 (mc2_01 147, mc2_03 84, mc2_10 105, mc2_17 132, mc2_24 122 fps).
- 1 abldbug 1319->753 (dd946d83d)  2 genactor 1297->730 (0196b20c5)  3 logisticsmissioninfo 1316->511 (dc3a26c8a)  4 logisticsvariant 1221->551 (aa9130de3)
- 5 cevfx 1217->669 (2d6c73efc)  6 mechbayscreen 1176->325 (b43d4b40c)  7 group 1102->253 (f0d37de6c)  8 team 1077->543 (be79c6ead)
- Tooling: capture_ab.py (424baaeca), split_cpp.py --stage-dir/--install (09d1ec377), tools/split/tier_census.py + survey appendix (1da41b04d).
- Every pair: build_all_targets ok, harnesses 20/20 + doctest 704, board 164/165 (doc_links only), smoke-lite 2/2, capture_ab vs previous set at noise floor.

## Next
- userinput.cpp (1036) is NOT a cheap split: 390 lines are twelve cursor UV tables (smallTextureTLUVX ... largeTextureBRUVY) that nothing references
  (dead data, defined mid-file); delete after a provenance audit instead of splitting, which brings the file to ~650 lines. Left alone.
- Remaining census candidates: cone_glow 1021 (30 funcs), ablxstd 1071 (52 funcs, ABL builtins), mlr_i_tmesh 1206, multplyr 1203 (148 stub methods), logistics.cpp 1002 (MID).
  Survey LEFT (skip): objmgr_update, contact, ablscan, soundsys, estring, quad_draw, mlr_i_pmesh. PHASE files (one dominant function) need a phase pass, not a split.
- Nice-to-have tooling (not built): (1) cfg generator from first/last function names (like .claude/in_cfg_gen.py), (2) split_cpp.py that also writes the CMake source-list
  edit (every list from `git grep -n '<orig>.cpp' -- '*CMakeLists.txt'`) and the allow-json skeleton with base = HEAD and the scripts[] split_verify names.
- split_verify gap: a file-static function used by another piece is not caught (logisticsvariant bumpSpeedClass broke the build, fixed by moving it with its caller).

## Recipe per split
1. `git grep -n '<orig>.cpp' -- '*CMakeLists.txt' '*.cmake'`; write a cfg (.claude/l2cfg/*_cfg.py as models; allow json is read from $SPLIT_ALLOW_DIR) and stage:
   `py -3 tools/split/split_cpp.py CFG --stage-dir /tmp/l2/stage_x` (verify runs inside), then the same command plus `--install`. Tree stays clean until install.
2. Add the pieces to every CMake list; `scripts/ninja_build.py`; `py -3 tools/build_contract_harnesses.py --run --unit`; `.claude/l2cfg/post.sh "<plan row>"`
   (row + split_map + size ratchet + full board; only doc_links may stay red); fix row counts; commit by pathspec; split_commit sha goes in the allow json at the next report.
3. Per pair: `py -3 tools/build_all_targets.py`, deploy lane2, `run_smoke.py --lite --exe`, `capture_ab.py capture --tag X` then `compare --before PREV --after X`
   (mc2_01 GL has a bimodal ~1700 / ~6000-9000 px noise mode in the bottom-right corner; compare against the NOISE line). Tier1 after every four splits.
4. Run the board BEFORE committing a new tool: background_launch fails on any subprocess call without console_child.child_kwargs and needs an allowlist entry for wrappers.
- Files are CRLF in the worktree; git warns and stores new files LF (autocrlf=input). `py` sees bash /tmp differently from Git Bash only for open() of literal paths; use relative paths.
- Bash tool: foreground `sleep` is blocked; use `until <cond>; do sleep N; done` with timeout. PowerShell Get-Process instead of findstr for the tasklist check.

## Caveats
- Lane 1 holds a host-wide game lease (12c1d7272); launches wait automatically.
- Do not touch: gameosmain*, gos_static_prop_registry*, tgl_transform*, mission_update*, missionbegin*, gos_terrain_indirect_dispatch*, gamecam*, EditorInspector*,
  brain_special_dispatch*, mlrlightmap*; skip files dirty in A:/Games/mc2-main.
