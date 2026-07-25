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


@dataclass
class RunConfig:
    exe: List[str]                      # argv[0..n]; usually [exe_path, --profile, stock, --mission, stem, --duration, d]
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
    env["MC2_SMOKE_MODE"] = "1"
    # MC2_LOG master gate: keep the engine's stdout telemetry live so the smoke
    # harness can parse [SMOKE v1]/[PERF v1]/oracle tags. Without it the engine
    # redirects stdout -> NUL (default, so NORMAL PLAY never hitches on the
    # unbuffered per-mission printf flush). See gameosmain.cpp after setvbuf.
    env["MC2_LOG"] = "1"
    env["MC2_HEARTBEAT"] = "1"
    env["MC2_TGL_POOL_TRACE"] = "1"
    env["MC2_ASSET_SCALE_TRACE"] = "1"
    # Engine-side cursor-theft suppression (gos_CursorControlSuppressed,
    # f0c07b87). MC2_SMOKE_MODE already implies it, but set it explicitly so
    # the intent survives if the smoke-mode implication is ever narrowed.
    # NOTE: this only works on exes built AFTER f0c07b87 -- the harness-side
    # CursorGuard below is what covers older deploy lanes.
    env["MC2_NO_CURSOR_GRAB"] = "1"
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
    # environment; setdefault means an explicit value always wins).
    #
    # Same shape as MC2_NO_CURSOR_GRAB above (f0c07b87): this is the single
    # shared launcher behind run_smoke / run_visual_capture / capture_baseline /
    # quick_shot / mech_shot / smoke_with_screenshots / renderdoc_capture /
    # camera_sweep_smoke, so no per-harness edit is needed. Interactive play
    # never goes through here, and the engine-side default stays OFF.
    env.setdefault("MC2_FORCE_ALL_VISIBLE", "1")
    env.pop("MC2_GL_ERROR_DRAIN_SILENT", None)
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
