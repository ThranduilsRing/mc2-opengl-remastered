"""laptop_gate fast offline repros: case-variant preflight delete (W148 run 1) and stale kept candidate over the seed."""
import csv
import hashlib
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "scripts"))

import laptop_gate  # noqa: E402


def test_tree_diff_never_deletes_a_case_variant_of_a_source_file():
    # W148 run 1: the PC lists MCL_CM_5.png, the laptop (case-insensitive) holds mcl_cm_5.png. The old diff copied the PC name
    # and deleted the laptop name, which on Windows is the same file, so the preflight removed the file it had just placed.
    pc = ["data/MCL_CM_5.png|5|100", "data/Mcl_mn_4.png|7|100"]
    lap = ["data/mcl_cm_5.png|5|100", "data/mcl_mn_4.png|7|100", "data/stale.png|1|1"]
    assert laptop_gate.tree_diff(pc, lap) == ([], [], ["data/stale.png"])
    # changed content under a variant name: copy the PC name, still no delete of the variant
    lap2 = ["data/mcl_cm_5.png|9|100"]
    copy, touch, delete = laptop_gate.tree_diff(["data/MCL_CM_5.png|5|100"], lap2)
    assert copy == ["data/MCL_CM_5.png"] and delete == []


def test_tree_problems_ignores_case_only_differences():
    assert laptop_gate.tree_problems(["A/B.png|5|100"], ["a/b.png|5|100"]) == []
    assert laptop_gate.tree_problems(["A/B.png|5|100"], ["a/b.png|6|100"]) != []


def _mk(base, rel, commit, files):
    d = base / rel
    d.mkdir(parents=True)
    with open(d / ".deployed_manifest.csv", "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["manifest_version", "v1"])
        w.writerow(["relpath", "sha256", "bytes", "src_commit", "timestamp", "mtime_ns"])
        for rel_f, data in files.items():
            w.writerow([rel_f, hashlib.sha256(data).hexdigest(), len(data), commit, "t", 0])
    return d


def test_candidate_install_non_keep_run_picks_the_seed_over_a_leftover_candidate(tmp_path, monkeypatch):
    monkeypatch.setattr(laptop_gate, "G", tmp_path.as_posix())
    base = tmp_path / "drv/E/mc2-installs"
    kept = _mk(base, "merge-gate-candidate", "2222222222bb", {"mc2.exe": b"x", "mods/m.dat": b"m"})
    seed = _mk(base, "_gate-reuse/deployed", "2222222222bb", {"mc2.exe": b"x"})
    head = "2222222222bbcc"
    assert laptop_gate.candidate_install(head) == seed
    assert laptop_gate.candidate_install(head, keep=False) == seed
    assert laptop_gate.candidate_install(head, keep=True) == kept


def test_candidate_identity_problems_flags_a_manifest_that_differs_from_the_seed(tmp_path, monkeypatch):
    monkeypatch.setattr(laptop_gate, "G", tmp_path.as_posix())
    base = tmp_path / "drv/E/mc2-installs"
    kept = _mk(base, "merge-gate-candidate", "2222222222bb", {"mc2.exe": b"x", "mods/m.dat": b"m"})
    seed = _mk(base, "_gate-reuse/deployed", "2222222222bb", {"mc2.exe": b"x"})
    assert laptop_gate.candidate_identity_problems(seed, seed) == []
    why = laptop_gate.candidate_identity_problems(kept, seed)
    assert len(why) == 1 and "manifest" in why[0] and "merge-gate-candidate" in why[0]
    monkeypatch.setattr(laptop_gate, "G", (tmp_path / "none").as_posix())
    assert laptop_gate.candidate_identity_problems(kept, None) == []   # no seed to bind to: nothing to compare
