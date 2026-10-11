# Lane 2 state (worktree A:/Games/mc2-lane2, branch claude/lane2-s3, build64-ninja, install mc2-win64-lane2)

2026-10-11 second session (09:24-); C++23 STEP 2 (LG13 `#define` -> `inline constexpr`, LG14 `enum class`). Lane 1 keeps step 3. Plan: .claude/cleanup/cxx23-step2-define-enum-plan.md.
Proof ledger (per commit, per object, incl. the unitdesg.h outcome): .claude/cleanup/step2-never-touch-audit.md. LG14 design note: .claude/cleanup/cxx23-step2-enums-note.md.

## Where it stands (ratchet baseline: LG13 1,758, LG14 321)
- Merged nifty twice (0d6991f16 = 78c3be62d, 90bfbfed9 = d6fcd12aa). Verified at 90bfbfed9: ninja build, build_all_targets every stage (1,142 steps), harnesses 20/20 + doctest 2,719 cases, board 163/168, tier1 5/5.
  The 5 red board rows are inherited from nifty and reproduce in mc2-main: background_launch (+ scripts_tests, which wraps it): tools/sim_determinism.py not in scripts/background-launch-allow.json;
  capture_provenance + selftest: exe_fingerprint.BANNER_FMT no longer matches the printf in mechcmd2.cpp (lane 1's logging batches); doc_links: docs/beta/* only exist untracked in mc2-main.
- Commits this session: 726d585cc objhash `--per-symbol` (LAYOUT-ONLY) + ledger ruling; 9eb664271 LG14 note; 23de3aeda rewrite_defines refuses constants named in a stringizing wrapper macro's arguments;
  38e5a1dd9 rewrite_enums: qualified uses reach the arithmetic scan + tools/tidy/enum_skip.txt (23 names with reasons).
- LG13: no sites converted this session. `code/unitdesg.h` (51 sites) is blocked by ONE object, controlgui.cpp.obj (LAYOUT-ONLY: static data reordered, plain objhash says changed). 30 wide headers (~660 sites) remain.
  Both need a proof-mode ruling from the orchestrator (opt-in layout-only acceptance in the rewriters' Proof, or relaxed batches with a reason). The lane does not change a proof default.
- LG14: design note written, NO enum batch run (review first). Probe result: 68 census candidates, 9 never-touch, 59 compiled at once: 23 convert (19 narrow, 2 at 172-173 includers, 2 with no tracked includer), 13 deferred (wide: MC_TextureKey + 12 GameOS),
  23 skip with reasons (tools/tidy/enum_skip.txt). After the qualifier fix the census has 65 candidates. Anonymous (101) and typedef (101) enums stay: constants / aliases.

## Tools (tested, committed)
- tools/objhash.py: `hash --per-symbol`, `compare --per-symbol`, `diff A.obj B.obj` (IDENTICAL / LAYOUT-ONLY / ORDER-ONLY / DIFF). Opt-in; `.bss` is compared by symbol set only (no size in an object).
- tools/tidy/rewrite_defines.py (LG13): default proof PLAIN objhash, `--relaxed-proof REASON` writes the reason into the commit message; NEVER_TOUCH + 50 overload TUs; `stringized-by-wrapper-macro` refusal.
- tools/tidy/rewrite_enums.py (LG14): `census`, `run`; skip ledger tools/tidy/enum_skip.txt (`--skip-enums` overrides). Per-FILE granularity: one failing enum in a file reverts the file.
- Untracked scratch (kept): .claude/l2cfg/enum_probe.py (apply | restore | report: compile probe of every candidate, mapping errors to enums), enum_note_scan.py, enum_ser.py, unitdesg_classify.py, reprove.py, patch_*.py.
  A probe needs a clean tree, ~16 min per tree (`build64-ninja` and `build_all_targets.py --root-only --keep-going`), then `restore` (git checkout of exactly the touched files).

## Never-touch (coordinator, steps 2 and 3)
mclib/estring*, elist*, heap*, stuff/*, mlr/*, gosfx/*, memorystream*, file*, packet*, fastfile*, abl*; code/multplyr*, dmultplyr.h; every *_internal.h; the 50 TUs of the audit's section D.
Ratchet policy: never contort code for a ratchet; a legitimate rise gets a baseline rewrite with the reason in the commit.

## Exact next steps for the next lane 2
1. Orchestrator rulings: (a) LG14 note review (convert list, wide/GameOS group, skip ledger); (b) LAYOUT-ONLY in the rewriter Proof for unitdesg.h (one object) and the wide-header policy.
2. After approval, LG14 batch A: `py -3 tools/tidy/rewrite_enums.py run --scope all --tree build-alltargets --max-files 20 --max-objs 400 --max-header-includers 150 --commit` (dry-run first; plain proof;
   do NOT pass `--relaxed-proof` unless ruled), then batch B with `--max-header-includers 200` (Capability, CarnageEnumType). Doctest + `build_all_targets.py` + board + `saveload_roundtrip` row per batch; tier1 per two batches.
3. Never edit tracked files while a batch runs; commit by pathspec; scripts via the Write tool (bash heredocs collapse backslashes; python and Git Bash see /tmp differently, use repo-relative paths).

## Rules learned
- Deploy to the lane install needs `--allow-new-target` (existing folder; deploy_payload only knows the canonical names): `deploy_payload.py --target game --ninja --allow-new-target A:/Games/mc2-opengl/mc2-win64-lane2`.
- The rewriters scan every tracked .py / .json / .cmake for identifiers: a test or docstring that names a real constant or enum makes it `named-outside-c++` (it happened twice this session). Use invented names in tests.
- Host load makes tier1 flake; it was 5/5 when lane 1 was only building elsewhere and the lease was free. `ninja -k 0` after a mass edit gives every compile error in one pass (the LG14 probe).
