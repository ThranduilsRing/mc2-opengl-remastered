"""run_visual_capture: the exit code, the manifest and the log use ONE predicate (fail_reasons)."""
import inspect
import sys
from types import SimpleNamespace
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import run_visual_capture as rvc  # noqa: E402


def test_path_timing_ignores_system_perf(monkeypatch, tmp_path):
    """Trigger: run_visual_capture.py and tools/perf/**; Linux perf may already be loaded."""
    class SystemPerf:
        def __getattr__(self, name):
            raise AssertionError("system perf must not be used")

    monkeypatch.setitem(sys.modules, "perf", SystemPerf())
    import tracy_capture_compare as tracy
    launches = []

    def capture(*args, **kwargs):
        launches.append(kwargs)
        return 73  # Stop at the real recorder boundary without starting a game.

    monkeypatch.setattr(tracy, "capture_trace", capture)
    args = SimpleNamespace(tracy_output=str(tmp_path / "capture.tracy"),
                           tracy_commands="", env_declare=[], exe=str(tmp_path / "mc2"),
                           mission="mc2_01", tracy_seconds=10)
    assert rvc.capture_path_timing(args) == 73
    assert len(launches) == 1
    assert callable(launches[0]["spawn_game"])


def _ok():
    return {"all_present": True, "engine_capture_fired": True,
            "backend": {"disagree": []}, "render_options": {"disagree": []}}


def test_clean_result_has_no_reasons():
    assert rvc.fail_reasons(_ok()) == []


@pytest.mark.parametrize("mut", [
    {"all_present": False},
    {"engine_capture_fired": False},
    {"render_options": {"disagree": ["shadow quality differs"]}},
    {"backend": {"disagree": ["drawn by gl"]}},
])
def test_each_evidence_gap_is_a_failure(mut):
    r = {**_ok(), **mut}
    assert rvc.fail_reasons(r)


def test_exit_codes_come_from_fail_reasons_not_a_hand_copied_subset():
    src = inspect.getsource(rvc.main)
    assert "return 0 if not fail_reasons(result) else 1" in src
    assert 'result["backend"]["disagree"]) else 1' not in src


def test_multi_run_stable_but_engine_capture_never_fired_exits_nonzero(monkeypatch, tmp_path):
    """p31: multi-run dropped 'did not fire' from fail_reasons and exited 0 on byte-stable images without --candidate-set."""
    exe = tmp_path / "mc2.exe"
    exe.write_bytes(b"x")
    bm = tmp_path / "bm.json"
    bm.write_text("{}")
    monkeypatch.setattr(rvc, "guard_exe", lambda exe: True)
    # The fake install isolates the exit predicate; asset parity has its own tests.
    monkeypatch.setattr(rvc.asset_parity, "capture_drift", lambda install: None)
    monkeypatch.setattr(rvc, "_save_cursor", lambda: None)
    monkeypatch.setattr(rvc.atexit, "register", lambda *a, **k: None)
    monkeypatch.setattr(rvc.time, "sleep", lambda s: None)
    monkeypatch.setattr(rvc, "write_manifest", lambda r, exe: None)
    monkeypatch.setattr(rvc, "run_one", lambda *a, **k: {**_ok(), "engine_capture_fired": False})
    monkeypatch.setattr(rvc, "compare_runs", lambda runs: (True, {"b1": {"shas": ["a", "a"], "stable": True}}))
    monkeypatch.setattr(sys, "argv", ["run_visual_capture.py", "--exe", str(exe), "--mission", "mc2_01", "--bookmarks", str(bm),
                                      "--out-dir", str(tmp_path / "out"), "--runs", "2", "--warmup", "0", "--no-kill"])
    assert rvc.main() == 1
    monkeypatch.setattr(rvc, "run_one", lambda *a, **k: _ok())
    assert rvc.main() == 0
