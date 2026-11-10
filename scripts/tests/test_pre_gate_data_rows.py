"""Tests for the two W138h pre-gate data rows: the stock-data contract and the sim data fingerprint (no game, no gate run)."""
import os
import sys
import types
import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(ROOT, "scripts"))

from tools import lane_merge as lm  # noqa: E402
import sim_determinism as sd  # noqa: E402


def _r(rc, out=""):
    return types.SimpleNamespace(returncode=rc, stdout=out)


def test_pre_gate_runs_stock_mission_data_with_contracts_args(monkeypatch, tmp_path, capsys):
    seen = []
    monkeypatch.setattr(lm, "run", lambda r, cmd, env=None: seen.append(cmd) or _r(0))
    monkeypatch.setattr(lm, "data_fingerprint_check", lambda r: ("OK", ""))
    rc = lm.pre_gate(str(tmp_path))
    assert rc == 0
    stock = [c for c in seen if any("check-stock-mission-data.py" in a for a in c)]
    assert len(stock) == 1
    assert stock[0][-2:] == ["--no-data", "--quiet"]   # the contracts board's own args
    out = capsys.readouterr().out
    assert "pre-gate: stock_mission_data: OK" in out
    assert "pre-gate: data_fingerprint: OK" in out
    assert "PRE-GATE PASS" in out


def test_pre_gate_stock_mission_data_red_fails_the_gate(monkeypatch, tmp_path, capsys):
    def fake_run(r, cmd, env=None):
        return _r(2, "STOCK-DATA-TRUTH-1 FAIL: a hard-coded mission dir") if any("check-stock-mission-data.py" in a for a in cmd) else _r(0)
    monkeypatch.setattr(lm, "run", fake_run)
    monkeypatch.setattr(lm, "data_fingerprint_check", lambda r: ("OK", ""))
    rc = lm.pre_gate(str(tmp_path))
    assert rc == 1
    out = capsys.readouterr().out
    assert "pre-gate: stock_mission_data: FAIL" in out
    assert "PRE-GATE FAIL: stock_mission_data" in out


def test_pre_gate_data_fingerprint_fail_fails_the_gate(monkeypatch, tmp_path, capsys):
    monkeypatch.setattr(lm, "run", lambda r, cmd, env=None: _r(0))
    monkeypatch.setattr(lm, "data_fingerprint_check", lambda r: ("FAIL", "stock_digest x is not the baseline's y"))
    rc = lm.pre_gate(str(tmp_path))
    assert rc == 1
    out = capsys.readouterr().out
    assert "pre-gate: data_fingerprint: FAIL" in out
    assert "stock_digest x is not the baseline's y" in out
    assert "PRE-GATE FAIL: data_fingerprint" in out


def test_data_fingerprint_check_matches(monkeypatch):
    fp = {"digest": "abc123", "inputs": ["mission.fst"], "stock_digest": "s1", "problems": []}
    base = {"digest": "abc123", "inputs": ["mission.fst"], "stock_digest": "s1"}
    monkeypatch.setattr(sd, "data_fingerprint", lambda root: fp)
    monkeypatch.setattr(lm, "recorded_data_fingerprint", lambda root: base)
    assert lm.data_fingerprint_check("/fake") == ("OK", "")


def test_data_fingerprint_check_mismatch_names_stock_parts(monkeypatch):
    fp = {"digest": "abc123", "inputs": ["mission.fst"], "stock_digest": "s1", "problems": []}
    base = {"digest": "def456", "inputs": ["mission.fst", "tgl.fst"], "stock_digest": "s2"}
    monkeypatch.setattr(sd, "data_fingerprint", lambda root: fp)
    monkeypatch.setattr(lm, "recorded_data_fingerprint", lambda root: base)
    verdict, detail = lm.data_fingerprint_check("/fake")
    assert verdict == "FAIL"
    assert "tgl.fst" in detail and "s1" in detail and "s2" in detail


def test_data_fingerprint_check_no_baseline_skips(monkeypatch):
    monkeypatch.setattr(sd, "data_fingerprint", lambda root: {"digest": "abc", "inputs": [], "stock_digest": "s", "problems": []})
    monkeypatch.setattr(lm, "recorded_data_fingerprint", lambda root: None)
    assert lm.data_fingerprint_check("/fake") == ("SKIPPED", "no recorded baseline data fingerprint")


def test_differing_stock_parts_lists_absent_extra_and_problems():
    fp = {"inputs": ["mission.fst", "new.fst"], "stock_digest": "s1", "problems": ["overlay file x is missing"]}
    base = {"inputs": ["mission.fst", "tgl.fst"], "stock_digest": "s2"}
    detail = lm.differing_stock_parts(fp, base)
    assert "tgl.fst" in detail and "absent from the tree" in detail
    assert "new.fst" in detail and "not in the baseline" in detail
    assert "s1" in detail and "s2" in detail
    assert "overlay file x is missing" in detail


def test_recorded_data_fingerprint_reads_common_digest(monkeypatch, tmp_path):
    paths = {}
    for m in lm.FINGERPRINT_MISSIONS:
        p = tmp_path / (m + ".json")
        p.write_text('{"data_fingerprint": {"digest": "common123", "inputs": ["mission.fst"]}}', encoding="utf-8")
        paths[m] = str(p)
    monkeypatch.setattr(sd, "baseline_path", lambda m: paths[m])
    fp = lm.recorded_data_fingerprint("/fake")
    assert fp is not None and fp["digest"] == "common123"


def test_recorded_data_fingerprint_none_when_disagree(monkeypatch, tmp_path):
    paths = {}
    for i, m in enumerate(lm.FINGERPRINT_MISSIONS):
        p = tmp_path / (m + ".json")
        p.write_text('{"data_fingerprint": {"digest": "%s"}}' % ("d1" if i < 2 else "d2"), encoding="utf-8")
        paths[m] = str(p)
    monkeypatch.setattr(sd, "baseline_path", lambda m: paths[m])
    assert lm.recorded_data_fingerprint("/fake") is None
