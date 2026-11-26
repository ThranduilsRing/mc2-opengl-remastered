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

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import gate_hosts  # noqa: E402

needs_proofs_archive = pytest.mark.skipif(
    gate_hosts.current_host() != "pc" and not Path("G:/mc2-proofs").exists(),
    reason="PC-only proofs archive G:/mc2-proofs is absent on this host")


@pytest.mark.parametrize("problem", ["none", "missing", "diagnostic", "failed", "ambiguous"])
def test_gate_tier1_quiet_sample_fails_closed(tmp_path, monkeypatch, problem):
    run = tmp_path / "run"
    run.mkdir()
    (run / "mc2_17.log").write_text("[SMOKE v1] done")
    if problem != "missing":
        (run / "quiet_sample.json").write_text(json.dumps({"mission": "mc2_17", "passed": problem != "failed",
            "env_extra": {}, "ambient": {"MC2_DEBUG_STATE_DUMP": "1"} if problem == "diagnostic" else {}}))
    if problem == "ambiguous":
        (tmp_path / "second").mkdir()
        (tmp_path / "second/mc2_17.log").write_text("old green")
    monkeypatch.setattr(sys, "argv", ["quiet", "--run-root", str(tmp_path)])
    monkeypatch.setattr(quiet, "load_baseline", lambda: {"tags": {"SMOKE v1": {}}})
    assert quiet.main() == (0 if problem == "none" else 1)


@needs_proofs_archive
@pytest.mark.parametrize("rc,logs", [(2, True), (0, False), (0, True)])
def test_fresh_smoke_requires_success_and_new_logs(tmp_path, monkeypatch, rc, logs):
    monkeypatch.setattr(quiet.tempfile, "mkdtemp", lambda **kw: str(tmp_path))
    monkeypatch.setenv("MC2_DEBUG_STATE_DUMP", "1")
    monkeypatch.setenv("MC2_HOST_LEASE_BATCH", "fixture-chain")
    monkeypatch.setenv("MC2_IN_GATE", "1")
    def run(command, **kw):
        assert "MC2_DEBUG_STATE_DUMP" not in kw["env"]
        assert kw["env"]["MC2_HOST_LEASE_BATCH"] == "fixture-chain"
        assert kw["env"]["MC2_IN_GATE"] == "1"   # W115: a scrubbed gate child was refused by GATE QUIET (rc 75)
        assert kw["env"]["MC2_SMOKE_ARTIFACT_ROOT"] == str(tmp_path)
        assert command[-2:] == ["--exe", "own/mc2.exe"]
        if logs:
            run_dir = tmp_path / "fresh"
            run_dir.mkdir()
            (run_dir / "manifest.json").write_text(json.dumps(dict(env_gates={"MC2_SMOKE_ARTIFACT_ROOT": str(tmp_path)}, missions=["mc2_10"])))
            (run_dir / "mc2_10.log").write_text("[NEW_SPAM v1]")
        return SimpleNamespace(returncode=rc)
    monkeypatch.setattr(quiet.subprocess, "run", run)
    if rc or not logs:
        with pytest.raises(RuntimeError):
            quiet.fresh_smoke_logs("own/mc2.exe")
    else:
        assert quiet.tags_in_log(quiet.fresh_smoke_logs("own/mc2.exe")[0]) == {"NEW_SPAM v1"}


def test_offline_default_invocation_never_reaches_game_launch(monkeypatch, capsys):
    """check-contracts runs this tool with no flags: it must never spawn anything, only grade or SKIP."""
    monkeypatch.setattr(sys, "argv", ["quiet"])
    monkeypatch.setattr(quiet, "newest_smoke_logs", lambda: [])
    monkeypatch.setattr(quiet, "fresh_smoke_logs", lambda exe: pytest.fail("game-launch path reached"))
    monkeypatch.setattr(quiet.subprocess, "run", lambda *a, **k: pytest.fail("subprocess spawned"))
    assert quiet.main() == 0
    assert "graded at merge gate" in capsys.readouterr().out


def test_contracts_row_is_offline_and_launch_needs_explicit_exe():
    # gate-slim (4a3747ded) deleted the contracts row; the check runs only at the merge gate with --fresh-smoke --exe.
    # If a contracts row comes back it must stay offline.
    root = Path(__file__).resolve().parents[2]
    for row in (root / "scripts/check-contracts.sh").read_text(encoding="utf-8").splitlines():
        if 'run_check "default_build_quiet"' in row:
            assert "--fresh-smoke" not in row and "--exe" not in row
    with pytest.raises(RuntimeError):
        quiet.fresh_smoke_logs(None)
    body = Path(quiet.__file__).read_text(encoding="utf-8").split("def fresh_smoke_logs", 1)[1].split("def main", 1)[0]
    assert "--deploy" not in body


def test_fresh_smoke_flag_requires_exe(monkeypatch):
    monkeypatch.setattr(sys, "argv", ["quiet", "--fresh-smoke"])
    monkeypatch.setattr(quiet.subprocess, "run", lambda *a, **k: pytest.fail("subprocess spawned"))
    assert quiet.main() == 1


@pytest.mark.parametrize("commit,dirty,current", [
    ("a" * 12, False, True), ("b" * 12, False, False),
    ("a" * 12, True, False), ("", False, False), (None, False, False),
])
def test_offline_sample_requires_current_clean_binary(tmp_path, monkeypatch, commit, dirty, current):
    run = tmp_path / "sample"
    run.mkdir()
    (run / "manifest.json").write_text(json.dumps({
        "env_gates": {}, "missions": ["mc2_10"], "git_head": "a" * 40,
        "identity": {"binary_provenance": {"commit": commit, "dirty": dirty}},
    }))
    (run / "mc2_10.log").write_text("[NEW_SPAM v1]")
    monkeypatch.setattr(quiet, "ARTIFACT_ROOT", tmp_path)
    monkeypatch.setattr(quiet.subprocess, "check_output", lambda *a, **kw: "a" * 40 + "\n")
    monkeypatch.setattr(quiet, "load_baseline", lambda: {"tags": {}})
    monkeypatch.setattr(sys, "argv", ["quiet"])
    assert quiet.main() == (1 if current else 0)
