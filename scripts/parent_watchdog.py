"""Parent-death check for long-running polling loops.

WHAT: `ParentWatch()` remembers the process that started this one (pid and creation time); `.gone()` turns True once that process has
exited or its pid was reused. A polling loop calls it each iteration and stops.
WHY: an agent's TaskStop (or a closed terminal) kills the shell, not always its children; a watcher whose parent is gone keeps polling,
holds files open and, for the dev-shell watcher, keeps sending commands to whatever engine later listens on its port. Unbounded loops
in scripts/ and tools/ carry this check (inventory: lane 3, 2026-10-15).
"""
from __future__ import annotations

import os
import sys
from typing import Callable, Optional


def _create_time(pid: int) -> Optional[float]:
    try:
        import psutil
        return psutil.Process(pid).create_time()
    except Exception:
        return None


def _alive(pid: int) -> bool:
    try:
        import psutil
        return psutil.pid_exists(pid) and psutil.Process(pid).status() != psutil.STATUS_ZOMBIE
    except Exception:
        return True                     # cannot tell: never stop a loop on a probe failure


class ParentWatch:
    """See the module docstring. `pid` overrides the parent (tests); `log` gets one line when the parent is found gone."""

    def __init__(self, pid: Optional[int] = None, log: Callable[[str], None] = lambda m: print(m, file=sys.stderr, flush=True)):
        self.pid = os.getppid() if pid is None else pid
        self.created = _create_time(self.pid)
        self._log = log
        self._reported = False

    def gone(self) -> bool:
        if self.pid <= 1:
            return False                # no parent to watch (init/session root)
        dead = not _alive(self.pid)
        if not dead and self.created is not None:
            now = _create_time(self.pid)
            dead = now is not None and abs(now - self.created) > 1.0        # pid reused by another process
        if dead and not self._reported:
            self._reported = True
            self._log(f"[parent-watchdog] parent pid {self.pid} is gone; stopping")
        return dead
