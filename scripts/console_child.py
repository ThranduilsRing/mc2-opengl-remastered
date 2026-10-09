"""Spawn console children without a window AND without losing their output.

WHY THIS EXISTS
---------------
The console-spawn burn-down put `creationflags=CREATE_NO_WINDOW` on every
helper child so none pops a console window. For a child that INHERITS stdio
(no capture_output / stdout=), that flag silently breaks output:

  * Popen with stdin/stdout/stderr all None passes the child NO handles
    (no STARTF_USESTDHANDLES). The child's std handles then come from its
    console.
  * Without the flag the child shares the parent's console, so it ends up on
    the parent's stdout/stderr (a tty, or the agent's pipe behind the parent's
    ConPTY).
  * With CREATE_NO_WINDOW the child gets a brand-new hidden console and its
    std handles are that console's. Everything it prints goes nowhere; the
    parent's pipe never sees a byte. (Exit codes still propagate, which is why
    the symptom was "empty output" rather than a crash.)

The old guard `if not sys.stdout.isatty()` did not separate the cases that
matter: an agent's stdout is a pipe but its process still HAS a console, so the
flag was applied and the output was lost.

THE RULE
--------
A window can only appear when the spawning process has NO console (an MCP
server, a hook, a GUI-launched or DETACHED_PROCESS parent). So:

  * parent has a console (terminal, ConPTY, agent shell) -> no flag; the child
    shares the console and inherits stdio exactly as before.
  * parent has no console -> CREATE_NO_WINDOW, and the parent's own std handles
    are passed EXPLICITLY (stdin/stdout/stderr), which restores STARTF_USESTDHANDLES
    so the child writes to the parent's pipe/file instead of its hidden console.

Use `child_kwargs()` for spawns that inherit stdio. Spawns that capture
(capture_output / stdout=PIPE) never lose output and can keep a plain
`creationflags=CREATE_NO_WINDOW`.
"""
from __future__ import annotations

import os
import subprocess
import sys

CREATE_NO_WINDOW = 0x08000000


def has_console() -> bool:
    """True when this process is attached to a console (visible or hidden).

    GetConsoleProcessList returns 0 when there is no console; ConPTY and hidden
    consoles still report their attached processes, which is what we want: a
    child spawned from them shares that console and can never open a window.
    """
    if sys.platform != "win32":
        return True
    try:
        import ctypes
        buf = (ctypes.c_uint * 2)()
        return ctypes.windll.kernel32.GetConsoleProcessList(buf, 2) > 0
    except Exception:
        # Unknown -> assume headless: the flag is the safe side (no stray window).
        return False


def _std_arg(fd: int):
    """The Popen argument that forwards our own std fd, or DEVNULL if it is dead."""
    try:
        os.fstat(fd)
        return fd
    except OSError:
        return subprocess.DEVNULL


def child_kwargs(**extra) -> dict:
    """Kwargs to splat into a stdio-INHERITING subprocess call.

    `subprocess.run(cmd, **console_child.child_kwargs())`. Returns `extra`
    unchanged when a console is present (or off Windows); otherwise adds
    CREATE_NO_WINDOW plus explicit stdin/stdout/stderr forwarding so the child's
    output and exit code reach the caller. Keys already in `extra` win.
    """
    kw = dict(extra)
    # Our own buffered text must land before the child's, or the two interleave
    # out of order on a shared pipe (a pipe-attached Python buffers in blocks, so
    # "running suite..." would otherwise surface AFTER the suite's output).
    for s in (sys.stdout, sys.stderr):
        try:
            s.flush()
        except Exception:
            pass
    if sys.platform != "win32" or has_console():
        return kw
    kw["creationflags"] = kw.get("creationflags", 0) | CREATE_NO_WINDOW
    for name, fd in (("stdin", 0), ("stdout", 1), ("stderr", 2)):
        kw.setdefault(name, _std_arg(fd))
    return kw
