# Lane 2 state (worktree A:/Games/mc2-lane2, branch claude/lane2-s3, build64-ninja, install mc2-win64-lane2)

Rotated lane-2 agent #2 (started 2026-10-10 17:58). Phase-helper round 3 is merged into nifty; this lane now does the
1000-1500 tier verbatim splits (code/ and mclib/ first, largest first). Read ORCHESTRATOR-HANDOFF.md Ground rules first.

## Done
- Task 1: merged nifty, full tier1 on 055dacab9 (covers DrawLightMaps/drawImGui): 5/5 (mc2_01 113, mc2_03 79, mc2_10 102, mc2_17 118, mc2_24 113 fps).
- Split 1 dd946d83d mclib/abldbug.cpp 1319 -> 753 + abldbug_print.cpp 488 + abldbug_internal.h.
- Split 2 0196b20c5 mclib/genactor.cpp 1297 -> 730 + genactor_frame.cpp 498 + genactor_internal.h.
- Pair 1 verified: build, doctest 704, harnesses 20/20, board 164/165 (doc_links only), smoke-lite 2/2, captures GL x2 + VK mc2_01/mc2_10
  (before-vs-after DIFF the same size and place as before-vs-before noise). build_all_targets: every stage built (887d9d1f9).

## Next (queue, largest first; each needs a census read before cutting)
- code/logisticsmissioninfo.cpp 1316 (3 pieces: core 500 / progress 377 / query 411) - cfg .claude/l2cfg/lm_cfg.py, generated pieces parked in /tmp/l2/hold
  (regenerate, they are cheap: `py -3 tools/split/split_cpp.py .claude/l2cfg/lm_cfg.py`; the root CMakeLists.txt list needs the two pieces).
- code/logisticsvariant.cpp 1221 (core 548 / stats 645) - cfg .claude/l2cfg/lv_cfg.py; allow json must list tools/gate_codemod.py and
  tools/mc2_modlint/modlint_variant_csv.h as reviewed script mentions. Gates in it are function-local lazy statics (stay lazy).
- then mclib/cevfx.cpp 1217 (per-effect draw classes), code/mechbayscreen.cpp, code/group.cpp, code/team.cpp, mclib/userinput.cpp.
- Survey verdict LEFT (skip): objmgr_update, contact, ablscan, soundsys, estring, quad_draw, mlr_i_pmesh. The survey only names the top 20 of the tier;
  the cheap candidates above come from my own census (`/tmp/l2/census.py`-style: cpp_units funcs/largest/other per file).
- tier1 owed after every four splits (next due after split 4 or 5).

## Recipe per split (what worked)
1. `git grep -n '<orig>.cpp' -- '*CMakeLists.txt' '*.cmake'` first: every list that compiled the original gets the new pieces.
2. cfg (see .claude/l2cfg/*.py; INCLUDE_RAW = include bundle lines, PREAMBLE_END = first function unit, GROUPS by cpp_units line ranges) +
   tools/split/allow/<stem>.json (base = HEAD sha; scripts[] for every script split_verify names). split_cpp.py runs split_verify itself.
3. mv <src>.new over <src>; ninja_build; `py -3 tools/build_contract_harnesses.py --run --unit` (20 harnesses + doctest 704);
   `py -3 tools/split/split_map.py --write`; `file_size_ratchet.py --check --tighten`; board `bash scripts/check-contracts.sh > file` (~2.5 min; only doc_links red);
   plan-doc row in giant-file-split-plan.md; commit by pathspec; split_commit sha goes into the allow json with the next report commit.
4. Per pair: `py -3 tools/build_all_targets.py` (15 min cold, run it in the background when no capture runs), deploy lane2, `run_smoke.py --lite --exe`, captures with
   `bash .claude/rl_cap.sh <tag>` (BEFORE-set must be taken on the old install first; output .claude/l2cap/<tag>{1,2}-gl,<tag>-vk), compare with
   `py -3 tools/visual_verify/montage_compact.py <before.png> <after.png> --out f` (before-vs-before gives the noise floor: mc2_01 ~6000 px at 1657,977).
- Files are CRLF in the worktree; git warns and stores new files LF (autocrlf=input), ignore.
- Bash tool: foreground `sleep` is blocked, use `until <cond>; do sleep N; done` with timeout; `findstr` under Git Bash fails, use PowerShell Get-Process.

## Caveats
- Lane 1 holds a host-wide game lease now (12c1d7272); launches wait automatically.
- The nifty background_launch row was red once (tools/test_git_util.py) - lane 1's, since fixed upstream or not mine.
- Do not touch: gameosmain*, gos_static_prop_registry*, tgl_transform*, mission_update*, missionbegin*, gos_terrain_indirect_dispatch*, gamecam*, EditorInspector*, brain_special_dispatch*, mlrlightmap*; skip files dirty in A:/Games/mc2-main.
