# scripts/smoke_lib/host_lease.py
"""Host-wide game lease: one lane's harness at a time may have an engine (mc2.exe / EditRel.exe) running on this host.

WHAT: a process that is about to launch the game holds this lease for the rest of its life. Taking it waits until (1) no other live
process holds the lease and (2) no game or editor process is running that this process did not start, whatever install it came from.
WHY: the deploy-folder lease only serialises two harnesses on the SAME install. Two lanes on different installs (or a lane next to a
game the owner started) still ran two engines at once, and the runner used to log "ignoring foreign mc2.exe" and carry on; the second
engine's window handling and cursor guard then fought the first (2026-10-10). A captured frame or a timing taken while another engine
renders is also not a measurement. The lease is the single place that says so.

  * default is to WAIT (agents never poll); `wait=False` / `--no-wait` raises HostLeaseBusy at once, naming the holder.
  * a process that already holds the lease re-enters freely, so a runner that launches several games takes it once.
  * a lease whose holder pid is dead is taken over; a live game with no live lease holder is never killed here (only
    `kill_orphans`, which child_registry restricts to provable orphans of a dead run).
"""
from __future__ import annotations

import atexit
import json
import os
import sys
import time
from pathlib import Path
from typing import Callable, List, Optional, Tuple

try:
    from . import child_registry, win_input
except ImportError:                       # imported as a top-level module from scripts/
    import child_registry                 # type: ignore
    import win_input                   # type: ignore

OFF_ENV = "MC2_HOST_LEASE"                # "off" -> never lease (tests, owner debugging)
TIMEOUT_ENV = "MC2_HOST_LEASE_TIMEOUT"    # seconds, default 3600
DEFAULT_TIMEOUT_S = 3600.0
POLL_S = 2.0
REPORT_EVERY_S = 30.0


class HostLeaseBusy(RuntimeError):
    """Another lane holds the host lease, or a game this process did not start is still running."""


def lease_path() -> Path:
    override = os.environ.get("MC2_HOST_LEASE_PATH")
    return Path(override) if override else child_registry.REGISTRY_DIR.parent / ".host_game_lease.json"


def _read(path: Path) -> Optional[dict]:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


def _holder_alive(rec: Optional[dict]) -> bool:
    if not rec:
        return False
    pid = int(rec.get("pid", 0))
    if not child_registry.pid_alive(pid):
        return False
    created = rec.get("created")
    now = child_registry.process_create_time(pid)
    return created is None or now is None or int(created) == int(now)      # a reused pid is not the holder


def _try_take(path: Path, label: str) -> Tuple[bool, Optional[dict]]:
    """-> (we hold it, the other holder's record when we do not)."""
    path.parent.mkdir(parents=True, exist_ok=True)
    for _ in range(3):
        try:
            fd = os.open(str(path), os.O_CREAT | os.O_EXCL | os.O_WRONLY)
        except FileExistsError:
            rec = _read(path)
            if rec and int(rec.get("pid", 0)) == os.getpid():
                return True, None
            if _holder_alive(rec):
                return False, rec
            try:
                path.unlink()                  # holder died without releasing
            except OSError:
                pass
            continue
        with os.fdopen(fd, "w", encoding="utf-8") as fh:
            json.dump({"pid": os.getpid(), "created": child_registry.process_create_time(os.getpid()), "label": label,
                       "cwd": os.getcwd(), "argv": sys.argv[:3], "since": time.time()}, fh)
        return True, None
    return False, _read(path)


def release() -> None:
    """Give the lease back if this process holds it."""
    path = lease_path()
    rec = _read(path)
    if rec and int(rec.get("pid", 0)) == os.getpid():
        try:
            path.unlink()
        except OSError:
            pass


_registered = False


def holds() -> bool:
    """True when this process already holds the lease."""
    return bool(_registered and (_read(lease_path()) or {}).get("pid") == os.getpid())


def _describe(rec: Optional[dict]) -> str:
    if not rec:
        return "unknown holder"
    return f"{rec.get('label', '?')} (pid {rec.get('pid')}, {' '.join(map(str, rec.get('argv', [])))})"


def acquire(label: str, *, wait: bool = True, timeout_s: Optional[float] = None, kill_orphans: bool = False,
            log: Callable[[str], None] = lambda m: print(m, file=sys.stderr, flush=True),
            sleep: Callable[[float], None] = time.sleep, clock: Callable[[], float] = time.monotonic,
            scan: Optional[Callable[..., List[Tuple[int, str]]]] = None) -> None:
    """Hold the host lease, or raise HostLeaseBusy. See the module docstring for the rules."""
    global _registered
    scan = scan or win_input.find_foreign_game_pids
    if timeout_s is None:
        timeout_s = float(os.environ.get(TIMEOUT_ENV) or DEFAULT_TIMEOUT_S)
    path = lease_path()
    deadline = clock() + timeout_s
    last_report = -REPORT_EVERY_S
    already = (_read(path) or {}).get("pid") == os.getpid()
    while True:
        have, other = _try_take(path, label)
        why = ""
        if have:
            foreign = scan()
            if foreign and kill_orphans:
                for pid, _img in foreign:
                    killed, note = child_registry.kill_if_orphan(pid)
                    log(f"[host-lease] game PID {pid}: {'killed, ' if killed else 'left running, '}{note}")
                foreign = scan()
            if not foreign:
                if not _registered:
                    atexit.register(release)
                    _registered = True
                return
            why = "a game this process did not start is running: " + ", ".join(f"{img} pid {pid}" for pid, img in foreign)
        else:
            why = "the host game lease is held by " + _describe(other)
        if not wait:
            if have and not already:
                release()
            raise HostLeaseBusy(why + "; another lane is using the engine (rerun without --no-wait to queue)")
        if clock() >= deadline:
            if have and not already:
                release()
            raise HostLeaseBusy(f"gave up after {timeout_s:.0f} s: {why}")
        if clock() - last_report >= REPORT_EVERY_S:
            last_report = clock()
            log(f"[host-lease] waiting: {why}; giving up in {max(0, int(deadline - clock()))} s")
        sleep(POLL_S)


def disabled() -> bool:
    """True when leasing is switched off (MC2_HOST_LEASE=off) or the test cursor guard is off (no real engine is ever started)."""
    return os.environ.get(OFF_ENV, "").lower() == "off" or win_input.guard_off()


def ensure_held(label: str) -> None:
    """Hold the lease before an engine launch (re-entrant). No-op when disabled."""
    if disabled():
        return
    if holds():
        return              # our own earlier launches are running; they are not foreign
    acquire(label)
