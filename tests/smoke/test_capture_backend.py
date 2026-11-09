"""A capture that asked for Vulkan and was drawn by GL must fail (scripts/smoke_lib/capture_backend.py).

WHY: the Vulkan arm of an A/B fell back to the GL framebuffer readback once and was graded as Vulkan evidence for a session.
"""
import importlib.util
import sys
import io
import json
from pathlib import Path

import pytest

from scripts.smoke_lib import capture_backend as cb

ROOT = Path(__file__).resolve().parents[2]

VK_SIDECAR = {"backend": "vulkan", "readback_source": "vk_present"}
GL_SIDECAR = {"backend": "gl", "readback_source": "gl_fbo"}


def test_requested_follows_the_engine_selector():
    for text in ("vulkan", "VULKAN", "vk"):
        assert cb.requested({"MC2_RENDER_BACKEND": text}) == "vulkan"
    for env in (None, {}, {"MC2_RENDER_BACKEND": ""}, {"MC2_RENDER_BACKEND": "Vulkan "}, {"MC2_RENDER_BACKEND": "bogus"},
                {"MC2_RENDER_BACKEND": "gl"}, {"MC2_RENDER_BACKEND": "GL"}):
        assert cb.requested(env) == "vulkan"   # the engine has one backend: gl is ignored (render_backend_select.cpp)


def test_a_vulkan_request_drawn_by_gl_is_refused():
    out = cb.check("vulkan", [("pose_a", GL_SIDECAR)])
    assert len(out) == 1 and "requested vulkan" in out[0] and "gl drew it" in out[0]


def test_a_vulkan_capture_passes():
    assert cb.check("vulkan", [("a", VK_SIDECAR), ("b", VK_SIDECAR)]) == []


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
    assert cb.requested(env) == "vulkan"   # an ambient shell value is scrubbed; nothing declared means the default, Vulkan
    env, _ = rvc.build_capture_env(*args, {"MC2_RENDER_BACKEND": "gl"})
    assert cb.requested(env) == "vulkan"   # a declared gl is ignored by the engine and by the harness


@pytest.mark.parametrize("drew,fails", [("vulkan", False), ("gl", True)])
def test_the_sweep_fails_on_a_backend_mismatch(tmp_path, monkeypatch, drew, fails):
    capture = _load("tools/visual_verify/capture.py", "vv_capture_backend_under_test")
    import ab
    monkeypatch.setattr(ab, "payload_identity", lambda install: {"commit": "a" * 40, "payload": {}})
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
    assert manifest["backend"] == {"requested": "vulkan", "drew": [drew]}
    assert bool(capture.backend_failures(manifest)) is fails


# --- the exit codes of the two drivers (the contract the A/B tooling relies on) -------------------------------------------------------------

@pytest.fixture
def drivers(monkeypatch):
    """capture.py and run_visual_capture.py with their process side effects (cursor park, exe guard, live-process scan) switched off."""
    capture = _load("tools/visual_verify/capture.py", "vv_capture_exit_codes_under_test")
    rvc = capture.rvc
    for name in ("_save_cursor", "_restore_cursor", "_reap_active"):
        monkeypatch.setattr(rvc, name, lambda *a, **k: None)
    monkeypatch.setattr(rvc, "guard_exe", lambda exe: True)
    monkeypatch.setattr(rvc.time, "sleep", lambda s: None)
    monkeypatch.setattr(rvc, "_print_result", lambda *a, **k: None)
    monkeypatch.setattr(rvc, "write_manifest", lambda result, exe: Path("manifest.json"))
    return capture, rvc


def run_result(disagree):
    return {"mission": "m", "all_present": True, "engine_capture_fired": True, "log": "x.log",
            "bookmarks": [{"name": "pose", "present": True, "png": "p.png", "sha256": "0" * 64, "engine_deterministic": True}],
            "backend": {"requested": "vulkan", "drew": ["gl" if disagree else "vulkan"], "disagree": disagree}}


@pytest.mark.parametrize("disagree,rc", [([], 0), (["pose: requested vulkan but the sidecar says gl drew it"], 1)])
def test_run_visual_capture_single_run_exits_1_on_a_backend_mismatch(drivers, monkeypatch, tmp_path, disagree, rc):
    _capture, rvc = drivers
    (tmp_path / "mc2.exe").write_bytes(b"")
    (tmp_path / "bm.json").write_text("{}", encoding="utf-8")
    monkeypatch.setattr(rvc, "run_one", lambda *a, **k: run_result(disagree))
    monkeypatch.setattr(sys, "argv", ["run_visual_capture.py", "--exe", str(tmp_path / "mc2.exe"), "--bookmarks", str(tmp_path / "bm.json"),
                                      "--out-dir", str(tmp_path / "out"), "--env-declare", "MC2_RENDER_BACKEND=vulkan"])
    assert rvc.main() == rc


def test_run_visual_capture_multi_run_exits_1_when_any_run_drew_the_wrong_backend(drivers, monkeypatch, tmp_path):
    _capture, rvc = drivers
    (tmp_path / "mc2.exe").write_bytes(b"")
    (tmp_path / "bm.json").write_text("{}", encoding="utf-8")
    results = iter([run_result([]), run_result(["pose: requested vulkan but the sidecar says gl drew it"])])
    monkeypatch.setattr(rvc, "run_one", lambda *a, **k: next(results))
    monkeypatch.setattr(sys, "argv", ["run_visual_capture.py", "--exe", str(tmp_path / "mc2.exe"), "--bookmarks", str(tmp_path / "bm.json"),
                                      "--out-dir", str(tmp_path / "out"), "--runs", "2"])
    assert rvc.main() == 1


@pytest.mark.parametrize("runs", [1, 2])
def test_classic_assets_reaches_each_capture(drivers, monkeypatch, tmp_path, runs):
    _capture, rvc = drivers
    (tmp_path / "mc2.exe").write_bytes(b"")
    (tmp_path / "bm.json").write_text("{}", encoding="utf-8")
    choices = []

    def capture(*a, **k):
        choices.append(a[-1])
        return run_result([])

    monkeypatch.setattr(rvc, "run_one", capture)
    monkeypatch.setattr(sys, "argv", ["run_visual_capture.py", "--exe", str(tmp_path / "mc2.exe"),
                                    "--bookmarks", str(tmp_path / "bm.json"), "--out-dir", str(tmp_path / "out"),
                                    "--runs", str(runs), "--classic-assets"])
    assert rvc.main() == 0
    assert choices == [True] * runs


@pytest.mark.parametrize("drew,rc", [("vulkan", 0), ("gl", 1)])
def test_capture_main_exits_1_when_the_sweep_drew_the_wrong_backend(drivers, monkeypatch, tmp_path, drew, rc):
    capture, rvc = drivers
    install = tmp_path / "inst"
    (install / "saved_bookmarks").mkdir(parents=True)
    (install / "mc2.exe").write_bytes(b"")
    bm = install / "saved_bookmarks" / "pose.json"
    bm.write_text("{}", encoding="utf-8")
    monkeypatch.setattr(capture, "discover_bookmarks", lambda inst: [bm])
    monkeypatch.setattr(capture, "_running_mc2_from", lambda inst: [])
    problems = [] if drew == "vulkan" else ["pose: requested vulkan but the sidecar says gl drew it"]

    def fake_sweep(install, files, out_dir, **kw):
        return {"n_captured": 1, "n_requested": 1, "poses": [{"bookmark": "pose", "engine_deterministic": True, "backend_disagree": problems}],
                "backend": {"requested": "vulkan", "drew": [drew]}}

    monkeypatch.setattr(capture, "capture_sweep", fake_sweep)
    assert capture.main(["--install", str(install), "--bookmarks", "pose", "--out", str(tmp_path / "out"),
                         "--env", "MC2_RENDER_BACKEND=vulkan"]) == rc


def test_the_manifest_verdict_names_why_it_failed_and_a_candidate_set_records_its_backend(drivers):
    _capture, rvc = drivers
    bad = dict(run_result(["pose: requested vulkan but the sidecar says gl drew it"]),
               render_options={"values": {}, "disagree": ["ShadowCSM: harness true, engine false"]})
    reasons = rvc.fail_reasons(bad)
    assert any(r.startswith("render backend:") and "gl drew it" in r for r in reasons)
    assert any(r.startswith("render options:") and "ShadowCSM" in r for r in reasons)
    assert rvc.fail_reasons(run_result([])) == []
    assert rvc.fail_reasons(dict(run_result([]), all_present=False)) == ["not every bookmark produced a PNG"]


def test_a_candidate_set_records_the_backend_and_the_options_it_was_drawn_with(drivers, monkeypatch, tmp_path):
    _capture, rvc = drivers
    monkeypatch.setattr(rvc, "GOLDENS_DIR", tmp_path / "goldens")
    exe = tmp_path / "mc2.exe"
    exe.write_bytes(b"")
    png = tmp_path / "pose.png"
    png.write_bytes(b"png")
    last = dict(run_result([]), mission="mc2_01", render_options={"values": {"ShadowCSM": True}, "disagree": []},
                bookmarks=[{"name": "pose", "present": True, "png": str(png), "sha256": "0" * 64, "covers": "x", "engine_deterministic": True}])
    set_dir = rvc.materialize_candidate("cand", last, {"pose": {"stable": True, "shas": ["0" * 64]}}, exe, 3, 1)
    record = json.loads((set_dir / "set.json").read_text(encoding="utf-8"))
    assert record["backend"] == {"requested": "vulkan", "drew": ["vulkan"]}
    assert record["render_options"] == {"ShadowCSM": True}


def test_the_deterministic_clock_also_seeds_the_simulation_stream(tmp_path):
    rvc = _load("scripts/run_visual_capture.py", "run_visual_capture_rng_under_test")
    env, _ = rvc.build_capture_env(tmp_path / "bm.json", tmp_path, 431, 30, True, None)
    assert env.get("MC2_DETERMINISTIC_RNG") == "1"
    env, _ = rvc.build_capture_env(tmp_path / "bm.json", tmp_path, 431, 30, False, None)
    assert "MC2_DETERMINISTIC_RNG" not in env
