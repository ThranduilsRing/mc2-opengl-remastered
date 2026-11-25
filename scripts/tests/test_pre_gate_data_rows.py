"""Tests for the two W138h pre-gate data rows: the stock-data contract and the sim data fingerprint (no game, no gate run)."""
import json
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


@pytest.mark.parametrize("changed", [False, True])
def test_pre_gate_checks_selected_install(tmp_path, monkeypatch, capsys, changed):
    install = _stock_install(tmp_path)
    tree = tmp_path / "tree"
    tree.mkdir()
    baseline = sd.data_fingerprint(str(install), {"overlay": {}, "stock_digest": ""})
    monkeypatch.setattr(lm, "recorded_data_fingerprint", lambda root: baseline)
    monkeypatch.setattr(lm, "run", lambda *args, **kwargs: _r(0))
    if changed:
        (install / "mission.fst").write_bytes(b"changed stock")
    args = lm.build_parser().parse_args(["--pre-gate", "--install", str(install)])
    assert lm.pre_gate(str(tree), install=args.install) == int(changed)
    assert "pre-gate: data_fingerprint: " + ("FAIL" if changed else "OK") in capsys.readouterr().out


def test_pre_gate_data_fingerprint_fail_fails_the_gate(monkeypatch, tmp_path, capsys):
    monkeypatch.setattr(lm, "run", lambda r, cmd, env=None: _r(0))
    monkeypatch.setattr(lm, "data_fingerprint_check", lambda r: ("FAIL", "stock_digest x is not the baseline's y"))
    rc = lm.pre_gate(str(tmp_path))
    assert rc == 1
    out = capsys.readouterr().out
    assert "pre-gate: data_fingerprint: FAIL" in out
    assert "stock_digest x is not the baseline's y" in out
    assert "PRE-GATE FAIL: data_fingerprint" in out


def test_pre_gate_data_fingerprint_skipped_prints_reason_and_passes(monkeypatch, tmp_path, capsys):
    monkeypatch.setattr(lm, "run", lambda r, cmd, env=None: _r(0))
    monkeypatch.setattr(lm, "data_fingerprint_check", lambda r: ("SKIPPED", "no stock reference install at X to fingerprint"))
    rc = lm.pre_gate(str(tmp_path))
    assert rc == 0
    out = capsys.readouterr().out
    assert "pre-gate: data_fingerprint: SKIPPED" in out
    assert "no stock reference install at X to fingerprint" in out   # the one-line reason
    assert "PRE-GATE PASS" in out


def _stock_install(tmp_path, name="install", parts=("mission.fst",)):
    """A tiny stock reference install: the named archives with distinct content (the gate's deploy target, stand-in)."""
    install = tmp_path / name
    install.mkdir()
    for part in parts:
        (install / part).write_bytes(part.encode() + b"-stock")
    return install


def test_candidate_fingerprint_reads_the_install_not_the_tree(tmp_path, monkeypatch):
    install = _stock_install(tmp_path, parts=("mission.fst", "tgl.fst"))
    tree = tmp_path / "tree"; tree.mkdir()   # a source worktree: the stock parts are gitignored and absent
    fp, reason = lm.candidate_data_fingerprint(str(tree), str(install))
    assert reason == "" and fp is not None
    assert "mission.fst" in fp["inputs"] and "tgl.fst" in fp["inputs"]   # read from the install, not the empty tree


def test_candidate_fingerprint_no_stock_install_skips(tmp_path, monkeypatch):
    install = str(tmp_path / "absent-install")
    fp, reason = lm.candidate_data_fingerprint(str(tmp_path), install)
    assert fp is None and "no stock reference install" in reason


def test_missing_default_install_warns_loudly(tmp_path, monkeypatch, capsys):
    isdir = lm.os.path.isdir
    monkeypatch.setattr(lm.os.path, 'isdir', lambda path: False if path == 'G:/mc2-installs/merge-gate' else isdir(path))
    verdict, reason = lm.data_fingerprint_check(str(tmp_path))
    assert verdict == 'SKIPPED' and 'no stock reference install' in reason
    out = capsys.readouterr().out
    assert 'WARN: default pre-gate install missing: G:/mc2-installs/merge-gate' in out
    assert 'data fingerprint unchecked' in out and '--install' in out


def test_candidate_fingerprint_uses_the_tree_overlay_manifest(tmp_path, monkeypatch):
    """The overlay comes from the candidate tree's data_overlay.json, not the module's checkout (whose declared fixtures
    are absent from the stand-in install and would all read 'missing')."""
    install = _stock_install(tmp_path)
    os.makedirs(os.path.join(install, "data", "missions"))
    overlay_file = os.path.join(install, "data", "missions", "fx.fit")
    with open(overlay_file, "wb") as fh:
        fh.write(b"fixture")
    tree = tmp_path / "tree"
    os.makedirs(os.path.join(tree, "tests", "smoke", "sim_determinism"))
    manifest = {"overlay": {"data/missions/fx.fit": sd._sha256(overlay_file)}, "stock_digest": ""}
    with open(os.path.join(tree, "tests", "smoke", "sim_determinism", "data_overlay.json"), "w", encoding="utf-8") as fh:
        json.dump(manifest, fh)
    fp, _ = lm.candidate_data_fingerprint(str(tree), str(install))
    assert set(fp["overlay"]) == {"data/missions/fx.fit"} and not fp["problems"]


def test_data_fingerprint_check_ok_when_install_matches_baseline(tmp_path, monkeypatch):
    install = _stock_install(tmp_path)
    tree = tmp_path / "tree"; tree.mkdir()   # no .fst in the tree: it must compare against the install, never a false FAIL
    fp, _ = lm.candidate_data_fingerprint(str(tree), str(install))
    monkeypatch.setattr(lm, "recorded_data_fingerprint", lambda root: fp)
    assert lm.data_fingerprint_check(str(tree), install=str(install)) == ("OK", "")


def test_data_fingerprint_check_skips_without_baseline(tmp_path, monkeypatch):
    install = _stock_install(tmp_path)
    tree = tmp_path / "tree"; tree.mkdir()
    monkeypatch.setattr(lm, "recorded_data_fingerprint", lambda root: None)
    assert lm.data_fingerprint_check(str(tree), install=str(install)) == ("SKIPPED", "no recorded baseline data fingerprint")


def test_data_fingerprint_check_skips_without_stock_install(tmp_path, monkeypatch):
    install = str(tmp_path / "absent-install")
    monkeypatch.setattr(lm, "recorded_data_fingerprint", lambda root: {"digest": "d", "inputs": [], "stock_digest": "s"})
    assert lm.data_fingerprint_check(str(tmp_path), install=install) == (
        "SKIPPED", "no stock reference install at " + str(tmp_path / "absent-install") + " to fingerprint")


def test_data_fingerprint_check_fails_on_real_stock_part_change(tmp_path, monkeypatch):
    install = _stock_install(tmp_path)
    tree = tmp_path / "tree"; tree.mkdir()
    fp_before, _ = lm.candidate_data_fingerprint(str(tree), str(install))
    monkeypatch.setattr(lm, "recorded_data_fingerprint", lambda root: fp_before)
    assert lm.data_fingerprint_check(str(tree), install=str(install)) == ("OK", "")
    (install / "mission.fst").write_bytes(b"stock-v2-changed")   # a real stock-part change in the install
    verdict, detail = lm.data_fingerprint_check(str(tree), install=str(install))
    assert verdict == "FAIL" and "stock_digest" in detail


def test_differing_stock_parts_lists_absent_extra_and_problems():
    fp = {"inputs": ["mission.fst", "new.fst"], "stock_digest": "s1", "problems": ["overlay file x is missing"]}
    base = {"inputs": ["mission.fst", "tgl.fst"], "stock_digest": "s2"}
    detail = lm.differing_stock_parts(fp, base)
    assert "tgl.fst" in detail and "absent from the install" in detail
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
