"""One exclusive queue for everything that loads the farm laptop: farm jobs (perf timing, smoke) and the laptop gate's build + gate.

p31: the laptop gate builder ran over ssh beside the runner, so a build could overlap a perf job and corrupt its timing. Both now take
this lock: runner.py around every job, tools/laptop_gate.py around its build + gate. An OS byte-range lock on a file, so a crashed
holder releases it by dying (no stale-lock cleanup). Stdlib only; copied next to runner.py in C:/mc2farm and next to the gate's remote
half in C:/mc2gate/bin.
"""
from __future__ import annotations

import contextlib
import os
import time

DEFAULT_PATH = "C:/mc2farm/queue.lock"


def _try_lock(f):
    try:
        if os.name == "nt":
            import msvcrt
            f.seek(0)
            msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
        return True
    except OSError:
        return False


def _unlock(f):
    if os.name == "nt":
        import msvcrt
        f.seek(0)
        msvcrt.locking(f.fileno(), msvcrt.LK_UNLCK, 1)
    else:
        import fcntl
        fcntl.flock(f, fcntl.LOCK_UN)


@contextlib.contextmanager
def hold(path=DEFAULT_PATH, timeout=None, poll=1.0, log=None):
    """Block until this process owns the queue; TimeoutError after `timeout` seconds (None waits forever)."""
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    started = time.monotonic()
    announced = False
    with open(path, "a+b") as f:
        if os.fstat(f.fileno()).st_size == 0:   # never read: a held byte-range lock makes reads fail on Windows
            f.write(b"\0")   # the locked byte must exist
            f.flush()
        while not _try_lock(f):
            if timeout is not None and time.monotonic() - started >= timeout:
                raise TimeoutError(f"farm queue busy for {timeout:.0f}s: {path}")
            if log and not announced:
                log(f"farm queue busy; waiting for the running job ({path})")
                announced = True
            time.sleep(poll)
        try:
            yield
        finally:
            _unlock(f)
