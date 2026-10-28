---
name: mc2-merge
description: Merge window for MC2: batch several lane tips into claude/nifty-mendeleev with tools/lane_merge.py --batch and run the full gate once.
model: sonnet
effort: high
---

First read and follow `A:/Games/mc2-ops/lane-rules.md` (JOBS in `A:/Games/mc2-ops/JOBS`). Do not restate it here.

- Batch all ready tips in ONE `tools/lane_merge.py --batch A B C` run; run the full gate (check-contracts, harnesses, tier1) once, not per tip.
- Before the gate, at the merged HEAD, run `py -3 tools/lane_merge.py --pre-gate` (regenerate-all dry run, check-background-launch, gate_gen --check, cxx17 ratchet, a no-game headless run_smoke row). Every step runs; the last line is `PRE-GATE PASS` or `PRE-GATE FAIL: <steps>`. Fix what it names and commit it before starting the full gate, so one gate run is enough.
- Long builds and gates: launch `py -3 scripts/gate_wait.py --timeout SEC -- <cmd>` with run_in_background (never wait inside one Bash call). It prints a single `PASS|FAIL rc=N secs=N log=PATH` line when done; read the log only on FAIL.
- Resolve conflicts by understanding both sides; never take one side blindly and never bypass a hook.
- Verify vegetation still draws (capture or smoke evidence) and that the baseline chains (ratchets, smoke and determinism baselines) are intact at the merged HEAD.
- Report what merged, what was skipped and why, then the Time and token budget table (lane-rules section 7). Commit locally; never push.
