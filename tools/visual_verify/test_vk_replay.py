"""What: tests for vk_replay.py - the layer environment, the capture and replay command lines, the frame hash, and the comparison of a live frame with replayed
frames. No Vulkan, no engine and no GFXReconstruct is run: the argument building and the pixel comparison are the parts that can be wrong without a GPU.
Why: the proof is only as good as its arguments (a layer that is not on the path captures nothing, a screenshot range that misses the frame compares nothing) and
its hash (a comparison that ignores the size or the channel order would call two different frames equal)."""
import os
import sys
from pathlib import Path

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vk_replay  # noqa: E402


def write_png(path, pixels, mode="RGB"):
    from PIL import Image
    Image.fromarray(pixels, mode).save(path)


def frame(seed, h=8, w=12):
    import numpy as np
    return np.random.default_rng(seed).integers(0, 256, size=(h, w, 3), dtype=np.uint8)


def test_the_frame_range_is_one_based_and_inclusive():
    assert vk_replay.parse_frames("440-470") == (440, 470)
    assert vk_replay.parse_frames("455") == (455, 455)
    for bad in ("0-5", "9-3", "abc"):
        with pytest.raises(ValueError):
            vk_replay.parse_frames(bad)


def test_the_capture_environment_puts_the_layer_under_the_engine():
    env = vk_replay.capture_env(Path("out/mc2.gfxr"), (440, 470), Path("C:/VulkanSDK/1.4.350.0/Bin"), Path("out/cap.log"), environ={})
    assert env["VK_INSTANCE_LAYERS"] == "VK_LAYER_LUNARG_gfxreconstruct"
    assert env["VK_LAYER_PATH"] == str(Path("C:/VulkanSDK/1.4.350.0/Bin"))
    assert env["GFXRECON_CAPTURE_FILE"] == str(Path("out/mc2.gfxr"))
    assert env["GFXRECON_CAPTURE_FRAMES"] == "440-470"
    assert env["GFXRECON_CAPTURE_FILE_TIMESTAMP"] == "false"
    assert env["GFXRECON_LOG_FILE"] == str(Path("out/cap.log"))


def test_a_layer_or_path_already_in_the_environment_is_kept_behind_ours():
    env = vk_replay.capture_env(Path("a.gfxr"), (1, 2), Path("SDK"), environ={"VK_INSTANCE_LAYERS": "VK_LAYER_KHRONOS_validation", "VK_LAYER_PATH": "X"})
    assert env["VK_INSTANCE_LAYERS"].split(os.pathsep) == ["VK_LAYER_LUNARG_gfxreconstruct", "VK_LAYER_KHRONOS_validation"]
    assert env["VK_LAYER_PATH"].split(os.pathsep) == ["SDK", "X"]


def test_the_capture_command_runs_the_engine_through_the_capture_harness_on_the_vulkan_umbrella():
    cmd = vk_replay.capture_command(Path("A:/inst"), "mc2_01_saved_205644", Path("out/live"), 431)
    assert cmd[1].endswith("capture.py")
    assert cmd[cmd.index("--bookmarks") + 1] == "mc2_01_saved_205644"
    assert cmd[cmd.index("--frame") + 1] == "431" and "--clean" in cmd
    assert int(cmd[cmd.index("--duration") + 1]) >= 600         # the budget includes a wait for the host lease
    declared = [cmd[i + 1] for i, a in enumerate(cmd) if a == "--env"]
    assert declared == ["MC2_RENDER_BACKEND=vulkan", "MC2_VK_SHARED_GBUFFER=1"]       # the backend alone falls back to GL


def test_the_replay_command_has_no_window_and_screenshots_the_asked_frames():
    cmd = vk_replay.replay_command(Path("gfxrecon-replay.exe"), Path("out/mc2.gfxr"), Path("out/replay"), (1, 31), Path("out/replay.log"))
    assert cmd[0] == "gfxrecon-replay.exe" and cmd[-1] == str(Path("out/mc2.gfxr"))
    assert cmd[cmd.index("--swapchain") + 1] == "offscreen" and "--wsi" not in cmd
    assert cmd[cmd.index("--screenshots") + 1] == "1-31"
    assert cmd[cmd.index("--screenshot-format") + 1] == "png"
    assert cmd[cmd.index("--screenshot-dir") + 1] == str(Path("out/replay"))
    assert cmd[cmd.index("--log-file") + 1] == str(Path("out/replay.log"))


def test_the_sdk_is_found_from_the_tool_override_then_the_sdk_variable():
    assert vk_replay.sdk_bin({"MC2_TOOL_VULKAN_SDK": "S1", "VULKAN_SDK": "S2"}) == Path("S1") / "Bin"
    assert vk_replay.sdk_bin({"VULKAN_SDK": "S2"}) == Path("S2") / "Bin"
    assert vk_replay.replay_exe({"MC2_TOOL_GFXRECON_REPLAY": "R.exe"}) == Path("R.exe")
    assert vk_replay.replay_exe({"VULKAN_SDK": "S2"}) == Path("S2") / "Bin" / "gfxrecon-replay.exe"


def test_the_hash_is_of_the_pixels_not_the_file_format_or_the_alpha_channel(tmp_path):
    px = frame(1)
    write_png(tmp_path / "a.png", px)
    from PIL import Image
    Image.fromarray(px, "RGB").save(tmp_path / "a.bmp")
    import numpy as np
    rgba = np.dstack([px, np.full(px.shape[:2], 7, dtype=np.uint8)])
    write_png(tmp_path / "rgba.png", rgba, "RGBA")
    h = vk_replay.frame_hash(tmp_path / "a.png")
    assert vk_replay.frame_hash(tmp_path / "a.bmp") == h and vk_replay.frame_hash(tmp_path / "rgba.png") == h


def test_the_hash_tells_one_changed_pixel_and_a_different_size_apart(tmp_path):
    px = frame(2)
    write_png(tmp_path / "a.png", px)
    other = px.copy()
    other[3, 4, 1] ^= 1
    write_png(tmp_path / "b.png", other)
    write_png(tmp_path / "c.png", px[:, :-1])
    assert len({vk_replay.frame_hash(tmp_path / n) for n in ("a.png", "b.png", "c.png")}) == 3


def test_a_swapped_channel_order_is_a_different_frame(tmp_path):
    px = frame(3)
    write_png(tmp_path / "rgb.png", px)
    write_png(tmp_path / "bgr.png", px[:, :, ::-1].copy())
    assert vk_replay.frame_hash(tmp_path / "rgb.png") != vk_replay.frame_hash(tmp_path / "bgr.png")


def test_compare_finds_the_replayed_frame_that_equals_the_live_one(tmp_path):
    live = tmp_path / "live.png"
    write_png(live, frame(10))
    shots = tmp_path / "shots"
    shots.mkdir()
    for i, seed in enumerate((11, 12, 10, 13), start=1):                       # replay frame 3 is the live frame
        write_png(shots / f"replay_{i:04d}.png", frame(seed))
    result = vk_replay.compare(live, shots)
    assert result["equal"] and result["matches"] == ["replay_0003.png"]
    assert [s["file"] for s in result["shots"]][:2] == ["replay_0001.png", "replay_0002.png"]


def test_compare_says_different_when_no_replayed_frame_matches(tmp_path):
    live = tmp_path / "live.png"
    write_png(live, frame(20))
    shots = tmp_path / "shots"
    shots.mkdir()
    write_png(shots / "replay_0001.png", frame(21))
    result = vk_replay.compare(live, shots)
    assert not result["equal"] and result["matches"] == []
    assert vk_replay.main(["compare", "--live", str(live), "--shots", str(shots)]) == 1
    write_png(shots / "replay_0002.png", frame(20))
    assert vk_replay.main(["compare", "--live", str(live), "--shots", str(shots)]) == 0


def test_dry_run_prints_the_commands_and_runs_nothing(tmp_path, capsys):
    rc = vk_replay.main(["proof", "--install", "A:/inst", "--bookmark", "mc2_01_saved_205644", "--out", str(tmp_path / "o"), "--dry-run"])
    out = capsys.readouterr().out
    assert rc == 0 and "GFXRECON_CAPTURE_FRAMES" in out and "--swapchain" in out and "offscreen" in out
    assert not (tmp_path / "o" / "mc2.gfxr").exists()


def test_the_capture_file_is_found_under_the_name_the_layer_gave_it(tmp_path):
    (tmp_path / "mc2_frames_440_through_470.gfxr").write_bytes(b"x" * 10)
    (tmp_path / "stale.gfxr").write_bytes(b"x")
    assert vk_replay.captured_file(tmp_path).name == "mc2_frames_440_through_470.gfxr"
    with pytest.raises(FileNotFoundError):
        vk_replay.captured_file(tmp_path / "nothing")


def test_a_match_is_named_by_its_application_frame(tmp_path):
    live = tmp_path / "live.png"
    write_png(live, frame(30))
    shots = tmp_path / "shots"
    shots.mkdir()
    write_png(shots / "replay_frame_22.png", frame(31))
    write_png(shots / "replay_frame_23.png", frame(30))
    result = vk_replay.compare(live, shots, first_app_frame=440)
    assert result["matches"] == ["replay_frame_23.png"] and result["app_frames"] == [462]     # replay frame 23 is application frame 440 + 22
    assert vk_replay.replay_frame_number("replay_frame_7.png") == 7 and vk_replay.replay_frame_number("x.png") is None
