#!/usr/bin/env python3
# scripts/tests/test_deploy_build_layout.py
"""Tests for deploy_payload's VS-vs-Ninja build-tree layout detection.

The VS tree nests outputs under a config dir; the Ninja dev tree (build64-ninja)
is single-config and does not. Deploy must find the exe, launcher and the
freshness-reference .obj files in both without the caller saying which.

Run:
  py -3 -m pytest scripts/tests/test_deploy_build_layout.py -v
"""
from __future__ import annotations

import os
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from scripts import deploy_payload as dp  # noqa: E402


def _touch(p: Path, mtime=None):
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(b"x")
    if mtime is not None:
        os.utime(p, (mtime, mtime))
    return p


def _vs_tree(tmp_path):
    b = tmp_path / "build64"
    _touch(b / "RelWithDebInfo" / "mc2.exe")
    _touch(b / "out" / "mc2_launcher" / "RelWithDebInfo" / "mc2-launcher.exe")
    _touch(b / "mc2.dir" / "RelWithDebInfo" / "a.obj")
    return b


def _ninja_tree(tmp_path):
    b = tmp_path / "build64-ninja"
    _touch(b / "build.ninja")
    _touch(b / "mc2.exe")
    _touch(b / "out" / "mc2_launcher" / "mc2-launcher.exe")
    _touch(b / "CMakeFiles" / "mc2.dir" / "code" / "a.cpp.obj")
    return b


def test_vs_layout_not_single_config(tmp_path):
    b = _vs_tree(tmp_path)
    bd = str(b / "RelWithDebInfo")
    root = dp.find_build_root(bd)
    assert root == str(b)
    assert not dp.is_single_config(bd, root)
    assert dp.launcher_path(root, "RelWithDebInfo", False) == str(
        b / "out" / "mc2_launcher" / "RelWithDebInfo" / "mc2-launcher.exe")
    assert dp.mc2_obj_dir(root, "RelWithDebInfo", False) == str(b / "mc2.dir" / "RelWithDebInfo")


def test_ninja_layout_detected(tmp_path):
    b = _ninja_tree(tmp_path)
    root = dp.find_build_root(str(b))
    assert root == str(b)
    assert dp.is_single_config(str(b), root)
    assert Path(dp.launcher_path(root, "build64-ninja", True)).is_file()
    assert Path(dp.mc2_obj_dir(root, "build64-ninja", True)).is_dir()


def test_enumerate_payload_finds_ninja_launcher(tmp_path):
    b = _ninja_tree(tmp_path)
    items = dp.enumerate_payload(str(ROOT), str(b), "mc2.exe", "mc2.pdb")
    rel = {r for _s, r, _k in items}
    assert "mc2.exe" in rel and "mc2-launcher.exe" in rel
    launcher = [s for s, r, _k in items if r == "mc2-launcher.exe"][0]
    assert Path(launcher) == b / "out" / "mc2_launcher" / "mc2-launcher.exe"


def test_stale_exe_detected_in_ninja_tree(tmp_path):
    """An .obj newer than the exe means the link did not happen."""
    b = _ninja_tree(tmp_path)
    now = time.time()
    os.utime(b / "mc2.exe", (now - 100, now - 100))
    os.utime(b / "CMakeFiles" / "mc2.dir" / "code" / "a.cpp.obj", (now, now))
    try:
        dp.check_exe_fresh(str(b / "mc2.exe"), str(b), False)
    except SystemExit:
        return
    raise AssertionError("stale exe not detected in Ninja layout")


def test_fresh_exe_passes_in_ninja_tree(tmp_path):
    b = _ninja_tree(tmp_path)
    now = time.time()
    os.utime(b / "CMakeFiles" / "mc2.dir" / "code" / "a.cpp.obj", (now - 100, now - 100))
    os.utime(b / "mc2.exe", (now, now))
    dp.check_exe_fresh(str(b / "mc2.exe"), str(b), False)
