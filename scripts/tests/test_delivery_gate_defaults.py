"""Delivery defaults follow the build's gate table; missing evidence never passes."""
import importlib.util
import json
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
spec = importlib.util.spec_from_file_location("delivery_gate_defaults", ROOT / "scripts/check-delivery.py")
delivery = importlib.util.module_from_spec(spec)
spec.loader.exec_module(delivery)


def row(kind="Bool", default=True, registry=True, **extra):
    return {"name": "MC2_TEST", "kind": kind, "default": default,
            "registry": {"defaultOn": registry} if registry is not None else None, **extra}


def read(*rows):
    return delivery.default_from_table(json.dumps({"gates": rows}), "MC2_TEST")


@pytest.mark.parametrize("kind,value,registry,expected", [
    ("Bool", True, True, "ON"), ("Bool", False, False, "OFF"),
    ("Int", 0, False, "0"), ("Int", 2, True, "2"),
    ("Float", 0.349999994, True, "0.35"), ("Float", 1e-8, True, "1e-08")])
def test_typed_defaults(kind, value, registry, expected):
    result = read(row(kind, value, registry))
    assert result["default"] == expected
    assert result["agree"] is True


@pytest.mark.parametrize("kind,value", [("Bool", "false"), ("Bool", 1),
    ("Int", True), ("Int", 0.5), ("Float", "0.35"),
    ("Float", float("nan")), ("Float", float("inf"))])
def test_invalid_default_stays_unknown(kind, value):
    result = read(row(kind, value))
    assert result["default"] is None
    assert result["notes"]


@pytest.mark.parametrize("text", ["{", "null", "[]", '{}', '{"gates": {}}',
    '{"gates": [null]}', '{"gates": [{"name": "MC2_TEST"}]}'])
def test_malformed_table_stays_unknown(text):
    result = delivery.default_from_table(text, "MC2_TEST")
    assert result["default"] is None
    assert result["notes"]


def test_missing_duplicate_divergent_and_unverified_gates_stay_unknown():
    for result in (read(), read(row(), row()), read(row(divergent=True)),
                   read(row(verified=False)), read(row(verified="false")),
                   read(row(divergent="false")), read(row("String", "", None))):
        assert result["default"] is None
        assert result["notes"]


def test_registry_disagreement_remains_visible():
    for result in (read(row(default=False)), read(row("Int", 2, False))):
        assert result["agree"] is False
        assert any("REGISTRY LIES" in note for note in result["notes"])
    assert read(row(registry=None))["default"] == "ON"
    assert read(row(registry="true"))["default"] is None


def test_defaults_use_requested_revision_and_never_fall_back(monkeypatch):
    queries = []
    def show(sha, path):
        queries.append((sha, path))
        if sha == "before-gate-table":
            return None
        return json.dumps({"gates": [row(default=sha is None, registry=sha is None)]})
    monkeypatch.setattr(delivery, "show", show)
    assert delivery.read_default_at(None, "MC2_TEST")["default"] == "ON"
    assert delivery.read_default_at("built-commit", "MC2_TEST")["default"] == "OFF"
    assert delivery.read_default_at("before-gate-table", "MC2_TEST")["default"] is None
    assert queries == [(None, delivery.GATE_TABLE_REL), ("built-commit", delivery.GATE_TABLE_REL),
                       ("before-gate-table", delivery.GATE_TABLE_REL)]


def test_unreadable_table_is_unknown(monkeypatch):
    def unreadable(*args):
        raise PermissionError("table denied")
    monkeypatch.setattr(delivery, "show", unreadable)
    result = delivery.read_default_at(None, "MC2_TEST")
    assert result["default"] is None
    assert "table denied" in result["notes"][0]


def test_absent_historical_table_cannot_grade_as_delivered(monkeypatch):
    monkeypatch.setattr(delivery, "show", lambda *args: None)
    result = delivery.grade_default({"gate": "MC2_TEST"},
                                   {"fp": {"present": True, "sha": "old"}}, {})
    assert result["state"] == delivery.UNK
    assert result["value"] == "?"


def test_manifest_defaults_still_match_the_current_table():
    for fix in delivery.load_manifest():
        if fix.get("gate"):
            result = delivery.read_default_at(None, fix["gate"])
            assert result["default"] is not None, result["notes"]
            assert result["agree"] is True, result["notes"]
            assert delivery.expect_mismatch(result["default"], fix["expect_default"]) is None
