---
name: mc2-build
description: Build the assigned MC2 worktree using the canonical recipe
---

Read [CLAUDE.md](../../CLAUDE.md), Build → deploy, and follow the assignment's host and slot rules.
Commit locally first so the executable embeds the source SHA. Reuse the assigned build tree, RelWithDebInfo:

```bash
py -3 scripts/ninja_build.py
```

Run from the assigned worktree. Report the build verdict; on failure, stop before deployment.
