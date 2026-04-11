---
name: mc2-check
description: Check if deployed exe and shaders are up-to-date with source — flags what needs deploying
---

# MC2 Check

Verify whether the deployed runtime files match the current worktree source. Does NOT copy anything — just reports what's stale.

## Paths
- **Source worktree**: Auto-detect from CWD (or `A:/Games/mc2-opengl-src`)
- **Deploy target**: `A:/Games/mc2-opengl/mc2-win64-v0.1.1`

## Steps

1. **Detect worktree**: Find worktree root from CWD.

2. **Check exe**:
   - Compare timestamps: `<worktree>/build64/RelWithDebInfo/mc2.exe` vs `A:/Games/mc2-opengl/mc2-win64-v0.1.1/mc2.exe`
   - Report if source is newer (needs deploy) or if they match

3. **Check ALL shaders**: For every `*.vert`, `*.frag`, `*.tesc`, `*.tese` in `<worktree>/shaders/` and `<worktree>/shaders/include/`:
   - `diff -q` against deployed version
   - Report files that differ (need deploying)
   - Report files in source but missing from deploy (new, need copying)
   - Report files in deploy but missing from source (stale, potentially from another branch)

4. **Summary**: Print a clear summary:
   - "All up to date" if everything matches
   - List of files that need deploying if any are stale
   - **Warn loudly about stale deploy-only files** — these cause the worst bugs (wrong shaders from old branches)

## When to Use
- Before testing: "are my changes deployed?"
- After switching worktrees: "did the shaders come with me?"
- When something looks wrong: "are the deployed shaders from this branch?"
