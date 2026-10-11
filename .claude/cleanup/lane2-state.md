# Lane 2 state (worktree A:/Games/mc2-lane2, branch claude/lane2-s3, build64-ninja, install mc2-win64-lane2)

2026-10-11 second session (09:24-); C++23 STEP 2 (LG13 `#define` -> `inline constexpr`, LG14 `enum class`). Lane 1 keeps step 3. Plan: .claude/cleanup/cxx23-step2-define-enum-plan.md.
Proof ledger (per commit, per object, rulings applied): .claude/cleanup/step2-never-touch-audit.md. LG14 note and results: .claude/cleanup/cxx23-step2-enums-note.md.

## Where it stands (ratchet baseline: LG13 1,707, LG14 306)
- Merged nifty at 90bfbfed9 and 0b05530e3 (1a16f578e); the coordinator merged 566d76a3e afterwards. Earlier checkpoint at 797e508c3: ninja build, build_all_targets (all stages), harnesses 20/20 + doctest 2,719 cases, board 165/168, tier1 5/5.
  The 3 red rows are not this lane's: capture_provenance + selftest (BANNER_FMT vs the mechcmd2.cpp printf, lane 1) and doc_links (docs/beta/* exist only untracked in mc2-main). `background_launch` is green here since
  c409592cc registered tools/tidy/vs_trees.py (the nifty row for tools/sim_determinism.py was fixed on nifty).
- LG14 DONE for this step: 18 enums converted in 4 strict commits (929ecc662 cab34b9e2 edb92d936 b31a71d36), every object of the Ninja trees AND mc2_tests / harness trees IDENTICAL. Skips with reasons: tools/tidy/enum_skip.txt (41 names);
  the gate refused EquipmentReasonCode, UnitProfileReasonCode, Capability (tests index / loop / size arrays with `X__COUNT`) and ClassPolicy (C++17 harness tree rejects `using enum`). ActorState: dead header, skipped.
- LG13 (ratchet LG13 1,120 from 1,707): unitdesg.h (69b935d96) and 19 wide headers in 5 commits (04f05bb87 cc8a4ab2d 17ecb3df2 51e4ae3aa 06e2957c9) under the rulings: `--layout-only-ok --order-only-ok --vs-gate`, every accepted
  object named in the commit message, `tools/tidy/proof_allow.json` (MapGeneratorDialog, bounded, used once). Checkpoints (build_all_targets, doctest 2,719, tier1 5/5, board 165/168) at 13:13 and 14:40.
  OPEN (6 headers, ~56 sites): dstd.h inifile.h tgl.h txmmgr.h vfx.h (harness-gate failures, unattributed for three: bisect per header) and gameos.hpp (`controlgui_hudtabs.cpp.obj`, flaky/combination-dependent: classify first).
  The 8 headers with no including object (platform_win*.h, actor.h) cannot be proven by objects.

## Tools (tested, committed)
- tools/objhash.py: `hash --per-symbol`, `compare --per-symbol`, `diff A.obj B.obj` (IDENTICAL / LAYOUT-ONLY / ORDER-ONLY / DIFF).
- tools/tidy/proof_allow.json: bounded per-object exceptions (class `unreferenced-data-dropped`), honoured by `proof_ext.LayoutMixin.accepted` after `objhash.classify_allowed`.
- tools/tidy/proof_ext.py: `LayoutMixin` (opt-in LAYOUT-ONLY / ORDER-ONLY acceptance in the shared Proof, the ledger) and `Gate` (harness / mc2_tests / standalone tool trees, snapshot before the edit, settle after: a failing file is restored and re-verified).
- tools/tidy/vs_trees.py: includer map of the msbuild trees from the CL.read tlogs (a batched compile entry is a union: over-approximation), `snapshot` / `verify`, a failed target blames only the objects its errors name.
  Needs the VS trees BUILT first (`tools/build_contract_harnesses.py` builds the harnesses, `cmake --build build64-tests --config RelWithDebInfo` all of tests/unit); a failed gate build leaves partial tlogs, so rebuild them after.
- tools/tidy/rewrite_defines.py / rewrite_enums.py: flags `--layout-only-ok`, `--vs-gate`, (enums) `--max-enums N`, `--skip-enums`; default proof PLAIN objhash; `--relaxed-proof REASON` unchanged; `stringized-by-wrapper-macro` and qualified-use refusals.
- Untracked scratch (kept): .claude/l2cfg/enum_probe.py (compile probe of every candidate), vs_probe.py / vs_why.py (what a header edit does to the VS trees), wide_scan.py (all headers at once, per-object classification),
  bisect_obj.py (which headers change one object), wide_diag.py, unitdesg_classify.py.

## Never-touch (coordinator, steps 2 and 3)
mclib/estring*, elist*, heap*, stuff/*, mlr/*, gosfx/*, memorystream*, file*, packet*, fastfile*, abl*; code/multplyr*, dmultplyr.h; every *_internal.h; the 50 TUs of the audit's section D.
Ratchet policy: never contort code for a ratchet; a legitimate rise gets a baseline rewrite with the reason in the commit.

## Exact next steps for the next lane 2
1. Merge nifty (lane 1 keeps moving). Then the 6 open wide headers, one at a time with a fresh `--taint-file` (a differing object taints every header it includes): `py -3 tools/tidy/rewrite_defines.py run --scope all
   --tree build-alltargets --max-files 1 --max-objs 100000 --max-header-includers 100000 --layout-only-ok --order-only-ok --vs-gate --done D --taint-file T --commit` after putting the header name in D's complement.
   For the three unattributed gate failures run `py -3 .claude/l2cfg/vs_why.py mclib/tgl.h` (applies the enum rewrite today; adapt to the define rewrite or run the batch with `--vs-gate` and read the `FAIL` lines of `vs_trees.verify`).
   For gameos.hpp: `python .claude/l2cfg/wide_diag.py GameOS/include/gameos.hpp CMakeFiles/mc2.dir/code/controlgui_hudtabs.cpp.obj`, then `objhash.py diff`: if it is a __TIME__ / noise class, the Proof's noise check should have caught it; if not, classify.
2. LG13 beyond those: nothing eligible remains (the other literal defines are refused by the census: named outside C++, defined twice, C-side, macro-body). LG14: nothing eligible remains (ledger `enum_skip.txt`).
3. Never edit tracked files while a batch runs; commit by pathspec; scripts via the Write tool (bash heredocs and non-raw python strings collapse backslashes; python and Git Bash see /tmp differently).

## Rules learned
- Deploy to the lane install needs `--allow-new-target` (existing folder): `deploy_payload.py --target game --ninja --allow-new-target A:/Games/mc2-opengl/mc2-win64-lane2`.
- The rewriters scan every tracked .py / .json / .cmake for identifiers: a test or docstring that names a real constant or enum makes it `named-outside-c++`. Use invented names in tests.
- The harness trees are msbuild trees: no `ninja -t deps`. The tlog of a batched compile lists several sources and the UNION of their reads.
- A header-only change proof of "no TU includes it" (ActorState) is a reason to skip, not to convert.
