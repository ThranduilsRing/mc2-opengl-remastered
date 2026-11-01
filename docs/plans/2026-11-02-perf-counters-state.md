# perf-counters lane state (branch claude/perf-counters)

Done: [PERF_COUNTERS v1] line (draws.*, wait_*_ms, extract.bytes, heap_allocs, walk.*, first_vk_wait_*) on frame 1, every 300 frames and at exit; extended `cpu` row in bench frames.jsonl; `bench_freeze on|off` and `bench_walk N` commands (MC2_TEST_SPAWN channel, prints [WALK_BENCH v1]); tools/perf/main_busy.py; smoke presence rule perf_counters_missing (mc2_01/03/10/17/24) and perf_counter ceiling bucket (tests/smoke/perf_counter_ceilings.json, mc2_01 and mc2_10 only, max observed x1.25 + 0.5, count-type counters only).
Gaps: FrameArena not instrumented; bench_walk skips the renderView walk (needs DrawFrameScope); wait_sealed is not split into GPU/CPU intersection; `active` = alive and moving only; mc2_03/17/24 have presence rule but no ceilings.
Proofs: contracts 140/0, harnesses + doctest pass, pytest 1859 passed, smoke-lite 2/2 PASS with ceilings, SIMID mc2_03 PASS identical and baseline match.
