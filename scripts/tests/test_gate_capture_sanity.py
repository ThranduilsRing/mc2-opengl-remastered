"""capture_pipeline_mismatch: before/after capture logs with different VKIsl.Post.* scopes are INFRA, not a pixel FAIL.

Excerpts are trimmed from merge-gate proofs 20261107-050748 (broken: seed without amd_fidelityfx_vk.dll) and 20261107-063723 (good)."""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import merge_gate as gate

NO_FSR = ("[VK_RESOURCE_GRAPH] chain=SMAA passes=4 transient_images=3 slots=3 alias_savings=0\n"
          "[VK_GPU_PASSES v1] frame=121 n=120 scopes=49 dropped=0 | VKIsl.ActorCull=0.009(x1.0) "
          "VKIsl.Post.Fog=0.1(x1.0) VKIsl.Post.ShadowResolve=0.2(x1.0)\n")
FSR = ("[VK_RESOURCE_GRAPH] chain=SMAA passes=4 transient_images=3 slots=3 alias_savings=0\n"
       "[VK_GPU_PASSES v1] frame=122 n=120 scopes=49 dropped=0 | VKIsl.ActorCull=0.009(x1.0) "
       "VKIsl.Post.MotionVectors=0.1(x1.0) VKIsl.Post.FsrAA=0.3(x1.0) VKIsl.Post.Fog=0.1(x1.0) VKIsl.Post.ShadowResolve=0.2(x1.0)\n")


def capture(root, side, text, install=None):
    d = root / side
    (d / "mc2_01_saved_205644").mkdir(parents=True)
    (d / "mc2_01_saved_205644" / "mc2_01_capture.log").write_text(text)
    if install:
        (d / "capture_run.log").write_text(f"[vv-capture] start 2026-11-07T11:17:50Z install={install} bookmarks=2\n")
    return d


def test_broken_seed_without_fsr_is_infra_naming_scopes_and_miss(tmp_path):
    seed = tmp_path / "seed"
    seed.mkdir()
    before = capture(tmp_path, "before", NO_FSR, seed)
    after = capture(tmp_path, "after", FSR)
    reason = gate.capture_pipeline_mismatch(before, after)
    assert "VKIsl.Post.FsrAA" in reason and "VKIsl.Post.MotionVectors" in reason
    assert "MISSed amd_fidelityfx_vk.dll" in reason and "\n" not in reason


def test_good_captures_match(tmp_path):
    before = capture(tmp_path, "before", FSR)
    after = capture(tmp_path, "after", FSR.replace("frame=122", "frame=242"))
    assert gate.capture_pipeline_mismatch(before, after) is None


def test_seed_with_dll_still_reports_difference(tmp_path):
    seed = tmp_path / "seed"
    seed.mkdir()
    (seed / "amd_fidelityfx_vk.dll").write_bytes(b"x")
    reason = gate.capture_pipeline_mismatch(capture(tmp_path, "before", NO_FSR, seed), capture(tmp_path, "after", FSR))
    assert "MISSed" not in reason and "FsrAA" in reason


def test_no_log_evidence_defers_to_pixel_verdict(tmp_path):
    (tmp_path / "before").mkdir()
    assert gate.capture_pipeline_mismatch(tmp_path / "before", capture(tmp_path, "after", FSR)) is None
