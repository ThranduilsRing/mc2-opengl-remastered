# Fast precheck (lane claude/fast-precheck)

Problem: `lane_merge.py --precheck <tip>` cost 10-20 min (W97: 527/717/1021/1045 s) because it cold-built mc2, mc2fx and
mc2fx_preview (about 1000 compile steps) in a scratch clone created inside the main worktree. The same builds run again in
`--batch --verify full` and the gate.

## What each stage proves

| Check | precheck (default) | precheck --with-link | batch --verify full | gate |
|---|---|---|---|---|
| virtual nifty merge conflicts | yes | yes | real merge | - |
| 5 ratchets (no tighten) | yes | yes | yes (tighten) | - |
| callers refresh + `--check` | yes | yes | yes | - |
| scripts_tests / tools_tests | yes | yes | contracts board | - |
| mc2 / mc2fx / mc2fx_preview link | no | yes (incremental, warm tree) | yes (full build) | yes |
| doctest tier, contracts board | no | no | yes | yes |
| smoke / captures | no | no | no | yes |

Nothing is dropped: a link break is still caught at batch verify or the gate, and `--with-link` gives it early on demand.

## Design

- Default path: virtual merge, then four independent chains run concurrently, each in its own `git clone --shared`
  under G:/mc2-tmp (never inside the main worktree): ratchets, callers refresh+check, scripts_tests, tools_tests.
  A per-stage seconds table and WALL time print at the end.
- `--with-link`: one persistent git worktree at `$MC2_PRECHECK_TREE` (default `E:/mc2-installs/_precheck/tree`, Defender-excluded, off A:),
  created once (`git worktree add --detach`, `ensure_deps` donor copy), then `checkout -f --detach <virtual commit>` and an
  incremental `configure_ninja` + `ninja_build` of the three targets. Exclusive OS file lock `<tree>.lock` (released on process death)
  serialises users. The first run is a one-time cold build.
- Guards: tree and scratch inside `--root` raise Stop(5).

Tests: `tools/test_lane_merge.py` (-k precheck / tree_lock).
