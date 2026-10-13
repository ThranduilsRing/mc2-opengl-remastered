"""What: tests for capture_index.py - a set that asked for Vulkan and was drawn by GL is flagged VOID, a set without a record is VOID and "unrecorded", the
line carries the exe commit, the option differences from the defaults of the exe that drew it and frozen/unfrozen, the ledger keeps a set whose directory is
gone, a VOID flag never clears, and a regeneration changes nothing.
Why: the ledger is the committed record of scratch capture sets; a wrong line sends a later lane to cite GL pixels as Vulkan evidence, and a record that
followed the directory would lose every VOID flag the day the PNGs are deleted."""
import json
import os
import subprocess
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import capture_index  # noqa: E402
import console_child  # noqa: E402  (scripts/ is on sys.path through capture_index)

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
    assert line_of(tmp_path, "bare").startswith("bare **VOID** | unrecorded |") and "no capture_run.json" in line_of(tmp_path, "bare")
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


def test_options_are_read_against_the_defaults_of_the_exe_that_drew_the_set(tmp_path):
    """The commit before the PBR default flip had the option ON by default: a set that ran PBR on there shows no difference, and OFF shows the difference."""
    from smoke_lib import render_options_pin as rop
    parent = "e9e3f19d6^"
    shown = subprocess.run(["git", "-C", capture_index.REPO, "show", f"{parent}:Gates/render_options.h"], capture_output=True, text=True, **console_child.child_kwargs(redirected=True))
    if shown.returncode != 0:
        pytest.skip("the commit before the PBR default flip is not in this clone")
    old = {o.key: o.default for o in rop.table_from_text(shown.stdout)}
    assert old["StaticPropPbr"] is True
    pbr_on = {o.key: o.default for o in rop.table()}
    pbr_on["StaticPropPbr"] = True
    make_set(tmp_path, "oldexe-on", "gl", {"backend": "gl", "readback_source": "gl_fbo"}, options={**{k: v for k, v in old.items()}}, commit=parent)
    assert "| defaults |" in line_of(tmp_path, "oldexe-on")
    make_set(tmp_path, "oldexe-off", "gl", {"backend": "gl", "readback_source": "gl_fbo"}, options={**old, "StaticPropPbr": False}, commit=parent)
    assert "StaticPropPbr=false" in line_of(tmp_path, "oldexe-off") and "[vs HEAD defaults]" not in line_of(tmp_path, "oldexe-off")
    make_set(tmp_path, "unknown-exe", "gl", {"backend": "gl", "readback_source": "gl_fbo"}, options=pbr_on, commit="0123456789ab")
    assert "StaticPropPbr=true [vs HEAD defaults]" in line_of(tmp_path, "unknown-exe")


def test_the_ledger_keeps_a_set_whose_directory_is_gone_and_a_void_flag_never_clears(tmp_path):
    import shutil
    make_set(tmp_path, "a-vk", "vulkan", {"backend": "gl", "readback_source": "gl_fbo"})
    make_set(tmp_path, "b-gl", "gl", {"backend": "gl", "readback_source": "gl_fbo"})
    assert capture_index.main(["--root", str(tmp_path)]) == 0
    ledger = json.loads((tmp_path / "ledger.json").read_text(encoding="utf-8"))["sets"]
    assert ledger["a-vk"]["void"] is True and ledger["b-gl"]["void"] is False
    index = (tmp_path / "index.md").read_text(encoding="utf-8")
    assert "2 set(s), 1 VOID" in index
    shutil.rmtree(tmp_path / "a-vk")
    shutil.rmtree(tmp_path / "b-gl")
    assert capture_index.main(["--root", str(tmp_path)]) == 0
    assert (tmp_path / "index.md").read_text(encoding="utf-8") == index          # nothing on disk: nothing lost
    assert capture_index.main(["--root", str(tmp_path), "--check"]) == 0
    # a later description of the same name that looks clean does not clear the flag
    make_set(tmp_path, "a-vk", "vulkan", {"backend": "vulkan", "readback_source": "vk_present"})
    assert capture_index.main(["--root", str(tmp_path), "--refresh", "a-vk"]) == 0
    row = json.loads((tmp_path / "ledger.json").read_text(encoding="utf-8"))["sets"]["a-vk"]
    assert row["void"] is True and "requested vulkan" in row["void_reason"]


def test_a_known_set_is_not_described_again_unless_refreshed(tmp_path):
    make_set(tmp_path, "s", "gl", {"backend": "gl", "readback_source": "gl_fbo"})
    capture_index.main(["--root", str(tmp_path)])
    shutil_target = tmp_path / "s" / "capture_run.json"
    run = json.loads(shutil_target.read_text(encoding="utf-8"))
    run["declared_env"] = {"MC2_SMOKE_FREEZE_ANIM": "1"}
    shutil_target.write_text(json.dumps(run), encoding="utf-8")
    capture_index.main(["--root", str(tmp_path)])
    assert json.loads((tmp_path / "ledger.json").read_text(encoding="utf-8"))["sets"]["s"]["animation"] == "unfrozen"
    capture_index.main(["--root", str(tmp_path), "--refresh"])
    assert json.loads((tmp_path / "ledger.json").read_text(encoding="utf-8"))["sets"]["s"]["animation"] == "frozen"


def test_regeneration_is_stable_and_check_notices_a_stale_file(tmp_path):
    make_set(tmp_path, "s1", "gl", {"backend": "gl", "readback_source": "gl_fbo"})
    (tmp_path / "montages").mkdir()
    assert capture_index.main(["--root", str(tmp_path)]) == 0
    first = (tmp_path / "index.md").read_text(encoding="utf-8")
    assert "s1 |" in first and "montages" not in first.split("1 set(s)")[1]
    assert capture_index.main(["--root", str(tmp_path), "--check"]) == 0
    make_set(tmp_path, "s2", "gl", {"backend": "gl", "readback_source": "gl_fbo"})
    assert capture_index.main(["--root", str(tmp_path), "--check"]) == 1          # a set on disk the ledger does not know
    capture_index.main(["--root", str(tmp_path)])
    assert capture_index.main(["--root", str(tmp_path), "--check"]) == 0
