# Land-as-ready (merge train)

Green tips land on nifty continuously; a full runtime gate runs about every 3 h; lanes branch from the newest
`nifty-green-<n>` tag. Spec and numbers: `A:/Games/mc2-ops/studies/throughput-adversarial.md` (J2). Tool:
`tools/land_queue.py`. There is no daemon: the orchestrator or cron runs one subcommand per call.

| Command | What it does |
|---|---|
| `py -3 tools/land_queue.py submit <branch> <sha>` | Adds the tip to the queue file (`$MC2_LAND_QUEUE`, default `A:/Games/mc2-ops/land_queue.json`). Submitting a branch again replaces its entry (use this after fixing a rejected tip). |
| `py -3 tools/land_queue.py land` | For each queued tip in order: `lane_merge.py --lint`, then `--precheck`, then the single-tip `lane_merge.py <sha>` path (merge, derived follow-ups, build + doctest verify). The first red stops that tip: it stays in the queue as `rejected` with the failing step and output tail, and the next tip is tried. Run it from the nifty worktree (`--root`). |
| `py -3 tools/land_queue.py gate [--publish]` | Runs `merge_gate.py --full` on the nifty tip. Green: tags `nifty-green-<n>` and clears suspect ranges. Red: records the commit range since the last green tag as suspect (bisect or revert it); no tag. `--publish` additionally runs `merge_gate.py --publish` after a green gate; leave it off until the montage has been viewed. |
| `py -3 tools/land_queue.py status` | Queue, green tags and suspect ranges. |

Rules:
- A tip is a candidate only with a clean `--lint` and a `--precheck` PASS on the current `nifty-green` (lane-rules).
- Gate callers must wait at least 10800 s (see `scripts/merge_gate.py`).
- A rejected tip is the lane's to fix; the queue never edits a tip.
- Test: `tools/test_land_queue.py` drives the queue against a throwaway git repo with a fake runner.
