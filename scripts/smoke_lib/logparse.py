# scripts/smoke_lib/logparse.py
"""Parse mc2.exe smoke-mode stdout+stderr into a structured summary."""
from __future__ import annotations

import re
from dataclasses import dataclass, field
from typing import Optional

PERF_RE = re.compile(
    r"\[PERF v1\] avg_fps=(?P<avg>[\d.]+) p50_ms=(?P<p50>[\d.]+) "
    r"p99_ms=(?P<p99>[\d.]+) p1low_fps=(?P<p1l>[\d.]+) peak_ms=(?P<peak>[\d.]+) "
    r"samples=(?P<samples>\d+)"
)
TIMING_RE = re.compile(r"\[TIMING v1\] event=(?P<ev>\w+) elapsed_ms=(?P<ms>[\d.]+)")
SUMMARY_RE = re.compile(
    r"\[SMOKE v1\] event=summary result=(?P<r>\w+)(?: reason=(?P<reason>\S+))?"
    r"(?: stage=(?P<stage>\S+))?"
)
HEARTBEAT_RE = re.compile(r"\[HEARTBEAT\] frames=(?P<f>\d+) elapsed_ms=(?P<ms>\d+)")

# Known crash / fail signatures.
CRASH_PATTERNS = (
    re.compile(r"^CRASH:", re.M),
    re.compile(r"^\[CRASHBUNDLE\]", re.M),
    re.compile(r"unhandled exception", re.I),
)
SHADER_ERROR_PATTERNS = (
    # Matches shader_builder.cpp:358 and friends.
    re.compile(r"Shader filename:.*failed to load shader", re.I),
    re.compile(r"shader.*compile.*fail", re.I),
    re.compile(r"GLSL link.*failed", re.I),
)
MISSING_FILE_PATTERNS = (
    re.compile(r"cannot open.*required", re.I),
    re.compile(r"Missing file:", re.I),
)

# SMOKE-MODMISSION-ENV-GUARDS-1: detect engine STOP("Unable to open Mission File")
# abort during load phase.  mission.cpp:2072 calls this when .fit resolution fails.
MISSION_LOAD_FAIL_PATTERNS = (
    re.compile(r"Unable to open Mission File", re.I),
    re.compile(r"STOP\(.*Mission File", re.I),
)


# ---------------------------------------------------------------------------
# META-FIX INFRA-3: table-driven marker buckets.
#
# Generalizes the VEG-SMOKE-FLOOR / asset_oob clone-per-feature pattern ONCE:
# each recurrence-proofing design registers ONE ROW here instead of re-cloning
# a bespoke counter + gate + manifest key. A row is scanned per stdout line;
# counts land in LogSummary.marker_counts[bucket]; gates.py turns red-severity
# counts into verdict buckets (per-mission downgrade via the manifest override
# key, mirroring allow_asset_oob); advisory-severity counts become details only.
#
# Later batches append one row each (design A: [MOVER_COHERENCE] VIOLATION;
# design B: asset_load_fail; design F: ORDER_LIVENESS via the summary-parser
# path) — see .claude/META-FIX-PLAN-2026-07-16.md INFRA-3.
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class MarkerBucket:
    pattern: "re.Pattern[str]"          # searched against each stdout line
    bucket: str                          # bucket name (red) / detail tag (advisory)
    severity: str                        # "red" | "advisory"
    manifest_override_key: Optional[str] = None  # manifest key that downgrades
                                                 # red -> advisory per mission

MARKER_BUCKETS: tuple = (
)


@dataclass
class PerfRow:
    avg_fps: float = 0.0
    p50_ms: float = 0.0
    p99_ms: float = 0.0
    p1low_fps: float = 0.0
    peak_ms: float = 0.0
    samples: int = 0


@dataclass
class LogSummary:
    instr_banner_seen: bool = False
    smoke_banner_seen: bool = False
    mission_resolve_seen: bool = False
    profile_ready_ms: Optional[float] = None
    logistics_ready_ms: Optional[float] = None
    mission_load_start_ms: Optional[float] = None
    mission_ready_ms: Optional[float] = None
    first_frame_ms: Optional[float] = None
    mission_quit_ms: Optional[float] = None
    heartbeats_load: int = 0
    heartbeats_play: int = 0
    # Runner-side wallclock (monotonic seconds since spawn) at which the most
    # recent heartbeat line was *received*. None if no heartbeat yet. These
    # fields are authoritative for freeze detection — do NOT use the engine's
    # [HEARTBEAT] elapsed_ms, which is mission-relative and drifts from
    # runner walltime during load pauses.
    last_heartbeat_wall_s_load: Optional[float] = None
    last_heartbeat_wall_s_play: Optional[float] = None
    gl_errors: int = 0
    pool_nulls: int = 0
    asset_oob: int = 0
    shader_errors: int = 0
    missing_files: int = 0
    crash_handler_hit: bool = False
    smoke_summary_result: Optional[str] = None
    smoke_summary_reason: Optional[str] = None
    smoke_summary_stage: Optional[str] = None
    destroys: int = 0
    perf: PerfRow = field(default_factory=PerfRow)
    # SMOKE-MODMISSION-ENV-GUARDS-1: set True if the engine printed an
    # "Unable to open Mission File" / STOP line before play phase started.
    mission_load_fail_seen: bool = False
    # META-FIX INFRA-3: per-bucket marker counts (bucket -> hit count) for
    # every registered MARKER_BUCKETS row. Empty dict when no marker fired.
    marker_counts: dict = field(default_factory=dict)


def parse_log(text: str,
              line_wallclocks: Optional[list[float]] = None) -> LogSummary:
    """Parse engine stdout.

    If `line_wallclocks` is provided, it must be the same length as text's
    line count and contain monotonic-clock seconds (since spawn) for each line.
    Used for freeze-detection gates. When None (e.g. fixture-based unit tests),
    wallclock fields remain unset and the gate evaluator can fall back to the
    summary-level walltime parameter.
    """
    s = LogSummary()
    in_play_phase = False

    lines = text.splitlines()
    for idx, line in enumerate(lines):
        line_wall = line_wallclocks[idx] if line_wallclocks and idx < len(line_wallclocks) else None
        if "[INSTR v1] enabled:" in line:
            s.instr_banner_seen = True
        if "[SMOKE v1] event=banner" in line:
            s.smoke_banner_seen = True
        if "[SMOKE v1] event=mission_resolve" in line:
            s.mission_resolve_seen = True

        m = TIMING_RE.search(line)
        if m:
            ev = m.group("ev"); ms = float(m.group("ms"))
            if ev == "profile_ready":      s.profile_ready_ms = ms
            elif ev == "logistics_ready":    s.logistics_ready_ms = ms
            elif ev == "mission_load_start": s.mission_load_start_ms = ms
            elif ev == "first_frame":       s.first_frame_ms = ms
            elif ev == "mission_ready":
                s.mission_ready_ms = ms
                in_play_phase = True
            elif ev == "mission_quit":     s.mission_quit_ms = ms

        m = HEARTBEAT_RE.search(line)
        if m:
            # Runner-side wallclock is authoritative for freeze detection.
            if in_play_phase:
                s.heartbeats_play += 1
                if line_wall is not None:
                    s.last_heartbeat_wall_s_play = line_wall
            else:
                s.heartbeats_load += 1
                if line_wall is not None:
                    s.last_heartbeat_wall_s_load = line_wall

        if "[GL_ERROR v1]" in line:        s.gl_errors += 1
        if "[TGL_POOL v1]" in line and "nulls=" in line: s.pool_nulls += 1
        if "[ASSET_SCALE v1] event=oob_blit" in line: s.asset_oob += 1
        if "[DESTROY v1]" in line and "event=destroy" in line: s.destroys += 1

        # META-FIX INFRA-3: registered marker-bucket scan.
        for mb in MARKER_BUCKETS:
            if mb.pattern.search(line):
                s.marker_counts[mb.bucket] = s.marker_counts.get(mb.bucket, 0) + 1

        for p in CRASH_PATTERNS:
            if p.search(line): s.crash_handler_hit = True; break
        for p in SHADER_ERROR_PATTERNS:
            if p.search(line): s.shader_errors += 1; break
        for p in MISSING_FILE_PATTERNS:
            if p.search(line): s.missing_files += 1; break

        if not in_play_phase:
            for p in MISSION_LOAD_FAIL_PATTERNS:
                if p.search(line):
                    s.mission_load_fail_seen = True
                    break

        m = SUMMARY_RE.search(line)
        if m:
            s.smoke_summary_result = m.group("r")
            s.smoke_summary_reason = m.group("reason")
            s.smoke_summary_stage = m.group("stage")

        m = PERF_RE.search(line)
        if m:
            s.perf = PerfRow(
                avg_fps=float(m.group("avg")),
                p50_ms=float(m.group("p50")),
                p99_ms=float(m.group("p99")),
                p1low_fps=float(m.group("p1l")),
                peak_ms=float(m.group("peak")),
                samples=int(m.group("samples")),
            )

    return s
