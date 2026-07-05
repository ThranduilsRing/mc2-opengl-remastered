#!/usr/bin/env python3
"""
MC2 Render-State MCP Server (MC2-MCP-WRAPPER-1 + MC2-MCP-WRAPPER-2
+ MCP-ANTI-CHURN-1: get_runtime_gate / get_mission_sidecars)

Read-only bridge between Claude agents and the running MC2 engine.
Reads JSON snapshots written by MC2_DEBUG_STATE_DUMP=1.
Also wraps run_smoke.py to capture baselines and read artifact reports.

Configuration (env vars):
  MC2_DEPLOY_DIR  — path to mc2-win64-v0.4 deploy directory
                    default: A:/Games/mc2-opengl/mc2-win64-v0.4

The engine must be running with MC2_DEBUG_STATE_DUMP=1 for state reads to
return live data. History tools additionally require MC2_DEBUG_STATE_DUMP_HISTORY=1.

No gameplay, renderer, or feature-gate mutation. Read-only.
"""

import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

from mcp.server.fastmcp import FastMCP

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

_DEPLOY_DIR = Path(os.environ.get("MC2_DEPLOY_DIR", "A:/Games/mc2-opengl/mc2-win64-v0.4"))
_EDITOR_DEPLOY_DIR = Path(os.environ.get(
    "MC2_EDITOR_DEPLOY_DIR",
    "A:/Games/mc2-opengl/releases/0.5 testing/mc2-win64-v0.5.0"))
_STATE_DIR = _DEPLOY_DIR / "debug_state"
_LATEST = _STATE_DIR / "latest_render_state.json"
_HISTORY_SLOTS = 8
_SCHEMA_V1 = "MC2_DEBUG_STATE_V1"
_SCHEMA_V2 = "MC2_DEBUG_STATE_V2"
_SUPPORTED_SCHEMAS = {_SCHEMA_V1, _SCHEMA_V2}

# Worktree root — two levels up from scripts/mcp/
_WORKTREE_DIR = Path(__file__).resolve().parents[2]
_ARTIFACTS_DIR = _WORKTREE_DIR / "tests" / "smoke" / "artifacts"
_RUN_SMOKE = _WORKTREE_DIR / "scripts" / "run_smoke.py"

_VALID_MISSION = re.compile(r"^[a-z0-9_]{1,32}$")

mcp = FastMCP(
    "mc2-render-state",
    instructions=(
        "Read-only bridge to running MC2 engine. "
        "State: engine needs MC2_DEBUG_STATE_DUMP=1. Use get_render_health() to check liveness. "
        "Smoke: use get_latest_smoke_report() for structured results. "
        "Diagnostics: use get_diagnostic_events(tag) to query JSONL trace. "
        "ANTI-CHURN: get_runtime_gate(name) answers 'is gate X actually on in "
        "the running/last exe?' from the state dump — no game launch, no log "
        "tailing; get_mission_sidecars() shows the loaded mission's beauty "
        "sidecars on disk + related gates in one call. "
        "All tools are read-only — no gameplay or renderer mutation."
    ),
)

# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------

def _read_json(path: Path) -> dict[str, Any] | None:
    """Read and parse a JSON file. Returns None on missing/invalid."""
    if not path.exists():
        return None
    try:
        text = path.read_text(encoding="utf-8")
        return json.loads(text)
    except (json.JSONDecodeError, OSError):
        # Engine may be mid-write; retry once after a short sleep
        time.sleep(0.05)
        try:
            text = path.read_text(encoding="utf-8")
            return json.loads(text)
        except Exception:
            return None


def _latest() -> dict[str, Any] | None:
    return _read_json(_LATEST)


_STALE_THRESHOLD_S = 30  # seconds; dump cadence ~5s at 60fps so 30s = clearly not running


def _file_age_s() -> float | None:
    """Return age of latest snapshot file in seconds, or None if missing."""
    try:
        mtime = _LATEST.stat().st_mtime
        return time.time() - mtime
    except OSError:
        return None


def _stale_banner() -> str:
    """Return a STALE warning line if the snapshot is old, else empty string."""
    age = _file_age_s()
    if age is None:
        return ""
    if age >= _STALE_THRESHOLD_S:
        return f"*** STALE — file is {age:.0f}s old; game likely not running or MC2_DEBUG_STATE_DUMP=1 not set\n"
    return ""


def _pid_alive(pid: int | None) -> bool | None:
    """Return True if PID is running, False if not, None if check unavailable."""
    if pid is None:
        return None
    try:
        import subprocess
        r = subprocess.run(
            ["tasklist", "/fi", f"PID eq {pid}", "/nh", "/fo", "CSV"],
            capture_output=True, text=True, timeout=3,
        )
        return str(pid) in r.stdout
    except Exception:
        return None


def _get_liveness() -> dict:
    """
    Return liveness dict using V2 schema fields when available.

    live = dump_kind=="periodic" AND pid_alive AND seconds_since_update < threshold
    For V1 snapshots (no pid/dump_kind), falls back to age-only heuristic.
    """
    data = _latest()
    age = _file_age_s()

    result: dict = {
        "live": False,
        "seconds_since_update": round(age, 1) if age is not None else None,
        "pid_alive": None,
        "dump_kind": None,
        "session_id": None,
        "pid": None,
        "schema": None,
    }

    if data is None:
        return result

    schema = data.get("schema", "")
    result["schema"] = schema
    pid = data.get("pid")
    dump_kind = data.get("dump_kind")
    session_id = data.get("session_id")

    result["dump_kind"] = dump_kind
    result["session_id"] = session_id
    result["pid"] = pid

    pid_alive = _pid_alive(pid)
    result["pid_alive"] = pid_alive

    recent = age is not None and age < _STALE_THRESHOLD_S

    if schema == _SCHEMA_V2:
        # V2: use dump_kind + pid_alive + age
        result["live"] = (dump_kind == "periodic" and pid_alive is True and recent)
    else:
        # V1: age-only heuristic (no pid/dump_kind in schema)
        result["live"] = recent and pid_alive is not False

    return result


def _not_available(detail: str = "") -> str:
    msg = (
        f"State file not available: {_LATEST}\n"
        "Ensure the engine is running with MC2_DEBUG_STATE_DUMP=1.\n"
        f"Deploy dir: {_DEPLOY_DIR}"
    )
    if detail:
        msg += f"\nDetail: {detail}"
    return msg


def _validate(data: dict[str, Any]) -> list[str]:
    """Return list of validation errors (empty = PASS)."""
    errors: list[str] = []

    def require(obj, key, typ, path=""):
        full = f"{path}.{key}" if path else key
        if key not in obj:
            errors.append(f"missing: {full}")
            return False
        val = obj[key]
        if not isinstance(val, typ) or (typ is int and isinstance(val, bool)):
            errors.append(f"{full}: expected {typ.__name__}, got {type(val).__name__}")
            return False
        return True

    if not isinstance(data, dict):
        errors.append("root is not an object")
        return errors

    schema = data.get("schema", "")
    if schema not in _SUPPORTED_SCHEMAS:
        errors.append(f"schema: expected one of {_SUPPORTED_SCHEMAS!r}, got {schema!r}")

    require(data, "frame", int)
    for section in ("mission", "build", "features", "engineView", "renderSnapshot", "staticPropOpaque"):
        if section not in data:
            errors.append(f"missing section: {section}")

    rs = data.get("renderSnapshot", {})
    for k in ("staticPropValidationFail", "staticPropPacketRangesFail",
              "staticPropPacketInvalid", "spBuildCountMismatch",
              "spBuildPacketMismatch", "spBuildMetaMismatch"):
        require(rs, k, int, "renderSnapshot")
    require(rs, "ok", bool, "renderSnapshot")
    require(rs, "arenaOverflow", bool, "renderSnapshot")

    sp = data.get("staticPropOpaque", {})
    for k in ("snapshotDispatchDefault", "legacyDispatch", "materialGpuEnabled",
              "materialGpuSample", "iblShEnabled", "pbrEnabled", "pbrRoughnessOverrideEnabled"):
        require(sp, k, bool, "staticPropOpaque")
    if sp.get("snapshotDispatchDefault") and sp.get("legacyDispatch"):
        errors.append("staticPropOpaque: snapshotDispatchDefault and legacyDispatch both true")

    return errors

# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------

@mcp.tool()
def get_render_state() -> str:
    """
    Return the full latest MC2 render-state snapshot as JSON.

    Includes: schema, frame, mission, build, feature gates, engineView,
    renderSnapshot counters, and staticPropOpaque visual globals.
    Requires engine running with MC2_DEBUG_STATE_DUMP=1.
    """
    data = _latest()
    if data is None:
        return _not_available()
    return _stale_banner() + json.dumps(data, indent=2)


@mcp.tool()
def get_render_health() -> str:
    """
    Return a compact render health summary.

    Reports: frame, mission, renderSnapshot.ok, all mismatch counters,
    arenaOverflow, and any failing counters highlighted. Fast diagnostic check.
    """
    liveness = _get_liveness()
    data = _latest()
    if data is None:
        return _not_available()

    rs = data.get("renderSnapshot", {})
    sp_opaque = data.get("staticPropOpaque", {})
    mission = data.get("mission", {})
    frame = data.get("frame", "?")
    ok = rs.get("ok", False)

    lines = [
        f"frame:   {frame}",
        f"mission: {mission.get('name', '?')} (known={mission.get('known', False)})",
        f"ok:      {ok}",
    ]

    counters = {
        "staticPropValidationFail":  rs.get("staticPropValidationFail", "?"),
        "staticPropPacketRangesFail": rs.get("staticPropPacketRangesFail", "?"),
        "staticPropPacketInvalid":   rs.get("staticPropPacketInvalid", "?"),
        "arenaOverflow":             rs.get("arenaOverflow", "?"),
        "spBuildCountMismatch":      rs.get("spBuildCountMismatch", "?"),
        "spBuildPacketMismatch":     rs.get("spBuildPacketMismatch", "?"),
        "spBuildMetaMismatch":       rs.get("spBuildMetaMismatch", "?"),
        "spBuildFallback":           rs.get("spBuildFallback", "?"),
    }

    any_fail = False
    for k, v in counters.items():
        nonzero = v not in (0, False, "?")
        flag = " *** FAIL" if nonzero else ""
        if nonzero:
            any_fail = True
        lines.append(f"  {k}: {v}{flag}")

    lines.append("")
    lines.append(f"snapshotDispatch: {sp_opaque.get('snapshotDispatchDefault', '?')}")
    lines.append(f"materialGpu:      {sp_opaque.get('materialGpuEnabled', '?')} / sample={sp_opaque.get('materialGpuSample', '?')}")
    lines.append(f"iblSh:            {sp_opaque.get('iblShEnabled', '?')} strength={sp_opaque.get('iblShStrength', '?')} set={sp_opaque.get('iblShSet', '?')!r}")
    lines.append(f"pbr:              {sp_opaque.get('pbrEnabled', '?')} strength={sp_opaque.get('pbrStrength', '?')}")

    # RENDER-FRAME-CONTEXT-1 / VIEW-EPOCH-DEDUPE-1 currency telemetry.
    fc = data.get("frame_context", {})
    if fc:
        mm = fc.get("mismatch_count", 0)
        mok = fc.get("mirror_ok", True)
        mm_fail = mm not in (0, "?")
        mok_fail = mok is False
        if mm_fail or mok_fail:
            any_fail = True
        lines.append("")
        lines.append("frame_context:")
        lines.append(f"  engine_frame:       {fc.get('engine_frame', '?')}")
        lines.append(f"  view_epoch:         {fc.get('view_epoch', '?')} (raw publish counter)")
        lines.append(f"  view_content_epoch: {fc.get('view_content_epoch', '?')} (semantic; ~engine_frame, not 2x)")
        lines.append(f"  mvp_snapshot_used:  {fc.get('mvp_snapshot_used', '?')} (z-fight fix active when ~engine_frame)")
        lines.append(f"  stale_mvp_reads:    {fc.get('stale_mvp_reads', '?')} (rises only on real camera motion)")
        lines.append(f"  mismatch_count:     {mm}{' *** FAIL' if mm_fail else ''}")
        lines.append(f"  mirror_ok:          {mok}{' *** FAIL' if mok_fail else ''}")

    # FRAME-GRAPH-SKELETON-1: resource-DAG validity of the shipped pass table.
    fg = data.get("frame_graph", {})
    if fg:
        fg_valid = fg.get("valid", True)
        if fg_valid is False:
            any_fail = True
        lines.append("")
        lines.append("frame_graph:")
        lines.append(f"  valid:            {fg_valid}{'' if fg_valid else ' *** FAIL'}")
        if fg_valid is False:
            lines.append(f"  offending_pass:   {fg.get('offending_pass', '?')}")
            lines.append(f"  missing_resource: {fg.get('missing_resource', '?')}")
            lines.append(f"  unknown_pass:     {fg.get('unknown_pass', '?')}")
        # Runtime ambient cross-check (MC2_AMBIENT_PROBE). samples>0 proves the probe
        # fired; mismatches>0 = live GL ambient state diverged from the declared ledger.
        amb_samples = fg.get("ambient_probe_samples", 0)
        amb_miss = fg.get("ambient_probe_mismatches", 0)
        if amb_samples or amb_miss:
            if amb_miss not in (0, "?"):
                any_fail = True
            lines.append(f"  ambient_samples:  {amb_samples}")
            lines.append(f"  ambient_mismatch: {amb_miss}{' *** FAIL' if amb_miss not in (0, '?') else ''}")
        # FRAME-GRAPH-FBO-LEDGER-1: bound-FBO-vs-declared-target guard.
        fbo_samples = fg.get("fbo_samples", 0)
        fbo_miss = fg.get("fbo_mismatches", 0)
        if fbo_samples or fbo_miss:
            if fbo_miss not in (0, "?"):
                any_fail = True
            lines.append(f"  fbo_samples:      {fbo_samples}")
            lines.append(f"  fbo_mismatch:     {fbo_miss}{' *** FAIL' if fbo_miss not in (0, '?') else ''}")

    # TERRAIN-PATH-TELEMETRY-1: which terrain-solid branch drew (retirement evidence).
    tp = data.get("terrain_path", {})
    if tp:
        lines.append("")
        lines.append("terrain_path (draws):")
        lines.append(f"  lod_chunk:    {tp.get('lod_chunk', '?')} (authority)")
        lines.append(f"  indirect:     {tp.get('indirect', '?')} (deprecated)")
        lines.append(f"  patch_stream: {tp.get('patch_stream', '?')}")
        lines.append(f"  legacy_mlr:   {tp.get('legacy_mlr', '?')} (deprecated)")

    if any_fail:
        lines.insert(0, "HEALTH: FAIL — nonzero counters detected")
    elif not ok:
        lines.insert(0, "HEALTH: FAIL — ok=false")
    else:
        lines.insert(0, "HEALTH: PASS")

    if not liveness["live"]:
        age = _file_age_s()
        if age is None:
            banner = "*** NO DATA — engine not running or MC2_DEBUG_STATE_DUMP=1 not set\n"
        elif age >= _STALE_THRESHOLD_S:
            banner = f"*** STALE — file is {age:.0f}s old; game likely not running\n"
        else:
            banner = "*** NOT LIVE — pid dead or shutdown dump\n"
    else:
        banner = ""
    if banner:
        lines.insert(0, banner.rstrip())

    age = _file_age_s()
    liveness = _get_liveness()
    lines.append(f"\nfile_age:     {age:.0f}s" if age is not None else "\nfile_age:     unknown")
    lines.append(f"live:         {liveness['live']}")
    lines.append(f"dump_kind:    {liveness.get('dump_kind') or '(V1/unknown)'}")
    lines.append(f"pid:          {liveness.get('pid') or '(V1/unknown)'}")
    lines.append(f"pid_alive:    {liveness.get('pid_alive')}")
    lines.append(f"session_id:   {liveness.get('session_id') or '(V1/unknown)'}")

    return "\n".join(lines)


@mcp.tool()
def get_frame_context() -> str:
    """
    Return the RENDER-FRAME-CONTEXT-1 frame-context block as JSON — the per-frame
    view-epoch currency telemetry. Use to verify VIEW-EPOCH-DEDUPE-1 /
    RENDER-VIEW-CURRENCY-1 without reading logs:
      - view_content_epoch should track ~engine_frame (NOT ~2x; 2x = view_epoch, the
        raw publish counter), proving same-camera republishes are deduped.
      - mvp_snapshot_used ~engine_frame + stale_mvp_reads ~0 on a static camera means
        the FixB depth-matched snapshot path is active (object/mech z-fight fix live).
      - stale_mvp_reads rises only on real camera motion between producer and consumer.
      - mirror_ok must be true and mismatch_count 0 (the additive-context self-check;
        MC2_FRAMECTX_MISMATCH_FATAL=1 aborts the engine on divergence).
    """
    data = _latest()
    if data is None:
        return _not_available()
    fc = data.get("frame_context")
    if not fc:
        return ("frame_context absent from the dump — the running exe predates "
                "RENDER-FRAME-CONTEXT-1, or MC2_DEBUG_STATE_DUMP is off.")
    banner = _stale_banner()
    return (banner if banner else "") + json.dumps(fc, indent=2)


@mcp.tool()
def get_feature_gates() -> str:
    """
    Return current MC2 feature gate states as JSON.

    Shows which MC2_* feature env vars are active (true) or suppressed (false)
    at the time of the last snapshot. Gates are sampled once at process start;
    this reflects the launch-time env, not any runtime change.
    """
    data = _latest()
    if data is None:
        return _not_available()

    features = data.get("features", {})
    frame = data.get("frame", "?")
    lines = [_stale_banner().rstrip()] if _stale_banner() else []
    lines.append(f"# Feature gates — snapshot at frame {frame}")
    for k, v in features.items():
        lines.append(f"  {k}: {v}")
    return "\n".join(lines)


@mcp.tool()
def get_visual_settings() -> str:
    """
    Return full StaticPropOpaque visual state: render passes, shader path,
    dispatch, draw counts, material table sizes, IBL/SH, PBR, debug mode.

    Covers: what pass? what shader path? what material? what sliders?
    All fields reflect the value at the last dump (~300 frames / ~5s cadence).
    IBL/PBR strengths may change during the run via ImGui sliders.
    """
    data = _latest()
    if data is None:
        return _not_available()

    sp = data.get("staticPropOpaque", {})
    rp = data.get("renderPasses", {})
    frame = data.get("frame", "?")

    return _stale_banner() + json.dumps({
        "frame": frame,
        "renderPasses": {
            "shadow":       rp.get("shadow"),
            "screenShadow": rp.get("screenShadow"),
            "bloom":        rp.get("bloom"),
            "fxaa":         rp.get("fxaa"),
            "tonemap":      rp.get("tonemap"),
        },
        "dispatch": {
            "snapshotDefault": sp.get("snapshotDispatchDefault"),
            "legacy":          sp.get("legacyDispatch"),
            "shaderVariant":   sp.get("shaderVariant"),
        },
        "drawCounts": {
            "spV6DrawCalls":    sp.get("spV6DrawCalls"),
            "spAlphaOffPackets": sp.get("spAlphaOffPackets"),
        },
        "material": {
            "gpuEnabled":          sp.get("materialGpuEnabled"),
            "gpuSample":           sp.get("materialGpuSample"),
            "gpuTableSize":        sp.get("materialGpuTableSize"),
            "inventorySize":       sp.get("materialInventorySize"),
            "debugMode":           sp.get("debugMaterialMode"),
        },
        "iblSh": {
            "enabled":  sp.get("iblShEnabled"),
            "strength": sp.get("iblShStrength"),
            "set":      sp.get("iblShSet"),
        },
        "pbr": {
            "enabled":                sp.get("pbrEnabled"),
            "strength":               sp.get("pbrStrength"),
            "roughnessOverrideEnabled": sp.get("pbrRoughnessOverrideEnabled"),
            "roughnessOverride":      sp.get("pbrRoughnessOverride"),
        },
    }, indent=2)


@mcp.tool()
def get_history(slot: int) -> str:
    """
    Return a historical render-state snapshot from the rolling ring.

    slot: 0..7 (0 = oldest written, 7 = most recently written, wrapping).
    Requires the engine was launched with MC2_DEBUG_STATE_DUMP_HISTORY=1.
    Each slot covers 300 frames of history.
    """
    if not 0 <= slot < _HISTORY_SLOTS:
        return f"Invalid slot {slot}. Valid range: 0..{_HISTORY_SLOTS - 1}."

    path = _STATE_DIR / f"history_{slot}.json"
    data = _read_json(path)
    if data is None:
        return (
            f"History slot {slot} not available: {path}\n"
            "Ensure engine was launched with MC2_DEBUG_STATE_DUMP=1 and MC2_DEBUG_STATE_DUMP_HISTORY=1."
        )
    return json.dumps(data, indent=2)


@mcp.tool()
def validate_state() -> str:
    """
    Validate the latest render-state snapshot against the MC2_DEBUG_STATE_V1 schema.

    Checks: schema version, required fields, types, value ranges, consistency
    invariants (e.g. snapshotDispatchDefault XOR legacyDispatch).
    Returns PASS or FAIL with specific errors.
    """
    data = _latest()
    if data is None:
        return _not_available()

    errors = _validate(data)
    frame = data.get("frame", "?")
    banner = _stale_banner()

    if errors:
        lines = [f"FAIL — {len(errors)} error(s) in snapshot at frame {frame}:"]
        for e in errors:
            lines.append(f"  - {e}")
        return banner + "\n".join(lines)

    ok = data.get("renderSnapshot", {}).get("ok", False)
    return banner + f"PASS — schema valid, frame={frame}, renderSnapshot.ok={ok}"


@mcp.tool()
def get_frame_info() -> str:
    """
    Return frame index, mission name, and build configuration from the snapshot.

    Lightweight call to orient an agent to what mission is loaded,
    how many frames have elapsed, and what build config is running.
    """
    data = _latest()
    if data is None:
        return _not_available()

    frame = data.get("frame", "?")
    mission = data.get("mission", {})
    build = data.get("build", {})
    schema = data.get("schema", "?")

    age = _file_age_s()
    age_str = f"{age:.0f}s" if age is not None else "unknown"
    stale = age is not None and age >= _STALE_THRESHOLD_S

    views = data.get("registeredViews", [])
    view_names = [f"{v.get('viewKind', '?')}(id={v.get('viewId', '?')})" for v in views]

    liveness = _get_liveness()
    lines = [
        f"schema:   {schema}",
        f"frame:    {frame}",
        f"mission:  {mission.get('name', '')} (known={mission.get('known', False)})",
        f"build:    config={build.get('config', '?')} commit={build.get('commit', '?')}",
        f"views:    [{', '.join(view_names)}]" if view_names else "views:    []",
        f"stateDir: {_STATE_DIR}",
        f"file_age: {age_str}" + (" *** STALE — game likely not running" if stale else ""),
    ]
    lines.extend([
        f"live:     {liveness['live']} (dump_kind={liveness.get('dump_kind') or 'V1'}, pid_alive={liveness.get('pid_alive')})",
        f"pid:      {data.get('pid', '(V1/unknown)')}",
        f"session:  {data.get('session_id', '(V1/unknown)')}",
        f"written:  {data.get('written_at_epoch', '(V1/unknown)')}",
    ])
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Capture / artifact helpers (MC2-MCP-WRAPPER-2)
# ---------------------------------------------------------------------------

def _latest_artifact_dir() -> Path | None:
    """Return the newest artifact directory, or None if none exist."""
    if not _ARTIFACTS_DIR.exists():
        return None
    dirs = [d for d in _ARTIFACTS_DIR.iterdir() if d.is_dir()]
    if not dirs:
        return None

    def _sort_key(d: Path):
        # Directory names are ISO timestamps like 2026-06-17T14-23-45
        # Alphabetical sort works for ISO format, but fall back to mtime if name differs
        try:
            # Normalize hyphens-in-time back to colons for parsing, but sort as string
            return (d.name,)
        except Exception:
            return (str(d.stat().st_mtime),)

    dirs.sort(key=_sort_key, reverse=True)
    return dirs[0]


@mcp.tool()
def diag_tag(tag: str, log: str = "editor", last_n: int = 80,
             log_path: str = "") -> str:
    """
    Grep the latest editor/game stderr log for a printf diagnostic tag prefix
    and return ONLY the matching lines plus any SUMMARY line — for the
    run -> verify loop without tailing the whole log.

    A diag tag is a bracketed printf prefix the engine emits, e.g.
    EDITOR_STATIC_PRIME or BLDG_CMD_DIAG. This tool returns lines containing
    the literal "[<tag>]".

    Args:
      tag      — the tag name WITHOUT brackets (e.g. "EDITOR_STATIC_PRIME").
      log      — "editor" (default; editor-stderr.log in the editor deploy dir)
                 or "game" (mc2-stderr.log in the game deploy dir, falling back
                 to the latest smoke artifact stderr).
      last_n   — cap the returned matching lines [1..500] (default 80).
      log_path — explicit log file path; overrides `log`.

    Returns JSON: {tag, log_path, match_count, returned, summary_lines, lines}.
    """
    last_n = max(1, min(int(last_n), 500))

    if log_path:
        path = Path(log_path)
    elif log == "editor":
        path = _EDITOR_DEPLOY_DIR / "editor-stderr.log"
    else:  # "game"
        path = _DEPLOY_DIR / "mc2-stderr.log"
        if not path.exists():
            adir = _latest_artifact_dir()
            if adir is not None:
                fallback = adir / "mc2-stderr.log"
                if fallback.exists():
                    path = fallback

    if not path.exists():
        return json.dumps({
            "error": "no_log",
            "path": str(path),
            "tip": ("run editor/game; editor-stderr.log is written by "
                    "run-editor.bat"),
        }, indent=2)

    needle = f"[{tag}]"
    matches = []
    try:
        with open(path, "r", encoding="utf-8", errors="ignore") as f:
            for line in f:
                if needle in line:
                    matches.append(line.rstrip("\n"))
    except OSError as e:
        return json.dumps({"error": "read_error", "path": str(path),
                           "detail": str(e)}, indent=2)

    summary_lines = [m for m in matches if "SUMMARY" in m]
    kept = matches[-last_n:]

    return json.dumps({
        "tag": tag,
        "log_path": str(path),
        "match_count": len(matches),
        "returned": len(kept),
        "summary_lines": summary_lines,
        "lines": kept,
    }, indent=2)


@mcp.tool()
def run_capture_baseline(mission: str = "mc2_01", duration: int = 30) -> str:
    """
    Run a single-mission smoke capture and return the report summary.

    Launches run_smoke.py with --tier adhoc for one mission. Blocks until
    the run completes (roughly duration + ~15s startup). Returns the parsed
    report rows as text, or an error message.

    mission: mission stem, e.g. mc2_01, mc2_10, mc2_24 (default: mc2_01)
    duration: seconds per mission, clamped to 10..120 (default: 30)
    """
    if not _VALID_MISSION.match(mission):
        return f"Invalid mission name {mission!r}. Use alphanumeric + underscore, max 32 chars."

    duration = max(10, min(120, duration))

    # Pre-check: refuse to run if mc2.exe is already running (smoke lock is concurrency-safe
    # but a running game produces unreliable frames and the smoke lock may fail to acquire).
    try:
        import subprocess as _sp
        _chk = _sp.run(
            ["tasklist", "/fi", "IMAGENAME eq mc2.exe", "/nh", "/fo", "CSV"],
            capture_output=True, text=True, timeout=5,
        )
        if "mc2.exe" in _chk.stdout:
            return json.dumps({
                "error": "mc2_already_running",
                "message": "mc2.exe is already running. Close the game before running a capture baseline.",
                "action": "Close mc2.exe first, then retry.",
            })
    except Exception:
        pass  # If tasklist fails, proceed (don't block on check failure)

    # run_smoke.py contract: missions are passed as repeated --mission (there is
    # no --missions flag and no 'adhoc' tier), and --kill-existing is forbidden
    # (it taskkills concurrent mc2.exe -> false crash_silent; run_smoke already
    # holds a concurrency-safe lock). See scripts/check-smoke-matrices.py.
    cmd = [
        "py", "-3", str(_RUN_SMOKE),
        "--mission", mission,
        "--duration", str(duration),
        "--keep-logs",
    ]

    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=duration + 120,
            cwd=str(_WORKTREE_DIR),
        )
    except subprocess.TimeoutExpired:
        return f"run_smoke.py timed out after {duration + 120}s."
    except Exception as e:
        return f"Failed to launch run_smoke.py: {e}"

    artifact_dir = _latest_artifact_dir()
    if artifact_dir is None:
        return (
            f"run_smoke.py exited {result.returncode} but no artifact dir found.\n"
            f"stdout: {result.stdout[-1000:]}\nstderr: {result.stderr[-500:]}"
        )

    report_path = artifact_dir / "report.json"
    report = _read_json(report_path)
    if report is None:
        return (
            f"run_smoke.py exited {result.returncode}. No report.json in {artifact_dir}.\n"
            f"stdout: {result.stdout[-1000:]}"
        )

    lines = [
        f"exit: {result.returncode}",
        f"dir:  {artifact_dir.name}",
        f"tier: {report.get('tier', '?')}  profile: {report.get('profile', '?')}",
        "",
    ]
    for row in report.get("rows", []):
        r = row.get("result", "?")
        fps = row.get("avg_fps", "?")
        ready_ms = row.get("mission_ready_ms", "?")
        lines.append(f"  {row.get('stem', '?'):12s}  result={r}  fps={fps}  ready_ms={ready_ms}")

    return "\n".join(lines)


@mcp.tool()
def list_capture_sets() -> str:
    """
    List all smoke artifact sets, newest first.

    Each entry shows the timestamp directory name, tier, and per-mission results.
    Requires at least one prior run_capture_baseline() or run_smoke.py call.
    """
    if not _ARTIFACTS_DIR.exists():
        return f"Artifact directory not found: {_ARTIFACTS_DIR}"

    dirs = sorted(
        (d for d in _ARTIFACTS_DIR.iterdir() if d.is_dir()),
        key=lambda d: d.name,
        reverse=True,
    )
    if not dirs:
        return f"No artifact sets found in {_ARTIFACTS_DIR}"

    lines = [f"# Capture sets in {_ARTIFACTS_DIR} ({len(dirs)} total)"]
    for d in dirs:
        report = _read_json(d / "report.json")
        if report is None:
            lines.append(f"  {d.name}  (no report.json)")
            continue
        tier = report.get("tier", "?")
        rows = report.get("rows", [])
        summary = "  ".join(f"{r.get('stem', '?')}={r.get('result', '?')}" for r in rows)
        lines.append(f"  {d.name}  tier={tier}  {summary}")

    return "\n".join(lines)


@mcp.tool()
def get_latest_smoke_report() -> str:
    """
    Return structured result of the most recent smoke test run.

    Parses report.json from the newest artifact directory and returns:
    overall pass/fail, per-mission results with FPS, destroys_delta,
    artifact location, and staleness. Prefer this over reading log files directly.
    """
    artifact_dir = _latest_artifact_dir()
    if artifact_dir is None:
        return json.dumps({"error": "no_artifact_dir", "artifact_dir": None}, indent=2)

    report_path = artifact_dir / "report.json"
    report = _read_json(report_path)
    if report is None:
        return json.dumps({
            "error": "no_report_json",
            "artifact_dir": str(artifact_dir),
            "files": [f.name for f in sorted(artifact_dir.iterdir()) if f.is_file()],
        }, indent=2)

    # Staleness
    try:
        age_s = round(time.time() - report_path.stat().st_mtime, 1)
    except OSError:
        age_s = None

    rows = report.get("rows", [])
    overall = "PASS" if rows and all(r.get("result") == "PASS" for r in rows) else "FAIL"

    return json.dumps({
        "schema_version": 1,
        "overall": overall,
        "timestamp": report.get("timestamp"),
        "tier": report.get("tier"),
        "profile": report.get("profile"),
        "artifact_dir": str(artifact_dir),
        "artifact_dir_exists": artifact_dir.exists(),
        "seconds_since_run": age_s,
        "missions": [
            {
                "stem": r.get("stem"),
                "result": r.get("result"),
                "avg_fps": r.get("avg_fps"),
                "p1low_fps": r.get("p1low_fps"),
                "mission_ready_ms": r.get("mission_ready_ms"),
                "destroys_delta": r.get("destroys_delta"),
                "buckets": r.get("buckets"),
                "details": r.get("details"),
            }
            for r in rows
        ],
    }, indent=2)


# Diagnostic JSONL trace path (written by engine when MC2_DIAGNOSTIC_TRACE_FILE is set)
_DIAG_TRACE_PATH = _STATE_DIR / "diagnostic_trace.jsonl"

# Registered diagnostic tags (must match mc2_diag::knownTags() in diagnostic_trace.cpp)
_KNOWN_DIAG_TAGS: frozenset[str] = frozenset({
    "GPU_CULL",
    "LIGHTBAKE_PROOF",
    "ANIM_GATE",
    "SPFLUSH_COST_SPLIT",
    "TerrainLOD_prod",
    "TERRAIN_ACTIVE_AB",
    "TERRAIN_SOLID_AB",
    "CONFIG",
    "ENV",
    "BUILD",
    "DEVICE",
    "SHADER_COMPILE",
})


@mcp.tool()
def get_diagnostic_events(tag: str, last_n: int = 50) -> str:
    """
    Return the last N diagnostic events for a tag from diagnostic_trace.jsonl.

    tag: registered tag name (GPU_CULL, SPFLUSH_COST_SPLIT, ANIM_GATE, etc.)
         Use tag="*" to get all events regardless of tag.
         Unknown/unregistered tag returns an error with the known tag list.
    last_n: max events to return (default 50, capped at 500).

    Known tags with no matching events return an empty list (not an error).
    Requires engine running with MC2_DIAGNOSTIC_TRACE_FILE set (or default path)
    and the tag enabled via MC2_DIAG_TAGS.
    """
    last_n = max(1, min(500, last_n))

    # Validate tag
    if tag != "*" and tag not in _KNOWN_DIAG_TAGS:
        return json.dumps({
            "error": "unknown_tag",
            "tag": tag,
            "known_tags": sorted(_KNOWN_DIAG_TAGS),
            "tip": "Use tag='*' to see all events regardless of tag name.",
        }, indent=2)

    if not _DIAG_TRACE_PATH.exists():
        return json.dumps({
            "error": "no_trace_file",
            "path": str(_DIAG_TRACE_PATH),
            "tip": (
                "Engine must be running with MC2_DIAGNOSTIC_TRACE_FILE set "
                "(default: debug_state/diagnostic_trace.jsonl). "
                "Also ensure MC2_DIAG_TAGS includes the desired tag."
            ),
        }, indent=2)

    # Read and filter
    try:
        text = _DIAG_TRACE_PATH.read_text(encoding="utf-8")
    except OSError as e:
        return json.dumps({"error": "read_error", "detail": str(e)}, indent=2)

    events: list[dict] = []
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if tag == "*" or event.get("tag") == tag:
            events.append(event)

    events = events[-last_n:]

    return json.dumps({
        "tag": tag,
        "count": len(events),
        "trace_path": str(_DIAG_TRACE_PATH),
        "events": events,
    }, indent=2)


@mcp.tool()
def summarize_latest_capture() -> str:
    """
    Return a detailed summary of the most recent smoke artifact set.

    Shows tier, profile, fps_note, and per-mission result + FPS + counters.
    """
    artifact_dir = _latest_artifact_dir()
    if artifact_dir is None:
        return f"No artifact sets found in {_ARTIFACTS_DIR}"

    report = _read_json(artifact_dir / "report.json")
    if report is None:
        return f"No report.json in {artifact_dir}"

    lines = [
        f"dir:      {artifact_dir.name}",
        f"tier:     {report.get('tier', '?')}",
        f"profile:  {report.get('profile', '?')}",
        f"fps_note: {report.get('fps_note', '(none)')}",
        "",
    ]
    for row in report.get("rows", []):
        lines.append(f"mission: {row.get('stem', '?')}")
        lines.append(f"  result:      {row.get('result', '?')}")
        lines.append(f"  avg_fps:     {row.get('avg_fps', '?')}")
        lines.append(f"  p1low_fps:   {row.get('p1low_fps', '?')}")
        lines.append(f"  ready_ms:    {row.get('mission_ready_ms', '?')}")
        lines.append(f"  destroys_d:  {row.get('destroys_delta', '?')}")
        buckets = row.get("buckets", {})
        if buckets:
            lines.append(f"  buckets:     {json.dumps(buckets)}")
        details = row.get("details", "")
        if details:
            lines.append(f"  details:     {details}")

    return "\n".join(lines)


@mcp.tool()
def get_latest_artifact_paths() -> str:
    """
    List all files in the most recent smoke artifact directory with sizes.

    Useful for locating log files, screenshots, or the report itself after a capture.
    """
    artifact_dir = _latest_artifact_dir()
    if artifact_dir is None:
        return f"No artifact sets found in {_ARTIFACTS_DIR}"

    files = sorted(artifact_dir.iterdir(), key=lambda f: f.name)
    lines = [f"# {artifact_dir}"]
    for f in files:
        if f.is_file():
            size_kb = f.stat().st_size / 1024
            lines.append(f"  {f.name:<40s}  {size_kb:8.1f} KB")

    return "\n".join(lines)


# ---------------------------------------------------------------------------
# MCP-TOOLING-1: get_executor_health
# ---------------------------------------------------------------------------

@mcp.tool()
def get_executor_health() -> str:
    """
    Return the frame-graph executor split metrics + dryrun divergence in one call.

    Replaces manual `python -c "json.load(...)['frame_graph']..."` done each session.

    Reports from the dump's frame_graph block:
      Executor split metrics:
        executor_owned_wrappers, executor_validated_top_level_passes,
        executor_apply_state_passes, executor_scheduled_passes,
        executor_validation_failures, executor_skipped_deferred_passes,
        executor_owned_passes (legacy field, same as owned_wrappers in older builds)

      Dryrun divergence (MC2_FRAMEGRAPH_DRYRUN):
        enabled, frames, out_of_order, unobserved_total, observed_pass_count,
        terrain_mutex_violations, latch_miss_frames, known_early_suppressed

      Ambient + FBO ledger cross-checks:
        ambient_probe_mismatches, fbo_mismatches

    VERDICT line:
      "EXECUTOR CLEAN" — validation_failures==0 && out_of_order==0 &&
                         ambient_probe_mismatches==0 && fbo_mismatches==0
      "EXECUTOR FAIL"  — lists failing axes

    Requires engine running with MC2_DEBUG_STATE_DUMP=1.
    MC2_FRAMEGRAPH_EXECUTOR (default OFF) enables the executor path.
    MC2_FRAMEGRAPH_DRYRUN (default OFF) enables the dryrun observer.
    """
    data = _latest()
    if data is None:
        return _not_available()

    fg = data.get("frame_graph", {})
    if not fg:
        return (
            "frame_graph block absent from the dump — the running exe predates "
            "FRAME-GRAPH-SKELETON-1, or MC2_DEBUG_STATE_DUMP is off."
        )

    dr = fg.get("frame_graph_dryrun", {})

    # --- executor metrics ---
    val_fail  = fg.get("executor_validation_failures", 0)
    owned_w   = fg.get("executor_owned_wrappers", 0)
    owned_p   = fg.get("executor_owned_passes", owned_w)   # older name alias
    top_level = fg.get("executor_validated_top_level_passes", 0)
    apply_st  = fg.get("executor_apply_state_passes", 0)
    # PER-PASS-APPLY-COUNTERS-1 + Mech/Water slices: per-pass apply-state breakdown
    # (6 PostProcess + TerrainDecal/TerrainOverlay/StaticPropOpaque/MechOpaque/Water).
    # Absent on builds predating PER-PASS-APPLY-COUNTERS-1 — handled gracefully below.
    apply_by_pass = fg.get("executor_apply_state_by_pass", {})
    scheduled = fg.get("executor_scheduled_passes", 0)
    skipped   = fg.get("executor_skipped_deferred_passes", 0)

    # --- dryrun metrics ---
    dr_enabled   = dr.get("enabled", False)
    dr_frames    = dr.get("frames", 0)
    dr_ooo       = dr.get("out_of_order", 0)
    dr_unobs     = dr.get("unobserved_total", 0)
    dr_obs       = dr.get("observed_pass_count", 0)
    dr_terrain   = dr.get("terrain_mutex_violations", 0)
    dr_latch     = dr.get("latch_miss_frames", 0)
    dr_ke_supp   = dr.get("known_early_suppressed", 0)

    # --- ambient + fbo cross-checks ---
    amb_miss  = fg.get("ambient_probe_mismatches", 0)
    amb_samp  = fg.get("ambient_probe_samples", 0)
    fbo_miss  = fg.get("fbo_mismatches", 0)
    fbo_samp  = fg.get("fbo_samples", 0)
    # AMBIENT-VIEWPORT-PROBE-1: observe-only — displayed but NOT in fail_axes (measure-first).
    vp_miss   = fg.get("viewport_probe_mismatches", 0)
    vp_samp   = fg.get("viewport_probe_samples", 0)

    # --- verdict ---
    fail_axes: list[str] = []
    if val_fail not in (0, "?"):
        fail_axes.append(f"validation_failures={val_fail}")
    if dr_enabled and dr_ooo not in (0, "?"):
        fail_axes.append(f"out_of_order={dr_ooo}")
    if amb_miss not in (0, "?"):
        fail_axes.append(f"ambient_probe_mismatches={amb_miss}")
    if fbo_miss not in (0, "?"):
        fail_axes.append(f"fbo_mismatches={fbo_miss}")

    if fail_axes:
        verdict = "EXECUTOR FAIL — " + ", ".join(fail_axes)
    else:
        verdict = "EXECUTOR CLEAN"

    frame = data.get("frame", "?")

    def _flag(v, bad=True) -> str:
        """Return ' *** FAIL' if v is nonzero/bad."""
        nonzero = v not in (0, False, "?")
        return " *** FAIL" if (nonzero and bad) else ""

    lines = [
        verdict,
        f"frame: {frame}",
        "",
        "# Executor split",
        f"  executor_owned_wrappers:                {owned_w}",
        f"  executor_owned_passes:                  {owned_p}",
        f"  executor_validated_top_level_passes:    {top_level}",
        f"  executor_apply_state_passes:            {apply_st}",
        "",
        "# Apply-state by pass",
    ]

    if isinstance(apply_by_pass, dict) and apply_by_pass:
        for _pass_name, _pass_count in apply_by_pass.items():
            lines.append(f"  {_pass_name + ':':<38} {_pass_count}")
    else:
        lines.append(
            "  (per-pass map not in dump — pre-PER-PASS-APPLY-COUNTERS-1 build)"
        )

    lines += [
        "",
        f"  executor_scheduled_passes:              {scheduled}",
        f"  executor_skipped_deferred_passes:       {skipped}",
        f"  executor_validation_failures:           {val_fail}{_flag(val_fail)}",
        "",
        f"# Dryrun observer  (MC2_FRAMEGRAPH_DRYRUN enabled={dr_enabled})",
        f"  frames:                   {dr_frames}",
        f"  out_of_order:             {dr_ooo}{_flag(dr_ooo)}",
        f"  unobserved_total:         {dr_unobs}",
        f"  observed_pass_count:      {dr_obs}",
        f"  terrain_mutex_violations: {dr_terrain}{_flag(dr_terrain)}",
        f"  latch_miss_frames:        {dr_latch}{_flag(dr_latch)}",
        f"  known_early_suppressed:   {dr_ke_supp}",
        "",
        f"# Ambient + FBO ledger",
        f"  ambient_probe_samples:    {amb_samp}",
        f"  ambient_probe_mismatches: {amb_miss}{_flag(amb_miss)}",
        f"  fbo_samples:              {fbo_samp}",
        f"  fbo_mismatches:           {fbo_miss}{_flag(fbo_miss)}",
        f"  viewport_probe_samples:   {vp_samp}",
        f"  viewport_probe_mismatches:{vp_miss}  (observe-only, not a health gate)",
    ]

    banner = _stale_banner()
    if banner:
        lines.insert(0, banner.rstrip())

    age = _file_age_s()
    lines.append(f"\nfile_age: {age:.0f}s" if age is not None else "\nfile_age: unknown")
    liveness = _get_liveness()
    lines.append(f"live:     {liveness['live']}")

    return "\n".join(lines)


# ---------------------------------------------------------------------------
# MCP-ANTI-CHURN-1: runtime gate + sidecar queries from the last state dump.
# Kills the "is displacement actually on?" class of question without a game
# launch or log tailing.
# ---------------------------------------------------------------------------

_SIDECAR_FILES = ("control_map.png", "shoreline_mask.png",
                  "visual_height_4x.r32", "sidecar.json")
# Gate-name substrings related to sidecar-driven visuals, surfaced alongside
# the on-disk sidecar check so "file present but gate off" is visible at once.
_SIDECAR_GATE_HINTS = ("CONTROL_MAP", "SHORELINE", "VISUAL_HEIGHT",
                       "DISPLACE", "BEAUTY")


@mcp.tool()
def get_runtime_gate(name: str) -> str:
    """
    Answer "is gate X actually on in the running (or last-run) exe?" from the
    latest MC2_DEBUG_STATE_DUMP snapshot — no game launch, no log tailing.

    name: exact MC2_* gate name, or a substring ("DISPLACE", "SHADOW") to list
    every sampled gate containing it (case-insensitive).

    Returns JSON: {query, exact_match, matches:[{gate, active}], frame, live,
    stale_note}. Gates are sampled once at process start — this reflects the
    LAUNCH env of that exe, not your current shell. A gate absent from the
    dump means the running exe does not sample it into the features block
    (older build, or var read outside the registry) — absence is NOT "off".
    """
    data = _latest()
    if data is None:
        return _not_available()

    features = data.get("features", {})
    q = (name or "").strip()
    if not q:
        return json.dumps({"error": "gate name (or substring) required"}, indent=2)

    exact = q in features
    if exact:
        matches = [{"gate": q, "active": features[q]}]
    else:
        ql = q.lower()
        matches = [{"gate": k, "active": v} for k, v in features.items()
                   if ql in k.lower()]

    liveness = _get_liveness()
    return json.dumps({
        "query": q,
        "exact_match": exact,
        "matches": matches,
        "match_count": len(matches),
        "sampled_gates_total": len(features),
        "frame": data.get("frame"),
        "live": liveness["live"],
        "stale_note": _stale_banner().strip() or None,
        "note": ("gates are sampled at process start from the exe's launch env; "
                 "absence from the dump is NOT 'off' — it means unsampled"),
    }, indent=2)


@mcp.tool()
def get_mission_sidecars() -> str:
    """
    One call: which beauty sidecars exist on disk for the CURRENTLY LOADED
    mission (from the last state dump), in both the game and editor deploy
    dirs, plus any sampled gates related to sidecar-driven visuals.

    Checks <deploy>/data/missions/<stem>.{pak,fit} and
    <stem>.beauty/{control_map.png, shoreline_mask.png, visual_height_4x.r32,
    sidecar.json}. Answers "is the sidecar deployed AND is its gate on?"
    without a launch (kills the pak/dir spelunking loop).
    """
    data = _latest()
    if data is None:
        return _not_available()

    stem = (data.get("mission", {}) or {}).get("name") or ""
    stem = str(stem).strip()
    if not stem:
        return json.dumps({"error": "no mission name in the latest dump"}, indent=2)

    def _scan(root: Path) -> dict:
        md = root / "data" / "missions"
        beauty = md / f"{stem}.beauty"
        present = {}
        for fname in _SIDECAR_FILES:
            p = beauty / fname
            if p.is_file():
                present[fname] = p.stat().st_size
        return {
            "deploy_dir": str(root),
            "pak": (md / f"{stem}.pak").is_file(),
            "fit": (md / f"{stem}.fit").is_file(),
            "beauty_dir": beauty.is_dir(),
            "sidecars_present": present,
        }

    features = data.get("features", {})
    related = {k: v for k, v in features.items()
               if any(h in k.upper() for h in _SIDECAR_GATE_HINTS)}

    return json.dumps({
        "mission": stem,
        "frame": data.get("frame"),
        "game_deploy": _scan(_DEPLOY_DIR),
        "editor_deploy": _scan(_EDITOR_DEPLOY_DIR),
        "related_gates": related,
        "stale_note": _stale_banner().strip() or None,
        "convention": "data/missions/<stem>.beauty/ (control_map_tool.py / "
                      "cook_shoreline.py install; mclib/terrain.cpp load)",
    }, indent=2)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    mcp.run(transport="stdio")
