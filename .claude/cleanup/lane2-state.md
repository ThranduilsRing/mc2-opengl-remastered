# Lane 2 state (worktree A:/Games/mc2-lane2, branch claude/lane2-s3, build64-ninja, install mc2-win64-lane2)

2026-10-11. The 1000-1500 line splits are DONE and merged (sizes, shas and recipe: giant-file-split-plan.md, tools/split/*, post_split.py). This lane is now on
C++23 STEP 2 (lane 1 keeps step 1). Plan: .claude/cleanup/cxx23-step2-define-enum-plan.md. Audit of what step 2 touched: .claude/cleanup/step2-never-touch-audit.md.

## Tools (all tested, committed)
- tools/tidy/rewrite_defines.py: LG13 `#define NAME literal` -> `inline constexpr` (`.cpp`-local: `constexpr`); census / run / --dry-run; proof = rebuild every including object
  (run_tidy_headers.Proof); fast bisect (probes rebuild only the objects the first probe found bad, then a full verification); --done / --taint-file ledgers.
  DEFAULT PROOF IS NOW PLAIN objhash. `--relaxed-proof REASON` switches to tools/tidy/canonical_obj.py and writes the reason into the commit message.
  String constants are refused (a constexpr pointer changes codegen). NEVER-TOUCH list built in (below) + the 50 overload-sensitive TUs read from the audit doc.
- tools/tidy/rewrite_enums.py: LG14 candidates (`enum class` + `using enum`); census found 174 named enums, 64 candidates; NO batch has been run (design note first).
- tools/tidy/canonical_obj.py: order / layout-blind digest (synthetic-COFF tests). Only for explicit relaxed batches.

## Batches done (LG13, all committed; ratchet baselines tightened after each) - proof mode in brackets
- cpp-local: b2e18ac9e 108, 15f309252 170, c2674b26b 49 (plain). Headers: f90d911f2 29, 3a181227a 21, 8aa5e975d 166, 3a899cf80 32 (plain).
- 311b8c9d8 94, 645576c12 17, a1cd5fc80 1, 3a80e116f 1 (canonical, order-blind).  11a3cf696 442, 8aac2756f 92, d9230570c 24 (canonical + blind .data/.bss).
- Totals: plain 575 sites, canonical 113, canonical+blind-data 558. LG13 2,892 -> about 1,680. Tier1 5/5 at 69a0d5450-era runs; at f7236141c 3/5 (two heartbeat_freeze_play under
  host load) then both missions passed alone (84 / 134 fps).
- 54 file edits in 14 commits hit the never-touch list (listed in step2-never-touch-audit.md); awaiting the coordinator's revert-or-keep decision. Nothing further is committed.

## Never-touch (coordinator 2026-10-11, applies to steps 2 and 3)
mclib/estring*, elist*, heap*, stuff/*, mlr/*, gosfx/*, memorystream*, file*, packet*, fastfile*, abl*; code/multplyr*, dmultplyr.h; every *_internal.h; and the 50 TUs of
legacy-subsystem-audit.md section D (the PCH sqrt list). Enforced in rewrite_defines.py / rewrite_enums.py (`never_touch`), tested.

## Ratchet policy (coordinator): never contort code for a ratchet; a legitimate rise gets a baseline rewrite with the reason in the commit.

## Next steps
1. Wait for the revert-or-keep ruling on the 54 never-touch file edits (a revert is `git checkout <pre-step-2 sha> -- <files>` plus a baseline tighten).
2. Optional strict re-proof of the relaxed commits (rebuild their includers at the parent state with plain objhash and list the objects that differ): the answer decides which of the
   113 + 558 relaxed sites stay.
3. Remaining eligible LG13 after the new exclusions: only code/unitdesg.h (52 sites, one reverted attempt) - the rest were never-touch, overload-sensitive, >200-includer headers
   that need a relaxed batch (MapGeneratorDialog needs --ignore-digest-file), or refused (named in scripts, string constants, C-side).
4. LG14: write the enum design note (which enums are used arithmetically), then batches with the same proof mode rules.

## Rules learned
- Commit by pathspec; the board scans COMMITTED files (a new tool script needs its background-launch allowlist entry only after it is tracked).
- Bash heredocs collapse backslashes (write Python with escapes via the Write tool); `py` and Git Bash see /tmp differently - use repo-relative paths.
- Never edit tracked files while a rewrite batch is running (the proof builds from the working tree); kill the batch and `git checkout` its edited files first.
- Host load makes tier1 flake (heartbeat_freeze_play): run missions alone and quiet before concluding anything.
