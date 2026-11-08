# perf-attrib state

Assignment: A:/Games/mc2-slot02, claude/perf-attrib, base f269b312d.

Implemented: tools/perf/tracy_table.py preserves thread/name/full source/line,
splits startup/load/first 60 playable frames/steady frames, reports exclusive
and inclusive per-frame p99/worst (including zero-call frames). Readiness is
conservatively bounded by the first active mission-interface update after
Mission::init; no rounded wall-clock timestamps are aligned to Tracy.
Pre-ready events are excluded from gameplay. GPU timestamp queries are disabled
for all CPU-profile paths. HZB has one new wait zone; no other engine change.

Green: targeted test_mission_cpu_profile.py, including attribution CSV fixture.
Receipts: E:/mc2-archive-2026-10/proofs/perf-attrib/tests-profile.log.

Next: build Ninja; produce the exact 128-clone + two-vehicle battle fixture
baseline headlessly if supported; strict baseline-env regression; owning
contract rows, covering doctest, final lint; record final SHA and cleanup.
Scout was unresponsive and its own process tree was stopped; direct source
inspection confirms the command channel is consumed in updateFrontEnd.
