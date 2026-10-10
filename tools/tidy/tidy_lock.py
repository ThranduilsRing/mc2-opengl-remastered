"""One tidy tool at a time per build tree: a lock file plus a cooperative stop request.

WHAT: `<tree>/.tidy.lock` holds the pid and creation time of the tool that is rewriting sources and building in that tree.
`<tree>/.tidy.stop` asks a looping tool to finish its current batch and exit.
WHY: two tidy runs over one tree edit the same headers and rebuild the same objects (the second run's baseline build died on the first
run's object files), and the only way out used to be killing compilers by image name, which also ends the other lane's builds. A refused
start and a stop request replace both.
"""
from __future__ import annotations

import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
from smoke_lib import child_registry  # noqa: E402  (pid liveness and creation time without os.kill)

LOCK_NAME = ".tidy.lock"
STOP_NAME = ".tidy.stop"


class TidyBusy(RuntimeError):
    pass


def _read(path):
    try:
        with open(path, encoding="utf-8") as fh:
            return json.load(fh)
    except (OSError, ValueError):
        return None


def holder_alive(rec) -> bool:
    """True when the process that wrote `rec` is still the same live process (a reused pid is not the holder)."""
    if not rec:
        return False
    pid = int(rec.get("pid", 0))
    if not child_registry.pid_alive(pid):
        return False
    created, now = rec.get("created"), child_registry.process_create_time(pid)
    return created is None or now is None or int(created) == int(now)


class TidyLock:
    """Context manager: `with TidyLock(tree_abs): ...`. Raises TidyBusy when a live tool already holds the tree."""

    def __init__(self, tree_abs: str):
        self.path = os.path.join(tree_abs, LOCK_NAME)

    def __enter__(self):
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        for _ in range(3):
            try:
                fd = os.open(self.path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            except FileExistsError:
                rec = _read(self.path)
                if holder_alive(rec):
                    raise TidyBusy(f"{self.path} is held by pid {rec.get('pid')} ({' '.join(map(str, rec.get('argv', [])))}), "
                                   f"started {time.strftime('%H:%M:%S', time.localtime(rec.get('since', 0)))}; wait for it, or run "
                                   f"with --stop-after-batch to make a looping holder stop after its current batch")
                try:
                    os.unlink(self.path)         # the holder died without releasing
                except OSError:
                    pass
                continue
            with os.fdopen(fd, "w", encoding="utf-8") as fh:
                json.dump({"pid": os.getpid(), "created": child_registry.process_create_time(os.getpid()),
                           "argv": sys.argv[:4], "since": time.time()}, fh)
            return self
        raise TidyBusy(f"could not take {self.path}")

    def __exit__(self, *exc):
        rec = _read(self.path)
        if rec and int(rec.get("pid", 0)) == os.getpid():
            try:
                os.unlink(self.path)
            except OSError:
                pass
        return False


def request_stop(tree_abs: str) -> str:
    """Ask the tool holding `tree_abs` to stop after its current batch. -> a one-line report."""
    rec = _read(os.path.join(tree_abs, LOCK_NAME))
    if not holder_alive(rec):
        return "no tidy tool is running in this tree; nothing to stop"
    with open(os.path.join(tree_abs, STOP_NAME), "w", encoding="utf-8") as fh:
        fh.write(str(time.time()))
    return f"stop requested: pid {rec['pid']} will exit after its current batch"


def stop_requested(tree_abs: str) -> bool:
    return os.path.exists(os.path.join(tree_abs, STOP_NAME))


def clear_stop(tree_abs: str) -> None:
    try:
        os.unlink(os.path.join(tree_abs, STOP_NAME))
    except OSError:
        pass
