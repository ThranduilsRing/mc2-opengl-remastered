"""ENV-DROPPED / --strict-env: only real unregistered MC2_* names count; harness plumbing (slot, gate, host-lease names) does not."""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from smoke_lib import runner  # noqa: E402

HARNESS = {"MC2_GATE_BASE": "x", "MC2_GATE_REASON": "scheduled tier", "MC2_HOST_CORES_PER_SLOT": "2", "MC2_HOST_GAME_SLOTS": "2",
           "MC2_IN_GATE": "1", "MC2_LANE_SCRATCH": "A:/x", "MC2_SMOKE_ARTIFACT_ROOT": "A:/y"}


def test_harness_names_are_not_reported_dropped():
    assert runner.known_gate_names() is not None, "Gates/gate_table.json unreadable"
    assert runner.reportable_unregistered(dict(HARNESS)) == {}
    # still dropped from the engine env (callers pop unregistered_ambient under MC2_CONFIG_STRICT=1); only the warning is narrowed
    assert set(runner.unregistered_ambient(dict(HARNESS))) == set(HARNESS)


def test_real_unregistered_name_is_still_reported():
    env = dict(HARNESS, MC2_NOT_A_REAL_GATE_ZZ="1")
    assert runner.reportable_unregistered(env) == {"MC2_NOT_A_REAL_GATE_ZZ": "1"}


def test_name_the_run_sets_itself_is_never_dropped():
    assert runner.unregistered_ambient({"MC2_TYPO_ZZ": "1"}, {"MC2_TYPO_ZZ": "1"}) == {}


def test_visual_capture_strict_env_ignores_harness_and_fails_on_stray_name(monkeypatch, capsys):
    import run_visual_capture as rvc
    for k, v in HARNESS.items():
        monkeypatch.setenv(k, v)
    monkeypatch.setenv("MC2_NOT_A_REAL_GATE_ZZ", "1")
    monkeypatch.setattr(sys, "argv", ["run_visual_capture.py", "--strict-env"])
    assert rvc.main() == 2
    err = capsys.readouterr().err
    assert "MC2_NOT_A_REAL_GATE_ZZ" in err and "--strict-env" in err
    assert "MC2_LANE_SCRATCH" not in err and "MC2_IN_GATE" not in err
