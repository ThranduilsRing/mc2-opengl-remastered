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

Use `child_kwargs()` for EVERY console-child spawn. Spawns that redirect any
std stream (capture_output / stdout=PIPE / stdin=DEVNULL / check_output) pass
`child_kwargs(redirected=True)`: they never lose output, so only the flag is added.

WHY NOT A BARE `creationflags=CREATE_NO_WINDOW`
-----------------------------------------------
Every child spawned with the flag from a parent that already has a console
allocates ITS OWN hidden console, i.e. one `conhost.exe` per child. A
check-contracts run spawns hundreds; enough of them crashed the owner's VPN
split-tunnel process monitor (Private Internet Access, 2026-10-09). A consoled
parent's children share its console, so no window can appear and no conhost is
created -- the flag is only ever needed when the parent has no console.
"""
from __future__ import annotations

import os
import subprocess
import sys

CREATE_NO_WINDOW = 0x08000000


def _tolerate_unencodable_output() -> None:
    """Make this process's stdout/stderr print any character instead of raising.

    WHY: every tool imports this module, and a tool that prints an arrow or an em dash must not crash under a cp1252 console. The
    streams are reconfigured once, to UTF-8 with replacement, and left alone when they are not text streams (a captured pipe object).
    """
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError, OSError):
            pass


_tolerate_unencodable_output()


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


def child_kwargs(*, redirected: bool = False, **extra) -> dict:
    """Kwargs to splat into a console-child subprocess call.

    `subprocess.run(cmd, **console_child.child_kwargs())`. Returns `extra`
    unchanged when a console is present (or off Windows); otherwise adds
    CREATE_NO_WINDOW plus explicit stdin/stdout/stderr forwarding so the child's
    output and exit code reach the caller. Keys already in `extra` win.

    `redirected=True` says the call itself redirects at least one std stream
    (capture_output, stdout=, stderr=, stdin=, input=, check_output): Popen then
    already hands the child explicit handles, so forwarding ours would clash with
    the call's own arguments and only the flag is added.
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
    if redirected:
        return kw
    for name, fd in (("stdin", 0), ("stdout", 1), ("stderr", 2)):
        kw.setdefault(name, _std_arg(fd))
    return kw
