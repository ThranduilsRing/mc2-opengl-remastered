"""Keep the harness cursor guard off the real desktop for every test in this directory.

WHY: tests that run the smoke runner against a fake child construct a real CursorGuard, whose
watchdog calls ClipCursor(NULL) every 20 ms and restores the pointer on exit. On a machine a person
is using that takes the mouse from them (owner incident 2026-10-10). MC2_CURSOR_GUARD_OFF=1 makes
smoke_lib.win_input issue no win32 call of any kind.
"""
import os
import tempfile

os.environ["MC2_CURSOR_GUARD_OFF"] = "1"
# A private host lease file per test process: tests that spawn a fake engine contend with each other, never with the lanes
# (scripts/smoke_lib/host_lease.py raises when a test reaches the real lease without this).
os.environ.setdefault("MC2_HOST_LEASE_PATH", os.path.join(tempfile.gettempdir(), f"mc2-host-lease-test-{os.getpid()}.json"))


# The gate sets MC2_IN_GATE only on game-launching stages (merge_gate.gate_env); a test run that inherits it from an
# outer shell would change lease priority and trip the nested-gate refusal, so every test starts without it.
import pytest  # noqa: E402


@pytest.fixture(autouse=True)
def _no_inherited_gate_flag(monkeypatch):
    monkeypatch.delenv("MC2_IN_GATE", raising=False)
