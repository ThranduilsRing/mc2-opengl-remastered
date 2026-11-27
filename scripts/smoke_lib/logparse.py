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
TIMING_RE = re.compile(r"\[TIMING v1\] event=(?P<ev>\w+) elapsed_ms=(?P<ms>\d+(?:\.\d+)?)")
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
    # exit; without them such a run would be classified as an external,
    # evidence-free crash_silent.
    re.compile(r"^MC2 Remastered Crash Report", re.M),
    re.compile(r"^\[CRASH\] FAST-EXIT ", re.M),
    # The fatal-backend sink line (a lost Vulkan device). It reaches the ring trace a bundle keeps (last_trace.txt), so a log that carries it is a crash.
    re.compile(r"^fatal backend exit \d+: ", re.M),
    re.compile(r"unhandled exception", re.I),
)
#: What the record says that the next session writes for a process that died with no handler (a harness-killed game, W135 3274): not that session's crash.
SENTINEL_RECORD = "unclean exit"
SHADER_ERROR_PATTERNS = (
    # Matches shader_builder.cpp:358 and friends.
    re.compile(r"Shader filename:.*failed to load shader", re.I),
    re.compile(r"shader.*compile.*fail", re.I),
    re.compile(r"GLSL link.*failed", re.I),
    # Retained for historical logs from the retired GL compiler.
    re.compile(r"\[SHADER ERROR\]"),
    re.compile(r"\[GPUPROPS\] failed to compile"),
    # Current native loader errors (vk_island_util::loadSpv).
    re.compile(r"\bcannot open spv\b", re.I),
    re.compile(r"\bspv\b.*\bbad size\b", re.I),
    re.compile(r"\bspv short read\b", re.I),
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

# Detect engine STOP("Unable to open Mission File")
# abort during load phase.  mission.cpp:2072 calls this when .fit resolution fails.
MISSION_LOAD_FAIL_PATTERNS = (
    re.compile(r"Unable to open Mission File", re.I),
    re.compile(r"STOP\(.*Mission File", re.I),
)


# ---------------------------------------------------------------------------
# Table-driven marker buckets.
#
# Generalizes the clone-per-feature marker pattern ONCE:
# each recurrence-proofing design registers ONE ROW here instead of re-cloning
# a bespoke counter + gate + manifest key. A row is scanned per stdout line;
# counts land in LogSummary.marker_counts[bucket]; gates.py turns red-severity
# counts into verdict buckets (per-mission downgrade via the manifest override
# key, mirroring allow_asset_oob); advisory-severity counts become details only.
#
# Designs A and B are below; design F (ORDER_LIVENESS) is handled through the
# summary-parser path.
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
    diag_tag: str = ""                   # the line only prints when this MC2_DIAG_TAGS tag is on (see docs/plans/2026-11-02-quiet-tags.md)


PRESENCE_RULES: tuple = (
    # [PERF_COUNTERS v1]: the per-frame counter line (draws, waits, walk) prints on the first frame and every 300 frames; a vanished line means
    # the counter plumbing is gone and the perf ceilings below it would silently grade nothing.
    *(PresenceRule(stem, re.compile(r"\[PERF_COUNTERS v1\] frames="), "perf_counters_missing",
                   "no [PERF_COUNTERS v1] line: the per-frame counter log is gone", diag_tag="PERF_COUNTERS")
      for stem in ("mc2_01", "mc2_03", "mc2_10", "mc2_17", "mc2_24")),
    # The in-mission video surface: mc2_10 requests its briefing movies through the ABL play-video call, and the request is
    # logged before the frame-rate check, so the line appears whether or not the movie decodes. A vanished request line
    # means the in-mission video path is gone and the cinematic can disappear silently.
    PresenceRule("mc2_10", re.compile(r"\[VIDEO v1\] event=play_requested"), "video_floor_missing",
                 "no [VIDEO v1] event=play_requested line: the in-mission video request path is gone"),
    # Vegetation must draw at default quality on every tier1 map (merge 38b: the residency preset refused it, fallback=off, and every gate
    # stayed green). Gated on the population, not the per-frame draw counter (which scales with fps and run length): the once-per-run
    # "native_geometry kind=vegetation ... instances=N" line. Current logs show N = 600k..2.5M on the tier1 maps; the floor is N >= 100k,
    # so a refused or 64x stride-thinned population fails. That line prints after the first submit whatever survives the cull, so the
    # GPU cull must also draw: the MAX over the run's periodic "[VEG_GPU] visible survivors=" samples must reach 100 (any one sample >= 100
    # satisfies the pattern, so a single low sample cannot fail a run; a cull or compact that drops everything fails even though the population
    # looks healthy; samples are camera dependent, 94..682 at the smoke camera, 800+ on the mc2_24 battle camera).
    *(PresenceRule(stem, re.compile(r"native_geometry kind=vegetation\b.*\binstances=[1-9][0-9]{5,}\b"), "vegetation_floor_missing",
                   "no native_geometry kind=vegetation line with instances >= 100k: GPU vegetation never started or its population was thinned (residency refusal)")
      for stem in ("mc2_01", "mc2_03", "mc2_10", "mc2_17", "mc2_24")),
    *(PresenceRule(stem, re.compile(r"\[VEG_GPU\] visible survivors=[1-9][0-9]{2,}"), "vegetation_visible_missing",
                   "no [VEG_GPU] visible survivors>=100 line: the vegetation cull drew (almost) nothing")
      for stem in ("mc2_01", "mc2_03", "mc2_10", "mc2_17", "mc2_24")),
    # HZB cull parity (vulkan_post_hzb_native.cpp): the GPU keep mask is compared with the CPU reference on one frame in 64 and the totals print
    # every 600 runs and at exit. The line prints with engaged=0 when the pass never reached 1024 candidates; a vanished line means the check is gone.
    *(PresenceRule(stem, re.compile(r"\[CULL_PARITY v1\] engaged="), "cull_parity_missing",
                   "no [CULL_PARITY v1] line: the HZB GPU/CPU cull parity check is gone", diag_tag="CULL_PARITY")
      for stem in ("mc2_01", "mc2_10")),
    # Colour-prop light contract (W1): the bake-time parity check must have run (checks > 0) with no mismatch.
    *(PresenceRule(stem, re.compile(r"\[LIGHT_CONTRACT v1\] checks=[1-9][0-9]* mismatches=0\b"), "light_contract_unchecked",
                   "no [LIGHT_CONTRACT v1] checks>0 mismatches=0 line: the prop light parity check never ran or never passed", diag_tag="LIGHT_CONTRACT")
      for stem in ("mc2_01", "mc2_10")),
)

MARKER_BUCKETS: tuple = (
    # Numeric GPU thresholds are counted below; register their advisory severity here.
    MarkerBucket(re.compile(r"\[GPU_SPIKE_ADVISORY v1\]"), "gpu_spike", "advisory", None),
    # Colour-prop light contract (W1): any row that disagrees with contractSun is a look change.
    MarkerBucket(re.compile(r"\[LIGHT_CONTRACT v1\] checks=\d+ mismatches=[1-9]"),
                 "light_contract_mismatch", "red", None),
    # Contract must reproduce the retired baked row exactly: any max_delta above 0 is a divergence.
    MarkerBucket(re.compile(r"\[LIGHT_CONTRACT v1\] .* max_delta=(?:[9]|[1-9][0-9]+)\b"),
                 "light_contract_shared_delta", "red", None),
    # Stock tier1 baseline must be clean; missing textures are install failures.
    MarkerBucket(re.compile(r"\[SPLATTING\] FAILED\b"),
                 "splatting_failed", "red", None),
    MarkerBucket(re.compile(r"\[TEXMGR_MISSING\]"),
                 "texmgr_missing", "red", None),
    # G4: a VK gate resolved before main() was read by a static initializer, i.e. ahead of the launcher's
    # environment apply, so it can disagree with the environment the game then runs under. The VK gates
    # are read lazily; Gates/gate.cpp dumpConfig reports every snapshotted pre-main VK slot as
    # warn=premain_read (whether or not its value changed; warn=premain is the changed-value case).
    MarkerBucket(re.compile(r"\[CONFIG v1\] warn=premain_read name=MC2_VK_"),
                 "config_premain_vk", "red", None),
    # Design A runtime tier: mover-registry coherence.
    # Emitter: code/mission.cpp auditMoverCoherence under
    # MC2_MOVER_COHERENCE_AUDIT (force-set by run_smoke for --tier runs).
    # VIOLATION = registry divergence -> red; ADVISORY (I1b
    # bar-full, I2b self/commander-origin order) surfaces in details only —
    # this is what makes those carve-outs safe.
    MarkerBucket(re.compile(r"\[MOVER_COHERENCE\] VIOLATION\b"),
                 "mover_coherence", "red", "allow_mover_coherence"),
    MarkerBucket(re.compile(r"\[MOVER_COHERENCE\] ADVISORY\b"),
                 "mover_coherence_advisory", "advisory", None),
    # Design B: fail-loud asset-load policy.
    # asset_load_fail counts parse_fail + required_missing + appr_type_fail
    # ONLY — deliberately NOT bare file_missing (missing optional slots are
    # normal stock degradation and are not logged). These lines are
    # always-on and load-time-only, so every tier1 run goes red the moment
    # a stock base shape stops loading.
    MarkerBucket(re.compile(
        r"\[ASSET_LOAD v1\] event=(parse_fail|required_missing|appr_type_fail)\b"),
        "asset_load_fail", "red", "allow_asset_load_fail"),
    # icon_oob ARMED RED: precondition met -- the
    # cellForIndexChecked/iconTileChecked row bound was validated against
    # all three shipped atlas geometries (retail 256x256, MC2X 512,
    # MCO 1024/40) by tools/icon_atlas_harness (16/16 PASS).
    MarkerBucket(re.compile(r"\[ASSET_LOAD v1\] event=icon_oob\b"),
                 "icon_oob", "red", "allow_icon_oob"),
    # appear_audit REGISTERED BUT DISARMED (advisory): whether any object
    # class legitimately reports NULL appearance at mission_ready is
    # unverified. Promote to red ONLY after a full 24-map MC2_APPEAR_AUDIT=1
    # failed=0 baseline over all 24 maps; a tier1-only baseline is not enough.
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
    # Backstop: a raw layer line ("Validation Error: [ VUID-...") that the engine callback did not format into a [VK_VALIDATION v1] line (stderr before the
    # messenger, another instance, a tool process). Red unless an unexpired known entry covers it. Emitter: the layer itself.
    MarkerBucket(re.compile(r"^(?!.*\[VK_VALIDATION v1\]).*Validation Error: \[ "),
                 "vk_validation_raw", "red", None),
    # The G7b sim/render overlap audit caught a thread touching a domain it does not own (MC2_SIM_OVERLAP_AUDIT, on in tier1 and the
    # determinism run; audit mode aborts on the first one). Emitter: GameOS/gameos/sim_overlap.cpp violation(). Never downgraded.
    MarkerBucket(re.compile(r"\[SIM_OVERLAP v1\] VIOLATION\b"),
                 "sim_overlap_violation", "red", None),
    # A GPU cull that hides a prop the CPU reference keeps is a real over-cull bug. false_keeps (GPU keeps what the CPU culls) is advisory and unflagged.
    MarkerBucket(re.compile(r"\[CULL_PARITY v1\] .*\bfalse_culls=[1-9]"),
                 "cull_parity_false_cull", "red", None),
    # A static-prop light row past the uploaded light table (gos_static_prop_registry_flush_range.cpp flushLightRowDiag): the GPU reads a zero row.
    MarkerBucket(re.compile(r"\[STATIC_LIGHT_ROW v1\] event=zero_row_reached_gpu class=out_of_range"),
                 "static_light_row_out_of_range", "red", None),
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
    load_split: Optional[dict] = None
    load_total_ms: Optional[float] = None
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
    mission_quit_wall_s: Optional[float] = None
    pool_nulls: int = 0
    asset_oob: int = 0
    shader_errors: int = 0
    abl_failures: int = 0
    content_errors: int = 0
    content_summaries: int = 0
    content_counts: dict = field(default_factory=dict)
    missing_files: int = 0
    crash_handler_hit: bool = False
    smoke_summary_result: Optional[str] = None
    smoke_summary_reason: Optional[str] = None
    smoke_summary_stage: Optional[str] = None
    destroys: int = 0
    perf: PerfRow = field(default_factory=PerfRow)
    # Set True if the engine printed an
    # "Unable to open Mission File" / STOP line before play phase started.
    mission_load_fail_seen: bool = False
    # Per-bucket marker counts (bucket -> hit count) for
    # every registered MARKER_BUCKETS row. Empty dict when no marker fired.
    marker_counts: dict = field(default_factory=dict)
    # The first matching line per bucket, so a red verdict can print what fired.
    marker_first: dict = field(default_factory=dict)
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
    # The LAST `[PERF_COUNTERS v1]` line of the run as {field: mean per frame} (run-so-far means; wait_* in ms), plus `frames`. Empty when none printed.
    perf_counters: dict = field(default_factory=dict)
    # The `[PERF_WINDOW v1]` line: {field: mean per frame over the fixed sim-tick window} plus "frames", "complete" (0/1), "ticks" ("30..120"). Empty when none printed.
    perf_window: dict = field(default_factory=dict)


def parse_perf_counters(line: str) -> dict:
    """`[PERF_COUNTERS v1] frames=N draws.props=.. wait_sealed_ms=.. first_vk_wait_site=<label> ...` -> {name: float}; non-numeric values are skipped."""
    out: dict = {}
    for key, value in re.findall(r"([A-Za-z_][\w.]*)=(\S+)", line.split("[PERF_COUNTERS v1]", 1)[-1]):
        try:
            out[key] = float(value)
        except ValueError:
            pass
    return out


def parse_perf_window(line: str) -> dict:
    """`[PERF_WINDOW v1] ticks=30..120 complete=1 frames=N draws.mech_depth=.. ...` -> {name: float}, with "ticks" kept as the string."""
    out: dict = {}
    for key, value in re.findall(r"([A-Za-z_][\w.]*)=(\S+)", line.split("[PERF_WINDOW v1]", 1)[-1]):
        if key == "ticks":
            out[key] = value
            continue
        try:
            out[key] = float(value)
        except ValueError:
            pass
    return out


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
    raw_known = 0

    lines = text.splitlines()
    for idx, line in enumerate(lines):
        line_wall = line_wallclocks[idx] if line_wallclocks and idx < len(line_wallclocks) else None
        if "[INSTR v1] enabled:" in line:
            s.instr_banner_seen = True
        for record in re.finditer(r"\[SIMID v1\] simframe=[^\r\n\[]*", line):
            simid.update(record.group().rstrip().encode("utf-8", "replace"))
            s.simid_frames += 1
        if "[PERF_COUNTERS v1]" in line:
            s.perf_counters = parse_perf_counters(line)
        if "[LOAD_SPLIT v1]" in line:
            fields = dict(re.findall(r"(\w+)=(-?[\d.]+)(?=\s|$)", line))
            s.load_total_ms = float(fields.pop("total")) if "total" in fields else None
            s.load_split = {k: float(v) for k, v in fields.items()}
            coverage = re.search(r"\bbytes_coverage=(\w+)", line)
            if coverage:
                s.load_split["bytes_coverage"] = coverage.group(1)
                if coverage.group(1) == "unavailable":
                    for key in ("bytes_io", "bytes_decode", "bytes_upload"):
                        s.load_split.pop(key, None)
        if "[PERF_WINDOW v1]" in line:
            s.perf_window = parse_perf_window(line)
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
            elif ev == "mission_quit":
                s.mission_quit_ms = ms
                s.mission_quit_wall_s = line_wall

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

        if "[TGL_POOL v1]" in line and "nulls=" in line: s.pool_nulls += 1
        if "[ASSET_SCALE v1] event=oob_blit" in line: s.asset_oob += 1
        if "[DESTROY v1]" in line and "event=destroy" in line: s.destroys += 1

        if "Validation Error: [ " in line and vk_validation.RAW_RE.search(line) and vk_validation.raw_is_known(line):
            raw_known += 1
        for pattern, limit in (
            (r"\[SHADOW_BAKE\] gpu_ms object_map=([\d.]+)", 10.0),
            (r"\[SHADOW_BAKE\] dirty queue drained.*batch_gpu_ms mean=([\d.]+)", 1.0),
            (r"\[VK_GPU_PASSES v1\].*\bmax_frame_ms=([\d.]+)", 10.0),
        ):
            spike = re.search(pattern, line)
            if spike and float(spike.group(1)) > limit:
                s.marker_counts["gpu_spike"] = s.marker_counts.get("gpu_spike", 0) + 1
                s.marker_first.setdefault("gpu_spike", line.strip()[:400])
                break
        for mb in MARKER_BUCKETS:
            if mb.pattern.search(line):
                s.marker_counts[mb.bucket] = s.marker_counts.get(mb.bucket, 0) + 1
                s.marker_first.setdefault(mb.bucket, line.strip()[:400])

        for rule in PRESENCE_RULES:
            if rule.pattern.search(line):
                s.presence_counts[rule.bucket] = s.presence_counts.get(rule.bucket, 0) + 1

        for p in CRASH_PATTERNS:
            if SENTINEL_RECORD not in line and p.search(line): s.crash_handler_hit = True; break
        for p in SHADER_ERROR_PATTERNS:
            if p.search(line): s.shader_errors += 1; break
        if "[ABL_FALLBACK]" in line or "[ABL_FATAL]" in line:
            s.abl_failures += 1
        if "[CONTENT_SUMMARY]" in line:
            values = dict(re.findall(r"(\w+)=(\d+)", line))
            kinds = {"texture", "shape", "objtype", "ammo", "movetype", "sound", "missiondata"}
            if kinds <= values.keys() and "total" in values and int(values["total"]) == sum(int(values[k]) for k in kinds):
                s.content_summaries += 1
                s.content_errors += int(values.pop("total"))
                for key, value in values.items():
                    s.content_counts[key] = s.content_counts.get(key, 0) + int(value)
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
    # Reviewed, unexpired known hits (tests/syncval/known_vuids.json) are not red; every other error or raw VUID line is.
    exempt = {"vk_validation_error": vk_validation.known_error_count(s.vk_validation), "vk_validation_raw": raw_known}
    for bucket, n in exempt.items():
        if n and bucket in s.marker_counts:
            s.marker_counts[bucket] -= n
            if s.marker_counts[bucket] <= 0:
                del s.marker_counts[bucket]
    return s
