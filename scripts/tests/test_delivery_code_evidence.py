"""Source splits preserve delivery evidence without making ancestry a pass."""
import importlib.util
from pathlib import Path


spec = importlib.util.spec_from_file_location(
    "delivery_code_evidence", Path(__file__).resolve().parents[1] / "check-delivery.py")
delivery = importlib.util.module_from_spec(spec)
spec.loader.exec_module(delivery)


def grade(monkeypatch, bodies, *, dirty=False):
    monkeypatch.setattr(delivery, "show", lambda sha, path: bodies.get((sha, path)))
    monkeypatch.setattr(delivery, "is_ancestor", lambda ancestor, sha: True)
    fix = {"commit": "introducer", "code_evidence": [
        {"path": "declaration.h", "needle": "gather_all"},
        {"path": "old.cpp", "needle": "gather_all();", "alternate_paths": ["split.cpp"]},
    ]}
    inst = {"fp": {"present": True, "sha": "installed", "dirty": dirty}}
    return delivery.grade_code(fix, inst)


def test_original_source_location_still_counts(monkeypatch):
    result = grade(monkeypatch, {("installed", "declaration.h"): "void gather_all();",
                                ("installed", "old.cpp"): "obj.gather_all();"})
    assert (result["state"], result["tier"]) == (delivery.YES, "source@sha")


def test_relocated_call_counts_at_the_installed_commit(monkeypatch):
    result = grade(monkeypatch, {("installed", "declaration.h"): "void gather_all();",
                                ("installed", "old.cpp"): "other code",
                                ("installed", "split.cpp"): "obj.gather_all();"})
    assert (result["state"], result["tier"]) == (delivery.YES, "source@sha")


def test_declaration_without_call_still_fails_despite_ancestry(monkeypatch):
    result = grade(monkeypatch, {("installed", "declaration.h"): "void gather_all();",
                                ("installed", "split.cpp"): "other code",
                                ("HEAD", "split.cpp"): "obj.gather_all();"})
    assert (result["state"], result["tier"]) == (delivery.NO, "source@sha")


def test_call_without_declaration_still_fails(monkeypatch):
    result = grade(monkeypatch, {("installed", "split.cpp"): "obj.gather_all();"})
    assert result["state"] == delivery.NO


def test_dirty_build_cannot_use_relocated_source_as_binary_evidence(monkeypatch):
    result = grade(monkeypatch, {("installed", "declaration.h"): "void gather_all();",
                                ("installed", "split.cpp"): "obj.gather_all();"}, dirty=True)
    assert result["state"] == delivery.UNK


def test_relocation_cannot_replace_evidence_at_the_introducing_commit(monkeypatch):
    monkeypatch.setattr(delivery, "commit_exists", lambda sha: True)
    monkeypatch.setattr(delivery, "show", lambda sha, path:
                        "obj.gather_all();" if path == "split.cpp" else "other code")
    fix = {"commit": "introducer", "code_evidence": [
        {"path": "old.cpp", "needle": "gather_all();", "alternate_paths": ["split.cpp"]},
    ]}
    problems = delivery.check_integrity(fix)
    assert len(problems) == 1 and "absent from old.cpp at introducer" in problems[0]
