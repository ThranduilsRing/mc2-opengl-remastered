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


def test_noise_mask_file_is_well_formed():
    """The mask is data a proof depends on: every class is a real record class, every range lies inside the record, and the ranges of one scope are ordered."""
    path = os.path.join(HERE, "save_noise_mask.json")
    sizes = {name: size for size, name in mod.RECORD_SIZES.items()}
    mask = mod.load_mask(path)
    assert mask
    for cls, scopes in mask.items():
        assert cls in sizes, cls + " is not a record class of save_inspect"
        assert set(scopes) == {"run", "build"}
        for scope, ranges in scopes.items():
            for lo, hi in ranges:
                assert 0 <= lo <= hi < sizes[cls], f"{cls} {scope} range [{lo}, {hi}] is not inside a {sizes[cls]}-byte record"
            assert all(a[1] < b[0] for a, b in zip(ranges, ranges[1:])), f"{cls} {scope} ranges are not ordered and disjoint"
    import json
    with open(path, encoding="utf-8") as f:
        doc = json.load(f)
    assert doc["known_writer_defects"] and all(isinstance(t, str) and t for t in doc["known_writer_defects"])
    # The ranges are 0-based inclusive offsets: the 4800-byte MovePath step lists of the pilot record are masked whole, byte 10071 is not.
    assert mod.masked(mask, "MechWarriorData", 10072, False) and mod.masked(mask, "MechWarriorData", 14871, False)
    assert not mod.masked(mask, "MechWarriorData", 10071, False) and not mod.masked(mask, "MechWarriorData", 14872, False)
    # The pilot strings and the numeric pilot state are never masked.
    assert not any(mod.masked(mask, "MechWarriorData", o, True) for o in range(0, 2168))


def test_pilot_suffix_requires_validated_schema():
    import struct
    prefix = bytes(mod.PILOT_SIZE)
    # signature, version, targets count, global path count, cursor, goal count
    suffix = struct.pack("<IIIIiI", 0x32545157, 2, 0, 0, 0, 0)
    assert mod.packet_kind(4, prefix + suffix) == "MechWarriorData"
    extended = struct.pack("<III2II", 0x32545157, 2, 2, 1, 2, 1)
    extended += struct.pack("<iii6fiii", 0, 1, 2, 1., 2., 3., 4., 5., 6., 2, 3, 4)
    extended += struct.pack("<iI2h", 1, 2, 3, 4)
    assert mod.packet_kind(4, prefix + extended) == "MechWarriorData"
    for bad in (suffix[:-1], suffix + b"x", suffix.replace(b"WQT2", b"WQT3"),
                struct.pack("<IIIIiI", 0x32545157, 3, 0, 0, 0, 0),
                struct.pack("<IIIIiI", 0x32545157, 2, 17, 0, 0, 0),
                struct.pack("<IIIIiI", 0x32545157, 2, 0, 0, -2, 0)):
        assert mod.packet_kind(4, prefix + bad) == "data"
    for size in (19865, 20000, 23960):
        assert mod.packet_kind(4, b"a" * size) == "text"
        assert mod.packet_kind(4, bytes(size)) == "data"
    mask = mod.load_mask(os.path.join(HERE, "save_noise_mask.json"))
    text = b"a" * 20000
    changed = bytearray(text); changed[10072] = ord("b")
    result = mod.compare([(4, text)], [(4, bytes(changed))], mask)
    assert not result["masked"]
    assert result["outside"] == {"text": [1, 1]}


def test_checked_in_v1_fixture_upgrades_to_v2_sections():
    """A real v1 .ims on disk (tests/fixtures/saves/v1_minimal.ims, 133 bytes): stamp, R2 packet after END -> ANIM + ORDQ."""
    import tempfile
    src = os.path.join(HERE, "..", "tests", "fixtures", "saves", "v1_minimal.ims")
    assert mod.load(src)[1][0][1] == mod.STAMP_V1
    with tempfile.TemporaryDirectory() as td:
        dst = os.path.join(td, "v2.ims")
        sections = mod.upgrade(src, dst)
        assert [(s[0], s[3]) for s in sections] == [(mod.fourcc("ANIM"), b"\xde\xad\xbe\xef"), (mod.fourcc("ORDQ"), b"\x01\x02\x03")]
        assert mod.load(dst)[1][0][1] == mod.STAMP_V2
