<!-- doc-status: current | reviewed:2026-10-11 -->
# SMOKE-FIRST-LAUNCH-CRASH-SILENT-RECON-1 — verdict

**Status:** RECON — **INCONCLUSIVE / ENVIRONMENT-SUSPECTED** (no fix). Next step is an evidence-capture
slice (SMOKE-CRASH-SILENT-EVIDENCE-1), not a fix.
**Against:** nifty `97b80c1a`.

> **CORRECTION (supersedes the earlier "decisive GPU TDR" framing):** the evidence proves the *signature* of the
> termination, NOT its unique cause. crash_silent (no summary, nonzero exit, no app crash-handler, no minidump) is an
> **external / non-app-crash-style termination** — consistent with a GPU TDR/device-lost, but ALSO with a process
> kill, loader/runtime failure, resource exhaustion, AV/security interference, a bad deploy payload, a transient
> driver reset, or a runner/environment edge. GPU TDR is plausible, NOT proven. Concurrency (the v0.4c soak) is a
> suspected contributor, not a proven root cause (clean batches have run with soaks active before). Classify as
> environment-suspected and CAPTURE EXIT EVIDENCE before changing smoke timing or engine code.

## Symptom
In full `--tier tier1` batch runs, exactly one mission intermittently reports
`crash_silent` (0 frames after a normal Load ms) **or** `heartbeat_freeze_play`
(loaded, played N frames, then stalled). The failing mission VARIES run-to-run
(observed mc2_17 and mc2_24 across this session's runs) and ALWAYS passes on an
isolated `--mission <id>` re-run with healthy frames/FPS. Seen on unmodified
baselines — independent of any code change under test.

## NOT these (excluded)
- **STARTUP-INIT-ASYNC-1** (`6307b382`, the async-init mission-select null-deref):
  its own commit notes "tier1 smoke missed it because it seeds missions directly,
  **bypassing MissionSelectionScreen**." The batch flake happens INSIDE tier1's
  direct-seed path, so it is a different failure. (That was the user-facing
  first-launch crash; now fixed.)
- **`--kill-existing` image-name taskkill race** (`run_smoke_kill_existing_causes_flaky_crash`,
  `check-smoke-matrices.py:159`): produces a false `crash_silent` for the
  in-flight mission. NOT applicable here — these runs never passed `--kill-existing`.
- **A runner detection artifact:** ruled out by the bucket definitions
  (`scripts/smoke_lib/gates.py`): `crash_silent` requires
  `smoke_summary_result is None AND exit_code != 0` — the mc2.exe process really
  exited nonzero. It is not a mislabel of a healthy run.
- **An engine-level crash:** ruled out by `crash_handler_hit` — when the engine's
  SEH crash handler fires, the bucket is `crash_no_summary`, not `crash_silent`.
  crash_silent means nonzero exit with NO crash-handler output, and NO
  minidump/crash bundle is written. That is not an app-level C++ fault.

## What the termination signature DOES narrow it to
crash_silent = `smoke_summary_result is None AND exit_code != 0` with
`crash_handler_hit == False` and no minidump. That uniquely rules OUT an ordinary
caught C++ fault (which buckets `crash_no_summary`) and a runner mislabel (the
exit really is nonzero). It does NOT identify which external/non-app cause it is.

## Candidate causes (NONE proven; ranked by current suspicion)
1. GPU driver TDR / device-lost (plausible — but no event-log/exit-code evidence
   captured yet to confirm a driver reset).
2. Concurrent-resource contention from a separate soak/game mc2.exe on the same
   GPU (the v0.4c lane was active). NOTE: weakened — clean batches HAVE run with
   soaks active, so contention is a possible contributor, not a proven cause.
3. Rapid GL-context create/destroy churn between back-to-back launches (intrinsic
   runner/game churn) — would flake even solo.
4. Process kill / loader-runtime failure / resource exhaustion / AV interference /
   bad-deploy payload / transient driver reset / runner-environment edge.

## Why not jump to a fix
A settle-delay (SMOKE-INTER-MISSION-SETTLE-1) only helps if this is GL-teardown
churn — treating a hypothesis as fact. First improve classification by capturing
the exit evidence (next slice).

## Next: capture evidence, THEN test the hypothesis
**Slice SMOKE-CRASH-SILENT-EVIDENCE-1 (runner-only, no engine change, no settle
delay)** — on a crash_silent verdict, record into the artifact dir: process exit
code; Windows event-log hints (Display driver reset / TDR; Application Error;
Windows Error Reporting); whether a crash-handler file/minidump exists; the
concurrent mc2.exe process list; GPU/driver info if cheap; elapsed time since
launch + mission phase; and the stdout/stderr + heartbeat tail. This turns the
INCONCLUSIVE verdict into a classifiable one.

Then a small hypothesis matrix (each ~1 batch):
- **A.** solo tier1 repeated 3x (no other mc2.exe).
- **B.** tier1 while the soak is running.
- **C.** tier1 while another direct-launched mc2.exe is running.
Reading: only B/C flake -> concurrency/contention credible. A flakes -> intrinsic
runner/game/deploy churn. None flake -> rare environmental.

## Recommended handling (interim)
- Treat a SINGLE tier1 batch mission that reports `crash_silent` /
  `heartbeat_freeze_play` AND passes on isolated re-run as ENVIRONMENTAL/suspected
  — not a confirmed regression. (This session applied that rule.)
- Do NOT add a settle-delay or engine change yet — capture exit evidence first.
- SMOKE-INTER-MISSION-SETTLE-1 is a CANDIDATE only if evidence later points to
  GL-teardown churn (e.g. matrix case A flakes).

Related: [[smoke-runner-block-mechanisms]], `run_smoke_kill_existing_causes_flaky_crash`.
