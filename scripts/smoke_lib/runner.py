# scripts/smoke_lib/runner.py
"""Per-mission spawn/parse/timeout loop."""
from __future__ import annotations

import os
import queue
import subprocess
import threading
import time
from dataclasses import dataclass
from typing import List, Optional

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
    env["MC2_HEARTBEAT"] = "1"
    env["MC2_TGL_POOL_TRACE"] = "1"
    env["MC2_ASSET_SCALE_TRACE"] = "1"
    env.pop("MC2_GL_ERROR_DRAIN_SILENT", None)
    env.update(cfg.env_extra)

    cap = cfg.duration + cfg.grace_s
    # t0 must be captured BEFORE spawning the reader thread so both the reader
    # and the main loop share the same monotonic reference point.
    t0 = time.monotonic()
    proc = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            env=env, text=True, bufsize=1)

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
    walltime = time.monotonic() - t0
    stdout = "\n".join(lines)

    summary = parse_log(stdout, line_wallclocks=line_wallclocks)
    gcfg = GateConfig(
        heartbeat_timeout_load_s=cfg.heartbeat_timeout_load_s,
        heartbeat_timeout_play_s=cfg.heartbeat_timeout_play_s,
        duration_s=cfg.duration,
    )
    verdict = evaluate(summary, gcfg, exit_code=proc.returncode,
                       walltime_s=walltime, killed_by_timeout=killed)
    return RunResult(summary=summary, verdict=verdict, stdout_text=stdout or "",
                     exit_code=proc.returncode, walltime_s=walltime,
                     killed_by_timeout=killed)
