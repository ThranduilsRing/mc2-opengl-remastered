"""Selection and failures must remain authoritative after native contracts share doctest."""
from pathlib import Path
from types import SimpleNamespace
import sys
from unittest.mock import Mock

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools import build_contract_harnesses as builder


@pytest.fixture(autouse=True)
def production_seam(monkeypatch):
    seam = Mock(return_value=True)
    monkeypatch.setattr(builder, "build_and_run_order_seam", seam)
    return seam


def record_runs(monkeypatch, codes):
    commands = []
    results = iter(codes)

    def run(command, **kwargs):
        commands.append((command, kwargs))
        return SimpleNamespace(returncode=next(results), stdout="", stderr="failure detail")

    monkeypatch.setattr(builder.subprocess, "run", run)
    # the doctest run goes through the tree-kill helper; route it to the same recorder
    monkeypatch.setattr(builder, "run_with_tree_timeout",
                        lambda command, cwd, env, timeout: run(command, cwd=cwd, env=env).returncode)
    return commands


def test_selection_filters_the_shared_binary(monkeypatch):
    commands = record_runs(monkeypatch, [0, 0, 0])
    selected = ["icon_atlas_harness", "job_system_harness"]
    assert builder.main(["--run", "--unit", "--only", selected[0], "--only", selected[1]]) == 0
    assert len(commands) == 3
    assert "--target" in commands[1][0] and "mc2_tests" in commands[1][0]
    assert commands[2][0][-1] == "--test-case=icon_atlas_harness/*,job_system_harness/*"
    assert commands[2][1]["cwd"] == ROOT


@pytest.mark.parametrize("codes,expected_calls", [([1], 1), ([0, 1], 2), ([0, 0, 1], 3)])
def test_native_failures_do_not_run_stale_binaries_or_python(monkeypatch, codes, expected_calls, production_seam):
    commands = record_runs(monkeypatch, codes)
    assert builder.main(["--run", "--unit"]) == 1
    assert len(commands) == expected_calls
    production_seam.assert_not_called()


def test_default_run_keeps_all_python_contracts_and_reports_failure(monkeypatch):
    commands = record_runs(monkeypatch, [0, 0, 0, 0, 1, 0, 0])
    assert builder.main(["--run", "--unit"]) == 1
    assert [command[1] for command, _ in commands[3:]] == [
        str(ROOT / path) for path in builder.PY_HARNESSES]
    assert all(command[0] == sys.executable for command, _ in commands[3:])


def test_asan_build_and_runtime_are_paired(monkeypatch):
    commands = record_runs(monkeypatch, [0, 0, 0])
    assert builder.main(["--run", "--asan"]) == 0
    assert "-DMC2_ASAN=ON" in commands[0][0]
    assert "build64-tests-asan" in " ".join(commands[0][0])
    assert "ASAN_OPTIONS" in commands[2][1]["env"]
    assert len(commands) == 3


def test_unknown_selection_is_rejected_before_build(monkeypatch):
    commands = record_runs(monkeypatch, [])
    with pytest.raises(SystemExit) as error:
        builder.main(["--only", "icon_atlas_harness", "--only", "lost_harness"])
    assert error.value.code == 2
    assert not commands


def test_seam_is_opt_in(monkeypatch, production_seam):
    record_runs(monkeypatch, [0, 0, 0])
    assert builder.main(["--run", "--unit-only"]) == 0
    production_seam.assert_not_called()
    record_runs(monkeypatch, [0, 0, 0])
    assert builder.main(["--run", "--unit-only", "--seam"]) == 0
    production_seam.assert_called_once()


def test_only_accepts_a_single_doctest_case_and_wildcards(monkeypatch):
    commands = record_runs(monkeypatch, [0, 0, 0])
    case = "job_system_harness/fatal_if_aborts_and_traces"
    assert builder.main(["--run", "--only", case]) == 0
    assert commands[2][0][-1] == "--test-case=" + case
    assert builder.unknown_selection(["job_system_harness/*", case, "icon_atlas_harness", "nope/*"]) == ["nope/*"]


# run_with_tree_timeout: rc passthrough, and a hung child is killed on timeout (was scripts/tests/test_harness_timeout.py).
def test_tree_timeout_preserves_rc():
    import os
    rc = builder.run_with_tree_timeout([sys.executable, "-c", "import sys; sys.exit(7)"], os.getcwd(), dict(os.environ), 60)
    assert rc == 7


def test_tree_timeout_kills_a_hang():
    import os
    import time
    started = time.monotonic()
    rc = builder.run_with_tree_timeout([sys.executable, "-c", "import time; time.sleep(600)"], os.getcwd(),
                                       dict(os.environ), 2)
    assert rc is None
    assert time.monotonic() - started < 30
