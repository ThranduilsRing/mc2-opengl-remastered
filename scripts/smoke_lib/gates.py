# scripts/smoke_lib/gates.py
"""Fault-gate evaluator. Maps LogSummary -> pass/fail + bucket list."""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import List, Optional

from .logparse import LogSummary


@dataclass
class GateConfig:
    heartbeat_timeout_load_s: int = 60
    heartbeat_timeout_play_s: int = 3
    duration_s: int = 120


@dataclass
class Verdict:
    passed: bool
    buckets: List[str] = field(default_factory=list)
    details: List[str] = field(default_factory=list)


def evaluate(s: LogSummary, cfg: GateConfig, *,
             exit_code: int, walltime_s: float,
             killed_by_timeout: bool = False,
             final_elapsed_ms: Optional[int] = None) -> Verdict:
    buckets: List[str] = []
    details: List[str] = []

    if killed_by_timeout:
        buckets.append("timeout")
        details.append(f"walltime cap hit at {walltime_s:.1f}s")
        return Verdict(passed=False, buckets=buckets, details=details)

    if not s.instr_banner_seen:
        buckets.append("instrumentation_missing")
        details.append("[INSTR v1] banner absent")

    if s.crash_handler_hit and s.smoke_summary_result is None:
        buckets.append("crash_no_summary")
    elif s.smoke_summary_result == "fail":
        buckets.append("engine_reported_fail")
        details.append(f"reason={s.smoke_summary_reason} stage={s.smoke_summary_stage}")
    elif s.smoke_summary_result is None and exit_code != 0:
        buckets.append("crash_silent")

    if s.gl_errors > 0:
        buckets.append("gl_error"); details.append(f"{s.gl_errors} errors")
    if s.pool_nulls > 0:
        buckets.append("pool_null"); details.append(f"{s.pool_nulls} NULLs")
    if s.asset_oob > 0:
        buckets.append("asset_oob"); details.append(f"{s.asset_oob} oob")
    if s.shader_errors > 0:
        buckets.append("shader_error"); details.append(f"{s.shader_errors}")
    if s.missing_files > 0:
        buckets.append("missing_file"); details.append(f"{s.missing_files}")

    # Heartbeat freeze gates -- wallclock-based (runner-side). Engine-side
    # [HEARTBEAT] elapsed_ms is mission-relative and cannot be compared to
    # runner walltime during load pauses; see logparse.py docstring.
    if s.mission_ready_ms is None and s.heartbeats_load > 0:
        last = s.last_heartbeat_wall_s_load or 0.0
        if walltime_s - last > cfg.heartbeat_timeout_load_s:
            buckets.append("heartbeat_freeze_load")
            details.append(f"last load heartbeat at {last:.1f}s wallclock")
    elif s.mission_ready_ms is not None:
        last = s.last_heartbeat_wall_s_play
        if last is None:
            # No play-phase heartbeat at all -- definite freeze.
            buckets.append("heartbeat_freeze_play")
            details.append("no play-phase heartbeat observed")
        elif walltime_s - last > cfg.heartbeat_timeout_play_s:
            buckets.append("heartbeat_freeze_play")
            details.append(f"last play heartbeat at {last:.1f}s wallclock")

    passed = not buckets and s.smoke_summary_result == "pass" and exit_code == 0
    return Verdict(passed=passed, buckets=buckets, details=details)
