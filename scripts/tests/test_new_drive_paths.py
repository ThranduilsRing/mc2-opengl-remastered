"""tools/new_drive_paths.py: a diff may not add a hardcoded A:/ E:/ G:/ path to scripts/ or tools/ Python code (the laptop gate has no E:)."""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import new_drive_paths as ndp  # noqa: E402


def _git(repo, *a):
    subprocess.run(["git", "-C", str(repo), "-c", "user.name=t", "-c", "user.email=t@t", *a], check=True, capture_output=True)  # background-launch: allow test fixture git on a tmp repo


def _repo(tmp_path, old, new, rel="scripts/x.py"):
    (tmp_path / "scripts").mkdir()
    p = tmp_path / rel
    _git(tmp_path, "init", "-q", "-b", "main")
    p.write_text(old)
    _git(tmp_path, "add", ".")
    _git(tmp_path, "commit", "-qm", "base")
    _git(tmp_path, "checkout", "-qb", "lane")
    p.write_text(new)
    _git(tmp_path, "commit", "-qam", "lane")


def _check(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    return ndp.check("main", "lane")


def test_added_hardcoded_drive_path_is_flagged(tmp_path, monkeypatch):
    _repo(tmp_path, 'OLD = "A:/Games/old"\n', 'OLD = "A:/Games/old"\nCACHE = "E:/mc2-cook-cache"\n')
    bad = _check(tmp_path, monkeypatch)
    assert len(bad) == 1 and "scripts/x.py:2" in bad[0] and "E:/mc2-cook-cache" in bad[0]   # the pre-existing line is not re-flagged


def test_env_default_docstring_comment_marker_and_tests_are_fine(tmp_path, monkeypatch):
    new = ('"""Mentions E:/mc2-archive in prose."""\nimport os\n'
           'R = os.environ.get("MC2_ROOT", "A:/Games/mc2-main")\n'
           '# G:/mc2-tmp in a comment\n'
           'T = "G:/mc2-tmp"  # drive-path: ok PC-only tool\n')
    _repo(tmp_path, "X = 1\n", new)
    assert _check(tmp_path, monkeypatch) == []
    assert not ndp.checked("scripts/tests/test_a.py") and not ndp.checked("tools/lane_slot.py") and ndp.checked("scripts/merge_gate.py")
