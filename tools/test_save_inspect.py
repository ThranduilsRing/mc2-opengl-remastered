"""save_inspect: the offline selftest, run by pytest as well as by the contract board."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import save_inspect as mod  # noqa: E402


def test_selftest_passes():
    assert mod.selftest() == 0


def test_record_sizes_match_the_build_time_guard():
    import re
    text = open(os.path.join(HERE, "..", "code", "save_layout_guard.h"), encoding="utf-8").read()
    pinned = {m.group(1): int(m.group(2)) for m in re.finditer(r"^MC2_SAVE_STRUCT_SIZE\((\w+), (\d+)\);", text, re.M)}
    pinned.pop("StackItem")
    assert {name: size for size, name in mod.RECORD_SIZES.items()} == pinned
    assert len(mod.RECORD_SIZES) == len(pinned)          # no two record types share a size: the loader tells them apart by it
