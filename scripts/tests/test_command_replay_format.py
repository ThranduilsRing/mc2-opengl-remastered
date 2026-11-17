"""Replay format 2 refuses old checkpoints after combat hash coverage expands."""
import importlib.util
from pathlib import Path
from types import SimpleNamespace

import pytest

spec = importlib.util.spec_from_file_location(
    "command_replay_format", Path(__file__).resolve().parents[2] / "tools/command_replay.py")
replay = importlib.util.module_from_spec(spec)
spec.loader.exec_module(replay)


@pytest.mark.parametrize("version", [1, 3])
def test_unsupported_version(tmp_path, version):
    path = tmp_path / "replay.txt"
    path.write_text(f'MC2REPLAY {version} "unit" 123 123 123 "content"\nH 1 1 2 3 4 5\nE 0 1\n')
    with pytest.raises(ValueError, match=f"unsupported replay format version {version}; expected 2"):
        replay.inspect(path)


def test_current_version(tmp_path):
    path = tmp_path / "replay.txt"
    path.write_text('MC2REPLAY 2 "unit" 123 123 123 "content"\nH 1 1 2 3 4 5\nE 0 1\n')
    assert replay.inspect(path) == {"commands": 0, "checkpoints": 1, "first_tick": 1, "last_tick": 1}


@pytest.mark.parametrize("command", ["teleport 512 -2450 -5500", "spawn clone 512 0 0",
                                    "freeze_movers on", "force_state 512 sensor 4",
                                    "time_scale 2", "end_mission win"])
def test_fixture_mutations_are_not_recordable(command):
    with pytest.raises(ValueError, match="line 3:.*fixture mutations.*not replayed"):
        replay.validate_commands("select all\norder stop\n" + command)


@pytest.mark.parametrize("command", ["order jump -2450 -5500", "order hold_fire",
                                    "order powerup", "order powerdown", "order withdraw"])
def test_normal_player_order_is_recordable(command):
    replay.validate_commands("select 512\n" + command + "\n")


def test_recorded_order_script():
    replay.validate_commands("select all\r\norder stop\n\nselect 512\n"
                             "order move -2450 -5500 walk\norder attack 513\n")


@pytest.mark.parametrize("mode", ["record", "proof"])
@pytest.mark.parametrize("explicit", [False, True])
def test_proof_scripts_exercise_movement_and_combat(tmp_path, monkeypatch, mode, explicit):
    argv = ["command_replay.py", mode, str(tmp_path / "recording"),
            "--install", str(tmp_path), "--out", str(tmp_path / "output")]
    if explicit:
        script = tmp_path / "orders.txt"
        script.write_text(" # comment\n  select all\n\torder move -4900 -5400\norder attack 706\n")
        argv += ["--commands", str(script)]
    monkeypatch.setattr(replay.sys, "argv", argv)
    monkeypatch.setattr(replay, "fingerprint", lambda *args: "test")
    def check_script(args, label, env, expect):
        commands = Path(env["MC2_UI_COMMAND_FILE"]).read_text().splitlines()
        assert b"\r" not in Path(env["MC2_UI_COMMAND_FILE"]).read_bytes()
        assert commands[0] == "select all"
        assert any(line.startswith("order move ") for line in commands)
        assert any(line.startswith("order attack ") for line in commands)
        replay.validate_commands("\n".join(commands))
        raise InterruptedError("checked proof commands")
    monkeypatch.setattr(replay, "run", check_script)
    with pytest.raises(InterruptedError, match="checked proof commands"):
        replay.main()


def test_fixture_rejected_before_fingerprint_or_game(tmp_path, monkeypatch):
    commands = tmp_path / "commands.txt"
    commands.write_text("teleport 512 -2450 -5500\norder attack 513\n")
    output = tmp_path / "output"
    monkeypatch.setattr(replay.sys, "argv", ["command_replay.py", "record",
        str(tmp_path / "recording"), "--install", str(tmp_path), "--out", str(output),
        "--commands", str(commands)])
    monkeypatch.setattr(replay, "fingerprint", lambda *args: pytest.fail("fingerprinted fixture"))
    monkeypatch.setattr(replay.subprocess, "run", lambda *args, **kwargs: pytest.fail("launched fixture"))
    with pytest.raises(ValueError, match="teleport.*only select and order"):
        replay.main()
    assert not output.exists()


@pytest.mark.parametrize("extra", [{}, {"MC2_UI_COMMAND_FILE": "validated-script"}])
def test_only_explicit_recording_script_reaches_game(tmp_path, monkeypatch, extra):
    monkeypatch.setenv("MC2_UI_COMMAND_FILE", "inherited-teleport-script")
    for name in ("RECORD", "PLAY", "TICKS", "FUTURE"):
        monkeypatch.setenv("MC2_COMMAND_REPLAY_" + name, "inherited")
    extra = {**extra, "MC2_COMMAND_REPLAY_RECORD": "explicit"}
    def child(*args, env, **kwargs):
        assert env.get("MC2_UI_COMMAND_FILE") == extra.get("MC2_UI_COMMAND_FILE")
        assert {k: v for k, v in env.items() if k.startswith("MC2_COMMAND_REPLAY_")} == {
            "MC2_COMMAND_REPLAY_RECORD": "explicit"}
        raise InterruptedError("checked child environment")
    monkeypatch.setattr(replay.subprocess, "run", child)
    args = SimpleNamespace(out=tmp_path, seed=123, content="test", install=tmp_path,
                           mission="mc2_10", duration=30)
    with pytest.raises(InterruptedError, match="checked child environment"):
        replay.run(args, "run", extra, "unused")


def test_play_rejects_commands_before_reading_them(tmp_path, monkeypatch):
    monkeypatch.setattr(replay.sys, "argv", ["command_replay.py", "play", "unused",
        "--commands", str(tmp_path / "nonexistent")])
    with pytest.raises(SystemExit) as error:
        replay.main()
    assert error.value.code == 2


def test_record_failed_marker_overrides_success(tmp_path, monkeypatch):
    artifacts = tmp_path / "artifacts"
    artifacts.mkdir()
    (artifacts / "unit.log").write_text(
        "[COMMAND_REPLAY] record_failed reason=bench_freeze\n[REPLAY_RECORDED v1]\n")
    monkeypatch.setattr(replay.subprocess, "run", lambda *args, **kwargs: SimpleNamespace(
        returncode=0, stdout=f"[runner] artifacts: {artifacts}\n", stderr=""))
    args = SimpleNamespace(out=tmp_path, seed=123, content="test", install=tmp_path,
                           mission="unit", duration=30)
    with pytest.raises(RuntimeError, match="replay run failed"):
        replay.run(args, "run", {}, "[REPLAY_RECORDED v1]")
