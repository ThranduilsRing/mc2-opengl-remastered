"""What: tools/fs_links.py reports symlinks and Windows junctions (the raw
FILE_ATTRIBUTE_REPARSE_POINT, which os.path.islink/Path.is_symlink miss since
Python 3.13) and walk_no_links prunes link/junction dirs and yields no link files.
Why: round-7 item 1 -- on 3.14 a junction reads as a plain directory, os.walk
(followlinks=False) descends into it, and a delete or walk that trusts islink can
follow it out of its tree. All tests use tmp dirs only: a real junction made with
`cmd /c mklink /J` (skipped only if mklink fails) and a symlink (skipped when the
host lacks link privilege); never a real install, slot or scratch dir."""
import os
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
sys.path.insert(0, str(ROOT / "tools"))
import console_child  # noqa: E402
import fs_links  # noqa: E402


def make_junction(link, target):
    r = subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(target)],
                       capture_output=True, text=True,
                       **console_child.child_kwargs(redirected=True))
    if r.returncode != 0:
        pytest.skip(f"cannot create junction: {r.stderr.strip() or r.stdout.strip()}")


def make_symlink(link, target, is_dir):
    try:
        os.symlink(target, link, target_is_directory=is_dir)
    except OSError as exc:
        pytest.skip(f"symlink not permitted on this host: {exc}")


@pytest.mark.skipif(os.name != "nt", reason="Windows NTFS junction via cmd mklink /J")
def test_junction_is_detected(tmp_path):
    target = tmp_path / "target"
    target.mkdir()
    (target / "inside.txt").write_text("x")
    link = tmp_path / "link"
    make_junction(link, target)
    assert fs_links.is_link_or_junction(link) is True
    assert fs_links.is_link_or_junction(target) is False
    if os.name == "nt":
        assert os.path.islink(link) is False   # the 3.13+ gap the helper exists for
    assert (target / "inside.txt").is_file()


def test_symlink_is_detected(tmp_path):
    target = tmp_path / "target"
    target.mkdir()
    make_symlink(tmp_path / "link", target, True)
    assert fs_links.is_link_or_junction(tmp_path / "link") is True


def test_plain_dir_and_file_are_not_links(tmp_path):
    d = tmp_path / "d"
    d.mkdir()
    f = d / "f.txt"
    f.write_text("x")
    assert fs_links.is_link_or_junction(d) is False
    assert fs_links.is_link_or_junction(f) is False


def test_missing_path_is_false(tmp_path):
    assert fs_links.is_link_or_junction(tmp_path / "absent") is False


@pytest.mark.skipif(os.name != "nt", reason="Windows NTFS junction via cmd mklink /J")
def test_walk_no_links_prunes_junction_dirs(tmp_path):
    outside = tmp_path / "outside"
    outside.mkdir()
    (outside / "victim.txt").write_text("x")
    root = tmp_path / "root"
    (root / "real").mkdir(parents=True)
    (root / "real" / "keep.txt").write_text("x")
    (root / "top.txt").write_text("x")
    make_junction(root / "linked", outside)
    seen_dirs, seen_files = [], []
    for dirpath, dirs, files in fs_links.walk_no_links(root):
        seen_dirs.append(Path(dirpath))
        seen_files.extend(Path(dirpath) / n for n in files)
    assert root / "linked" not in seen_dirs
    assert Path(outside / "victim.txt") not in seen_files
    assert (root / "top.txt") in seen_files
    assert (root / "real" / "keep.txt") in seen_files
    assert (outside / "victim.txt").is_file()


def test_walk_no_links_yields_no_link_files(tmp_path):
    root = tmp_path / "root"
    root.mkdir()
    (root / "real.txt").write_text("x")
    (root / "target.txt").write_text("y")
    make_symlink(root / "linked.txt", root / "target.txt", False)
    files = [n for _, _, fs in fs_links.walk_no_links(root) for n in fs]
    assert sorted(files) == ["real.txt", "target.txt"]
