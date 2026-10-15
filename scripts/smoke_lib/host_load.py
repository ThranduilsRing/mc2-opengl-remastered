"""Host-load gate: before an engine launch, wait (bounded) while the host is busy with something that skews a smoke.

WHAT: a run is "loaded" when another engine (mc2.exe / EditRel.exe) is running, when a compiler/linker burst (cl.exe, clang-cl.exe,
link.exe, ninja.exe) exceeds a process count, or when Defender's scanner (MsMpEng.exe) uses more than a CPU percentage. The gate prints
one line naming the cause, waits, and hands back a record the smoke report carries (`host_load` in report.json).
WHY: heartbeat_freeze_play and other timing buckets flaked with no trace of why; a run that started under a build burst or a Defender
scan now says so in its own report. The wait is bounded and never fails the run: after the bound the launch proceeds and the record says
`timed_out`, so a busy host costs time, not a red verdict nobody can explain.
Like the host-lease wait, this wait happens before the engine starts, so it never eats the mission's run budget (which starts at spawn).

Knobs: MC2_HOST_LOAD=off (skip; the record says so), MC2_HOST_LOAD_BUILD_MAX (compiler/linker processes tolerated, default 4),
MC2_HOST_LOAD_DEFENDER_PCT (MsMpEng CPU percent of one core tolerated, default 50), MC2_HOST_LOAD_WAIT_S (bound, default 300).
"""
from __future__ import annotations

import os
import sys
import time
from typing import Callable, Optional

BUILD_IMAGES = ("cl.exe", "clang-cl.exe", "link.exe", "lld-link.exe", "ninja.exe")
ENGINE_IMAGES = ("mc2.exe", "editrel.exe")
DEFENDER_IMAGE = "msmpeng.exe"
DEFAULT_BUILD_MAX = 4
DEFAULT_DEFENDER_PCT = 50.0
DEFAULT_WAIT_S = 300.0
POLL_S = 5.0
REPORT_EVERY_S = 30.0


def _env_float(name: str, default: float, log: Callable[[str], None]) -> float:
    raw = os.environ.get(name)
    if not raw:
        return default
    try:
        return float(raw)
    except ValueError:
        log(f"[host-load] {name}={raw!r} is not a number; using {default}")
        return default


def sample(interval_s: float = 0.5) -> dict:
    """-> {"builds": {image: count}, "engines": [(pid, image)], "defender_pct": float | None}. Defender's CPU is measured over
    `interval_s` from its cpu times; None when the process is absent or its times are not readable (protected process)."""
    import psutil
    builds: dict = {}
    engines = []
    defender = None
    for p in psutil.process_iter(["name", "pid"]):
        name = (p.info.get("name") or "").lower()
        if name in BUILD_IMAGES:
            builds[name] = builds.get(name, 0) + 1
        elif name in ENGINE_IMAGES and p.info["pid"] != os.getpid():
            engines.append((p.info["pid"], name))
        elif name == DEFENDER_IMAGE:
            defender = p
    pct = None
    if defender is not None:
        try:
            t0 = sum(defender.cpu_times()[:2])
            time.sleep(interval_s)
            pct = 100.0 * (sum(defender.cpu_times()[:2]) - t0) / interval_s
        except (psutil.Error, OSError):
            pct = None
    return {"builds": builds, "engines": engines, "defender_pct": pct}


def busy_reason(s: dict, build_max: int = DEFAULT_BUILD_MAX, defender_pct: float = DEFAULT_DEFENDER_PCT) -> str:
    """One line naming every cause in sample `s`; "" when the host is quiet enough to launch."""
    parts = []
    if s.get("engines"):
        parts.append("engine running: " + ", ".join(f"{img} pid {pid}" for pid, img in s["engines"]))
    n = sum(s.get("builds", {}).values())
    if n > build_max:
        parts.append(f"build burst: {n} compiler/linker processes > {build_max} ("
                     + ", ".join(f"{k}={v}" for k, v in sorted(s["builds"].items())) + ")")
    pct = s.get("defender_pct")
    if pct is not None and pct > defender_pct:
        parts.append(f"MsMpEng at {pct:.0f}% > {defender_pct:.0f}%")
    return "; ".join(parts)


def wait_until_quiet(*, log: Callable[[str], None] = lambda m: print(m, file=sys.stderr, flush=True),
                     sampler: Callable[[], dict] = sample, sleep: Callable[[float], None] = time.sleep,
                     clock: Callable[[], float] = time.monotonic, timeout_s: Optional[float] = None) -> dict:
    """Wait while the host is loaded, at most the bound. -> the record for the smoke report:
    {"checked", "waited_s", "first_reason", "last_reason", "timed_out", "bound_s"}."""
    if os.environ.get("MC2_HOST_LOAD", "").lower() == "off":
        return {"checked": False, "waited_s": 0.0, "first_reason": "", "last_reason": "", "timed_out": False, "bound_s": 0.0,
                "note": "MC2_HOST_LOAD=off"}
    build_max = int(_env_float("MC2_HOST_LOAD_BUILD_MAX", DEFAULT_BUILD_MAX, log))
    defender_pct = _env_float("MC2_HOST_LOAD_DEFENDER_PCT", DEFAULT_DEFENDER_PCT, log)
    bound = timeout_s if timeout_s is not None else _env_float("MC2_HOST_LOAD_WAIT_S", DEFAULT_WAIT_S, log)
    start = clock()
    first = last = ""
    last_report = None
    while True:
        try:
            reason = busy_reason(sampler(), build_max, defender_pct)
        except Exception as exc:                      # a sampler failure must not block a smoke; it is recorded instead
            log(f"[host-load] sample failed ({exc}); launching without the gate")
            return {"checked": False, "waited_s": round(clock() - start, 1), "first_reason": first, "last_reason": f"sample failed: {exc}",
                    "timed_out": False, "bound_s": bound}
        waited = clock() - start
        if not reason:
            if first:
                log(f"[host-load] quiet after {waited:.0f} s; launching")
            return {"checked": True, "waited_s": round(waited, 1), "first_reason": first, "last_reason": last, "timed_out": False,
                    "bound_s": bound}
        first = first or reason
        last = reason
        if waited >= bound:
            log(f"[host-load] still loaded after {waited:.0f} s (bound {bound:.0f} s), launching anyway: {reason}")
            return {"checked": True, "waited_s": round(waited, 1), "first_reason": first, "last_reason": last, "timed_out": True,
                    "bound_s": bound}
        if last_report is None or clock() - last_report >= REPORT_EVERY_S:
            last_report = clock()
            log(f"[host-load] waiting (up to {bound - waited:.0f} s more): {reason}")
        sleep(POLL_S)
