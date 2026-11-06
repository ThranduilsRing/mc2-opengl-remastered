import json, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "perf"))
import rgp_capture


def test_set_output_rewrites_profiling_path_and_returns_original(tmp_path, monkeypatch):
    preset = tmp_path / "presets.json"
    original = json.dumps({"current": {"modules": {"Profiling": {"enabled": False, "data": json.dumps({"output_path": "old/", "x": 1})}}}})
    preset.write_text(original, encoding="utf-8")
    monkeypatch.setattr(rgp_capture, "PRESETS", preset)
    assert rgp_capture.set_output(Path("E:/out/dir")) == original
    mod = json.loads(preset.read_text())["current"]["modules"]["Profiling"]
    data = json.loads(mod["data"])
    assert mod["enabled"] is True and data["output_path"] == "E:/out/dir/" and data["x"] == 1


def test_rgp_capture_disables_gpu_pass_timers_and_engine_honors_it():
    root = Path(__file__).resolve().parents[2]
    assert 'env["MC2_VK_GPU_TIMESTAMPS"] = "0"' in (root / "tools" / "perf" / "rgp_capture.py").read_text(encoding="utf-8")
    src = (root / "GameOS" / "gameos" / "vk_gpu_profile.cpp").read_text(encoding="utf-8")
    assert 'isSet<"MC2_VK_GPU_TIMESTAMPS">()' in src and "if (tsOk && !tsOptOut)" in src
