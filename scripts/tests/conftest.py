"""Keep the harness cursor guard off the real desktop for every test in this directory.

WHY: tests that run the smoke runner against a fake child construct a real CursorGuard, whose
watchdog calls ClipCursor(NULL) every 20 ms and restores the pointer on exit. On a machine a person
is using that takes the mouse from them (owner incident 2026-10-10). MC2_CURSOR_GUARD_OFF=1 makes
smoke_lib.win_input issue no win32 call of any kind.
"""
import importlib
import os
import re
import sys
import tempfile

# deploy_payload imports as a top-level module from scripts/ (as every deploy test does it); resolved here, not by import order.
_SCRIPTS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
for _p in (os.path.dirname(_SCRIPTS), _SCRIPTS):   # repo root (scripts.deploy_payload), scripts/ (deploy_payload)
    if _p not in sys.path:
        sys.path.insert(0, _p)
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
def _fixture_deploys_tolerate_failed_cooks(monkeypatch, request):
    """box-data-overlay (W147) made a failed deploy cook exit non-zero unless --allow-stock-trees; the fixture installs hold fake
    textures the real cooks reject, so fixture deploys run as --allow-stock-trees (test_deploy_bootstrap tests the flag itself)."""
    if request.module.__name__.rsplit(".", 1)[-1] == "test_deploy_bootstrap":
        return
    for name in deploy_modules_named_by(request.module):
        monkeypatch.setattr(importlib.import_module(name), "ALLOW_STOCK_TREES", True)   # imports it: never depends on what ran before


def deploy_modules_named_by(module):
    """The deploy_payload module names a test module deploys through (`deploy_payload` and/or `scripts.deploy_payload`), read from its
    source: only test modules that name deploy_payload get the --allow-stock-trees default, whatever was imported before them."""
    try:
        with open(module.__file__, encoding="utf-8") as fh:
            text = fh.read()
    except (OSError, AttributeError, UnicodeDecodeError):
        return []
    names = []
    if "scripts.deploy_payload" in text or re.search(r"from scripts import [^\n]*\bdeploy_payload\b", text):
        names.append("scripts.deploy_payload")
    if re.search(r"(?<![.\w])deploy_payload\b", text):
        names.append("deploy_payload")
    return names


@pytest.fixture(autouse=True)
def _no_inherited_gate_flag(monkeypatch):
    monkeypatch.delenv("MC2_IN_GATE", raising=False)
    monkeypatch.setenv("MC2_NO_RECEIPT_CHECK", "1")   # batch tests drive lane_merge without a precheck receipt (R2)
