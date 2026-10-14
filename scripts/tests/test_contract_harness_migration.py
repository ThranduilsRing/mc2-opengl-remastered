"""Selection and failures must remain authoritative after native contracts share doctest."""
from pathlib import Path
from types import SimpleNamespace
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools import build_contract_harnesses as builder


def record_runs(monkeypatch, codes):
    commands = []
    results = iter(codes)

    def run(command, **kwargs):
        commands.append((command, kwargs))
        return SimpleNamespace(returncode=next(results), stdout="", stderr="failure detail")

    monkeypatch.setattr(builder.subprocess, "run", run)
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
def test_native_failures_do_not_run_stale_binaries_or_python(monkeypatch, codes, expected_calls):
    commands = record_runs(monkeypatch, codes)
    assert builder.main(["--run", "--unit"]) == 1
    assert len(commands) == expected_calls


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
