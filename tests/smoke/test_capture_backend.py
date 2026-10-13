"""A capture that asked for Vulkan and was drawn by GL must fail (scripts/smoke_lib/capture_backend.py).

WHY: the Vulkan arm of an A/B fell back to the GL framebuffer readback once and was graded as Vulkan evidence for a session.
"""
import importlib.util
import io
from pathlib import Path

import pytest

from scripts.smoke_lib import capture_backend as cb

ROOT = Path(__file__).resolve().parents[2]

VK_SIDECAR = {"backend": "vulkan", "readback_source": "vk_present"}
GL_SIDECAR = {"backend": "gl", "readback_source": "gl_fbo"}


def test_requested_follows_the_engine_selector():
    for text in ("vulkan", "VULKAN", "vk"):
        assert cb.requested({"MC2_RENDER_BACKEND": text}) == "vulkan"
    for env in (None, {}, {"MC2_RENDER_BACKEND": ""}, {"MC2_RENDER_BACKEND": "gl"}, {"MC2_RENDER_BACKEND": "Vulkan "},
                {"MC2_RENDER_BACKEND": "bogus"}):
        assert cb.requested(env) == "gl"   # the engine coerces anything it does not recognise to GL


def test_a_vulkan_request_drawn_by_gl_is_refused():
    out = cb.check("vulkan", [("pose_a", GL_SIDECAR)])
    assert len(out) == 1 and "requested vulkan" in out[0] and "gl drew it" in out[0]


def test_a_vulkan_capture_passes_and_a_gl_capture_passes():
    assert cb.check("vulkan", [("a", VK_SIDECAR), ("b", VK_SIDECAR)]) == []
    assert cb.check("gl", [("a", GL_SIDECAR)]) == []


def test_a_gl_request_that_came_out_vulkan_is_refused():
    assert cb.check("gl", [("a", VK_SIDECAR)])


def test_the_readback_source_is_checked_even_when_the_backend_field_agrees():
    assert cb.check("vulkan", [("a", {"backend": "vulkan", "readback_source": "gl_fbo"})])


def test_an_older_sidecar_without_backend_reads_the_readback_source():
    assert cb.drew({"readback_source": "vk_present"}) == "vulkan"
    assert cb.drew({"readback_source": "gl_fbo"}) == "gl"
    assert cb.check("vulkan", [("a", {"readback_source": "vk_present"})]) == []
    assert cb.check("vulkan", [("a", {"readback_source": "gl_fbo"})])


def test_a_missing_or_empty_sidecar_is_not_evidence():
    assert cb.check("vulkan", [("a", None)])
    assert cb.check("vulkan", [("a", {})])
    assert cb.check("gl", [("a", None)])   # unknown is unknown for either request


def test_every_bad_pose_is_named():
    out = cb.check("vulkan", [("a", GL_SIDECAR), ("b", VK_SIDECAR), ("c", GL_SIDECAR)])
    assert [m.split(":")[0] for m in out] == ["a", "c"]


def _load(path, name):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_the_capture_driver_env_carries_the_declared_backend(tmp_path):
    rvc = _load("scripts/run_visual_capture.py", "run_visual_capture_backend_under_test")
    args = (tmp_path / "bm.json", tmp_path, 431, 30, True)
    env, _ = rvc.build_capture_env(*args, None)
    assert cb.requested(env) == "gl"   # an ambient shell value is scrubbed; nothing declared means GL
    env, _ = rvc.build_capture_env(*args, {"MC2_RENDER_BACKEND": "vulkan"})
    assert cb.requested(env) == "vulkan"


@pytest.mark.parametrize("drew,fails", [("vulkan", False), ("gl", True)])
def test_the_sweep_fails_on_a_backend_mismatch(tmp_path, drew, fails):
    capture = _load("tools/visual_verify/capture.py", "vv_capture_backend_under_test")
    cap_rvc = capture.rvc
    sidecar = VK_SIDECAR if drew == "vulkan" else GL_SIDECAR
    problems = cb.check("vulkan", [("pose", sidecar)])

    def fake_run_one(**kw):
        return {"mission": "m", "all_present": True, "engine_capture_fired": True, "log": "x.log",
                "bookmarks": [{"name": "pose", "present": True, "png": "p.png", "sha256": "0" * 64, "engine_deterministic": True}],
                "backend": {"requested": "vulkan", "drew": drew, "disagree": problems}}

    real = cap_rvc.run_one
    cap_rvc.run_one = fake_run_one
    try:
        manifest = capture.capture_sweep(tmp_path, [tmp_path / "mc2_01_saved_1.json"], tmp_path / "out", frame=1, settle=1, duration=1,
                                         det_clock=True, show_window=False, logf=io.StringIO(),
                                         declared_env={"MC2_RENDER_BACKEND": "vulkan"})
    finally:
        cap_rvc.run_one = real
    assert manifest["backend"] == {"requested": "vulkan", "drew": drew}
    assert bool(capture.backend_failures(manifest)) is fails
