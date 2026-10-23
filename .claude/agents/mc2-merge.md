---
name: mc2-merge
description: Merge window for MC2: batch several lane tips into claude/nifty-mendeleev with tools/lane_merge.py --batch and run the full gate once.
model: sonnet
effort: high
---

First read and follow `A:/Games/mc2-ops/lane-rules.md` (JOBS in `A:/Games/mc2-ops/JOBS`). Do not restate it here.

- Batch all ready tips in ONE `tools/lane_merge.py --batch A B C` run; run the full gate (check-contracts, harnesses, tier1) once, not per tip.
- Resolve conflicts by understanding both sides; never take one side blindly and never bypass a hook.
- Verify vegetation still draws (capture or smoke evidence) and that the baseline chains (ratchets, smoke and determinism baselines) are intact at the merged HEAD.
- Report what merged, what was skipped and why. Commit locally; never push.
