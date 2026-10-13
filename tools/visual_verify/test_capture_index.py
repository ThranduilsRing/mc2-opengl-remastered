"""What: tests for capture_index.py - a set that asked for Vulkan and was drawn by GL is flagged VOID, an old set without a record is "unrecorded", the line
carries the exe commit, the option differences and frozen/unfrozen, and a regeneration changes nothing.
Why: the index is the committed record of scratch capture sets; a wrong line sends a later lane to cite GL pixels as Vulkan evidence."""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import capture_index  # noqa: E402

BM = "mc2_01_saved_205644"


def make_set(root, name, requested, sidecar, declared=None, options=None, commit="abc123def456", dirty=False):
    d = root / name
    (d / BM).mkdir(parents=True)
    if sidecar is not None:
        (d / BM / (BM + ".json")).write_text(json.dumps(sidecar), encoding="utf-8")
    run = {"declared_env": declared or {}, "poses": [{"bookmark": BM}], "generated_at": "2026-10-13T05:01:33Z",
           "backend": {"requested": requested, "drew": []}, "render_options": options,
           "identity": {"binary_provenance": {"commit": commit, "dirty": dirty}}}
    (d / "capture_run.json").write_text(json.dumps(run), encoding="utf-8")


def line_of(root, name):
    return capture_index.describe(str(root / name))


def test_a_vulkan_set_drawn_by_gl_is_void_and_a_real_one_is_not(tmp_path):
    make_set(tmp_path, "a-vk", "vulkan", {"backend": "gl", "readback_source": "gl_fbo"})
    make_set(tmp_path, "b-vk", "vulkan", {"backend": "vulkan", "readback_source": "vk_present"})
    assert "**VOID**" in line_of(tmp_path, "a-vk") and "| vulkan | gl | gl_fbo |" in line_of(tmp_path, "a-vk")
    assert "VOID" not in line_of(tmp_path, "b-vk") and "| vulkan | vulkan | vk_present |" in line_of(tmp_path, "b-vk")


def test_an_older_sidecar_is_read_through_its_readback_source(tmp_path):
    make_set(tmp_path, "old-vk", "vulkan", {"readback_source": "vk_present"})
    assert "VOID" not in line_of(tmp_path, "old-vk")


def test_a_set_without_a_record_or_without_any_sidecar(tmp_path):
    (tmp_path / "bare").mkdir()
    assert line_of(tmp_path, "bare").startswith("bare | unrecorded |")
    make_set(tmp_path, "nosidecar", "gl", None)
    assert "**VOID**" in line_of(tmp_path, "nosidecar") and "| 0/1 |" in line_of(tmp_path, "nosidecar")


def test_commit_options_and_animation_are_in_the_line(tmp_path):
    from smoke_lib import render_options_pin as rop
    values = {o.key: o.default for o in rop.table()}
    values["StaticPropPbr"] = not values["StaticPropPbr"]
    make_set(tmp_path, "x", "gl", {"backend": "gl", "readback_source": "gl_fbo"}, declared={"MC2_SMOKE_FREEZE_ANIM": "1"}, options=values, dirty=True)
    line = line_of(tmp_path, "x")
    assert "| abc123def456+dirty |" in line and "StaticPropPbr=true" in line and "| frozen |" in line and line.endswith("| 1/1 | 2026-10-13")
    make_set(tmp_path, "y", "gl", {"backend": "gl", "readback_source": "gl_fbo"}, options={o.key: o.default for o in rop.table()})
    assert "| defaults | unfrozen |" in line_of(tmp_path, "y")
    make_set(tmp_path, "z", "gl", {"backend": "gl", "readback_source": "gl_fbo"}, options=None)
    assert "| unrecorded | unfrozen |" in line_of(tmp_path, "z")


def test_regeneration_is_stable_and_check_notices_a_stale_file(tmp_path):
    make_set(tmp_path, "s1", "gl", {"backend": "gl", "readback_source": "gl_fbo"})
    (tmp_path / "montages").mkdir()
    assert capture_index.main(["--root", str(tmp_path)]) == 0
    first = (tmp_path / "index.md").read_text(encoding="utf-8")
    assert "s1 |" in first and "montages" not in first
    assert capture_index.main(["--root", str(tmp_path), "--check"]) == 0
    make_set(tmp_path, "s2", "gl", {"backend": "gl", "readback_source": "gl_fbo"})
    assert capture_index.main(["--root", str(tmp_path), "--check"]) == 1
    capture_index.main(["--root", str(tmp_path)])
    assert capture_index.main(["--root", str(tmp_path), "--check"]) == 0
