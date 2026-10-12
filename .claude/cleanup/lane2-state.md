# Lane 2 state (worktree A:/Games/mc2-lane2, branch claude/lane2-s3, build64-ninja, install mc2-win64-lane2)

2026-10-11 third session (16:50-19:10; the second ran 09:24-); C++23 STEP 2 (LG13 `#define` -> `inline constexpr`, LG14 `enum class`). Lane 1 keeps step 3. Plan: .claude/cleanup/cxx23-step2-define-enum-plan.md.
Proof ledger (per commit, per object, rulings applied): .claude/cleanup/step2-never-touch-audit.md. LG14 note and results: .claude/cleanup/cxx23-step2-enums-note.md.

## Where it stands (ratchet baseline: LG13 1,063, LG14 306; LG10 is nifty's 97 vs 96, see below)
- Third session: merged nifty at 018c09a06 (clean). FULL CHECKPOINT at the last code commit f0bfefa40 (HEAD b9af1ac02 only moves the ratchet baseline): ninja build, build_all_targets (every stage), harnesses 20/20 + doctest 2,729 cases, deploy to the lane install, tier1 5/5 (report 2026-10-11T18-49-12; the first of the session, 17-12-33, was 5/5 at the merge). Board not re-run.
  Earlier checkpoint (second session) at 797e508c3: board 165/168; the 3 red rows are not this lane's: capture_provenance + selftest (BANNER_FMT vs the mechcmd2.cpp printf, lane 1) and doc_links (docs/beta/* exist only untracked in mc2-main).
  RATCHET: `cxx17_ratchet --check` FAILS on nifty itself (LG10 96 -> 97, code/mech_init.cpp:221, commit 6876d17fd, lane 1) so `--tighten` refuses; the LG13 baseline was tightened by hand (b9af1ac02) with LG10 left at nifty's value. Lane 1 owns the fix.
- LG14 DONE for this step: 18 enums converted in 4 strict commits (929ecc662 cab34b9e2 edb92d936 b31a71d36), every object of the Ninja trees AND mc2_tests / harness trees IDENTICAL. Skips with reasons: tools/tidy/enum_skip.txt (41 names);
  the gate refused EquipmentReasonCode, UnitProfileReasonCode, Capability (tests index / loop / size arrays with `X__COUNT`) and ClassPolicy (C++17 harness tree rejects `using enum`). ActorState: dead header, skipped.
- LG13 (ratchet LG13 1,707 -> 1,063): unitdesg.h (69b935d96); 19 wide headers in 5 commits (04f05bb87 cc8a4ab2d 17ecb3df2 51e4ae3aa 06e2957c9); tgl.h (650208f38) and vfx.h alone; THIS SESSION dstd.h (ebd9233a2, 3 sites),
  txmmgr.h (ad5e4b271, 14) and gameos.hpp (f0bfefa40, 15), each ALONE under `--layout-only-ok --order-only-ok --vs-gate` (accepted objects named in each message: 24, 38, 55). Their failures in the earlier batches were interactions.
  gameos.hpp's `controlgui_hudtabs.cpp.obj` difference is BUILD NOISE (__TIME__ string-pool symbol): four recompiles of identical input gave 3 identical and 1 `DIFF symbol set; section .text$mn`. The rewriters' Proof had never run its noise
  check (no object-to-source map was passed); 9c7e7f774 passes it and names every ignored object in the commit message.
  OPEN (1 header, 15 sites): `mclib/inifile.h`. The harness gate fails `build64-tests::mc2_tests.dir/RelWithDebInfo/test_file_inifile.obj` (DIFF symbol set; .pdata; .rdata; .text$mn; .xdata) and it is REAL, not noise: original x2 IDENTICAL, edited x2
  IDENTICAL, original vs edited DIFF. Cause: tests/unit/test_file_inifile.cpp compares inifile.h constants inside doctest CHECK / REQUIRE (`== NO_ERR`, `== BLOCK_NOT_FOUND`): the operand is taken by const reference, so `inline constexpr` is ODR-used
  (`?BLOCK_NOT_FOUND@@3IB` emitted, `operator==<const unsigned int&>` replaces `operator==<unsigned int>`) and the stringized expression text changes from the value to the name. Left unconverted; ruling needed (see next steps).
  The 8 headers with no including object (platform_win*.h, actor.h) cannot be proven by objects.

## Tools (tested, committed)
- tools/objhash.py: `hash --per-symbol`, `compare --per-symbol`, `diff A.obj B.obj` (IDENTICAL / LAYOUT-ONLY / ORDER-ONLY / DIFF).
- tools/tidy/proof_allow.json: bounded per-object exceptions (class `unreferenced-data-dropped`), honoured by `proof_ext.LayoutMixin.accepted` after `objhash.classify_allowed`.
- tools/tidy/proof_ext.py: `LayoutMixin` (opt-in LAYOUT-ONLY / ORDER-ONLY acceptance in the shared Proof, the ledger) and `Gate` (harness / mc2_tests / standalone tool trees, snapshot before the edit, settle after: a failing file is restored and re-verified).
- tools/tidy/vs_trees.py: includer map of the msbuild trees from the CL.read tlogs (a batched compile entry is a union: over-approximation), `snapshot` / `verify`, a failed target blames only the objects its errors name.
  Needs the VS trees BUILT first (`tools/build_contract_harnesses.py` builds the harnesses, `cmake --build build64-tests --config RelWithDebInfo` all of tests/unit); a failed gate build leaves partial tlogs, so rebuild them after.
- tools/tidy/rewrite_defines.py / rewrite_enums.py: flags `--layout-only-ok`, `--vs-gate`, (enums) `--max-enums N`, `--skip-enums`; default proof PLAIN objhash; `--relaxed-proof REASON` unchanged; `stringized-by-wrapper-macro` and qualified-use refusals.
- NEW this session (each a commit with a pytest in tools/tidy/): `batch_run.py` (prove_batch + the batch-of-one fallback: a multi-header batch re-runs each rejected header alone, keeps those that pass each in its own commit, reports which passed alone / still fail; `--no-alone-fallback` disables; 8c38d759e), vs_trees failure text (`extract_errors`, full log kept in .claude/l2cfg/vs_gate_logs/, the ledger reason carries it; f1c47af3e), `check_batch_allowed.py` (what the taint / done ledgers would skip and why, tainted objects ranked by headers hidden; c9d83adb4), `rewrite_defines --only FILE` (f95e1d76f), noise check wired into the proof (9c7e7f774).
- Untracked scratch (kept, in .claude/l2cfg/; the predecessor's .claude/_*.py|txt|sh|cpp files were moved there): .claude/l2cfg/enum_probe.py (compile probe of every candidate), vs_probe.py / vs_why.py (what a header edit does to the VS trees), wide_scan.py (all headers at once, per-object classification),
  bisect_obj.py (which headers change one object), wide_diag.py, unitdesg_classify.py.

## Never-touch (coordinator, steps 2 and 3)
mclib/estring*, elist*, heap*, stuff/*, mlr/*, gosfx/*, memorystream*, file*, packet*, fastfile*, abl*; code/multplyr*, dmultplyr.h; every *_internal.h; the 50 TUs of the audit's section D.
Ratchet policy: never contort code for a ratchet; a legitimate rise gets a baseline rewrite with the reason in the commit.

## Exact next steps for the next lane 2
1. Merge nifty. inifile.h needs a ruling; the tool-side cure is small: let `analyse` also scan tests/unit and refuse (reason `used-in-doctest-assertion`) any constant named inside a doctest CHECK / REQUIRE / CHECK_EQ argument list, the same
   class as the existing `stringized-by-wrapper-macro` refusal. That converts 13 of the 15 inifile.h sites and leaves NO_ERR / BLOCK_NOT_FOUND macros. The alternative is a doctest-side fix (cast the right operand: `(unsigned)NO_ERR`), a test change
   outside this step's scope. A lane never changes the proof; the orchestrator rules.
2. One header at a time: `py -3 tools/tidy/check_batch_allowed.py --tree build-alltargets --scope all --max-header-includers 100000 --taint-file T --done D` first, then `py -3 tools/tidy/rewrite_defines.py run --scope all --tree build-alltargets --max-files 1 --max-objs 100000
   --max-header-includers 100000 --layout-only-ok --order-only-ok --vs-gate --only FILE --done D --taint-file T --commit` (empty fresh D and T). Never start a second rewriter while one runs: the gate re-verify after a REVERT keeps rebuilding for minutes after the REVERT line.
3. LG13 beyond inifile.h: nothing eligible remains (the other literal defines are refused by the census: named outside C++, defined twice, C-side, macro-body). LG14: nothing eligible remains (ledger `enum_skip.txt`).
4. Never edit tracked files while a batch runs; commit by pathspec; scripts via the Write tool (bash heredocs and non-raw python strings collapse backslashes; python and Git Bash see /tmp differently).

## Rules learned
- Deploy to the lane install needs `--allow-new-target` (existing folder): `deploy_payload.py --target game --ninja --allow-new-target A:/Games/mc2-opengl/mc2-win64-lane2`.
- The rewriters scan every tracked .py / .json / .cmake for identifiers: a test or docstring that names a real constant or enum makes it `named-outside-c++`. Use invented names in tests.
- The harness trees are msbuild trees: no `ninja -t deps`. The tlog of a batched compile lists several sources and the UNION of their reads.
- A header-only change proof of "no TU includes it" (ActorState) is a reason to skip, not to convert.

- `run_smoke.py --deploy` takes the NAME (`lane2`), not the path. A foreground command over 120 s is moved to the background: run long waits with `timeout 118 bash -c 'until ...'`.
- doctest `CHECK(x == MACRO)` stringizes the macro-expanded text and binds the right operand by const reference: a literal `#define` used there changes the test object when it becomes an `inline constexpr` (inifile.h).
- lane 1's `tools/sim_determinism.py` + a 12-worker burn script load the host: a 1,000-object proof takes 10-20 min instead of 5.