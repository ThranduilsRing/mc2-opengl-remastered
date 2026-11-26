"""Staging treats a dirty README.md (owner edits) as clean, and any other dirty path as blocking (docs/merge-queue-v2.md)."""
import os
import sys
import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(ROOT, "tools"))

import lane_merge_stage as st  # noqa: E402


def test_readme_alone_is_not_dirty(monkeypatch):
    monkeypatch.setattr(st._git_util, "status_entries", lambda root: [(" M", "README.md")])
    assert st._dirty_besides_readme("unused") == []


def test_other_paths_stay_dirty(monkeypatch):
    monkeypatch.setattr(st._git_util, "status_entries", lambda root: [(" M", "README.md"), (" M", "tools/x.py")])
    assert st._dirty_besides_readme("unused") == [(" M", "tools/x.py")]


@pytest.mark.parametrize("xy", ["M ", "A ", "MM"])
def test_staged_readme_stays_dirty(monkeypatch, xy):
    monkeypatch.setattr(st._git_util, "status_entries", lambda root: [(xy, "README.md")])
    assert st._dirty_besides_readme("unused") == [(xy, "README.md")]
