"""Repo-wide pytest setup: keep the harness cursor guard off the real desktop for every test.

WHY: tests that run the smoke runner against a fake child build a real CursorGuard, whose watchdog calls
ClipCursor(NULL) every 20 ms and restores the pointer, taking the mouse from whoever is using the machine
(owner incident 2026-10-10). MC2_CURSOR_GUARD_OFF=1 makes smoke_lib.win_input issue no win32 call.
The per-folder conftest.py files remain for runs that do not pick up this one.
"""
import os
import tempfile

os.environ["MC2_CURSOR_GUARD_OFF"] = "1"
# A private host lease file per test process: tests that spawn a fake engine contend with each other, never with the lanes
# (scripts/smoke_lib/host_lease.py raises when a test reaches the real lease without this).
os.environ.setdefault("MC2_HOST_LEASE_PATH", os.path.join(tempfile.gettempdir(), f"mc2-host-lease-test-{os.getpid()}.json"))


# Per-test timeout (300 s; MC2_TEST_TIMEOUT overrides, 0 disables). A hung test stalled a full run for 20 min (2026-10-27).
# pytest-timeout is not installed here, so a watchdog thread dumps stacks, kills the test's child processes (the usual hang is a
# subprocess wait) and raises TimeoutError in the main thread. Mark a test slow with @pytest.mark.timeout(N) to give it N seconds.
import ctypes  # noqa: E402
import faulthandler  # noqa: E402
import subprocess  # noqa: E402
import sys  # noqa: E402
import threading  # noqa: E402

import pytest  # noqa: E402

TEST_TIMEOUT = float(os.environ.get("MC2_TEST_TIMEOUT", "300"))


def pytest_configure(config):
    config.addinivalue_line("markers", "timeout(seconds): per-test timeout override (conftest watchdog)")


def _kill_children():
    try:
        import psutil
        for child in psutil.Process().children(recursive=True):
            try:
                child.kill()
            except psutil.Error:
                pass
    except ImportError:
        if os.name == "nt":
            subprocess.call(["taskkill", "/F", "/T", "/FI", f"PPID eq {os.getpid()}"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def _expire(main_ident, name, seconds, done):
    sys.stderr.write(f"\n[test-timeout] {name} exceeded {seconds:g} s; killing children and interrupting\n")
    faulthandler.dump_traceback(file=sys.stderr, all_threads=True)
    _kill_children()
    ctypes.pythonapi.PyThreadState_SetAsyncExc(ctypes.c_ulong(main_ident), ctypes.py_object(TimeoutError))
    # A blocking call that is neither a child wait nor interruptible (a Python sleep, a lock) never sees the exception: hard-exit.
    if not done.wait(30):
        sys.stderr.write(f"[test-timeout] {name} still stuck 30 s after the interrupt; exiting the run" + chr(10))
        os._exit(3)


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_call(item):
    marker = item.get_closest_marker("timeout")
    seconds = float(marker.args[0]) if marker and marker.args else TEST_TIMEOUT
    if seconds <= 0 or "pytest_timeout" in sys.modules:
        yield
        return
    done = threading.Event()
    timer = threading.Timer(seconds, _expire, (threading.main_thread().ident, item.nodeid, seconds, done))
    timer.daemon = True
    timer.start()
    try:
        yield
    finally:
        timer.cancel()
        done.set()
