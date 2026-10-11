# Lane 2 state (worktree A:/Games/mc2-lane2, branch claude/lane2-s3, build64-ninja, install mc2-win64-lane2)

Rotated lane-2 agent #2 (started 2026-10-10 17:58). This lane does the 1000-1500 tier verbatim splits (code/ and mclib/ first, largest first).
Read ORCHESTRATOR-HANDOFF.md Ground rules first. Whole-tier census: appendix of size-tier-1000-survey.md (`py -3 tools/split/tier_census.py`).

## Done (all on claude/lane2-s3; sizes before -> core after; sha = split commit)
- 1 abldbug 1319->753 dd946d83d | 2 genactor 1297->730 0196b20c5 | 3 logisticsmissioninfo 1316->511 dc3a26c8a | 4 logisticsvariant 1221->551 aa9130de3
- 5 cevfx 1217->669 2d6c73efc | 6 mechbayscreen 1176->325 b43d4b40c | 7 group 1102->253 f0d37de6c | 8 team 1077->543 be79c6ead
- 9 ablxstd 1071->239 6292520c3 | 10 mlr_i_tmesh 1206->427 27cd40e3b | 11 multplyr 1204->403 78da1679d | 12 logistics 1002->437 9af8c3f04
- userinput.cpp 1036->649: deleted the twelve unreferenced cursor UV tables (821775678; recovery 76a3e3d91).
- Tooling: capture_ab.py, split_cpp.py (--stage-dir/--install with a verified marker, GROUP_WRAP), post_split.py, tier_census.py, split_verify 'static' check
  (file-static / anonymous / named-namespace static function called from another piece).
- Tier1 5/5 at 055dacab9, aa9130de3, be79c6ead; and 69a0d5450 (after splits 9-12 + the deletion): 5/5 (147 / 84 / 105 / 132 / 123 fps).

## Next
- cone_glow.cpp (1021) SKIPPED: the gate readers are interleaved with static helpers shared by the queue code (vis_gate_enabled <-> vis_job_a_armed ...), a clean
  cut leaves a ~135-line piece. Not worth the linkage edits.
- Left (census CHEAP, survey LEFT; do not touch): contact, ablscan, soundsys, estring, quad_draw, mlr_i_pmesh. PHASE files need a phase pass.
- Census leftovers in code/ and mclib/ at 1000-1500 are pieces (PIECE) or LEFT; the tier in code/ and mclib/ is essentially done. GameOS / editor / GuiRuntime
  CHEAP files from the survey (gos_particle_bridge, vulkan_water_island, GuiRuntime.cpp, EditorObjects) are the next real candidates, if the coordinator wants them.
- Nice-to-have tooling (not built): (1) cfg generator from first/last function names, (2) split_cpp.py writing the CMake list edit and the allow-json skeleton,
  (3) split_verify: a header (via an `#else`) that DEFINES a global (multplyr EventsToMissionResultsScreen link error) is not caught by check_header.

## Recipe per split
1. `git grep -n '<orig>.cpp' -- '*CMakeLists.txt' '*.cmake'`; write a cfg (.claude/l2cfg/*_cfg.py as models; allow json via $SPLIT_ALLOW_DIR) and stage:
   `py -3 tools/split/split_cpp.py CFG --stage-dir /tmp/l2/stage_x` (split_verify runs inside), then the same plus `--install` (only after a passing run).
2. Add the pieces to every CMake list; `scripts/ninja_build.py`; `py -3 tools/build_contract_harnesses.py --run --unit`;
   `py -3 tools/split/post_split.py --src F --stem S --note "..."` (plan row, split_map, size ratchet, full board, failing-row digest; doc_links is the only known red);
   commit by pathspec; split_commit sha goes in the allow json at the next report.
3. Per pair: `py -3 tools/build_all_targets.py`, deploy lane2, `run_smoke.py --lite --exe`, `capture_ab.py capture --tag X` then `compare --before PREV --after X`
   (mc2_01 GL has a bimodal ~1700 / ~6000-9000 px noise mode, mechs and the bottom-right corner; compare against the NOISE line). Tier1 after every four splits.
4. Run the board BEFORE committing a new tool: background_launch needs console_child.child_kwargs and an allowlist entry for wrapper scripts.
- Files are CRLF in the worktree; git warns and stores new files LF (autocrlf=input). Bash heredocs collapse backslashes: write python with regexes via the Write tool.
- `py` and Git Bash see /tmp differently for relative file reads; use repo-relative paths. A bare `bash` from python can start WSL's: use shutil.which.

## Caveats
- Lane 1 holds a host-wide game lease (12c1d7272); launches wait automatically.
- Do not touch: gameosmain*, gos_static_prop_registry*, tgl_transform*, mission_update*, missionbegin*, gos_terrain_indirect_dispatch*, gamecam*, EditorInspector*,
  brain_special_dispatch*, mlrlightmap*; skip files dirty in A:/Games/mc2-main.
