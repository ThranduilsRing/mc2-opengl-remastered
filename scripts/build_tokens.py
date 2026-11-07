"""Host-wide build token pool: concurrent lanes share one -j budget instead of each running its own ninja at full width.

WHY: five lanes compiling at once stretched a 1023-step build past 1.5 h (2026-10-27). Each token is a byte-range lock on a slot file
under A:/Games/mc2-ops/build_tokens (MC2_BUILD_TOKEN_DIR overrides); the OS drops the lock when the holder dies, so a crashed build
never leaks a token. The pool size is the host-wide number in A:/Games/mc2-ops/BUILD_POOL (MC2_BUILD_POOL_FILE overrides), else cpu_count - 4.
A:/Games/mc2-ops/JOBS is the per-agent -j number and never sizes the pool.
A build asks for K tokens (its -jK), waits until at least one is free, takes up to K, and runs ninja at the width it got.
"""
from __future__ import annotations

import os
import time

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
        with open(os.path.join(OPS, "CPU_SPLIT"), encoding="utf-8") as fh:
            for line in fh:
                k, _, v = line.partition("=")
                if k.strip() == "compile" and v.strip():
                    return max(1, bin(int(v.strip(), 0)).count("1"))
    except (OSError, ValueError):
        pass
    return 12


def token_dir() -> str:
    return os.environ.get("MC2_BUILD_TOKEN_DIR") or os.path.join(OPS, "build_tokens")


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

    def __init__(self, handles, waited_s: float, total: int):
        self.handles, self.waited_s, self.total = handles, waited_s, total

    def __len__(self):
        return len(self.handles)

    def close(self):
        for fh in self.handles:
            fh.close()  # closing the handle drops the lock
        self.handles = []

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


def acquire(want: int, total: int | None = None, directory: str | None = None, poll_s: float = 2.0, timeout_s: float | None = None,
            on_wait=None, sleep=time.sleep, clock=time.monotonic) -> Tokens:
    """Wait until >= 1 token is free, then take up to `want`. After timeout_s, run with 1 unpooled slot rather than block forever."""
    total = total or pool_size()
    directory = directory or token_dir()
    os.makedirs(directory, exist_ok=True)
    want = max(1, min(want, total))
    start, announced = clock(), False
    while True:
        held = []
        for i in range(total):
            if len(held) >= want:
                break
            fh = open(os.path.join(directory, f"slot{i}.lock"), "a+b")
            if fh.seek(0, 2) == 0:
                fh.write(b"x")
                fh.flush()
            if _try_lock(fh):
                held.append(fh)
            else:
                fh.close()
        if held:
            return Tokens(held, clock() - start, total)
        if timeout_s is not None and clock() - start > timeout_s:
            return Tokens([], clock() - start, total)
        if not announced and on_wait:
            on_wait(total)
            announced = True
        sleep(poll_s)
