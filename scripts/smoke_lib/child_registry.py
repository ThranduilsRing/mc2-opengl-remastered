# scripts/smoke_lib/child_registry.py
"""Which game processes did the harness start, and which of them may it end?

WHY THIS EXISTS (launch-site audit 2026-10-10)
----------------------------------------------
Two kill paths could end a process the harness did not start:
  * `os.kill(pid, 0)` is a liveness probe on POSIX but TerminateProcess(pid, 0) on Windows; a lane-lock
    checker used it on a pid read from another lane's lock file.
  * `--kill-existing` ended every mc2.exe whose image path matched the target, including a game the owner
    started from the same install.
A harness may end a process only when it is provably an orphan of ITS OWN earlier run. `nofocus.spawn` records
every engine child it creates (pid, process creation time, image path, runner pid). A record authorises a kill
only while the pid is alive, its creation time and image path still match (pid reuse never matches), and the
runner that started it is dead. Everything else is reported, never killed.

`pid_alive` is the one liveness probe: OpenProcess + GetExitCodeProcess on Windows, never `os.kill`.
"""
from __future__ import annotations

import json
import os
import sys
import time
from pathlib import Path
from typing import Callable, List, Optional

_DEFAULT_DIR = Path("A:/Games/mc2-opengl/.smoke_children")
REGISTRY_DIR = Path(os.environ.get("MC2_CHILD_REGISTRY_DIR") or _DEFAULT_DIR)

_PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
_STILL_ACTIVE = 259


def _open(pid: int):
    import ctypes
    return ctypes.windll.kernel32.OpenProcess(_PROCESS_QUERY_LIMITED_INFORMATION, 0, int(pid))


def pid_alive(pid: int) -> bool:
    """True if a process with this pid exists on this host. Never sends a signal."""
    if pid <= 0:
        return False
    if os.name == "nt":
        import ctypes
        k32 = ctypes.windll.kernel32
        h = _open(pid)
        if not h:
            return False
        try:
            code = ctypes.c_ulong(0)
            if not k32.GetExitCodeProcess(h, ctypes.byref(code)):
                return True
            return code.value == _STILL_ACTIVE
        finally:
            k32.CloseHandle(h)
    try:
        os.kill(int(pid), 0)    # POSIX only: signal 0 sends nothing
    except ProcessLookupError:
        return False
    except OSError:
        return True
    return True


def process_create_time(pid: int) -> Optional[int]:
    """FILETIME (100 ns ticks) at which the process was created; None if unknown."""
    if os.name != "nt" or pid <= 0:
        return None
    import ctypes
    from ctypes import wintypes
    k32 = ctypes.windll.kernel32
    h = _open(pid)
    if not h:
        return None
    try:
        c, e, k, u = (wintypes.FILETIME() for _ in range(4))
        if not k32.GetProcessTimes(h, ctypes.byref(c), ctypes.byref(e), ctypes.byref(k), ctypes.byref(u)):
            return None
        return (c.dwHighDateTime << 32) | c.dwLowDateTime
    finally:
        k32.CloseHandle(h)


def image_path(pid: int) -> Optional[str]:
    """Full path of the process image, lower-cased with forward slashes; None if unknown."""
    if os.name != "nt" or pid <= 0:
        return None
    import ctypes
    from ctypes import wintypes
    k32 = ctypes.windll.kernel32
    h = _open(pid)
    if not h:
        return None
    try:
        buf = ctypes.create_unicode_buffer(1024)
        n = wintypes.DWORD(len(buf))
        if not k32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(n)):
            return None
        return _norm(buf.value)
    finally:
        k32.CloseHandle(h)


def _norm(p: str) -> str:
    return str(p).replace("\\", "/").casefold()


def _record_path(pid: int) -> Path:
    return REGISTRY_DIR / f"{int(pid)}.json"


def record(pid: int, exe, role: str = "engine", runner_pid: Optional[int] = None) -> None:
    """Remember that THIS process started `pid`. Never raises: bookkeeping must not fail a launch."""
    if "pytest" in sys.modules and REGISTRY_DIR == _DEFAULT_DIR:
        return      # a test must not write records (fake pids) into the shared directory
    try:
        REGISTRY_DIR.mkdir(parents=True, exist_ok=True)
        rec = {"pid": int(pid), "exe": _norm(Path(str(exe)).resolve()), "role": role,
               "runner_pid": int(runner_pid if runner_pid is not None else os.getpid()),
               "create_time": process_create_time(int(pid)), "recorded": time.time()}
        _record_path(pid).write_text(json.dumps(rec), encoding="utf-8")
    except Exception:
        pass


def read_record(pid: int) -> Optional[dict]:
    try:
        return json.loads(_record_path(pid).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


def forget(pid: int) -> None:
    try:
        _record_path(pid).unlink()
    except OSError:
        pass


def may_kill(pid: int, *, alive: Callable[[int], bool] = pid_alive,
             created: Callable[[int], Optional[int]] = process_create_time,
             image: Callable[[int], Optional[str]] = image_path,
             rec: Optional[dict] = None) -> "tuple[bool, str]":
    """-> (allowed, reason). Allowed only for a live orphan of an earlier harness run."""
    rec = rec if rec is not None else read_record(pid)
    if rec is None:
        return False, "not started by a harness run (no record)"
    if not alive(pid):
        return False, "already gone"
    if alive(int(rec.get("runner_pid", 0))):
        return False, f"its runner (pid {rec.get('runner_pid')}) is still running"
    want_ct = rec.get("create_time")
    if want_ct is None or created(pid) != want_ct:
        return False, "creation time differs from the record (pid reused)"
    if image(pid) != rec.get("exe"):
        return False, "image path differs from the record"
    return True, "orphan of a dead harness run"


def kill_if_orphan(pid: int, **kw) -> "tuple[bool, str]":
    """Terminate `pid` only when may_kill() allows it. Returns (killed, reason)."""
    ok, why = may_kill(pid, **kw)
    if not ok:
        return False, why
    import subprocess
    try:
        from . import nofocus
    except ImportError:
        from smoke_lib import nofocus
    subprocess.run(["taskkill", "/F", "/PID", str(pid)], stderr=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                   **nofocus.hidden_kwargs())
    forget(pid)
    return True, why


def prune() -> List[int]:
    """Drop records of processes that no longer exist. Returns the pids removed."""
    gone = []
    try:
        names = [p for p in REGISTRY_DIR.glob("*.json")]
    except OSError:
        return gone
    for p in names:
        try:
            pid = int(p.stem)
        except ValueError:
            continue
        if not pid_alive(pid):
            forget(pid)
            gone.append(pid)
    return gone
