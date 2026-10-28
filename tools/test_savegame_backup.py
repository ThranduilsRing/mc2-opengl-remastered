"""savegame_backup: the offline selftest, run by pytest as well as by the contract board."""
import datetime
import os
import sys
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import savegame_backup as mod  # noqa: E402


def test_selftest_passes():
    assert mod.selftest() == 0


def test_backup_keeps_only_newest_five():
    with mod.scratch_dir() as d:
        tmp_path = Path(d)
        save = tmp_path / mod.SAVE_NAME
        save.write_bytes(b"owner")
        for day in range(1, 8):
            (tmp_path / f"{mod.SAVE_NAME}.bak_2026100{day}_120000").write_bytes(b"owner")
        (tmp_path / "testgame.ims.other").write_bytes(b"keep me")
        (tmp_path / "unrelated.ims.bak_20261001_000000").write_bytes(b"keep me")

        assert mod.backup(str(tmp_path), datetime.datetime(2026, 10, 9, 12, 0, 0)).startswith("backed up to ")

        baks = sorted(p.name for p in tmp_path.glob(f"{mod.SAVE_NAME}.bak_*"))
        assert baks == [f"{mod.SAVE_NAME}.bak_2026100{d}_120000" for d in (4, 5, 6, 7)] + [
            f"{mod.SAVE_NAME}.bak_20261009_120000"]
        for day in (1, 2, 3):
            assert not (tmp_path / f"{mod.SAVE_NAME}.bak_2026100{day}_120000").exists()
        assert save.read_bytes() == b"owner"
        assert (tmp_path / "testgame.ims.other").read_bytes() == b"keep me"
        assert (tmp_path / "unrelated.ims.bak_20261001_000000").read_bytes() == b"keep me"


def test_prune_backups_is_noop_below_limit():
    with mod.scratch_dir() as d:
        tmp_path = Path(d)
        for day in (1, 2):
            (tmp_path / f"{mod.SAVE_NAME}.bak_2026100{day}_120000").write_bytes(b"x")
        assert mod.prune_backups(str(tmp_path)) == []
        assert len(list(tmp_path.glob(f"{mod.SAVE_NAME}.bak_*"))) == 2
