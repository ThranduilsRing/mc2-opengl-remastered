"""Capture lease ordering and cleanup, without filesystem writes or child launches."""
import io
import subprocess
import sys
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import MagicMock, Mock

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import tracy_capture_compare as capture  # noqa: E402


@pytest.fixture
def harness(monkeypatch):
    h = SimpleNamespace(held=False, events=[], logs=[], children=[])

    def acquire(label):
        assert not h.held
        h.held = True
        h.events.append("acquire")

    def release():
        assert h.held
        assert all(child.poll() is not None for child in h.children)
        assert all(log.closed for log in h.logs)
        h.events.append("release")
        h.held = False

    def child(name):
        proc = Mock(pid=len(h.children) + 1, returncode=None)

        def wait(timeout=None):
            assert h.held
            h.events.append(name + ".wait")
            proc.returncode = 0
            return 0

        def kill():
            assert h.held
            h.events.append(name + ".kill")
            proc.returncode = -1

        proc.poll.side_effect = lambda: proc.returncode
        proc.wait.side_effect = wait
        proc.kill.side_effect = kill
        h.children.append(proc)
        return proc

    def recorder_spawn(argv, **kwargs):
        assert h.held
        h.events.append("recorder.spawn")
        return child("recorder")

    def game_spawn(argv, **kwargs):
        assert h.held
        h.events.append("game.spawn")
        return child("game")

    def open_log(*args, **kwargs):
        assert h.held
        log = io.StringIO()
        h.logs.append(log)
        return log

    h.out = MagicMock(spec=Path)
    h.out.resolve.return_value = h.out
    h.out.is_file.return_value = True
    h.out.stat.return_value = SimpleNamespace(st_size=capture.MIN_TRACE_BYTES)
    h.out.with_suffix.side_effect = lambda suffix: Path("trace.tracy").with_suffix(suffix)
    h.install = MagicMock(spec=Path)
    h.install.__truediv__.return_value.is_file.return_value = True
    h.acquire = Mock(side_effect=acquire)
    h.release = Mock(side_effect=release)
    h.recorder_spawn = Mock(side_effect=recorder_spawn)
    h.game_spawn = Mock(side_effect=game_spawn)
    monkeypatch.setattr(capture, "Path", Mock(return_value=h.out))
    monkeypatch.setattr(capture, "open", open_log, raising=False)
    monkeypatch.setattr(capture, "tracy_exe", Mock(return_value=Path("tracy-capture.exe")))
    monkeypatch.setattr(capture.host_lease, "acquire", h.acquire)
    monkeypatch.setattr(capture.host_lease, "release", h.release)
    monkeypatch.setattr(capture.host_lease, "holds", lambda: h.held)
    monkeypatch.setattr(capture.subprocess, "Popen", h.recorder_spawn)
    monkeypatch.setattr(capture.subprocess, "run", Mock(side_effect=AssertionError("unexpected child")))
    monkeypatch.setattr(capture.nofocus, "spawn", h.game_spawn)
    monkeypatch.setattr(capture.nofocus, "hidden_kwargs", Mock(return_value={}))
    monkeypatch.setattr(capture.time, "sleep", Mock())
    monkeypatch.setattr(capture.render_options_pin, "record", Mock(return_value={"rejected": []}))
    monkeypatch.setattr(capture.render_options_pin, "write", Mock())
    h.run = lambda show_window=True: capture.capture_trace(h.install, "vk", "mc2_01", 15, h.out,
                                                          show_window=show_window)
    return h


@pytest.mark.parametrize("show_window", [True, False])
def test_lease_precedes_both_children_and_covers_normal_exit(harness, show_window):
    h = harness
    assert h.run(show_window) == 0
    assert h.events == ["acquire", "recorder.spawn", "game.spawn", "game.wait",
                        "recorder.wait", "game.wait", "recorder.wait", "release"]
    h.acquire.assert_called_once()
    h.release.assert_called_once()
    assert not h.held
    kwargs = h.game_spawn.call_args.kwargs
    assert kwargs["show_window"] is show_window and kwargs["small_window"] is False
    assert kwargs["env"]["MC2_WINDOW_OFFSCREEN"] == "1"


@pytest.mark.parametrize("which", ["recorder", "game"])
def test_spawn_failure_reaps_existing_child_and_releases(harness, which):
    h = harness
    getattr(h, which + "_spawn").side_effect = OSError("spawn failed")
    with pytest.raises(OSError, match="spawn failed"):
        h.run()
    h.release.assert_called_once()
    assert not h.held and h.events[-1] == "release"
    if which == "game":
        assert h.events == ["acquire", "recorder.spawn", "recorder.kill", "recorder.wait", "release"]
    else:
        h.game_spawn.assert_not_called()


def test_busy_lease_starts_no_children(harness):
    h = harness
    h.acquire.side_effect = capture.host_lease.HostLeaseBusy("busy")
    with pytest.raises(capture.host_lease.HostLeaseBusy, match="busy"):
        h.run()
    h.recorder_spawn.assert_not_called()
    h.game_spawn.assert_not_called()
    h.release.assert_not_called()
    assert not h.logs


def test_interruption_before_game_spawn_reaps_recorder(harness, monkeypatch):
    h = harness
    monkeypatch.setattr(capture.time, "sleep", Mock(side_effect=KeyboardInterrupt))
    with pytest.raises(KeyboardInterrupt):
        h.run()
    assert h.events == ["acquire", "recorder.spawn", "recorder.kill", "recorder.wait", "release"]
    h.game_spawn.assert_not_called()


@pytest.mark.parametrize("operation", ["record", "write"])
def test_options_failure_reaps_recorder_and_releases(harness, operation):
    h = harness
    getattr(capture.render_options_pin, operation).side_effect = OSError("options failed")
    with pytest.raises(OSError, match="options failed"):
        h.run()
    assert h.events == ["acquire", "recorder.spawn", "recorder.kill", "recorder.wait", "release"]
    h.game_spawn.assert_not_called()


@pytest.mark.parametrize("fail_on", [1, 2])
def test_log_open_failure_releases(harness, monkeypatch, fail_on):
    h = harness
    original_open = capture.open
    calls = 0

    def open_log(*args, **kwargs):
        nonlocal calls
        calls += 1
        if calls == fail_on:
            raise OSError("log failed")
        return original_open(*args, **kwargs)

    monkeypatch.setattr(capture, "open", open_log)
    with pytest.raises(OSError, match="log failed"):
        h.run()
    h.release.assert_called_once()
    assert not h.held
    h.game_spawn.assert_not_called()


def test_wait_failure_reaps_both_children_and_releases(harness):
    h = harness
    original_spawn = h.game_spawn.side_effect

    def game_spawn(*args, **kwargs):
        proc = original_spawn(*args, **kwargs)
        original_wait = proc.wait.side_effect

        def wait(timeout=None):
            if timeout is not None:
                raise RuntimeError("wait failed")
            return original_wait()

        proc.wait.side_effect = wait
        return proc

    h.game_spawn.side_effect = game_spawn
    with pytest.raises(RuntimeError, match="wait failed"):
        h.run()
    assert h.events == ["acquire", "recorder.spawn", "game.spawn", "game.kill", "game.wait",
                        "recorder.kill", "recorder.wait", "release"]


def test_trivial_trace_releases_on_system_exit(harness):
    h = harness
    h.out.stat.return_value.st_size = 0
    with pytest.raises(SystemExit, match="missing or trivial"):
        h.run()
    h.release.assert_called_once()
    assert not h.held


@pytest.mark.parametrize("which", ["recorder", "game"])
def test_timeout_reaps_children_before_release(harness, which):
    h = harness
    spawn = getattr(h, which + "_spawn")
    original_spawn = spawn.side_effect

    def timed_spawn(*args, **kwargs):
        proc = original_spawn(*args, **kwargs)
        original_wait = proc.wait.side_effect

        def wait(timeout=None):
            if timeout is not None:
                assert h.held
                raise subprocess.TimeoutExpired(which, timeout)
            return original_wait()

        proc.wait.side_effect = wait
        return proc

    spawn.side_effect = timed_spawn
    assert h.run() == 1
    assert which + ".kill" in h.events
    h.release.assert_called_once()
    assert not h.held


def test_vk_arm_leaves_island_defaults_to_the_engine(monkeypatch):
    # lane 2 owns VK semantics: the arm no longer forces MC2_VK_SHADOW_INTEROP; an explicit --env still sets it.
    monkeypatch.delenv("MC2_VK_SHADOW_INTEROP", raising=False)
    assert "MC2_VK_SHADOW_INTEROP" not in capture.VK_ENV
    assert "MC2_VK_SHADOW_INTEROP" not in capture.game_env("vk", True, None, None, [])
    assert capture.game_env("vk", True, None, None, ["MC2_VK_SHADOW_INTEROP=0"])["MC2_VK_SHADOW_INTEROP"] == "0"


def test_held_chain_lease_is_not_released_by_a_capture(harness):
    h = harness
    h.held = True                                   # main() took it for the whole command
    assert capture.capture_trace(h.install, "vk", "mc2_01", 15, h.out) == 0
    h.acquire.assert_not_called()
    h.release.assert_not_called()
    assert h.held


def test_gl_arm_is_refused_before_anything_runs():
    with pytest.raises(SystemExit, match="GL arm was retired"):
        capture.game_env("gl", False, 0, None, [])


def test_vk_measurement_without_the_backend_receipt_is_refused(tmp_path):
    log = tmp_path / "run.game.log"
    log.write_text("[BOOT] engine up\n", encoding="utf-8")
    with pytest.raises(SystemExit, match="Vulkan backend selected"):
        capture.assert_drew_vk(log)


def test_vk_measurement_that_fell_back_to_gl_is_refused(tmp_path):
    log = tmp_path / "run.game.log"
    log.write_text("Vulkan backend selected\n[RENDER_BACKEND] fail-soft to GL\n", encoding="utf-8")
    with pytest.raises(SystemExit, match="REFUSING"):
        capture.assert_drew_vk(log)


def test_measure_pose_refuses_a_run_that_did_not_draw_vk(tmp_path, monkeypatch):
    monkeypatch.setattr(capture, "capture_trace", lambda *a, **k: None)
    monkeypatch.setattr(capture, "assert_window_was_visible", lambda *a, **k: None)
    (tmp_path / "p.game.log").write_text("[BOOT] engine up\n", encoding="utf-8")
    with pytest.raises(SystemExit, match="Vulkan backend selected"):
        capture._measure_pose(Path("inst"), tmp_path, "p", "mc2_10", "", 5, [], Path("exp"), "DrawScreen")


def test_selfcheck_pair_is_the_two_resolution_tiers():
    assert capture.SELFCHECK_SIZES == ((1920, 1080), (3840, 2160))


def test_compare_takes_no_capture_lease(monkeypatch):
    events = []
    monkeypatch.setattr(capture.host_lease, "acquire", lambda label: events.append("acquire"))
    monkeypatch.setattr(capture.host_lease, "release", lambda: events.append("release"))
    monkeypatch.setattr(capture, "cmd_compare", lambda args: events.append("compare") or 0)
    monkeypatch.setattr(sys, "argv", ["tracy_capture_compare.py", "compare", "a.tracy", "b.tracy"])
    assert capture.main() == 0
    assert events == ["compare"]
