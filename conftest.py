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
