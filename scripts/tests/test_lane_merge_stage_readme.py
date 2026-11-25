"""Staging treats a dirty README.md (owner edits) as clean, and any other dirty path as blocking (docs/merge-queue-v2.md)."""
import os
import sys

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
