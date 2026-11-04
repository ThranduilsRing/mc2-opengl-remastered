# Land-as-ready (merge train)

Green tips land on nifty continuously; a full runtime gate runs about every 3 h; lanes branch from the newest
`nifty-green-<n>` tag. Spec and numbers: `A:/Games/mc2-ops/studies/throughput-adversarial.md` (J2). Tool:
`tools/land_queue.py`. There is no daemon: the orchestrator or cron runs one subcommand per call.

| Command | What it does |
|---|---|
| `py -3 tools/land_queue.py submit <branch> <sha>` | Adds the tip to the queue file (`$MC2_LAND_QUEUE`, default `A:/Games/mc2-ops/land_queue.json`). Submitting a branch again replaces its entry (use this after fixing a rejected tip). |
| `py -3 tools/land_queue.py land` | For each queued tip in order: `lane_merge.py --lint`, then `--precheck`, then `lane_merge.py --batch <sha> --verify full` (merge, derived follow-ups, ratchets, build, doctest). The pre-merge head is recorded; on any red nifty is reset to it. A tip red is `rejected` with the failing step and output tail and the next tip is tried; an infra failure (rc 3/4, tree busy, REFUSED, PARTIAL) re-queues the tip and stops with exit 75. nifty must be clean (branch, no tracked changes, no merge) before each tip. Run it from the nifty worktree (`--root`). |
| `py -3 tools/land_queue.py gate [--publish]` | Runs `merge_gate.py --preflight` then `--full` on the nifty tip with window W = max(`proofs/merge<N>*`)+1, consumed only once the gate really starts (a preflight failure is infra and uses no W; `--resume` at the same head keeps its W). Green: annotated tag `nifty-green-<W>`, suspect ranges cleared. Runtime red (parsed `failed stages`): the red stage names and the first-parent range since the last green tag are recorded as suspect. REFUSED, usage errors, PARTIAL and unparsed reds are infra and mark no range. `--publish` runs `merge_gate.py --publish --expect-head <tag commit>` and is refused when HEAD is not the tagged commit; leave it off until the montage has been viewed. `publish <W>` publishes an earlier tag the same way. |
| `py -3 tools/land_queue.py status` | Queue, green tags and suspect ranges. |

Rules:
- `submit`, `land` and `gate` share one lock (`<queue>.lock`); queue writes are atomic with a `.bak`. A busy lock exits 75.
- `lane_merge --batch` refuses a tip without a PASS receipt from `--precheck` on that exact SHA (`PRECHECK MISSING <sha>`; receipts in `$MC2_PRECHECK_RECEIPTS`, default `A:/Games/mc2-ops/precheck`; owner override `--no-receipt-check`).
- The merge gate reuses a passed runtime stage across a HEAD change when `git diff old..new` touches only docs/tooling paths, the exe sha and the stage signature are unchanged (unknown paths count as engine; offline stages always rerun).
- Known-failure expiry stays window-based; renew with first_window = the current W, expires = W+3.
- A tip is a candidate only with a clean `--lint` and a `--precheck` PASS on the current `nifty-green` (lane-rules).
- Gate callers must wait at least 10800 s (see `scripts/merge_gate.py`).
- A rejected tip is the lane's to fix; the queue never edits a tip.
- Test: `tools/test_land_queue.py` drives the queue against a throwaway git repo with a fake runner.
