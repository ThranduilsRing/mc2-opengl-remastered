"""The drift check grades committed goldens even in a fresh worktree."""
import importlib.util
import json
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("manifest_schema_check", ROOT / "scripts/check-manifest-schema.py")
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


def run(monkeypatch, root):
    monkeypatch.setattr(sys, "argv", ["check-manifest-schema.py", "--strict", str(root)])
    return checker.main()


def test_tracked_goldens_supply_a_conformant_corpus(monkeypatch, capsys):
    root = ROOT / "tests/visual/goldens"
    count = len(list(root.glob("*/golden.json")))
    assert count > 0
    assert run(monkeypatch, root) == 0
    assert f"scanned={count} conformant={count} legacy=0 broken=0" in capsys.readouterr().out


def test_golden_missing_identity_fails(monkeypatch, tmp_path):
    (tmp_path / "golden.json").write_text(json.dumps({"kind": "golden"}), encoding="utf-8")
    assert run(monkeypatch, tmp_path) == 1


def test_golden_invalid_identity_fails(monkeypatch, tmp_path):
    (tmp_path / "golden.json").write_text(json.dumps({"identity": {"schema": "wrong"}}), encoding="utf-8")
    assert run(monkeypatch, tmp_path) == 1


def test_empty_corpus_still_fails(monkeypatch, tmp_path):
    assert run(monkeypatch, tmp_path) == 1
