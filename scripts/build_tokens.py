"""Host-wide build token pool: concurrent lanes share one -j budget instead of each running its own ninja at full width.

WHY: five lanes compiling at once stretched a 1023-step build past 1.5 h (2026-10-27). Each token is a byte-range lock on a slot file
under A:/Games/mc2-ops/build_tokens (MC2_BUILD_TOKEN_DIR overrides); the OS drops the lock when the holder dies, so a crashed build
never leaks a token. The pool size is the host-wide number in A:/Games/mc2-ops/BUILD_POOL (MC2_BUILD_POOL_FILE overrides), else cpu_count - 4.
A:/Games/mc2-ops/JOBS is the per-agent -j number and never sizes the pool.
A build joins a FIFO queue and reserves at least three tokens, even for -j1, to cap concurrent builds.
BUILD_POOL optionally accepts a second number for this floor (default 3). Ninja never exceeds its requested width.
"""
from __future__ import annotations

import os
import sys
import time
import argparse
import json
from contextlib import contextmanager
from pathlib import Path

OPS = "A:/Games/mc2-ops"


def pool_size(pool_file: str | None = None) -> int:
    path = pool_file or os.environ.get("MC2_BUILD_POOL_FILE") or os.path.join(OPS, "BUILD_POOL")
    try:
        with open(path, encoding="utf-8") as fh:
            n = int(fh.read().split()[0])
        if n > 0:
            return min(n, compile_cpu_count())
    except (OSError, ValueError, IndexError):
        pass
    return min(max(1, (os.cpu_count() or 8) - 4), compile_cpu_count())


def compile_cpu_count() -> int:
    """Logical CPUs of the CPU_SPLIT compile mask (default 0xFFF0 = 12): the host-wide -j total never exceeds the cores compiles may use."""
    try:
        tools = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools")
        if tools not in sys.path:
            sys.path.append(tools)
        from lint_slots import parse_cpu_split   # the one CPU_SPLIT grammar (also accepts the old lane= key)
        with open(os.path.join(OPS, "CPU_SPLIT"), encoding="utf-8") as fh:
            return max(1, bin(parse_cpu_split(fh.read())["compile"]).count("1"))
    except (OSError, ImportError):
        return 12


def token_dir() -> str:
    return os.environ.get("MC2_BUILD_TOKEN_DIR") or os.path.join(OPS, "build_tokens")


def build_floor() -> int:
    """BUILD_POOL: total [minimum reservation per build]; old one-number files use 3."""
    path = os.environ.get("MC2_BUILD_POOL_FILE") or os.path.join(OPS, "BUILD_POOL")
    try:
        return max(1, int(Path(path).read_text(encoding="utf-8").split()[1]))
    except (OSError, ValueError, IndexError):
        return 3


def _try_lock(fh) -> bool:
    try:
        if os.name == "nt":
            import msvcrt
            fh.seek(0)
            msvcrt.locking(fh.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(fh.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        return True
    except OSError:
        return False


class Tokens:
    """Held tokens; close() (or exit) releases them."""

    def __init__(self, handles, waited_s: float, total: int, ticket=None):
        self.handles, self.waited_s, self.total = handles, waited_s, total
        self.ticket = ticket

    def __len__(self):
        return len(self.handles)

    def close(self):
        for fh in self.handles:
            fh.close()  # closing the handle drops the lock
        self.handles = []
        if self.ticket is not None:
            self.ticket.close()
            self.ticket = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


def _lock_file(path):
    fh = open(path, "a+b")
    if fh.seek(0, 2) == 0:
        fh.write(b"x")
        fh.flush()
    if _try_lock(fh):
        return fh
    fh.close()
    return None


@contextmanager
def _queue_lock(directory):
    # Only ticket registration, admission and snapshots are serialized, never a build.
    fh = _lock_file(Path(directory) / "queue.lock")
    while fh is None:
        time.sleep(0.01)
        fh = _lock_file(Path(directory) / "queue.lock")
    try:
        yield
    finally:
        fh.close()


def _live_tickets(directory):
    """Called under queue.lock; OS locks, rather than PIDs, prove liveness (including PID reuse)."""
    live = []
    for path in sorted(Path(directory).glob("ticket-*.lock")):
        probe = _lock_file(path)
        if probe is not None:
            probe.close()
            path.unlink()  # no live owner; a crash at any point cannot strand the queue
        else:
            with path.open("rb") as stream:
                stream.seek(1)  # byte zero is locked on Windows; metadata lives beyond it
                live.append((path, json.load(stream)))
    return live


def _write_ticket(ticket, record):
    ticket.seek(1)
    ticket.truncate()
    ticket.write(json.dumps(record).encode("utf-8"))
    ticket.flush()


def status(directory: str | None = None, total: int | None = None) -> dict:
    directory = directory or token_dir()
    os.makedirs(directory, exist_ok=True)
    now = time.time()
    holders, waiters = [], []
    with _queue_lock(directory):
        for _, record in _live_tickets(directory):
            if record["state"] == "holder":
                holders.append(dict(record, held_s=max(0, now - record["admitted"])))
            else:
                waiters.append(dict(record, waiting_s=max(0, now - record["created"])))
    return {"pool_size": total or pool_size(), "build_floor": build_floor(), "holders": holders, "waiters": waiters}


def acquire(want: int, total: int | None = None, directory: str | None = None, poll_s: float = 2.0, timeout_s: float | None = None,
            on_wait=None, sleep=time.sleep, clock=time.monotonic) -> Tokens:
    """Only the oldest live waiter may reserve tokens, and only when its build floor is available.
    A timeout returns an EMPTY set and abandons the ticket; normal builds wait without a timeout to retain FIFO position.
    """
    total = total or pool_size()
    directory = directory or token_dir()
    os.makedirs(directory, exist_ok=True)
    want = max(1, min(want, total))
    floor = min(total, build_floor())
    reservation = max(want, floor)
    start, announced = clock(), False
    ticket, held = None, []
    try:
        with _queue_lock(directory):
            live = _live_tickets(directory)
            number = max((int(path.stem.split("-")[1]) for path, _ in live), default=0) + 1
            path = Path(directory) / f"ticket-{number:020d}.lock"
            ticket = _lock_file(path)
            record = {"pid": os.getpid(), "worktree": str(Path(__file__).resolve().parent.parent), "requested": want,
                      "created": time.time(), "state": "waiter"}
            _write_ticket(ticket, record)
        while True:
            with _queue_lock(directory):
                waiters = [p for p, rec in _live_tickets(directory) if rec["state"] == "waiter"]
                if waiters[0] == path:
                    for i in range(total):
                        fh = _lock_file(Path(directory) / f"slot{i}.lock")
                        if fh is not None:
                            held.append(fh)
                        if len(held) >= reservation:
                            break
                    if len(held) >= floor:
                        record.update(state="holder", tokens=len(held), admitted=time.time())
                        _write_ticket(ticket, record)
                        result = Tokens(held, clock() - start, total, ticket)
                        ticket, held = None, []  # ownership transferred to Tokens
                        return result
                    for fh in held:
                        fh.close()
                    held = []
            if timeout_s is not None and clock() - start >= timeout_s:
                return Tokens([], clock() - start, total)
            if not announced and on_wait:
                on_wait(total)
                announced = True
            sleep(poll_s)
    finally:
        for fh in held:
            fh.close()
        if ticket is not None:
            ticket.close()


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["status"])
    parser.parse_args(argv)
    snapshot = status()
    print(f"Pool: {snapshot['pool_size']} tokens; per-build floor: {snapshot['build_floor']}")
    for holder in snapshot["holders"]:
        print(f"Holder pid={holder['pid']} worktree={holder['worktree']} tokens={holder['tokens']} held for {holder['held_s']:.1f}s")
    for position, waiter in enumerate(snapshot["waiters"], 1):
        print(f"Waiter {position} pid={waiter['pid']} worktree={waiter['worktree']} requested={waiter['requested']} "
              f"waiting for {waiter['waiting_s']:.1f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
