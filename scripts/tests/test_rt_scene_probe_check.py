import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import rt_scene_probe_check as c  # noqa: E402

PASS = ("[VK_RT_SCENE] material result=PASS props=8/8 vegetation=4/4 terrain=4/4 holes=37 false_holes=0 opaque_diffs=0 rows=812 rays=540\n")


def test_pass_line_passes():
    ok, msgs = c.check(PASS)
    assert ok, msgs


def test_fail_result_fails():
    ok, msgs = c.check(PASS.replace("result=PASS", "result=FAIL"))
    assert not ok


def test_missing_probe_or_no_holes_fails():
    assert not c.check(PASS.replace("props=8/8", "props=7/8"))[0]
    assert not c.check(PASS.replace("vegetation=4/4", "vegetation=0/0"))[0]
    assert not c.check(PASS.replace("holes=37", "holes=0"))[0]
    assert not c.check(PASS.replace("false_holes=0", "false_holes=2"))[0]


def test_no_line_fails():
    assert not c.check("[VK_RT_SCENE] probe result props=8/8\n")[0]


def test_arrays_fail_line_fails():
    ok, msgs = c.check("material result=FAIL reason=albedo_arrays off=0 on=1 need_off=1 need_on=1 rows=9\n")
    assert not ok and any("albedo_arrays" in m for m in msgs)
