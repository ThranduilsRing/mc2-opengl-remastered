"""savegame_backup: the offline selftest, run by pytest as well as by the contract board."""
import datetime
import os
import sys
from pathlib import Path

import pytest

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


def test_prune_backups_leaves_hand_named_copies_alone():
    with mod.scratch_dir() as d:
        tmp_path = Path(d)
        hand = tmp_path / f"{mod.SAVE_NAME}.bak_before_boss"
        hand.write_bytes(b"keep me")
        for day in range(1, 8):
            (tmp_path / f"{mod.SAVE_NAME}.bak_2026100{day}_120000").write_bytes(b"x")
        removed = mod.prune_backups(str(tmp_path))
        assert len(removed) == 2
        assert hand.read_bytes() == b"keep me"


def test_backup_never_prunes_the_one_it_just_wrote():
    with mod.scratch_dir() as d:
        tmp_path = Path(d)
        (tmp_path / mod.SAVE_NAME).write_bytes(b"owner")
        for day in range(1, 8):
            (tmp_path / f"{mod.SAVE_NAME}.bak_2026100{day}_120000").write_bytes(b"old")
        # The clock went backwards: the new stamp sorts below every existing backup.
        assert mod.backup(str(tmp_path), datetime.datetime(2025, 1, 1, 0, 0, 0)).startswith("backed up to ")
        just_written = tmp_path / f"{mod.SAVE_NAME}.bak_20250101_000000"
        assert just_written.exists()
        # The newest five plus the protected one; the next backup prunes back to five.
        assert len(list(tmp_path.glob(f"{mod.SAVE_NAME}.bak_*"))) == mod.BACKUP_KEEP + 1
        assert not (tmp_path / f"{mod.SAVE_NAME}.bak_20261001_120000").exists()


def test_a_lock_whose_holder_is_gone_is_taken_over_at_once(capsys, monkeypatch):
    monkeypatch.setattr(mod, "_pid_alive", lambda pid: False)
    with mod.scratch_dir() as d:
        lock = Path(d) / mod.LOCK_NAME
        lock.write_text("pid=999999 since=0", encoding="utf-8")
        with mod.run_lock(d, wait_s=0.1):
            assert lock.exists()
        assert not lock.exists()
    assert "took over stale lock" in capsys.readouterr().out


def test_a_live_holder_is_not_taken_over_within_the_age_bound():
    with mod.scratch_dir() as d:
        lock = Path(d) / mod.LOCK_NAME
        lock.write_text(f"pid={os.getpid()} since=0", encoding="utf-8")
        with pytest.raises(TimeoutError):
            with mod.run_lock(d, wait_s=0.1):
                pass
        assert lock.exists()


def test_an_unreadable_holder_record_is_stolen_after_the_age_bound():
    with mod.scratch_dir() as d:
        lock = Path(d) / mod.LOCK_NAME
        lock.write_text("no pid here", encoding="utf-8")
        os.utime(lock, (0, 0))
        with mod.run_lock(d, wait_s=0.1):
            pass
        assert not lock.exists()
