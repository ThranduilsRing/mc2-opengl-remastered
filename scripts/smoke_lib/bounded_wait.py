"""bounded_wait - wait for a busy resource with a visible countdown, then give up.

WHAT: `wait_until(ready, what, timeout_s)` polls `ready()` (an empty string means ready, a non-empty string says what is in the way) until it is ready or the
bound runs out. While it waits it prints one line every `report_s` seconds that names the blocker and the seconds left before it gives up.
WHY: another lane's long smoke run holds the host lease, the install folder lease, or has a game running from the install a deploy wants to overwrite.
Failing at once with "busy" sends the agent into an ad-hoc retry loop with no bound and no output; a bounded wait with a countdown does the queueing the
person would otherwise do by hand, and still ends (never an unbounded hang, never a kill of the other lane's process).

Pure: the clock, the sleep and the logger are parameters, so the tests run in no time.
"""
from __future__ import annotations

import sys
import time
from typing import Callable

DEFAULT_POLL_S = 2.0
DEFAULT_REPORT_S = 30.0


def countdown_line(what: str, reason: str, left_s: float, timeout_s: float) -> str:
    """The progress line: what is waited for, what blocks it, how long until the wait is abandoned."""
    return f"[wait] {what}: {reason}; giving up in {max(0, int(left_s))} s (bound {int(timeout_s)} s)"


def wait_until(ready: Callable[[], str], what: str, timeout_s: float, *, poll_s: float = DEFAULT_POLL_S, report_s: float = DEFAULT_REPORT_S,
               log: Callable[[str], None] | None = None, sleep: Callable[[float], None] | None = None,
               clock: Callable[[], float] | None = None) -> bool:
    """True when `ready()` returned '' within `timeout_s` seconds, False when the bound ran out first. `ready` is always called at least once, so a
    timeout of 0 is a single check. The first blocked check logs at once, then every `report_s` seconds."""
    log = log or (lambda m: print(m, file=sys.stderr, flush=True))
    sleep = sleep or time.sleep
    clock = clock or time.monotonic
    start = clock()
    last_report = None
    while True:
        reason = ready()
        if not reason:
            return True
        now = clock()
        left = timeout_s - (now - start)
        if left <= 0:
            return False
        if last_report is None or now - last_report >= report_s:
            last_report = now
            log(countdown_line(what, reason, left, timeout_s))
        sleep(min(poll_s, left))
