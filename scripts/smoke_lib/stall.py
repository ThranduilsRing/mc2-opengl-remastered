"""[SMOKE_STALL]: one diagnostic line when a smoke run ends with zero frames or hits the wall-clock cap.

The stage comes from the engine's existing `[TIMING]` lines (parsed into LogSummary), the holders from the host lease records.
Pure functions: run_smoke prints the line, tests feed fake summaries.
"""
from __future__ import annotations

import os
from typing import Iterable, Optional

STAGES = ("launch", "lease_wait", "load", "pipeline", "first_frame")


def is_stall(summary, killed_by_timeout: bool) -> bool:
    """True when the run timed out or never rendered a frame."""
    return bool(killed_by_timeout) or summary.first_frame_ms is None


def last_stage(summary, lease_waited_never_launched: bool = False) -> str:
    """The last engine stage the log reached.

    lease_wait: the host lease was never granted (no game was started).
    launch:     the game started but never began loading the mission (no mission_load_start line).
    load:       mission_load_start seen, mission_ready not.
    pipeline:   mission_ready seen (pipelines/shaders warming), first_frame not.
    first_frame: first_frame printed but no frame was counted (no heartbeat/perf sample).
    """
    if lease_waited_never_launched:
        return "lease_wait"
    if summary.first_frame_ms is not None:
        return "first_frame"
    if summary.mission_ready_ms is not None:
        return "pipeline"
    if summary.mission_load_start_ms is not None:
        return "load"
    return "launch"


def holders_text(records: Iterable[Optional[dict]], own_pid: Optional[int] = None) -> str:
    """`pid/label,pid/label` of the lease holders other than this process; `none` when empty."""
    own = os.getpid() if own_pid is None else own_pid
    out = []
    for rec in records:
        if not rec or rec.get("pid") == own:
            continue
        tag = str(rec.get("batch") or rec.get("label") or "?")
        tag = (tag[len("batch "):] if tag.startswith("batch ") else tag).replace(" ", "_")
        out.append(f"{rec.get('pid')}/{tag}")
    return ",".join(out) or "none"


def lease_holders() -> str:
    """Holders of the host lease now, best effort (a failing lease read never changes the verdict)."""
    try:
        from . import host_lease
        return holders_text(r for _, r in host_lease._records())
    except Exception as exc:  # pragma: no cover - defensive
        return f"unreadable({type(exc).__name__})"


def line(map_name: str, stage: str, secs: float, holders: str) -> str:
    return f"[SMOKE_STALL] map={map_name} last_stage={stage} secs={secs:.0f} lease_holders={holders}"
