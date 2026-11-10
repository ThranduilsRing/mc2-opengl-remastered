"""A delivery entry must be able to answer NO: its needles have to name live code, not retired text."""
import importlib.util
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("delivery_vacuous", ROOT / "scripts" / "check-delivery.py")
delivery = importlib.util.module_from_spec(spec)
spec.loader.exec_module(delivery)

RETIRED = set(json.loads((ROOT / "Gates" / "retired_names.json").read_text(encoding="utf-8")))


def _head(path):
    r = subprocess.run(["git", "show", f"HEAD:{path}"], cwd=ROOT, capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    return r.stdout if r.returncode == 0 else ""


def vacuity(fix, head=_head):
    """Reasons an entry cannot fail; empty when it is a real check."""
    why = []
    needle = fix.get("exe_needle")
    if needle and needle in RETIRED:
        why.append(f"exe_needle {needle} is a retired gate name")
    # gated entries grade source@installed-sha (history may differ); ungated ones must hold at HEAD
    for ev in (fix.get("code_evidence") or []) if not fix.get("gate") else []:
        paths = [ev["path"], *ev.get("alternate_paths", [])]
        if not any(ev["needle"] in head(p) for p in paths):
            why.append(f"source needle {ev['needle']!r} not found in {paths} at HEAD")
    if needle and not fix.get("gate"):
        srcs = "".join(head(ev_p) for ev in fix.get("code_evidence") or []
                       for ev_p in [ev["path"], *ev.get("alternate_paths", [])])
        if needle not in srcs:
            why.append(f"ungated exe_needle {needle!r} is not a literal in the evidence sources")
    return why


def test_manifest_entries_are_not_vacuous():
    bad = {f["id"]: vacuity(f) for f in delivery.load_manifest() if vacuity(f)}
    assert not bad, bad


def test_a_retired_gate_needle_is_flagged():
    fix = {"id": "x", "gate": None, "exe_needle": "MC2_OVERLAY_CEMENT_ADOPT", "code_evidence": []}
    assert vacuity(fix, head=lambda p: "")
