"""run_tidy_headers: the typedef check filter, the per-header bisect, and the lane exclusion (no clang-tidy, no ninja)."""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import run_tidy_headers as rth  # noqa: E402
import run_tidy_safe as rts  # noqa: E402


def test_typedef_aggregate_headers_skip_use_using():
    assert "modernize-use-using" not in rth.checks_for("typedef struct { int a; } T;\n")
    assert "modernize-use-using" not in rth.checks_for("typedef enum E { A } E;\n")
    assert "modernize-use-using" in rth.checks_for("typedef int (*Fn)(int);\n")
    assert "modernize-use-nullptr" in rth.checks_for("typedef struct { int a; } T;\n")


class _FakeProof(rth.Proof):
    """Objects that include a poisoned header fail whenever that header's edit is on disk."""

    poisoned: set = set()

    def bad_objects(self):
        bad = set()
        for h in self.headers:
            if self.on_disk[h] == self.edited[h] and h in self.poisoned:
                bad |= self.includers[h]
        return bad


def test_bisect_keeps_innocents_that_share_an_object_with_a_bad_header(tmp_path, monkeypatch):
    monkeypatch.setattr(rth, "ROOT", str(tmp_path))
    includers = {"a.h": {"o1"}, "b.h": {"o2"}, "c.h": {"o2", "o3"}, "d.h": {"o4"}}
    orig = {h: b"old\n" for h in includers}
    edited = {h: b"new\n" for h in includers}
    for h in includers:
        (tmp_path / h).write_bytes(orig[h])
    proof = _FakeProof(str(tmp_path), includers, orig, edited, 1)
    proof.poisoned = {"c.h"}
    kept, rejected = proof.solve()
    assert sorted(kept) == ["a.h", "b.h", "d.h"]
    assert [h for h, _ in rejected] == ["c.h"]
    assert (tmp_path / "c.h").read_bytes() == b"old\n" and (tmp_path / "a.h").read_bytes() == b"new\n"


def test_bisect_with_nothing_wrong_keeps_everything(tmp_path, monkeypatch):
    monkeypatch.setattr(rth, "ROOT", str(tmp_path))
    includers = {"a.h": {"o1"}, "b.h": {"o2"}}
    for h in includers:
        (tmp_path / h).write_bytes(b"old\n")
    proof = _FakeProof(str(tmp_path), includers, {h: b"old\n" for h in includers}, {h: b"new\n" for h in includers}, 1)
    kept, rejected = proof.solve()
    assert sorted(kept) == ["a.h", "b.h"] and not rejected


def _git(root, *a):
    subprocess.run(["git", "-C", str(root), "-c", "user.email=t@t", "-c", "user.name=t", *a], check=True, capture_output=True,
                   **rts.console_child.child_kwargs(redirected=True))


def test_lane_changed_files_lists_only_what_the_branch_changed(tmp_path):
    _git(tmp_path, "init", "-q", "-b", "main")
    (tmp_path / "x.cpp").write_text("x\n")
    (tmp_path / "y.cpp").write_text("y\n")
    _git(tmp_path, "add", "-A")
    _git(tmp_path, "commit", "-q", "-m", "init")
    _git(tmp_path, "checkout", "-q", "-b", "lane")
    (tmp_path / "y.cpp").write_text("y2\n")
    (tmp_path / "z.cpp").write_text("z\n")
    _git(tmp_path, "add", "-A")
    _git(tmp_path, "commit", "-q", "-m", "lane work")
    _git(tmp_path, "checkout", "-q", "main")
    assert rts.lane_changed_files("lane", str(tmp_path)) == {"y.cpp", "z.cpp"}


def test_lane_files_are_excluded():
    rts.LANE_FILES.clear()
    rts.LANE_FILES.add("mclib/some_lane_file.cpp")
    try:
        assert rts.is_excluded("mclib/some_lane_file.cpp") and not rts.is_excluded("mclib/other.cpp")
    finally:
        rts.LANE_FILES.clear()
