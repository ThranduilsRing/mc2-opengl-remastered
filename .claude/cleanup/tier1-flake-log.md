# tier1 / smoke-lite heartbeat_freeze_play flake log

One line per occurrence: when, run, mission, what else was on the host, whether lane 2 was building. A pattern (time of day, load, a neighbour build) is the
point of the file; add a line each time. Host-load figures are what the runner and the process list showed, not a measurement.

| time (CST, 2026-10-10) | run | mission | failure | my own load at that moment | lane 2 building? | result of rerun |
|---|---|---|---|---|---|---|
| 08:5x (G8) | tier1 attempts during G8 verification | mc2_03, mc2_10 | heartbeat_freeze_play | other agents' builds (noted in the G8 status) | yes (builds in mc2-lane2) | quiet rerun passed |
| 15:56:51 | smoke-lite after the F4 data edit (`26f46e205`) | mc2_10 | heartbeat_freeze_play (p1% 2) | none (no build, no tidy running) | not checked | passed on the next run (15:58:09) |
| 16:34:28 | tier1 at `9234a0393` after tidy batch 10 | mc2_24 | heartbeat_freeze_play (p1% 5) | none (the 4 preceding builds had finished) | not checked | mc2_24 alone passed twice (16:38:04, 16:38:54); full tier1 passed 5/5 (16:39:44) |
| 18:3x | smoke-lite at `d8b7bae32` after header batch 2 (first launch of the redeployed exe) | mc2_10 | heartbeat_freeze_play (p1% 15) | host CPU at 100 % when checked right after; no build of mine running | not checked (lane 2 not queried) | see next run |

| 2026-10-12 08:43 / 08:56 | lane 1 tier1 at d68203bf9 (first proof of b6c154f59), then mc2_01 alone | mc2_01 | timeout (load 167 s, 0 frames), then heartbeat_freeze_play (p1% 2, avg 37 fps; the log ends in a normal shutdown) | my doctest + mc2 builds during the first run; nothing of mine during the second | three other lanes resumed this morning: host CPU 90 %, 32 python.exe, another lane's mc2.exe right after | mc2_03/10/17/24 passed in the same run (mc2_24 with no [ASSET_LOAD v1] line); mc2_01 rerun at the checkpoint tier1 |
| 2026-10-12 10:56 | lane 1 checkpoint tier1 at 1c4f5f1d1 | mc2_24 | timeout (load 71.9 s, 0 frames; walltime cap 181 s) | none of mine (build_all_targets + board had finished) | other lanes: cl/msbuild and up to three mc2.exe at once seen all morning | mc2_24 alone PASS 8094 frames 68 fps (11:08) |
| 2026-10-12 11:13 | lane 1 final tier1 at 1daa78a43 | mc2_01, mc2_10 | heartbeat_freeze_play at the END of both runs (last play heartbeat 127.8 s / 127.4 s wallclock after the 120 s duration; mc2_10 had reached the 8192-frame cap; mc2_03 in the same run loaded in 57 s at 16 fps) | none of mine | yes (other lanes) | mc2_01 + mc2_10 alone, see next row |
Notes
- Both 2026-10-10 15:5x/16:3x occurrences followed a deploy + first launch of a freshly written exe (cold shader/texture caches after a rebuild);
  the very next run of the same exe passed in every case. Worth checking against lane 2's build activity from the lease/process log if the pattern continues.
