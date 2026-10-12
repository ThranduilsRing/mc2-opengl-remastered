# Lane 2 state (worktree A:/Games/mc2-lane2, branch claude/lane2-s3, build64-ninja, install mc2-win64-lane2)

2026-10-11 third session (16:50-20:00; the second ran 09:24-); C++23 STEP 2 (LG13 `#define` -> `inline constexpr`, LG14 `enum class`) is COMPLETE. Lane 1 keeps step 3. Plan: .claude/cleanup/cxx23-step2-define-enum-plan.md.
Proof ledger (per commit, per object, rulings applied): .claude/cleanup/step2-never-touch-audit.md. LG14 note and results: .claude/cleanup/cxx23-step2-enums-note.md.

## Where it stands (STEP 2 COMPLETE; ratchet baseline: LG13 1,049, LG14 306)
- Third session: merged nifty at 018c09a06 and again after bf4b3265e (LG10 rise fixed on nifty; `cxx17_ratchet --check` OK; my hand-tightened LG13 baseline b9af1ac02 merged cleanly). Checkpoints: at f0bfefa40 ninja build, build_all_targets, harnesses 20/20 + doctest 2,729, deploy, tier1 5/5
  (report 2026-10-11T18-49-12); at the last code commit 7e3e47c42 (inifile.h) build_all_targets (every stage) and harnesses 20/20 + doctest 2,730 cases green (no re-deploy / tier1: the game objects were proven identical). Board not re-run (second-session board 165/168; the 3 reds are not this lane's:
  capture_provenance + selftest = BANNER_FMT vs the mechcmd2.cpp printf, lane 1; doc_links = docs/beta/* untracked in mc2-main).
- LG14 DONE for this step: 18 enums converted in 4 strict commits (929ecc662 cab34b9e2 edb92d936 b31a71d36), every object of the Ninja trees AND mc2_tests / harness trees IDENTICAL. Skips with reasons: tools/tidy/enum_skip.txt (41 names);
  the gate refused EquipmentReasonCode, UnitProfileReasonCode, Capability (tests index / loop / size arrays with `X__COUNT`) and ClassPolicy (C++17 harness tree rejects `using enum`). ActorState: dead header, skipped.
- LG14 and LG13 are DONE for this step. LG13 1,707 -> 1,049: unitdesg.h (69b935d96); 19 wide headers in 5 commits (04f05bb87 cc8a4ab2d 17ecb3df2 51e4ae3aa 06e2957c9); tgl.h (650208f38) and vfx.h alone; THIS SESSION dstd.h (ebd9233a2, 3 sites),
  txmmgr.h (ad5e4b271, 14), gameos.hpp (f0bfefa40, 15) and inifile.h (7e3e47c42, 14), each ALONE under `--layout-only-ok --order-only-ok --vs-gate` (accepted objects named in each message). Earlier batch failures of dstd / txmmgr / gameos were interactions.
  gameos.hpp's `controlgui_hudtabs.cpp.obj` difference is BUILD NOISE (__TIME__ string-pool symbol): four recompiles of identical input gave 3 identical and 1 `DIFF symbol set; section .text$mn`. The rewriters' Proof had never run its noise check
  (no object-to-source map was passed); 9c7e7f774 passes it and names every ignored object in the commit message.
  inifile.h failed whole on `test_file_inifile.obj` (REAL: doctest `== NO_ERR` / `== BLOCK_NOT_FOUND` bind by const reference, so the inline variable is ODR-used and the stringized text changes). Ruling: the census refuses such constants (`odr-used-by-doctest`),
  the other 14 sites converted. LEDGER in step2-never-touch-audit.md section 7: `NO_ERR` and `BLOCK_NOT_FOUND` stay macros; the future cure (a later ruling) is acceptance of a changed TEST-ONLY object when the test tier passes, or a cast in the test.
- What the census STILL refuses (census at the end: 1,493 literal defines, 402 listed ELIGIBLE = 390 header + 12 .cpp-local sites): named outside C++ (613: scripts, cmake, resources, shaders, C-side includes), defined twice, inside another #define / #if / #undef (macro-body, directive-*),
  string literals (56 `string-literal-codegen-differs`: a constexpr pointer is loaded where the literal was addressed), stringized by a wrapper macro, `odr-used-by-doctest` (2), not at namespace scope, the never-touch list, and the 8 headers with no including object (platform_win*.h, actor.h).
  The 402 ELIGIBLE sites are all out of reach, not unproven: never-touch files (code/dmultplyr.h, multplyr.h, mclib/abl*.h, file.h, heap.h, packet.h, *_internal.h, ~160 sites), the 4 GameOS/include/platform_win*.h and code/actor.h (no including object, ~175), and files dirty in a worktree (code/artlry.cpp, bldng.cpp). `py -3 tools/tidy/check_batch_allowed.py` prints that list; step 4 (which touches the never-touch files) revisits it.

## Tools (tested, committed)
- tools/objhash.py: `hash --per-symbol`, `compare --per-symbol`, `diff A.obj B.obj` (IDENTICAL / LAYOUT-ONLY / ORDER-ONLY / DIFF).
- tools/tidy/proof_allow.json: bounded per-object exceptions (class `unreferenced-data-dropped`), honoured by `proof_ext.LayoutMixin.accepted` after `objhash.classify_allowed`.
- tools/tidy/proof_ext.py: `LayoutMixin` (opt-in LAYOUT-ONLY / ORDER-ONLY acceptance in the shared Proof, the ledger) and `Gate` (harness / mc2_tests / standalone tool trees, snapshot before the edit, settle after: a failing file is restored and re-verified).
- tools/tidy/vs_trees.py: includer map of the msbuild trees from the CL.read tlogs (a batched compile entry is a union: over-approximation), `snapshot` / `verify`, a failed target blames only the objects its errors name.
  Needs the VS trees BUILT first (`tools/build_contract_harnesses.py` builds the harnesses, `cmake --build build64-tests --config RelWithDebInfo` all of tests/unit); a failed gate build leaves partial tlogs, so rebuild them after.
- tools/tidy/rewrite_defines.py / rewrite_enums.py: flags `--layout-only-ok`, `--vs-gate`, (enums) `--max-enums N`, `--skip-enums`; default proof PLAIN objhash; `--relaxed-proof REASON` unchanged; `stringized-by-wrapper-macro` and qualified-use refusals.
- NEW this session (each a commit with a pytest in tools/tidy/): `batch_run.py` (prove_batch + the batch-of-one fallback: a multi-header batch re-runs each rejected header alone, keeps those that pass each in its own commit, reports which passed alone / still fail; `--no-alone-fallback` disables; 8c38d759e), vs_trees failure text (`extract_errors`, full log kept in .claude/l2cfg/vs_gate_logs/, the ledger reason carries it; f1c47af3e), `check_batch_allowed.py` (what the taint / done ledgers would skip and why, tainted objects ranked by headers hidden; c9d83adb4), `rewrite_defines --only FILE` (f95e1d76f), noise check wired into the proof (9c7e7f774).
- Also this session: `rewrite_defines` refuses `odr-used-by-doctest` (a candidate named inside a doctest CHECK / REQUIRE / WARN argument list in tracked tests/ files; commit after 3a666752f, test in test_rewrite_defines.py).
- Untracked scratch (kept, in .claude/l2cfg/; the predecessor's .claude/_*.py|txt|sh|cpp files were moved there): .claude/l2cfg/enum_probe.py (compile probe of every candidate), vs_probe.py / vs_why.py (what a header edit does to the VS trees), wide_scan.py (all headers at once, per-object classification),
  bisect_obj.py (which headers change one object), wide_diag.py, unitdesg_classify.py.

## Never-touch (coordinator, steps 2 and 3)
mclib/estring*, elist*, heap*, stuff/*, mlr/*, gosfx/*, memorystream*, file*, packet*, fastfile*, abl*; code/multplyr*, dmultplyr.h; every *_internal.h; the 50 TUs of the audit's section D.
Ratchet policy: never contort code for a ratchet; a legitimate rise gets a baseline rewrite with the reason in the commit.

## Exact next steps for the next lane 2
1. STEP 2 is complete. Nothing is queued for lane 2 inside it. The one open follow-up is a ruling: acceptance of a changed test-only object when the doctest tier passes (frees NO_ERR / BLOCK_NOT_FOUND), or a cast in tests/unit/test_file_inifile.cpp.
2. Merge nifty before any new work. One header at a time if the census ever lists more: `py -3 tools/tidy/check_batch_allowed.py --tree build-alltargets --scope all --max-header-includers 100000 --taint-file T --done D`, then `py -3 tools/tidy/rewrite_defines.py run --scope all
   --tree build-alltargets --max-files 1 --max-objs 100000 --max-header-includers 100000 --layout-only-ok --order-only-ok --vs-gate --only FILE --done D --taint-file T --commit` (empty fresh D and T). Never start a second rewriter while one runs: the gate re-verify after a REVERT keeps rebuilding for minutes.
3. Never edit tracked files while a batch runs; commit by pathspec; scripts via the Write tool (bash heredocs and non-raw python strings collapse backslashes).

## Rules learned
- Deploy to the lane install needs `--allow-new-target` (existing folder): `deploy_payload.py --target game --ninja --allow-new-target A:/Games/mc2-opengl/mc2-win64-lane2`.
- The rewriters scan every tracked .py / .json / .cmake for identifiers: a test or docstring that names a real constant or enum makes it `named-outside-c++`. Use invented names in tests.
- The harness trees are msbuild trees: no `ninja -t deps`. The tlog of a batched compile lists several sources and the UNION of their reads.
- A header-only change proof of "no TU includes it" (ActorState) is a reason to skip, not to convert.

- `run_smoke.py --deploy` takes the NAME (`lane2`), not the path. A foreground command over 120 s is moved to the background: run long waits with `timeout 118 bash -c 'until ...'`.
- doctest `CHECK(x == MACRO)` stringizes the macro-expanded text and binds the right operand by const reference: a literal `#define` used there changes the test object when it becomes an `inline constexpr` (inifile.h).
- lane 1's `tools/sim_determinism.py` + a 12-worker burn script load the host: a 1,000-object proof takes 10-20 min instead of 5.