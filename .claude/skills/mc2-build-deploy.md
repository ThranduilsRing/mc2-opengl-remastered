---
name: mc2-build-deploy
description: Commit, build and deploy MC2 using the canonical assigned-install recipe
---

Read [CLAUDE.md](../../CLAUDE.md), Build → deploy, and follow the assignment's host and slot rules.
From the assigned worktree:

1. Commit locally so the executable embeds the source SHA.
2. Build RelWithDebInfo in the existing assigned tree:
   ```bash
   py -3 scripts/ninja_build.py
   ```
3. After a successful build, deploy to the assignment's explicit install:
   ```bash
   py -3 scripts/deploy_payload.py <assigned install> --ninja --allow-new-target
   ```

Never target the golden install or deploy after a failed build. Report both verdicts.
