# Lane 2 state (worktree A:/Games/mc2-lane2, branch claude/lane2-s3, build64-ninja, install mc2-win64-lane2)

2026-10-11 second session (09:24-); C++23 STEP 2 (LG13 `#define` -> `inline constexpr`, LG14 `enum class`). Lane 1 keeps step 3. Plan: .claude/cleanup/cxx23-step2-define-enum-plan.md.
Proof ledger (per commit, per object, rulings applied): .claude/cleanup/step2-never-touch-audit.md. LG14 note and results: .claude/cleanup/cxx23-step2-enums-note.md.

## Where it stands (ratchet baseline: LG13 1,707, LG14 306)
- Merged nifty at 90bfbfed9 and 0b05530e3 (1a16f578e). Checkpoint at 797e508c3: ninja build, build_all_targets (all stages), harnesses 20/20 + doctest 2,719 cases, board 165/168, tier1 5/5.
  The 3 red rows are not this lane's: capture_provenance + selftest (BANNER_FMT vs the mechcmd2.cpp printf, lane 1) and doc_links (docs/beta/* exist only untracked in mc2-main). `background_launch` is green here since
  c409592cc registered tools/tidy/vs_trees.py (the nifty row for tools/sim_determinism.py was fixed on nifty).
- LG14 DONE for this step: 18 enums converted in 4 strict commits (929ecc662 cab34b9e2 edb92d936 b31a71d36), every object of the Ninja trees AND mc2_tests / harness trees IDENTICAL. Skips with reasons: tools/tidy/enum_skip.txt (41 names);
  the gate refused EquipmentReasonCode, UnitProfileReasonCode, Capability (tests index / loop / size arrays with `X__COUNT`) and ClassPolicy (C++17 harness tree rejects `using enum`). ActorState: dead header, skipped.
- LG13: unitdesg.h done (69b935d96, `--layout-only-ok`, 2 LAYOUT-ONLY objects named in the message). The 30 wide headers (~660 sites) are BLOCKED, evidence in ledger section 5: (a) every wide header produces an ORDER-ONLY
  object in editor TUs (`SingleValueDlg.cpp.obj`; even a 1-site header does), (b) 12 of them change `MapGeneratorDialog.cpp.obj` (stb_image tables dropped: DIFF). Both need rulings; nothing was relaxed by the lane.

## Tools (tested, committed)
- tools/objhash.py: `hash --per-symbol`, `compare --per-symbol`, `diff A.obj B.obj` (IDENTICAL / LAYOUT-ONLY / ORDER-ONLY / DIFF).
- tools/tidy/proof_ext.py: `LayoutMixin` (opt-in LAYOUT-ONLY acceptance in the shared Proof) and `Gate` (harness / mc2_tests / standalone tool trees, snapshot before the edit, settle after: a failing file is restored and re-verified).
- tools/tidy/vs_trees.py: includer map of the msbuild trees from the CL.read tlogs (a batched compile entry is a union: over-approximation), `snapshot` / `verify`, a failed target blames only the objects its errors name.
  Needs the VS trees BUILT first (`tools/build_contract_harnesses.py` builds the harnesses, `cmake --build build64-tests --config RelWithDebInfo` all of tests/unit); a failed gate build leaves partial tlogs, so rebuild them after.
- tools/tidy/rewrite_defines.py / rewrite_enums.py: flags `--layout-only-ok`, `--vs-gate`, (enums) `--max-enums N`, `--skip-enums`; default proof PLAIN objhash; `--relaxed-proof REASON` unchanged; `stringized-by-wrapper-macro` and qualified-use refusals.
- Untracked scratch (kept): .claude/l2cfg/enum_probe.py (compile probe of every candidate), vs_probe.py / vs_why.py (what a header edit does to the VS trees), wide_scan.py (all headers at once, per-object classification),
  bisect_obj.py (which headers change one object), wide_diag.py, unitdesg_classify.py.

## Never-touch (coordinator, steps 2 and 3)
mclib/estring*, elist*, heap*, stuff/*, mlr/*, gosfx/*, memorystream*, file*, packet*, fastfile*, abl*; code/multplyr*, dmultplyr.h; every *_internal.h; the 50 TUs of the audit's section D.
Ratchet policy: never contort code for a ratchet; a legitimate rise gets a baseline rewrite with the reason in the commit.

## Exact next steps for the next lane 2
1. Rulings needed for the wide LG13 headers: ORDER-ONLY acceptance (a pure symbol / relocation index shift; the lane would add `--order-only-ok`, named in the commit like LAYOUT-ONLY) and the MapGeneratorDialog class
   (`--ignore-digest-file out/editor/CMakeFiles/EditRel.dir/MapGeneratorDialog.cpp.obj`, compile-must-pass). Then batches of <= 5 headers: `py -3 tools/tidy/rewrite_defines.py run --scope all --tree build-alltargets --max-files 5
   --max-objs 100000 --max-header-includers 100000 --layout-only-ok --vs-gate --done X --taint-file Y --commit` (use a fresh --taint-file: a differing object taints every header it includes), tier1 after every 3 batches.
2. Without those rulings the next productive steps are outside LG13/LG14: see the orchestrator queue.
3. Never edit tracked files while a batch runs; commit by pathspec; scripts via the Write tool (bash heredocs and non-raw python strings collapse backslashes; python and Git Bash see /tmp differently).

## Rules learned
- Deploy to the lane install needs `--allow-new-target` (existing folder): `deploy_payload.py --target game --ninja --allow-new-target A:/Games/mc2-opengl/mc2-win64-lane2`.
- The rewriters scan every tracked .py / .json / .cmake for identifiers: a test or docstring that names a real constant or enum makes it `named-outside-c++`. Use invented names in tests.
- The harness trees are msbuild trees: no `ninja -t deps`. The tlog of a batched compile lists several sources and the UNION of their reads.
- A header-only change proof of "no TU includes it" (ActorState) is a reason to skip, not to convert.
