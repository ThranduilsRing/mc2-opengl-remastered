"""--pre-gate surfaces nightly freshness as a WARN line (never a fail), reusing lane_merge_stage.untriaged_nightly_fail."""
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "scripts"))

import lane_merge as lm  # noqa: E402


def _run_ok(root, cmd, env=None):
    return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")


def _pre_gate(monkeypatch, tmp_path, proofs):
    monkeypatch.setattr(lm, "run", _run_ok)
    monkeypatch.setattr(lm, "data_fingerprint_check", lambda root: ("OK", ""))
    return lm.pre_gate(str(tmp_path), str(tmp_path / "none.exe"), nightly_proofs=str(proofs))


def test_pre_gate_warns_when_no_nightly_exists_and_still_passes(monkeypatch, capsys, tmp_path):
    assert _pre_gate(monkeypatch, tmp_path, tmp_path / "proofs") == 0
    out = capsys.readouterr().out
    assert "pre-gate: nightly: WARN" in out and "no nightly verdict" in out
    assert out.strip().splitlines()[-1] == "PRE-GATE PASS"


def test_pre_gate_warns_on_untriaged_nightly_fail_but_not_a_fresh_pass(monkeypatch, capsys, tmp_path):
    proofs = tmp_path / "proofs"
    d = proofs / "nightly-2026-11-10"
    d.mkdir(parents=True)
    (d / "gate-verdict.json").write_text(json.dumps({"verdict": "FAIL", "failed": ["x"]}))
    assert _pre_gate(monkeypatch, tmp_path, proofs) == 0
    out = capsys.readouterr().out
    assert "pre-gate: nightly: WARN" in out and "untriaged" in out
    (d / "gate-verdict.json").write_text(json.dumps({"verdict": "PASS", "failed": []}))   # written = file mtime = now
    assert _pre_gate(monkeypatch, tmp_path, proofs) == 0
    assert "pre-gate: nightly: OK" in capsys.readouterr().out
