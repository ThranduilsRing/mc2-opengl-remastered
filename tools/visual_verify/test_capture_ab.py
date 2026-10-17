"""What: tests for capture_ab.py - the compare step reports the noise floor first, identical frames read as zero, a changed block is found, a missing set is named.
Why: a refactor is judged by comparing its diff to the run-to-run noise; a compare that silently skipped a missing set would read as 'no change'."""
import os, sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import capture_ab

BM = "mc2_01_saved_205644"


def put(root, set_name, arr):
    d = root / set_name / BM
    d.mkdir(parents=True)
    Image.fromarray(arr).save(d / (BM + ".png"))


def sets(tmp_path, changed):
    base = np.zeros((64, 64, 3), np.uint8)
    other = base.copy()
    if changed:
        other[10:30, 10:30] = 200
    for s in ("b1-vk", "b2-vk", "b-gl"):
        put(tmp_path, s, base)
    for s in ("a1-vk", "a2-vk", "a-gl"):
        put(tmp_path, s, other)


def test_identical_sets_read_as_zero(tmp_path):
    sets(tmp_path, False)
    lines = capture_ab.compare(str(tmp_path), "b", "a", [BM])
    assert len(lines) == 3 and lines[0].split()[1] == "NOISE" and "VK" in lines[0]
    assert all("outside_box=0 clusters=0" in l for l in lines)


def test_changed_block_is_found_but_noise_stays_zero(tmp_path):
    sets(tmp_path, True)
    lines = capture_ab.compare(str(tmp_path), "b", "a", [BM])
    assert "outside_box=0" in lines[0]
    assert all("outside_box=400" in l and "max_cluster=20x20@10,10" in l for l in lines[1:])


def test_there_is_no_gl_arm(tmp_path):
    sets(tmp_path, True)
    assert not any(" GL " in l for l in capture_ab.compare(str(tmp_path), "b", "a", [BM]))
    assert [n for n, _ in capture_ab.arms("t")] == ["t1-vk", "t2-vk"]
    assert "--gl-arm" not in capture_ab.__doc__


def test_missing_set_is_named(tmp_path):
    sets(tmp_path, False)
    lines = capture_ab.compare(str(tmp_path), "b", "nope", [BM])
    assert any("MISSING" in l for l in lines[1:])


def test_the_vulkan_arm_asks_for_the_shared_gbuffer_umbrella():
    # Without MC2_VK_SHARED_GBUFFER=1 the engine forces GL for the session and the Vulkan arm is a GL capture.
    vk = capture_ab.capture_cmd("inst", "out", BM, vulkan=True)
    gl = capture_ab.capture_cmd("inst", "out", BM, vulkan=False)
    assert "MC2_RENDER_BACKEND=vulkan" in vk and "MC2_VK_SHARED_GBUFFER=1" in vk
    assert "MC2_RENDER_BACKEND=gl" in gl and not any("MC2_VK_SHARED_GBUFFER" in a for a in gl)


def put_sidecar(root, set_name, **fields):
    d = root / set_name / BM
    d.mkdir(parents=True, exist_ok=True)
    (d / (BM + ".json")).write_text(__import__("json").dumps(fields), encoding="utf-8")


def record_options(tmp_path):
    """The default option values as every set's capture_run.json (compare refuses a set with no option record before it looks at backends)."""
    from smoke_lib import render_options_pin as rop
    values = {o.key: o.default for o in rop.table()}
    for s in ("b1-vk", "b2-vk", "b-gl", "a1-vk", "a2-vk", "a-gl"):
        (tmp_path / s / "capture_run.json").write_text(__import__("json").dumps({"render_options": values}), encoding="utf-8")


def backend_sets(tmp_path, vk_fields):
    for s in ("b-gl", "a-gl"):
        put_sidecar(tmp_path, s, backend="gl", readback_source="gl_fbo")
    for s in ("b1-vk", "b2-vk", "a1-vk", "a2-vk"):
        put_sidecar(tmp_path, s, **vk_fields)


def test_a_vk_set_that_drew_gl_is_refused_as_unproven(tmp_path):
    backend_sets(tmp_path, {"backend": "gl", "readback_source": "gl_fbo"})
    out = capture_ab.check_backends(str(tmp_path), "b", "a", [BM])
    assert len(out) == 4 and all("VK arm UNPROVEN" in m for m in out)
    assert ("b1-vk" in out[0]) and ("a2-vk" in out[3])


def test_an_older_vk_set_is_read_through_its_readback_source(tmp_path):
    backend_sets(tmp_path, {"readback_source": "vk_present"})        # no `backend` field yet
    assert capture_ab.check_backends(str(tmp_path), "b", "a", [BM]) == []
    backend_sets(tmp_path, {"readback_source": "gl_fbo"})
    assert capture_ab.check_backends(str(tmp_path), "b", "a", [BM])


def test_a_gl_set_that_drew_vulkan_and_a_missing_sidecar_are_refused(tmp_path):
    backend_sets(tmp_path, {"backend": "vulkan", "readback_source": "vk_present"})
    assert capture_ab.check_backends(str(tmp_path), "b", "a", [BM]) == []
    assert any("nope1-vk" in m for m in capture_ab.check_backends(str(tmp_path), "b", "nope", [BM]))


def test_compare_exits_1_for_a_vk_set_that_is_gl_and_0_for_a_real_one(tmp_path, capsys):
    sets(tmp_path, False)
    backend_sets(tmp_path, {"backend": "gl", "readback_source": "gl_fbo"})
    record_options(tmp_path)
    argv = ["compare", "--before", "b", "--after", "a", "--root", str(tmp_path), "--bookmarks", BM]
    assert capture_ab.main(argv) == 1
    assert "UNPROVEN" in capsys.readouterr().err
    backend_sets(tmp_path, {"backend": "vulkan", "readback_source": "vk_present"})
    assert capture_ab.main(argv) == 0
