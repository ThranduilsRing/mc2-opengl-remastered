"""LG14 part 2: the FIT schema checker's tactic vocabulary must accept every name the runtime accepts.

The runtime vocabulary is generated from code/tactic_table.h (canonical names plus brain aliases); this test parses that
table text and compares it with TACTIC_NAMES in scripts/check-brain-fit-schema.py.
"""
import importlib.util
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def _load_schema_module():
    spec = importlib.util.spec_from_file_location("check_brain_fit_schema", ROOT / "scripts" / "check-brain-fit-schema.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _table_text():
    return (ROOT / "code" / "tactic_table.h").read_text(encoding="utf-8")


def _runtime_names():
    text = _table_text()
    table = text[text.index("#define MC2_TACTIC_TABLE(X)"):text.index("#define MC2_TACTIC_ALIASES(X)")]
    rows = re.findall(r'X\(\s*\w+,\s*\w+,\s*(\d+),\s*"(\w+)"', table)
    alias_block = text[text.index("#define MC2_TACTIC_ALIASES(X)"):text.index("namespace tactic_table")]
    aliases = re.findall(r'X\("(\w+)",\s*(\d+)\)', alias_block)
    return rows, aliases


def test_table_has_twelve_rows_with_fixed_values():
    rows, _ = _runtime_names()
    assert [int(v) for v, _ in rows] == list(range(12))
    assert [n for _, n in rows] == [
        "None", "FlankLeft", "FlankRight", "FlankRear", "StopAndFire", "Turret", "Joust",
        "IndirectFire", "HullDown", "FightingWithdraw", "Pursue", "HitAndRun",
    ]


def test_schema_checker_accepts_every_runtime_tactic_name():
    rows, aliases = _runtime_names()
    runtime = {n for _, n in rows if n != "None"} | {a for a, _ in aliases}
    missing = runtime - set(_load_schema_module().TACTIC_NAMES)
    assert not missing, f"schema checker rejects runtime tactic names: {sorted(missing)}"
