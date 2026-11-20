"""--strict-env on run_smoke.py / run_visual_capture.py: dropped MC2_* names warn loudly by default and fail on request."""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def test_run_smoke_warns_and_has_strict_env():
    text = (ROOT / "scripts" / "run_smoke.py").read_text(encoding="utf-8")
    assert "[ENV-DROPPED] WARNING:" in text
    assert 'ap.add_argument("--strict-env"' in text
    # strict exits non-zero right after the warning, and the dropped set is unchanged (still recorded in the manifest)
    block = text[text.index("[ENV-DROPPED] WARNING:"):]
    assert 'getattr(args, "strict_env", False)' in block[:900] and "sys.exit(2)" in block[:900]
    assert '_env_manifest["dropped_unregistered_env"]' in text


def test_visual_capture_strict_env_fails_on_stray_name(monkeypatch, capsys):
    sys.path.insert(0, str(ROOT / "scripts"))
    import run_visual_capture as rvc
    monkeypatch.setenv("MC2_NOT_A_REAL_GATE_ZZ", "1")
    monkeypatch.setattr(sys, "argv", ["run_visual_capture.py", "--strict-env"])
    assert rvc.main() == 2
    err = capsys.readouterr().err
    assert "MC2_NOT_A_REAL_GATE_ZZ" in err and "--strict-env" in err
