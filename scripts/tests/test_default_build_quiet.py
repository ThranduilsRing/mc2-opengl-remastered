"""The runtime quiet check must grade its own fresh smoke, never an old green run."""
import importlib.util
import json
from pathlib import Path
import sys
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
spec = importlib.util.spec_from_file_location("quiet", Path(__file__).resolve().parents[1] / "check-default-build-quiet.py")
quiet = importlib.util.module_from_spec(spec)
spec.loader.exec_module(quiet)


@pytest.mark.parametrize("rc,logs", [(2, True), (0, False), (0, True)])
def test_fresh_smoke_requires_success_and_new_logs(tmp_path, monkeypatch, rc, logs):
    monkeypatch.setattr(quiet.tempfile, "mkdtemp", lambda **kw: str(tmp_path))
    monkeypatch.setenv("MC2_DEBUG_STATE_DUMP", "1")
    def run(command, **kw):
        assert "MC2_DEBUG_STATE_DUMP" not in kw["env"]
        assert kw["env"]["MC2_SMOKE_ARTIFACT_ROOT"] == str(tmp_path)
        assert command[-2:] == ["--exe", "own/mc2.exe"]
        if logs:
            run_dir = tmp_path / "fresh"
            run_dir.mkdir()
            (run_dir / "manifest.json").write_text(json.dumps(dict(env_gates={}, missions=["mc2_10"])))
            (run_dir / "mc2_10.log").write_text("[NEW_SPAM v1]")
        return SimpleNamespace(returncode=rc)
    monkeypatch.setattr(quiet.subprocess, "run", run)
    if rc or not logs:
        with pytest.raises(RuntimeError):
            quiet.fresh_smoke_logs("own/mc2.exe")
    else:
        assert quiet.tags_in_log(quiet.fresh_smoke_logs("own/mc2.exe")[0]) == {"NEW_SPAM v1"}


def test_default_invocation_never_selects_historical_logs(monkeypatch):
    monkeypatch.setattr(sys, "argv", ["quiet"])
    monkeypatch.setattr(quiet, "newest_smoke_logs", lambda: pytest.fail("stale logs selected"))
    monkeypatch.setattr(quiet, "fresh_smoke_logs", lambda exe: (_ for _ in ()).throw(RuntimeError("fresh failed")))
    assert quiet.main() == 1
