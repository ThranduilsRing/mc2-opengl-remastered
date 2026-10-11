# Lane 2 state (worktree A:/Games/mc2-lane2, branch claude/lane2-s3, build64-ninja, install mc2-win64-lane2)

2026-10-11, session closed by the 6-hour rule. The 1000-1500 line splits are done and merged (giant-file-split-plan.md, tools/split/*). This lane did C++23 STEP 2 (LG13) up to the
state below; lane 1 keeps step 1. Plan: .claude/cleanup/cxx23-step2-define-enum-plan.md. PROOF LEDGER (per commit, per object): .claude/cleanup/step2-never-touch-audit.md.

## State of step 2
- 14 LG13 commits (about 1,246 sites; LG13 2,892 -> about 1,790 after the revert). Seven strict-proven (b2e18ac9e 15f309252 c2674b26b f90d911f2 3a181227a 8aa5e975d 3a899cf80) keep their
  never-touch edits by ruling. Seven relaxed-proof commits (a1cd5fc80 311b8c9d8 645576c12 3a80e116f 11a3cf696 8aac2756f d9230570c).
- Reverted (cdd861f05, baseline 5246ce144): the never-touch files of the relaxed commits (artlry.cpp, EditorObjectMgr_internal.h, multplyr.h, dmultplyr.h, five abl headers).
- Strict re-proof of what the relaxed commits still hold (36 files, 479 objects): 440 identical, 36 order-only (kept, listed in the ledger), 3 UNRESOLVED (code/missiongui.cpp.obj,
  multplyr.cpp.obj, warrior.cpp.obj: .data / .bss layout reordered under any edit; blind-canonical equal). All 28 relaxed files these TUs include cause it. NEEDS A RULING: keep or revert them.
- NOT done (stopped by the rule): tier1 after the last batches (last tier1 at f7236141c: 3/5 with two heartbeat_freeze_play under host load, both missions then passed alone); the LG14 design note.

## Tools (tested, committed)
- tools/tidy/rewrite_defines.py (LG13). Default proof = PLAIN objhash; `--relaxed-proof REASON` (canonical_obj) writes the reason into the commit message. Built in: NEVER_TOUCH list + the 50
  overload-sensitive TUs (read from legacy-subsystem-audit.md), string constants refused, fast bisect, --done / --taint-file / --ignore-digest-file. Only code/unitdesg.h (52 sites) is still
  eligible; wide headers (>200 includers, e.g. gameos.hpp) need a relaxed batch because editor/MapGeneratorDialog.cpp.obj drops unreferenced stb_image tables under any edit.
- tools/tidy/rewrite_enums.py (LG14: `enum class` + `using enum`): census 174 named enums, 64 candidates, 55 after cast / sizeof / serialization flags; no batch run.
- tools/tidy/canonical_obj.py: order / layout-blind digest for explicit relaxed batches only.

## Never-touch (coordinator, steps 2 and 3)
mclib/estring*, elist*, heap*, stuff/*, mlr/*, gosfx/*, memorystream*, file*, packet*, fastfile*, abl*; code/multplyr*, dmultplyr.h; every *_internal.h; the 50 TUs of the audit's section D.
Ratchet policy: never contort code for a ratchet; a legitimate rise gets a baseline rewrite with the reason in the commit.

## Exact next steps for the fresh lane 2
1. Get the ruling on the 3 unresolved objects; if revert: `git checkout <commit>~1 -- <file>` per file listed in the ledger (section 2), one revert commit, baseline rewrite with the reason.
2. Merge nifty (handoff is now a 75-line live file + HANDOFF-LOG.md), rebuild build64-ninja, run build_all_targets.py, doctest, board, then ONE quiet tier1 (note the flake log if heartbeat_freeze_play hits).
3. LG14 design note first (which enums are used arithmetically / as array indices / in save formats: skip; the rest enum class), for the coordinator's review; then batches with plain proof.
   Anonymous enums (101) and typedef enums (101) are constants / aliases: candidates for `inline constexpr int` and `using`, not enum class. Counts: anonymous 101, typedef 101, named 174.
4. Do not start a rewrite batch without an explicit proof-mode decision; never edit tracked files while a batch runs (kill it and `git checkout` its edits first).

## Rules learned
- Commit by pathspec; the board scans COMMITTED files (a new script needs its background-launch allowlist entry once tracked). Bash heredocs collapse backslashes: write Python with escapes via
  the Write tool; `py` and Git Bash see /tmp differently, use repo-relative paths. Host load makes tier1 flake: run missions alone and quiet before concluding anything.
- Scratch tools of this session (untracked): .claude/l2cfg/reprove.py, ddmin.py, culprits.py, blindcheck.py, enum_flags.py, why_left.py (re-proof and census helpers; results in the ledger).
