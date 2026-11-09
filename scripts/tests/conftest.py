"""Keep the harness cursor guard off the real desktop for every test in this directory.

WHY: tests that run the smoke runner against a fake child construct a real CursorGuard, whose
watchdog calls ClipCursor(NULL) every 20 ms and restores the pointer on exit. On a machine a person
is using that takes the mouse from them (owner incident 2026-10-10). MC2_CURSOR_GUARD_OFF=1 makes
smoke_lib.win_input issue no win32 call of any kind.
"""
import os
import tempfile

os.environ["MC2_CURSOR_GUARD_OFF"] = "1"
os.environ.setdefault("MC2_LINT_SLOTS_DIR", os.path.join(tempfile.gettempdir(), f"mc2-lint-slots-test-{os.getpid()}"))   # lint tests never share the real host slots
os.environ["MC2_NO_CPU_PIN"] = "1"
os.environ["MC2_BOX_INFRA_STATE"] = os.path.join(tempfile.gettempdir(), f"mc2-box-infra-test-{os.getpid()}.json")   # gate tests never touch the real BOX_INFRA memory
os.environ["MC2_BOX_PROOFS"] = os.path.join(tempfile.gettempdir(), f"mc2-box-proofs-test-{os.getpid()}")   # box_run tests never write the real proofs archive

# Precheck receipts go to a private dir in tests, never to the real A:/Games/mc2-ops/precheck (a mocked git_out once left a forged abc.json there).
os.environ["MC2_PRECHECK_RECEIPTS"] = os.path.join(tempfile.gettempdir(), f"mc2-precheck-receipts-test-{os.getpid()}")
# A private host lease file per test process: tests that spawn a fake engine contend with each other, never with the lanes
# (scripts/smoke_lib/host_lease.py raises when a test reaches the real lease without this).
os.environ.setdefault("MC2_HOST_LEASE_PATH", os.path.join(tempfile.gettempdir(), f"mc2-host-lease-test-{os.getpid()}.json"))


# The gate sets MC2_IN_GATE only on game-launching stages (merge_gate.gate_env); a test run that inherits it from an
# outer shell would change lease priority and trip the nested-gate refusal, so every test starts without it.
import pytest  # noqa: E402


@pytest.fixture
def deploy_tmp_path(tmp_path):
    """Deploy fixtures must exercise permitted installs, never the system-drive refusal."""
    if os.name == "nt" and tmp_path.drive.casefold() == os.environ.get("SystemDrive", "C:").casefold():
        from pathlib import Path
        scratch = Path("G:/mc2-tmp")
        scratch.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="deploy-test-", dir=scratch) as work:
            yield Path(work)
    else:
        yield tmp_path


@pytest.fixture(autouse=True)
def _no_inherited_gate_flag(monkeypatch):
    monkeypatch.delenv("MC2_IN_GATE", raising=False)
    monkeypatch.setenv("MC2_NO_RECEIPT_CHECK", "1")   # batch tests drive lane_merge without a precheck receipt (R2)
