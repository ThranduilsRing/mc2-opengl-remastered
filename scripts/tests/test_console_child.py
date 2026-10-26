#!/usr/bin/env python3
"""console_child.child_kwargs(): an stdio-inheriting child keeps its output and
exit code when the PARENT is non-interactive.

The regression this pins: `creationflags=CREATE_NO_WINDOW` on a child that
inherits stdio gave it a fresh hidden console, so a pipe-attached parent saw no
output from it (tools/build_contract_harnesses.py --run printed nothing after
"running suite..."). The scenarios are run in a real throw-away parent process
because the behaviour depends on that process's console, not on pytest's.

Run:
  py -3 -m pytest scripts/tests/test_console_child.py -v
"""
from __future__ import annotations

import subprocess
import sys
import textwrap
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

import console_child  # noqa: E402

win32 = pytest.mark.skipif(sys.platform != "win32", reason="Win32 console semantics")

DETACHED_PROCESS = 0x00000008

# The spawned child: prints on both streams and exits non-zero.
CHILD = "import sys; print('CHILD-OUT'); sys.stdout.flush(); sys.stderr.write('CHILD-ERR\\n'); sys.exit(7)"

# The parent script. MODE=raw reproduces the burn-down pattern; MODE=helper uses
# console_child.child_kwargs(). It reports the child's rc on its own stdout.
PARENT = textwrap.dedent("""
    import subprocess, sys
    sys.path.insert(0, {scripts!r})
    import console_child
    mode = sys.argv[1]
    print('PARENT-START', flush=True)
    if mode == 'raw':
        kw = dict(creationflags=console_child.CREATE_NO_WINDOW)
    else:
        kw = console_child.child_kwargs()
    rc = subprocess.run([sys.executable, '-c', {child!r}], **kw).returncode
    print('PARENT-RC=%d' % rc, flush=True)
    print('HAS-CONSOLE=%s' % console_child.has_console(), flush=True)
""").format(scripts=str(ROOT / "scripts"), child=CHILD)


def _run_parent(mode: str, detached: bool) -> subprocess.CompletedProcess:
    """Run PARENT as a pipe-attached process, with or without a console."""
    flags = DETACHED_PROCESS if detached else 0
    # background-launch: allow test must spawn a consoled parent to reproduce the lost-output case
    return subprocess.run([sys.executable, "-c", PARENT, mode],
                          capture_output=True, text=True, creationflags=flags, timeout=60)


@win32
def test_consoleless_parent_helper_propagates_output_and_rc():
    # The hard case: parent with NO console (MCP server / hook / GUI-launched)
    # and a pipe for stdout. The flag is needed here, and must not eat output.
    r = _run_parent("helper", detached=True)
    assert "PARENT-START" in r.stdout, r.stderr
    assert "HAS-CONSOLE=True" in r.stdout, "detached parent adopted the shared hidden console"
    assert "PARENT-RC=7" in r.stdout
    assert "CHILD-OUT" in r.stdout
    assert "CHILD-ERR" in r.stderr
    # Ordering: parent text buffered before the spawn lands before the child's.
    assert r.stdout.index("PARENT-START") < r.stdout.index("CHILD-OUT")


@win32
def test_raw_flag_in_consoleless_parent_loses_output_control():
    # Documents the root cause. If Windows ever stops hiding the output this
    # becomes a skip, not a failure: the premise of the helper is gone.
    r = _run_parent("raw", detached=True)
    assert "PARENT-RC=7" in r.stdout, "exit code still propagates"
    if "CHILD-OUT" in r.stdout:
        pytest.skip("CREATE_NO_WINDOW no longer swallows inherited stdio on this Windows")
    assert "CHILD-OUT" not in r.stdout and "CHILD-ERR" not in r.stderr


@win32
def test_parent_with_console_behaves_like_before():
    # Pipe-attached parent that still has a console (agent shell, ConPTY, CI):
    # no flag is applied, the child shares the console and its output flows.
    r = _run_parent("helper", detached=False)
    assert "PARENT-RC=7" in r.stdout
    assert "CHILD-OUT" in r.stdout
    assert "CHILD-ERR" in r.stderr


def test_kwargs_passthrough_when_console_present(monkeypatch):
    monkeypatch.setattr(console_child, "has_console", lambda: True)
    assert console_child.child_kwargs(cwd="x", env={"a": "b"}) == dict(cwd="x", env={"a": "b"}, **({"creationflags": subprocess.BELOW_NORMAL_PRIORITY_CLASS} if sys.platform == "win32" else {}))


@win32
def test_kwargs_forward_std_handles_when_headless(monkeypatch):
    monkeypatch.setattr(console_child, "has_console", lambda: False)
    monkeypatch.setattr(console_child, "_adopt_shared_console", lambda: False)
    kw = console_child.child_kwargs(creationflags=0x200, stderr=subprocess.STDOUT)
    assert kw["creationflags"] == 0x200 | console_child.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS
    assert kw["stderr"] == subprocess.STDOUT, "caller's explicit stream wins"
    assert "stdout" in kw and "stdin" in kw


@win32
def test_redirected_headless_adds_only_the_flag(monkeypatch):
    """A call that captures/redirects supplies its own std handles; forwarding ours
    would raise 'stdout and stderr arguments may not be used with capture_output'."""
    monkeypatch.setattr(console_child, "has_console", lambda: False)
    monkeypatch.setattr(console_child, "_adopt_shared_console", lambda: False)
    kw = console_child.child_kwargs(redirected=True)
    assert kw == {"creationflags": console_child.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS}
    # background-launch: allow kw is console_child.child_kwargs(redirected=True) output under a patched has_console
    r = subprocess.run([sys.executable, "-c", "print('x')"], capture_output=True,
                       text=True, **kw)
    assert r.stdout.strip() == "x"


def test_redirected_with_console_adds_nothing(monkeypatch):
    """The conhost-per-child incident: a consoled parent must add no window flag."""
    monkeypatch.setattr(console_child, "has_console", lambda: True)
    assert console_child.child_kwargs(redirected=True) == ({"creationflags": subprocess.BELOW_NORMAL_PRIORITY_CLASS} if sys.platform == "win32" else {})


def test_nofocus_hidden_kwargs_follows_the_console_rule(monkeypatch):
    """hidden_kwargs() (the smoke_lib form, ~50 callers) must not allocate a hidden
    console per child from a consoled parent -- same rule as child_kwargs()."""
    from smoke_lib import nofocus
    monkeypatch.setattr(nofocus, "has_console", lambda: True)
    assert "creationflags" not in nofocus.hidden_kwargs()
    if sys.platform == "win32":
        monkeypatch.setattr(nofocus, "has_console", lambda: False)
        monkeypatch.setattr(nofocus, "_adopt_shared_console", lambda: False)
        assert nofocus.hidden_kwargs()["creationflags"] & nofocus.CREATE_NO_WINDOW


SHARED = textwrap.dedent("""
    import subprocess, sys
    sys.path.insert(0, {scripts!r})
    import console_child
    print('BEFORE=%s' % console_child.has_console(), flush=True)
    kw1 = console_child.child_kwargs()
    kw2 = console_child.child_kwargs()
    print('AFTER=%s' % console_child.has_console(), flush=True)
    print('FLAG=%s' % bool((kw1.get('creationflags', 0) | kw2.get('creationflags', 0)) & console_child.CREATE_NO_WINDOW), flush=True)
    print('RC=%d' % subprocess.run([sys.executable, '-c', {child!r}], **kw2).returncode, flush=True)
    print('RED=%s' % sorted(console_child.child_kwargs(redirected=True)), flush=True)
""").format(scripts=str(ROOT / "scripts"), child=CHILD)


@win32
def test_consoleless_parent_adopts_one_shared_console_no_flag():
    r = subprocess.run([sys.executable, "-c", SHARED], capture_output=True, text=True,
                       creationflags=DETACHED_PROCESS, timeout=60)  # background-launch: allow detached no-console parent premise
    assert "BEFORE=False" in r.stdout, r.stderr
    assert "AFTER=True" in r.stdout
    assert "FLAG=False" in r.stdout, "no CREATE_NO_WINDOW once the console is shared"
    assert "RC=7" in r.stdout and "CHILD-OUT" in r.stdout and "CHILD-ERR" in r.stderr
    assert "RED=['creationflags']" in r.stdout


@win32
def test_adopt_failure_falls_back_to_flag(monkeypatch):
    monkeypatch.setattr(console_child, "has_console", lambda: False)
    monkeypatch.setattr(console_child, "_adopt_shared_console", lambda: False)
    assert console_child.child_kwargs(redirected=True)["creationflags"] & console_child.CREATE_NO_WINDOW
