---
name: mc2-deploy
description: Deploy MC2 to the assignment's explicit install with verification
---

Read [CLAUDE.md](../../CLAUDE.md), Build → deploy, and follow the assignment's host and slot rules.
Commit locally and build successfully first. From the assigned worktree, use the assigned install as a positional argument:

```bash
py -3 scripts/deploy_payload.py <assigned install> --ninja --allow-new-target
```

Never target the golden install or infer a target from an old recipe. Report the deploy verdict.
