# scripts/smoke_lib/runner.py
"""Per-mission spawn/parse/timeout loop."""
from __future__ import annotations

import os
import json
import queue
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath
from typing import List, Optional

from . import nofocus, stall, vk_validation
from .gates import GateConfig, Verdict, evaluate, vegetation_cards_on
from .logparse import LogSummary, parse_log
from .firewall_dialog import Guard as FirewallGuard


def _reader_thread(stream, q: "queue.Queue[tuple[str, float] | tuple[None, float]]",
                   t0: float) -> None:
    """Read lines from stream and push (line, stamp) tuples to queue; sentinel (None, 0.0) on EOF.

    Stamping at read time rather than dequeue time eliminates the wallclock
    bias introduced by queue backpressure (up to 100ms per line under load).

    Runs in a daemon thread so readline() blocking on Windows pipes does not
    hold up the main timeout loop. The OS will clean up the thread when the
    process dies and the pipe closes.
    """
    try:
        for line in stream:
            q.put((line.rstrip("\n"), time.monotonic() - t0))
    finally:
        q.put((None, 0.0))  # EOF sentinel


# ---------------------------------------------------------------------------
# ENV COMPOSITION CONTRACT (HARNESS-HYGIENE-3)
# ---------------------------------------------------------------------------
# The child env is built as:
#
#     env = os.environ.copy()      # <-- ambient MC2_* DOES reach the engine
#     env.update(FORCED_ENV)       # hard overrides; ambient value is DISCARDED
#     env.setdefault(SOFT_ENV)     # defaults; an ambient value WINS
#     env.pop(POPPED_ENV)          # deleted outright
#     env.update(cfg.env_extra)    # per-run values; last writer wins
#
# CORRECTION OF A LONG-STANDING FALSE PREMISE: several comments in run_smoke.py
# and mc2_env_pin.py claim this runner "launches with a from-scratch env={...}
# dict" so unlisted MC2_* vars are "silently dropped by the Popen
# env-replacement". That is NOT true and has not been true: line 1 above is
# os.environ.copy(), so the ambient environment is inherited in full. The
# run_smoke "allowlist" entries that merely copy os.environ values into
# env_extra are therefore no-ops -- they re-set a var to the value it already
# has.
#
# What IS genuinely undriveable is exactly the three tables below (plus any
# env_extra key whose computed value differs from the ambient one): for those,
# and ONLY those, setting the var in your shell has no effect on the child.
FORCED_ENV: dict = {
    # Smoke-harness identity. Interactive play never goes through here.
    "MC2_SMOKE_MODE": "1",
    # MC2_LOG master gate: keep the engine's stdout telemetry live so the smoke
    # harness can parse [SMOKE v1]/[PERF v1]/oracle tags. Without it the engine
    # redirects stdout -> NUL (default, so NORMAL PLAY never hitches on the
    # unbuffered per-mission printf flush). See gameosmain.cpp after setvbuf.
    "MC2_LOG": "1",
    "MC2_HEARTBEAT": "1",
    # Typed gate registry (Gates/): a bad value or an MC2_* variable that is not a gate
    # (a typo in a harness, a renamed gate) ends the engine at startup with exit code 78
    # and `[CONFIG v1] warn=...` lines, instead of silently measuring the default config.
    # Exes built before Gates/ landed ignore it.
    "MC2_CONFIG_STRICT": "1",
    "MC2_ASSET_SCALE_TRACE": "1",
    # Engine-side cursor-theft suppression (gos_CursorControlSuppressed,
    # f0c07b87). MC2_SMOKE_MODE already implies it, but set it explicitly so
    # the intent survives if the smoke-mode implication is ever narrowed.
    # NOTE: this only works on exes built AFTER f0c07b87; nofocus.spawn refuses
    # any older exe (exe_guard.check_exe, MIN_COMMIT), so no exe here lacks it.
    "MC2_NO_CURSOR_GRAB": "1",
    # BACKGROUND-SMOKE-1: never take the display fullscreen. Without this the
    # engine inherits options.cfg FullScreen=TRUE and a tier1 smoke covers the
    # user's screen -- STARTUPINFO's SW_SHOWMINNOACTIVE does NOT prevent it,
    # because SDL calls ShowWindow(SW_SHOW) explicitly and that ignores
    # STARTUPINFO. Hard override at both engine decision points; see
    # smoke_lib/nofocus.py.
    "MC2_WINDOWED": "1",
    # NOFOCUS-FINISH-1: and never occupy visible desktop area either. WINDOWED
    # only demotes fullscreen to a window the size of options.cfg ResolutionX/Y
    # -- measured live at 1719x995 on a 1707x960 desktop, i.e. indistinguishable
    # from fullscreen to the user, which is the actual complaint.
    #
    # FORCED, not SOFT, and set HERE rather than inherited from
    # nofocus.OFFSCREEN_ENV: run_one() below builds its own STARTUPINFO and its
    # own subprocess.Popen and never calls nofocus.apply_env(), so the nofocus
    # default cannot reach the child. This is the tier-1 smoke path -- the most
    # frequently launched engine process in the project.
    #
    # Not in tension with cfg.show_window: an offscreen window is neither
    # MINIMIZED nor HIDDEN, and gameosmain.cpp's background throttle keys on
    # exactly those two SDL flags (`SDL_GetWindowFlags & (MINIMIZED|HIDDEN)`),
    # so a perf run still measures real throughput. Pixel-neutral: the GL
    # drawable keeps its full size.
    "MC2_WINDOW_OFFSCREEN": "1",
}

SOFT_ENV: dict = {
    # Agent launches are SILENT (user is at the desk): SDL dummy audio driver.
    # Soft, so an explicit SDL_AUDIODRIVER in the shell still wins.
    "SDL_AUDIODRIVER": "dummy",
    # FORCE-ALL-VISIBLE-1. Every automated capture marks all units visible.
    #
    # WHY: units hidden by the sensor/contact fade were repeatedly graded as
    # evidence, producing false negatives all session -- a ghost-cone leak
    # passed verification at four bookmark poses only because no mover happened
    # to be mid-fade in view, and vehicle-lighting lanes repeatedly captured
    # scenes containing no visible vehicle at all.
    #
    # CONSEQUENCE: a smoke capture is NO LONGER a fog-of-war-accurate frame.
    # Any test that specifically validates sensor / contact / visibility
    # behaviour MUST pass MC2_FORCE_ALL_VISIBLE=0 (env_extra or the outer
    # environment; setdefault means an explicit value always wins -- which is
    # why this lives in SOFT_ENV and is fully driveable from a shell).
    "MC2_FORCE_ALL_VISIBLE": "1",
}

POPPED_ENV: frozenset = frozenset({
    # Empty on purpose: no live gate needs its ambient value deleted. A name goes here only while a retired gate's stale shell value could still change a run.
})


def undriveable_ambient(environ: dict, env_extra: "dict | None" = None) -> dict:
    """Ambient MC2_* vars this runner CANNOT deliver to the engine.

    Returns {name: (ambient_value, effective_value_or_None, reason)} for every
    MC2_* var that is set in `environ` but whose value the child will NOT see.
    An empty dict means every ambient MC2_* var reaches mc2.exe unchanged.

    This is the honest replacement for the old "not in the allowlist" test,
    which flagged ~600 vars that in fact pass through untouched.
    """
    env_extra = env_extra or {}
    out: dict = {}
    for name, ambient in environ.items():
        if not name.startswith("MC2_"):
            continue
        if name in POPPED_ENV:
            out[name] = (ambient, None,
                         "deleted by the runner (POPPED_ENV)")
            continue
        if name in env_extra:
            eff = env_extra[name]
            if eff != ambient:
                out[name] = (ambient, eff,
                             "overwritten by this run's env_extra")
            continue
        if name in FORCED_ENV and FORCED_ENV[name] != ambient:
            out[name] = (ambient, FORCED_ENV[name],
                         "hard-forced by the runner (FORCED_ENV)")
    return out


_GATE_NAMES = None
_LEGACY_GATES = None


def legacy_gate_names(table_json: str) -> frozenset:
    """MC2_* names in a gate_table.json text (an older commit's `git show <sha>:Gates/gate_table.json`)."""
    import json as _json
    return frozenset(g["name"] for g in _json.loads(table_json)["gates"])


def set_legacy_gates(names) -> None:
    """--legacy-env: run an OLDER exe (A/B against a past build). Its strict config rejects MC2_* names its gate table lacks,
    so every env this runner would pass is filtered to `names` (see filter_legacy_env)."""
    global _LEGACY_GATES
    _LEGACY_GATES = frozenset(names) if names is not None else None


def filter_legacy_env(env: dict) -> list:
    """Drop MC2_* keys the legacy exe does not know, in place. -> the dropped names (sorted); a no-op unless set_legacy_gates ran."""
    if _LEGACY_GATES is None:
        return []
    dropped = sorted(k for k in env if k.startswith("MC2_") and k not in _LEGACY_GATES)
    for k in dropped:
        del env[k]
    return dropped


def known_gate_names() -> "frozenset | None":
    """Every MC2_* name in the compiled gate table (Gates/gate_table.json), or None when it cannot be read."""
    global _GATE_NAMES
    if _GATE_NAMES is None:
        try:
            import sys as _sys
            _sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
            import gate_table_reader
            _GATE_NAMES = frozenset(g["name"] for g in gate_table_reader.load_table(
                Path(__file__).resolve().parents[2])["gates"])
        except Exception:                                              # noqa: BLE001
            return None
    return _GATE_NAMES


# GameOS/gameos/diagnostic_trace.cpp kDefaultTags (what MC2_DIAG_TAGS unset means) and the tags a runner-owned run adds on top.
QUIET_TAG_DEFAULTS = "GPU_CULL,LIGHTBAKE_PROOF,ANIM_GATE,TerrainLOD_prod,CONFIG,BUILD,DEVICE,SHADER_COMPILE"
RUNNER_DIAG_TAGS = ("PERF_COUNTERS", "CULL_PARITY", "LIGHT_CONTRACT", "CURSOR", "LIGHTBAKE", "CAPTURE_FOCUS", "STATIC_LIGHT_ROW")


def unregistered_ambient(environ: dict, env_extra: "dict | None" = None) -> dict:
    """Ambient MC2_* variables that are not gates: {name: value}.

    These are harness-only knobs (win_input, nofocus, run_smoke require-flags) that Python reads
    from its own environment, or typos. The engine runs under MC2_CONFIG_STRICT=1, where an unknown MC2_* name is
    fatal, so the runner does not forward them. A name this run sets itself (env_extra, FORCED_ENV, SOFT_ENV) is
    never dropped: a typo THERE must still fail at startup.
    """
    names = known_gate_names()
    if names is None:
        return {}
    keep = set(env_extra or {}) | set(FORCED_ENV) | set(SOFT_ENV)
    return {k: v for k, v in environ.items()
            if k.startswith("MC2_") and k not in names and k not in keep}


def reportable_unregistered(environ: dict, env_extra: "dict | None" = None) -> dict:
    """unregistered_ambient() minus the harness-only names (slot/gate/host-lease plumbing every lane shell sets). Callers still
    DROP the full unregistered set from the engine env; only the ENV-DROPPED warning and --strict-env use this narrower view."""
    import sys as _sys
    _sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    import mc2_env_pin
    return {k: v for k, v in unregistered_ambient(environ, env_extra).items() if not mc2_env_pin.is_harness_only(k)}


@dataclass
class RunConfig:
    exe: List[str]                    # argv[0..n]; usually [exe_path, --profile, stock, --mission, stem, --duration, d]
    profile: str
    stem: str
    duration: int
    heartbeat_timeout_load_s: int
    heartbeat_timeout_play_s: int
    grace_s: int                        # walltime cap = duration + grace
    env_extra: dict
    allow_asset_oob: bool = False
    # Default-build quiet sample: leave the runner-added diag tags (PERF_COUNTERS, CULL_PARITY) off so the log is what a shipping build prints.
    quiet_default: bool = False
    content_error_budget: int = 0
    # META-FIX INFRA-3: per-mission marker-bucket downgrades (bucket -> True).
    marker_overrides: dict = field(default_factory=dict)
    show_window: bool = False
    headless: bool = False              # --headless: no window/GPU/audio (stage 1 of docs/sim/headless-load.md)
    ticks: int = 0                      # --ticks N: quit after N sim ticks (needs MC2_SMOKE_FIXED_TIMESTEP=1); duration stays the timeout


@dataclass
class RunResult:
    summary: LogSummary
    verdict: Verdict
    stdout_text: str
    exit_code: int
    walltime_s: float
    killed_by_timeout: bool = False


def _build_argv(base_exe: List[str], cfg: RunConfig) -> List[str]:
    return list(base_exe) + [
        "--profile", cfg.profile,
        "--mission", cfg.stem,
        "--duration", str(cfg.duration),
    ] + (["--ticks", str(cfg.ticks)] if cfg.ticks > 0 else []) + (["--headless"] if cfg.headless else [])


def box_validation_layer(env: dict) -> str:
    """Pin box smoke to the SDK, including runs outside box_run's validation row."""
    if sys.platform != "linux" or env.get("MC2_TOOLCHAINS") != "/workspace/toolchains":
        return ""
    layers = PurePosixPath(env["MC2_TOOLCHAINS"]) / "vvl-1.4.350.0/explicit_layer.d"
    manifest = Path(layers / "VkLayer_khronos_validation.json")
    layer = json.loads(manifest.read_text())["layer"]
    if layer["api_version"] != "1.4.350" or not Path(layer["library_path"]).is_file():
        raise RuntimeError(f"Box validation SDK missing or mismatched: {manifest}")
    env["VK_LAYER_PATH"] = str(layers)
    env.pop("VK_ADD_LAYER_PATH", None)
    return (f"[runner] validation_layer manifest={manifest} api={layer['api_version']} "
            f"impl={layer['implementation_version']} library={layer['library_path']} "
            f"preset={env.get('MC2_VULKAN_VALIDATION', 'off')} VK_ADD_LAYER_PATH=<unset>")


def run_one(cfg: RunConfig) -> RunResult:
    # Test fixtures (fake_mc2_*.py) don't parse argv; if the caller already
    # embedded ANY smoke flag, assume they've embedded all of them and pass
    # cfg.exe through verbatim. Production callers (the runner CLI) always
    # use _build_argv which appends all three.
    has_embedded = any(
        a.startswith("--mission") or a.startswith("--profile") or a.startswith("--duration")
        for a in cfg.exe
    )
    argv = list(cfg.exe) if has_embedded else _build_argv(cfg.exe, cfg)

    env = os.environ.copy()
    env.update(cfg.env_extra)
    layer_selection = box_validation_layer(env)
    for _k in unregistered_ambient(env, cfg.env_extra):
        env.pop(_k, None)
    for _k, _v in FORCED_ENV.items():
        env[_k] = _v
    for _k, _v in SOFT_ENV.items():
        env.setdefault(_k, _v)
    for _k in POPPED_ENV:
        env.pop(_k, None)
    env.update(cfg.env_extra)
    if layer_selection:
        print(layer_selection, flush=True)
    # [PERF_COUNTERS v1] / [CULL_PARITY v1] print only with their diag tag (docs/plans/2026-11-02-quiet-tags.md). MC2_DIAG_TAGS replaces the engine's
    # default list, so restate it. Every runner-owned run (smoke, bench, census, profilers) turns the tags on; a quiet sample does not.
    if not (cfg.quiet_default or os.environ.get("MC2_SMOKE_QUIET_DEFAULT") == "1"):
        _tags = env.get("MC2_DIAG_TAGS") or QUIET_TAG_DEFAULTS
        env["MC2_DIAG_TAGS"] = _tags + "".join(f",{t}" for t in RUNNER_DIAG_TAGS if t not in _tags)
    _legacy_dropped = filter_legacy_env(env)
    if _legacy_dropped:
        print("[runner] [LEGACY-ENV] not known to the older exe's gate table, not forwarded: " + ", ".join(_legacy_dropped), file=sys.stderr)

    cap = cfg.duration + cfg.grace_s
    # Spawn in the directory that contains the executable so that MC2's
    # CWD-relative file lookups (options.cfg, minprefs.cfg, etc.) resolve
    # against the install directory rather than the runner's working directory.
    # Matches the cwd=GAME_DIR precedent in scripts/game_auto.py.
    exe_dir = str(Path(cfg.exe[0]).resolve().parent)
    # OWNER RULE: the game ALWAYS launches in the background. The Popen below
    # goes through nofocus.spawn, which owns the three halves of that contract
    # (STARTUPINFO verb, engine-side env, offscreen backstop). This block used
    # to hand-roll its own STARTUPINFO and map cfg.show_window to
    # SW_SHOW(5) -- an ACTIVATING verb -- so every `--show-window` perf smoke
    # stole focus. show_window now only selects SW_SHOWNOACTIVATE(4) over
    # SW_SHOWMINNOACTIVE(7): a perf run must not be MINIMIZED (SDL throttles a
    # minimized window to ~100 fps, so Avg FPS would not be GPU throughput),
    # and the window is parked offscreen (MC2_WINDOW_OFFSCREEN, FORCED_ENV) so
    # it is neither minimized nor on the desktop.
    show_window = bool(getattr(cfg, "show_window", False)
                       or os.environ.get("MC2_SMOKE_VISIBLE"))
    # small_window=False: smoke grades perf/pixels at options.cfg size, so
    # the 640x480 MC2_WIDTH/HEIGHT default must not be injected here.
    proc = nofocus.spawn(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         env=env, text=True, bufsize=1, cwd=exe_dir,
                         show_window=show_window, small_window=False)
    # The host-lease wait belongs to spawn, not the child's walltime budget.
    # Capture this before the reader starts so line stamps use the same clock.
    t0 = time.monotonic()

    # Stream lines and record per-line wallclock so freeze detection can use
    # runner walltime rather than engine-emitted elapsed_ms (which is mission-
    # relative and drifts during load pauses).
    #
    # The reader thread stamps each line at read time so per-line wallclocks
    # reflect when the line was received, not when the main loop dequeued it.
    # This eliminates up to 100ms of bias per line under queue backpressure.
    #
    # On Windows, readline() on a pipe blocks until data arrives even after the
    # process is killed — the pipe handle stays open until the OS cleans it up.
    # To allow the walltime cap to fire promptly we push reads onto a background
    # daemon thread and poll a queue with a short timeout on the main thread.
    lines: list[str] = [layer_selection] if layer_selection else []
    firewall = FirewallGuard(Path(argv[0]))
    line_wallclocks: list[float] = [0.0] if layer_selection else []
    killed = False
    line_q: queue.Queue[tuple] = queue.Queue()
    reader = threading.Thread(target=_reader_thread,
                              args=(proc.stdout, line_q, t0), daemon=True)
    reader.start()
    first_frame_at: Optional[float] = None
    last_line_at = 0.0
    try:
        while True:
            firewall.poll()
            now = time.monotonic() - t0
            if stall.walltime_exceeded(now, cap, cfg.duration, first_frame_at, now - last_line_at):
                proc.kill()
                killed = True
                break
            try:
                item, stamp = line_q.get(timeout=0.1)
            except queue.Empty:
                # No data yet; loop back to re-check the walltime cap.
                continue
            if item is None:
                # EOF sentinel from reader thread — process has exited.
                break
            lines.append(item)
            line_wallclocks.append(stamp)
            last_line_at = stamp
            if first_frame_at is None and "event=first_frame " in item:
                first_frame_at = stamp
    finally:
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=2)
        # Drain any remaining items so the reader thread can unblock and exit.
        reader.join(timeout=2)
        firewall.poll(final=True)
    walltime = time.monotonic() - t0
    stdout = "\n".join(lines)

    summary = parse_log(stdout, line_wallclocks=line_wallclocks)
    from . import render_options_pin
    options = render_options_pin.record(Path(exe_dir), env)["values"]
    # MC2_VEGETATION_CARDS defaults off (owner directive): the vegetation floors apply only when the run turned it on.
    vegetation_expected = (options["ShadowQuality"] != 0 and options.get("BaseQuality", 2) == 2
                           and vegetation_cards_on(env))
    gcfg = GateConfig(
        heartbeat_timeout_load_s=cfg.heartbeat_timeout_load_s,
        heartbeat_timeout_play_s=cfg.heartbeat_timeout_play_s,
        duration_s=cfg.duration,
        allow_asset_oob=cfg.allow_asset_oob,
        marker_overrides=cfg.marker_overrides,
        validation_requested=vk_validation.requested(env),
        stem=cfg.stem,
        diag_tags=frozenset(t.strip() for t in env.get("MC2_DIAG_TAGS", "").split(",") if t.strip()),
        vegetation_expected=vegetation_expected,
        presentation_disabled=env.get("MC2_PRESENTATION_DISABLED", "").lower() in ("1", "true", "yes", "on"),
        content_error_budget=cfg.content_error_budget,
    )
    verdict = evaluate(summary, gcfg, exit_code=proc.returncode,
                       walltime_s=walltime, killed_by_timeout=killed)
    return RunResult(summary=summary, verdict=verdict, stdout_text=stdout or "",
                     exit_code=proc.returncode, walltime_s=walltime,
                     killed_by_timeout=killed)
