# scripts/smoke_lib/gates.py
"""Fault-gate evaluator. Maps LogSummary -> pass/fail + bucket list."""
from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional

from . import vk_validation
from .logparse import MARKER_BUCKETS, PRESENCE_RULES, LogSummary


PERF_CEILINGS_PATH = Path(__file__).resolve().parents[2] / "tests" / "smoke" / "perf_counter_ceilings.json"


def load_perf_ceilings(path: Path = PERF_CEILINGS_PATH) -> dict:
    """{stem: {counter: ceiling}}. W96 count ceilings are observed maxima x2
    (draws, extract, heap). Walk counters vary with run length and host load;
    walk and wait timings remain reported without ceilings. Ratchets are separate.
    A missing or unreadable file means no ceilings."""
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    return {k: v for k, v in data.items() if isinstance(v, dict)}


@dataclass
class GateConfig:
    heartbeat_timeout_load_s: int = 60
    heartbeat_timeout_play_s: int = 3
    duration_s: int = 120
    allow_asset_oob: bool = False
    # Per-mission downgrades for registered marker buckets
    # (bucket name -> True), sourced from the manifest override key
    # (allow_asset_oob analog). Red buckets present here become advisory.
    marker_overrides: dict = field(default_factory=dict)
    # The run asked for the Khronos validation layer (MC2_VULKAN_VALIDATION in the child's environment).
    validation_requested: bool = False
    # The smoke mission under test: selects the PRESENCE_RULES rows that apply.
    stem: str = ""
    # MC2_DIAG_TAGS the child ran with; None = unknown, every tag-gated presence rule applies. A rule with a diag_tag is skipped when its tag is off.
    diag_tags: Optional[frozenset] = None
    vegetation_expected: bool = True  # Low/Potato retain only stock objects.
    content_error_budget: Optional[int] = 0
    # {stem: {counter: ceiling}} for the perf_counter bucket; None loads tests/smoke/perf_counter_ceilings.json.
    perf_ceilings: Optional[dict] = None


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
        # Reclassify load-phase 0xC0000409 aborts
        # where STOP("Unable to open Mission File") was seen and no play-phase
        # heartbeat ever fired.  A mid-play 0xC0000409 (after profile_ready) stays
        # crash_silent — only the load-phase case is reclassified.
        _load_phase_abort = (
            s.mission_load_fail_seen
            and s.mission_ready_ms is None
        )
        if _load_phase_abort:
            buckets.append("mission_load_fail")
            details.append("engine STOP(Unable to open Mission File) during load "
                           "— check mod= / MC2_ACTIVE_MOD / .fit path, NOT a code crash")
        else:
            buckets.append("crash_silent")

    if s.pool_nulls > 0:
        buckets.append("pool_null"); details.append(f"{s.pool_nulls} NULLs")
    if s.asset_oob > 0 and not cfg.allow_asset_oob:
        buckets.append("asset_oob"); details.append(f"{s.asset_oob} oob")
    if s.shader_errors > 0:
        buckets.append("shader_error"); details.append(f"{s.shader_errors}")
    if cfg.content_error_budget is not None and s.content_errors > cfg.content_error_budget:
        buckets.append("content_error"); details.append(f"{s.content_errors} content errors")
    if not s.content_summaries:
        buckets.append("content_summary_missing")
    if s.missing_files > 0:
        buckets.append("missing_file"); details.append(f"{s.missing_files}")

    # Registered marker buckets. Red severity -> verdict
    # bucket (fails the mission) unless downgraded per mission via the
    # manifest override key; advisory severity -> detail line only.
    for mb in MARKER_BUCKETS:
        n = s.marker_counts.get(mb.bucket, 0)
        if n <= 0:
            continue
        overridden = bool(mb.manifest_override_key and
                          cfg.marker_overrides.get(mb.bucket, False))
        if mb.severity == "red" and not overridden:
            buckets.append(mb.bucket)
            first = s.marker_first.get(mb.bucket)
            details.append(f"{n} {mb.bucket} marker line(s)" + (f"; first: {first}" if first else ""))
        else:
            details.append(f"[advisory] {n} {mb.bucket} marker line(s)")

    # Regression floors: a line the mission must print (logparse.PRESENCE_RULES). Only a run that reached play can be judged; a load failure has its own bucket.
    for rule in PRESENCE_RULES:
        if rule.bucket.startswith("vegetation_") and not cfg.vegetation_expected:
            continue
        if rule.diag_tag and cfg.diag_tags is not None and "*" not in cfg.diag_tags and rule.diag_tag not in cfg.diag_tags:
            continue
        if rule.stem == cfg.stem and s.mission_ready_ms is not None and s.presence_counts.get(rule.bucket, 0) == 0:
            buckets.append(rule.bucket)
            details.append(rule.why)

    # perf_counter bucket: a run-mean counter above its ceiling (tests/smoke/perf_counter_ceilings.json). Only judged when the line printed (its
    # absence is the perf_counters_missing presence rule above).
    ceilings = (cfg.perf_ceilings if cfg.perf_ceilings is not None else load_perf_ceilings()).get(cfg.stem, {})
    if ceilings and s.perf_counters:
        over = [f"{name}={s.perf_counters[name]:.4g}>{limit:.4g}" for name, limit in sorted(ceilings.items())
                if name in s.perf_counters and s.perf_counters[name] > limit]
        if over:
            buckets.append("perf_counter")
            details.append("perf counters over ceiling: " + ", ".join(over))

    # A requested validation layer that never came up leaves nothing to grade: red, not clean. Errors the layer did report are already the
    # `vk_validation_error` marker bucket above.
    if cfg.validation_requested and not vk_validation.layer_enabled(s.vk_validation):
        buckets.append("vk_validation_unconfirmed")
        details.append(vk_validation.UNCONFIRMED)

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
        # A clean mission stops emitting play heartbeats during renderer teardown.
        # Keep timeout/crash checks and any gap BEFORE mission_quit fully enforced.
        play_end = walltime_s
        if exit_code == 0 and s.smoke_summary_result == "pass" and s.mission_quit_wall_s is not None:
            play_end = min(play_end, s.mission_quit_wall_s)
        if last is None:
            # No play-phase heartbeat at all -- definite freeze.
            buckets.append("heartbeat_freeze_play")
            details.append("no play-phase heartbeat observed")
        elif play_end - last > cfg.heartbeat_timeout_play_s:
            buckets.append("heartbeat_freeze_play")
            details.append(f"last play heartbeat at {last:.1f}s wallclock")

    passed = not buckets and s.smoke_summary_result == "pass" and exit_code == 0
    return Verdict(passed=passed, buckets=buckets, details=details)
