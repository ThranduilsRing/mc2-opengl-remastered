# scripts/smoke_lib/runner.py
"""Per-mission spawn/parse/timeout loop."""
from __future__ import annotations

import os
import queue
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional

from .cursor_guard import CursorGuard
from .gates import GateConfig, Verdict, evaluate
from .logparse import LogSummary, parse_log


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
    "MC2_TGL_POOL_TRACE": "1",
    "MC2_ASSET_SCALE_TRACE": "1",
    # Engine-side cursor-theft suppression (gos_CursorControlSuppressed,
    # f0c07b87). MC2_SMOKE_MODE already implies it, but set it explicitly so
    # the intent survives if the smoke-mode implication is ever narrowed.
    # NOTE: this only works on exes built AFTER f0c07b87 -- the harness-side
    # CursorGuard below is what covers older deploy lanes.
    "MC2_NO_CURSOR_GRAB": "1",
    # BACKGROUND-SMOKE-1: never take the display fullscreen. Without this the
    # engine inherits options.cfg FullScreen=TRUE and a tier1 smoke covers the
    # user's screen -- STARTUPINFO's SW_SHOWMINNOACTIVE does NOT prevent it,
    # because SDL calls ShowWindow(SW_SHOW) explicitly and that ignores
    # STARTUPINFO. Hard override at both engine decision points; see
    # smoke_lib/nofocus.py.
    "MC2_WINDOWED": "1",
}

SOFT_ENV: dict = {
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
    # Deleted so the GL error drain always speaks during a smoke run.
    "MC2_GL_ERROR_DRAIN_SILENT",
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
    # META-FIX INFRA-3: per-mission marker-bucket downgrades (bucket -> True).
    marker_overrides: dict = field(default_factory=dict)
    show_window: bool = False


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
    ]


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
    for _k, _v in FORCED_ENV.items():
        env[_k] = _v
    for _k, _v in SOFT_ENV.items():
        env.setdefault(_k, _v)
    for _k in POPPED_ENV:
        env.pop(_k, None)
    env.update(cfg.env_extra)

    cap = cfg.duration + cfg.grace_s
    # Spawn in the directory that contains the executable so that MC2's
    # CWD-relative file lookups (options.cfg, minprefs.cfg, etc.) resolve
    # against the install directory rather than the runner's working directory.
    # Matches the cwd=GAME_DIR precedent in scripts/game_auto.py.
    exe_dir = str(Path(cfg.exe[0]).resolve().parent)
    # Start the game window minimized so smoke runs don't steal focus or
    # interrupt the user's desktop. The game detects SDL_WINDOW_MINIMIZED and
    # throttles to ~100 fps (10ms sleep) but game logic and stdout logging
    # keep running, so heartbeat/ok gates are unaffected. Perf runs can opt into
    # a visible window to avoid that background throttle.
    startupinfo = None
    if sys.platform == "win32":
        startupinfo = subprocess.STARTUPINFO()
        startupinfo.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        # Default SW_SHOWMINNOACTIVE(7): minimized, no focus steal -- but SDL
        # then throttles to ~100fps, so Avg FPS is NOT GPU throughput.
        # MC2_SMOKE_VISIBLE=1 -> SW_SHOWNOACTIVATE(4); cfg.show_window -> SW_SHOW(5).
        startupinfo.wShowWindow = (5 if getattr(cfg, "show_window", False)
                                   else (4 if os.environ.get("MC2_SMOKE_VISIBLE") else 7))
    # t0 must be captured BEFORE spawning the reader thread so both the reader
    # and the main loop share the same monotonic reference point.
    t0 = time.monotonic()
    # CURSOR-STEAL-HARNESS-GUARD-1: exe-independent protection for the user's
    # pointer. Older deploy-lane exes (predating f0c07b87) still ClipCursor()
    # and warp to the window centre every frame; an engine-side fix cannot
    # reach a binary built before it. The guard releases the clamp on a timer
    # and snaps the pointer back whenever the game moves it, then restores the
    # anchor in the finally below (and via atexit if we are hard-killed).
    # Inert on non-win32 / headless, or with MC2_NO_CURSOR_GUARD=1.
    cursor_guard = CursorGuard().start()
    try:
        proc = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                env=env, text=True, bufsize=1, cwd=exe_dir,
                                startupinfo=startupinfo)
    except BaseException:
        cursor_guard.stop()
        raise

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
    lines: list[str] = []
    line_wallclocks: list[float] = []
    killed = False
    line_q: queue.Queue[tuple] = queue.Queue()
    reader = threading.Thread(target=_reader_thread,
                              args=(proc.stdout, line_q, t0), daemon=True)
    reader.start()
    try:
        while True:
            if time.monotonic() - t0 > cap:
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
    finally:
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=2)
        # Drain any remaining items so the reader thread can unblock and exit.
        reader.join(timeout=2)
        # Release the clamp and put the pointer back on the user's anchor. Runs
        # on every exit path -- normal EOF, walltime kill, or an exception out
        # of the read loop. (atexit is only the last-resort net for a hard kill
        # of the harness itself.)
        cursor_guard.stop()
    walltime = time.monotonic() - t0
    stdout = "\n".join(lines)

    summary = parse_log(stdout, line_wallclocks=line_wallclocks)
    gcfg = GateConfig(
        heartbeat_timeout_load_s=cfg.heartbeat_timeout_load_s,
        heartbeat_timeout_play_s=cfg.heartbeat_timeout_play_s,
        duration_s=cfg.duration,
        allow_asset_oob=cfg.allow_asset_oob,
        marker_overrides=cfg.marker_overrides,
    )
    verdict = evaluate(summary, gcfg, exit_code=proc.returncode,
                       walltime_s=walltime, killed_by_timeout=killed)
    return RunResult(summary=summary, verdict=verdict, stdout_text=stdout or "",
                     exit_code=proc.returncode, walltime_s=walltime,
                     killed_by_timeout=killed)
