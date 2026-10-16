# scripts/smoke_lib/logparse.py
"""Parse mc2.exe smoke-mode stdout+stderr into a structured summary."""
from __future__ import annotations

import hashlib
import re
from dataclasses import dataclass, field
from typing import Optional

from . import vk_profile, vk_validation, vram

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
    # The engine crash bundle (gos_crashbundle.cpp) announces itself with these two lines on every fatal fault, including the heap-corruption
    # exit; without them such a run was classified as an external, evidence-free crash_silent.
    re.compile(r"^MC2 Remastered Crash Report", re.M),
    re.compile(r"^\[CRASH\] FAST-EXIT ", re.M),
    re.compile(r"unhandled exception", re.I),
)
SHADER_ERROR_PATTERNS = (
    # Matches shader_builder.cpp:358 and friends.
    re.compile(r"Shader filename:.*failed to load shader", re.I),
    re.compile(r"shader.*compile.*fail", re.I),
    re.compile(r"GLSL link.*failed", re.I),
    # GL: shader_builder.cpp reports EVERY error as "[SHADER ERROR] ..." (its SHADER_LOG_ERROR macro; CompileShader/LinkProgram also print
    # the token to stdout). The pattern above missed "CompileShader: ERROR: ...": a static_prop.frag compile failure once passed tier1 5/5
    # with every static prop missing. test_logparse.py reads the macro, so the producer and this pattern cannot drift apart.
    re.compile(r"\[SHADER ERROR\]"),
    re.compile(r"\[GPUPROPS\] failed to compile"),
    # VK islands: pipeline / shader-module creation failure (fail-soft in the
    # engine, so the run would otherwise pass with the island silently off).
    re.compile(r"vkCreate(GraphicsPipelines|ComputePipelines|ShaderModule)\b.*failed", re.I),
    re.compile(r"fallback_reason=\"?\w*pipeline\w*fail\w*", re.I),
    re.compile(r"\bpipeline(_create)?_failed\b", re.I),
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

@dataclass(frozen=True)
class PresenceRule:
    """A line a mission MUST print: the run goes red when `pattern` never matches.

    A MarkerBucket flags the presence of a bad line; this is the absence rule for a regression floor, a line whose disappearance is the failure.
    """
    stem: str                            # the smoke mission the rule applies to
    pattern: "re.Pattern[str]"
    bucket: str                          # verdict bucket when the line never appeared
    why: str


PRESENCE_RULES: tuple = (
    # The in-mission video surface (owner ruling 2026-10-13, M1): mc2_10 requests its briefing movies through the ABL play-video call, and the request is
    # logged before the frame-rate check, so the line appears whether or not the movie decodes. The October 10 split of MissionBegin::update once lost the
    # campaign-start cinematic without any gate noticing; a vanished request line is the same regression for the in-mission calls.
    PresenceRule("mc2_10", re.compile(r"\[VIDEO v1\] event=play_requested"), "video_floor_missing",
                 "no [VIDEO v1] event=play_requested line: the in-mission video request path is gone"),
)

MARKER_BUCKETS: tuple = (
    # G4: a VK gate resolved before main() was read by a static initializer, i.e. ahead of the launcher's
    # environment apply, so it can disagree with the environment the game then runs under. The VK gates
    # are read lazily; Gates/gate.cpp dumpConfig reports every snapshotted pre-main VK slot as
    # warn=premain_read (whether or not its value changed; warn=premain is the changed-value case).
    MarkerBucket(re.compile(r"\[CONFIG v1\] warn=premain_read name=MC2_VK_"),
                 "config_premain_vk", "red", None),
    # Design A runtime tier (META-FIX-BATCH-3): mover-registry coherence.
    # Emitter: code/mission.cpp auditMoverCoherence under
    # MC2_MOVER_COHERENCE_AUDIT (force-set by run_smoke for --tier runs).
    # VIOLATION = registry divergence (#50 class) -> red; ADVISORY (I1b
    # bar-full, I2b self/commander-origin order) surfaces in details only —
    # this is what makes those carve-outs safe.
    MarkerBucket(re.compile(r"\[MOVER_COHERENCE\] VIOLATION\b"),
                 "mover_coherence", "red", "allow_mover_coherence"),
    MarkerBucket(re.compile(r"\[MOVER_COHERENCE\] ADVISORY\b"),
                 "mover_coherence_advisory", "advisory", None),
    # Design B (META-FIX-BATCH-3): fail-loud asset-load policy.
    # asset_load_fail counts parse_fail + required_missing + appr_type_fail
    # ONLY — deliberately NOT bare file_missing (missing optional slots are
    # normal stock degradation and are not logged). These lines are
    # always-on and load-time-only, so every tier1 run goes red the moment
    # a stock base shape stops loading.
    MarkerBucket(re.compile(
        r"\[ASSET_LOAD v1\] event=(parse_fail|required_missing|appr_type_fail)\b"),
        "asset_load_fail", "red", "allow_asset_load_fail"),
    # icon_oob ARMED RED 2026-07-16: precondition met — the
    # cellForIndexChecked/iconTileChecked row bound was validated against
    # all three shipped atlas geometries (retail 256x256, MC2X 512,
    # MCO 1024/40) by tools/icon_atlas_harness (16/16 PASS, batch-3).
    MarkerBucket(re.compile(r"\[ASSET_LOAD v1\] event=icon_oob\b"),
                 "icon_oob", "red", "allow_icon_oob"),
    # appear_audit REGISTERED BUT DISARMED (advisory): whether any object
    # class legitimately reports NULL appearance at mission_ready is
    # unverified. Promote to red ONLY after a full 24-map MC2_APPEAR_AUDIT=1
    # failed=0 baseline (tier1-missions baseline shipped with batch 3; the
    # 24-map run is the documented follow-up).
    MarkerBucket(re.compile(r"\[APPEAR_AUDIT v1\] event=missing\b"),
                 "appear_audit", "advisory", None),
    # The Vulkan device profile REFUSED the device (a required item of the 1.2 profile is missing): the engine fell back to GL, so a run that asked
    # for Vulkan would otherwise pass on the wrong backend. Emitter: GameOS/gameos/vulkan_context.cpp (vk_device_profile.h table).
    MarkerBucket(re.compile(r"\[VK_PROFILE v1\] verdict=refused\b"),
                 "vk_profile_refused", "red", None),
    # The Khronos validation layer reported an ERROR (or was requested and is not installed: id=layer_unavailable). Warnings are counted in the report,
    # never red. Emitter: GameOS/gameos/vulkan_context.cpp; the layer is on only when the run sets MC2_VULKAN_VALIDATION (smoke, never a player).
    MarkerBucket(re.compile(r"\[VK_VALIDATION v1\] severity=error\b"),
                 "vk_validation_error", "red", None),
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
    # Sim determinism: how many `[SIMID v1] simframe=` lines the engine printed (MC2_SIM_IDENTITY_TRACE) and a short digest of all of them.
    # Two runs under MC2_SMOKE_FIXED_TIMESTEP + MC2_DETERMINISTIC_RNG must carry the same chain over their common frames (tools/sim_determinism.py).
    simid_frames: int = 0
    simid_chain: Optional[str] = None
    # The `[VRAM v1]` samples of the run (load_end, run_end): GPU memory counters, see smoke_lib/vram.py.
    vram: list = field(default_factory=list)
    # The `[VK_PROFILE v1]` lines (the Vulkan device profile verdict), see smoke_lib/vk_profile.py.
    vk_profile: list = field(default_factory=list)
    # The `[VK_VALIDATION v1]` lines (Khronos validation layer messages), see smoke_lib/vk_validation.py.
    vk_validation: list = field(default_factory=list)
    # Per PRESENCE_RULES row (keyed by its bucket): how many lines matched. The gate applies the rows of the mission under test.
    presence_counts: dict = field(default_factory=dict)


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
    simid = hashlib.sha256()
    in_play_phase = False

    lines = text.splitlines()
    for idx, line in enumerate(lines):
        line_wall = line_wallclocks[idx] if line_wallclocks and idx < len(line_wallclocks) else None
        if "[INSTR v1] enabled:" in line:
            s.instr_banner_seen = True
        for record in re.finditer(r"\[SIMID v1\] simframe=[^\r\n\[]*", line):
            simid.update(record.group().rstrip().encode("utf-8", "replace"))
            s.simid_frames += 1
        if "[VRAM v1]" in line:
            s.vram.extend(vram.parse(line))
        if "[VK_PROFILE v1]" in line:
            s.vk_profile.extend(vk_profile.parse(line))
        if "[VK_VALIDATION v1]" in line:
            s.vk_validation.extend(vk_validation.parse(line))
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

        for mb in MARKER_BUCKETS:
            if mb.pattern.search(line):
                s.marker_counts[mb.bucket] = s.marker_counts.get(mb.bucket, 0) + 1

        for rule in PRESENCE_RULES:
            if rule.pattern.search(line):
                s.presence_counts[rule.bucket] = s.presence_counts.get(rule.bucket, 0) + 1

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

    if s.simid_frames:
        s.simid_chain = simid.hexdigest()[:16]
    return s
