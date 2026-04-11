---
name: mc2-build-deploy
description: Build mc2.exe then deploy exe + shaders to runtime directory with verification — the full cycle
---

# MC2 Build + Deploy

The complete build-deploy-verify cycle in one command.

## Steps

1. **Invoke `/mc2-build`** — build mc2.exe in the current worktree
2. **If build succeeds, invoke `/mc2-deploy`** — deploy exe + all shaders with verification
3. **If build fails, stop** — don't deploy a stale exe

This is the most common workflow. Use this instead of running build and deploy separately.
