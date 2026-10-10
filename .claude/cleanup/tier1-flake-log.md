# tier1 / smoke-lite heartbeat_freeze_play flake log

One line per occurrence: when, run, mission, what else was on the host, whether lane 2 was building. A pattern (time of day, load, a neighbour build) is the
point of the file; add a line each time. Host-load figures are what the runner and the process list showed, not a measurement.

| time (CST, 2026-10-10) | run | mission | failure | my own load at that moment | lane 2 building? | result of rerun |
|---|---|---|---|---|---|---|
| 08:5x (G8) | tier1 attempts during G8 verification | mc2_03, mc2_10 | heartbeat_freeze_play | other agents' builds (noted in the G8 status) | yes (builds in mc2-lane2) | quiet rerun passed |
| 15:56:51 | smoke-lite after the F4 data edit (`26f46e205`) | mc2_10 | heartbeat_freeze_play (p1% 2) | none (no build, no tidy running) | not checked | passed on the next run (15:58:09) |
| 16:34:28 | tier1 at `9234a0393` after tidy batch 10 | mc2_24 | heartbeat_freeze_play (p1% 5) | none (the 4 preceding builds had finished) | not checked | mc2_24 alone passed twice (16:38:04, 16:38:54); full tier1 passed 5/5 (16:39:44) |

Notes
- Both 2026-10-10 15:5x/16:3x occurrences followed a deploy + first launch of a freshly written exe (cold shader/texture caches after a rebuild);
  the very next run of the same exe passed in every case. Worth checking against lane 2's build activity from the lease/process log if the pattern continues.
