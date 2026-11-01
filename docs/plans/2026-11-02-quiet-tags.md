# Quiet diagnostic tags (2026-11-02)

Problem: `scripts/check-default-build-quiet.py --fresh-smoke` fails when a `[TAG vN]` line prints in a default build
(no MC2_* env). Counter lanes added `[CULL_PARITY v1]` and `[PERF_COUNTERS v1]` as unconditional output.

## Pattern (apply it to any new always-printing diagnostic tag)

1. Engine: print only when the tag is requested. `#include "diagnostic_trace.h"` (GameOS/gameos) and guard the print with
   `if (!mc2_diag::tagRequested("MY_TAG")) return;`. `tagRequested` reads MC2_DIAG_TAGS from the environment, needs no init and works at
   atexit/teardown. Keep the counters themselves collected as before; only the print is gated. No new feature gate, no registry entry.
   (Use `mc2_diag::tagEnabled` only for JSONL events written through the trace file.)
2. Runner: add the tag to `RUNNER_DIAG_TAGS` in `scripts/smoke_lib/runner.py`. Every runner-owned game launch (run_smoke, run_battle_bench,
   tools/perf census, mission_cpu_profile, main_busy) goes through `run_one`, which appends these tags to MC2_DIAG_TAGS (restating the engine's
   default list, since a set MC2_DIAG_TAGS replaces it) unless `RunConfig.quiet_default` or ambient `MC2_SMOKE_QUIET_DEFAULT=1` is set.
   The quiet check's fresh sample sets `MC2_SMOKE_QUIET_DEFAULT=1`, so it sees what a shipping build prints.
3. Presence rules: a `PresenceRule` for the tag gets `diag_tag="MY_TAG"` (logparse.py); gates.py skips it when the child ran without the tag
   (quiet sample), so smoke and ceilings keep grading in normal runs.
4. Baselines: do NOT add the tag to `tests/contract/default_build_quiet_baseline.json`. `tests/unit/log_tag_baseline.json` is a source census
   (the tag still exists in source), leave it alone. When the quiet check lists RETIRED tags, delete those baseline keys (the ratchet only tightens).

## Tags to gate with this pattern
- `[LIGHT_CONTRACT v1]` (lane9, claude/light-contract2): steps 1-3 with tag `LIGHT_CONTRACT`; add to `RUNNER_DIAG_TAGS` and the rule's `diag_tag`.
- `[CURSOR] os_cursor ...` (lane10, claude/cursor-double, `graphics::apply_os_cursor_policy`): prints on focus/enter/resize/window-mode changes
  in non-unattended runs, so the fresh smoke may not see it but a real default build would. Gate with tag `CURSOR` (`MC2_DIAG_TAGS=CURSOR`
  keeps it available); no presence rule exists for it, so only step 1 is needed. Note its line has no `vN`, so the quiet regex would not match it
  either way; gating is still right for a quiet shipping build.

Done in this lane: `CULL_PARITY` (vulkan_post_hzb_native.cpp `parity_log`) and `PERF_COUNTERS` (battle_bench.h `emitPerfCounters`).
Tags printed to users deliberately (BUILD_FINGERPRINT etc.) stay in the baseline with a `keep` reason.
