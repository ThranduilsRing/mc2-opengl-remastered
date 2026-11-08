# perf-attrib state

Assignment: A:/Games/mc2-slot02, claude/perf-attrib, base f269b312d.

Continuation 2026-11-08 (supersedes the historical handoff below):
- Merged Linux fix 8fc0c6c89 at 5770b9bfa4c9973525f43fdc95e0d70991ebce3e.
- Box battle pair: PASS, 301 identical SIMID frames and healthy 128-mech /
  two-vehicle / attack acknowledgements. Readiness 26.05 / 25.18 seconds.
  Committed Linux pin: tests/smoke/sim_determinism/linux/battle.json, 300 ticks.
  No increased load timeout needed on Linux; no speculative timeout change made.
- Recorded mc2_24 saveload pin and text reference from a detached box checkout
  of trusted 8fc0c6c89, rebuilt and deployed there (not the lane executable).
  An independent second trusted-base run matches the pin: PASS.
- Reproduced the profile row's host-lease failure using exported f269b312d
  sources: unknown `bogus` GPU_SPLIT key. Left unrelated lease code unchanged.
- Proof root: E:/mc2-archive-2026-10/proofs/perf-attrib. See battle-diagnosis.txt,
  lease-base.log, continuation-box/ and trusted-box/. Final-tip proof results
  and proved-at SHA belong in final-state.txt, written after the final checks.
- Queue mode unavailable (pueue missing); foreground box runs use GPU 0 lock.
- Cleanup rejected by automatic approval review ("blocked by policy"); retained
  exported-base scratch G:/mc2-tmp/perf-attrib-base*, no workaround attempted.

Still unproved: Windows battle.json and the original Windows terrain-load stall.
The task requires box-only game rows; Linux chains must not populate Windows
pins (existing mc2_24 Windows/Linux chains differ). Next Windows-authorized run:
deploy this tip, rerun the recorded pair below with a fresh --out; on a stall
capture the loader stack before deciding whether fixture-only grace helps.
Only a healthy Windows pair may populate tests/smoke/sim_determinism/battle.json.

Historical handoff from the first run:

Implemented: tools/perf/tracy_table.py preserves thread/name/full source/line,
splits startup/load/first 60 playable frames/steady frames, reports exclusive
and inclusive per-frame p99/worst (including zero-call frames). Readiness is
conservatively bounded by the first active mission-interface update after
Mission::init; no rounded wall-clock timestamps are aligned to Tracy.
Pre-ready events are excluded from gameplay. GPU timestamp queries are disabled
for all CPU-profile paths. HZB has one new wait zone; no other engine change.

Green: targeted test_mission_cpu_profile.py, including attribution CSV fixture.
Receipts: E:/mc2-archive-2026-10/proofs/perf-attrib/tests-profile.log.

Ninja mc2 + asset viewer: green at 6a38c83449df (viewer required by deploy).
Deploy to E:/mc2-installs/perf-attrib: green; HZB/depth doctests: 18 cases,
344 assertions green (doctest.log). Final-tip build/tests/rows/lint receipts
will be written alongside these logs; they determine final hand-back status.

Battle: NOT PINNED. Accepted headless attempt timed out during mission load
at 264.3 s, before mission_ready, with no SIMID lines. No farm or startup retry.
baseline-run.log and smoke/2026-11-08T11-50-07-603899-402428/ hold the failure.
The a.commands.txt.ack belongs to an earlier cancelled invocation: INVALID
evidence, deliberately retained for diagnosis. Never use it to pin a baseline.
The tool now rejects stale acknowledgements and missing/unhealthy spawns.

The exact profile recipe is tests/smoke/sim_determinism/battle-128.commands.txt;
the profile test compares it with generated profile commands (except bench_output).
`sim_determinism.py run battle` maps to mc2_24 and stages private command copies
for both runs. The baseline writer env is covered offline, not a fake game pin.
Next run after the mission-load blocker is resolved: on a freshly deployed own
install, `py -3 tools/sim_determinism.py run battle --headless-both --ticks 300
--duration 20 --min-frames 300 --install <install> --out <fresh-proof-dir>
--env MC2_TEST_SPAWN=1
--env MC2_UI_COMMAND_FILE=tests/smoke/sim_determinism/battle-128.commands.txt
--env MC2_DETERMINISTIC_RNG=1 --env MC2_SMOKE_SEED=0xC0FFEE --env MC2_RNG_SEED=0xC0FFEE`.
Only after the pair and healthy spawn/attack acknowledgements pass, use
`write-baseline <fresh-proof-dir>/a/mc2_24.log --frames 300 --mission battle
--out tests/smoke/sim_determinism/battle.json --install <install>` with the same
five --env arguments. Then replace the writer-only test with a committed-pin
assertion and run the strict tests. No production battle.json exists yet.

Extra background-launch diagnostic found only a stale generated inventory;
not regenerated, per the no-generated-baseline-churn instruction.
Final proof fixes: allowlisted the requested manual tracy_table CLI (no
generated caller census edits); baseline writer handles --out on another drive.
The owning rows are nightly rows: select --nightly with --rows
'^(mission_cpu_profile|sim_determinism)$', not the unfiltered nightly board.
Scout was unresponsive and its own process tree was stopped; direct source
inspection confirms the command channel is consumed in updateFrontEnd.
