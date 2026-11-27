import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import rt_scene_probe_check as c  # noqa: E402

PASS = ("[VK_RT_SCENE] material result=PASS props=8/8 vegetation=4/4 terrain=4/4 holes=37 false_holes=0 opaque_diffs=0 rows=812 rays=540\n")


def test_pass_line_passes():
    ok, msgs = c.check(PASS)
    assert ok, msgs


def test_publish_without_compaction_counters_passes():
    ok, msgs = c.check("[VK_RT_SCENE v1] publish why=props blas_bytes=220000 built_bytes=220000\n" + PASS)
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


INFO = "[VK_RT_SCENE v1] material info kind=prop ray=4 prim=1917/2528 t=0.246 custom=0x1000b7 row=3\n"
MISS_SAME_INSTANCE = "[VK_RT_SCENE v1] material miss kind=prop ray=4 hit=1 t=0.246 custom=0x1000b7/0x1000b7 row=3/3 prim=1917/2528 layer=4/4 packet=196/196 flags=16\n"
MISS_OTHER_INSTANCE = "[VK_RT_SCENE v1] material miss kind=prop ray=5 hit=1 t=0.201 custom=0x0/0x100383 row=193/27 prim=3907/2100 layer=-1/11 packet=25/364 flags=4\n"


def test_same_instance_different_triangle_passes():
    ok, msgs = c.check(INFO + PASS.replace("rows=812", "prim_info=1 rows=812"))
    assert ok, msgs


def test_different_instance_or_row_fails():
    ok, msgs = c.check(MISS_OTHER_INSTANCE + PASS)
    assert not ok and any("wrong instance" in m for m in msgs)
    ok, msgs = c.check(MISS_OTHER_INSTANCE.replace("props", "props") + PASS.replace("props=8/8", "props=7/8"))
    assert not ok


def test_same_instance_miss_line_alone_is_not_a_wrong_instance():
    ok, msgs = c.check(MISS_SAME_INSTANCE + PASS)
    assert ok, msgs
