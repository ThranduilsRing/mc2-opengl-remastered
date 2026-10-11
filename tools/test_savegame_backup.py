"""savegame_backup: the offline selftest, run by pytest as well as by the contract board."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import savegame_backup as mod  # noqa: E402


def test_selftest_passes():
    assert mod.selftest() == 0
