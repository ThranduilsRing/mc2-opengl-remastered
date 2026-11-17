"""Replay format 2 refuses old checkpoints after combat hash coverage expands."""
import importlib.util
from pathlib import Path

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
