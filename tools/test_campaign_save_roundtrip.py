"""campaign_save_roundtrip: the offline half (save comparison, the crafted variant, the launch environment), on small crafted saves."""
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import campaign_save_roundtrip as mod  # noqa: E402

PILOT_SIZE = 19864          # MechWarriorData, pinned by code/save_layout_guard.h
LOGISTICS = ('[General]\r\nl VariantCount = 0\r\nl PilotCount = 1\r\nl InventoryCount = 2\r\n\r\n'
             '[Inventory0]\r\nst Variant = "Atlas"\r\n\r\n[Inventory1]\r\nst Variant = "Bushwacker"\r\n\r\n'
             '[Version]\r\nl VersionNumber = 10004\r\n')


def write_save(path, logistics=LOGISTICS, pilot=None, head=b"version"):
    pilot = bytes(PILOT_SIZE) if pilot is None else pilot
    mod.si.scs.write(str(path), [(0, head), (4, b"campaign"), (4, logistics.encode("latin-1")), (4, pilot)], False)
    return str(path)


def pilot_with(offset, value=1):
    record = bytearray(PILOT_SIZE)
    record[offset] = value
    return bytes(record)


def compare_rc(tmp_path, **second):
    a = write_save(tmp_path / "a.ims")
    b = write_save(tmp_path / "b.ims", **second)
    return mod.compare(a, b)


def test_capture_saves_into_a_private_dir_under_out(tmp_path, monkeypatch):
    """The capture sets MC2_SAVE_DIR=<out>/savegame and reads the save from there, never the owner's folder."""
    seen = {}
    class P:
        def wait(self, timeout=None):
            (tmp_path / "out" / "savegame" / "testgame.ims").write_bytes(b"s")
    def spawn(argv, **kw):
        seen["dir"] = kw["env"]["MC2_SAVE_DIR"]
        return P()
    (tmp_path / "dep").mkdir()
    (tmp_path / "dep" / "mc2.exe").write_bytes(b"")
    monkeypatch.setattr(mod.nofocus, "spawn", spawn)
    monkeypatch.setattr(mod.host_lease, "acquire", lambda *_: None)
    args = mod.argparse.Namespace(deploy=str(tmp_path / "dep"), out=str(tmp_path / "out"), load=None, campaign="c", win_after_sec=1,
                                  save_at=1, load_at=2, check_screens=False, salvage=False, timeout=1, keep_going=False)
    assert mod.capture(args) == 0
    assert seen["dir"] == str(tmp_path / "out" / "savegame") and (tmp_path / "out" / "save1.ims").read_bytes() == b"s"


def test_identical_saves_compare_equal(tmp_path):
    assert compare_rc(tmp_path) == 0


def test_a_logistics_text_difference_fails(tmp_path):
    assert compare_rc(tmp_path, logistics=LOGISTICS.replace("Atlas", "Atlaz")) == 1


def test_a_version_or_campaign_packet_difference_fails(tmp_path):
    assert compare_rc(tmp_path, head=b"versioX") == 1


@pytest.mark.parametrize("offset, expected", [(5, 1), (150, 1), (300, 1), (2167, 1), (2168, 0), (9000, 0)])
def test_pilot_bytes_are_compared_up_to_2167(tmp_path, offset, expected):
    assert compare_rc(tmp_path, pilot=pilot_with(offset)) == expected


def test_the_per_run_pilot_roll_is_tolerated_and_its_neighbours_are_not(tmp_path):
    (offset,) = mod.PILOT_RUN_NOISE
    assert offset == 220                                                    # escapesThruEjection in the MechWarriorData layout
    assert compare_rc(tmp_path, pilot=pilot_with(offset)) == 0
    assert compare_rc(tmp_path, pilot=pilot_with(offset - 1)) == 1
    assert compare_rc(tmp_path, pilot=pilot_with(offset + 1)) == 1


def test_a_save_without_pilots_or_logistics_text_fails(tmp_path):
    a = write_save(tmp_path / "a.ims")
    bare = tmp_path / "bare.ims"
    mod.si.scs.write(str(bare), [(0, b"version"), (4, b"campaign")], False)
    assert mod.compare(a, str(bare)) == 1


def test_logistics_text_is_the_last_text_packet_that_has_a_variant_count(tmp_path):
    path = write_save(tmp_path / "a.ims")
    assert mod.logistics_text(path) == LOGISTICS
    bare = tmp_path / "bare.ims"
    mod.si.scs.write(str(bare), [(0, b"version"), (4, b"[General]\r\nl X = 1\r\n")], False)
    assert mod.logistics_text(str(bare)) == ""


def test_add_player_variant_writes_the_variant_and_repoints_the_second_mech(tmp_path):
    src = write_save(tmp_path / "src.ims")
    dst = str(tmp_path / "dst.ims")
    mod.add_player_variant(src, dst, "data/objects/bushwacker.csv", "Bush-Whacker 'Test' Z", "BUSH-WHACKER 'TEST' z")
    text = mod.logistics_text(dst)
    assert "VariantCount = 1" in text and "VariantCount = 0" not in text
    assert '[Variant0]\r\nst Chassis = "data/objects/bushwacker.csv"\r\nst VariantName = "Bush-Whacker \'Test\' Z"\r\nl ComponentCount = 0' in text
    inventory0, inventory1 = text.split("[Inventory1]")
    assert 'st Variant = "Atlas"' in inventory0                              # the first mech is untouched
    assert 'st Variant = "BUSH-WHACKER \'TEST\' z"' in inventory1            # the second names the variant in another spelling
    with pytest.raises(AssertionError):
        mod.add_player_variant(dst, str(tmp_path / "twice.ims"), "c.csv", "n", "n")   # a save that already has variants is refused


def test_campaign_env_scrubs_the_callers_flow_variables_and_arms_the_fixture(monkeypatch):
    monkeypatch.setenv("MC2_SOAK_KILL_ENEMY", "1")
    monkeypatch.setenv("MC2_BOOT_QUICKLOAD_AT", "5")
    monkeypatch.setenv("UNRELATED_VARIABLE", "kept")
    env = mod.campaign_env("campaign", 2.0, 2400, None)
    assert "MC2_SOAK_KILL_ENEMY" not in env and "MC2_BOOT_QUICKLOAD_AT" not in env
    assert env["UNRELATED_VARIABLE"] == "kept"
    assert env["MC2_BOOT_TO_BAY"] == "campaign" and env["MC2_BOOT_QUICKSAVE_AT"] == "2400" and env["MC2_SOAK_AUTOWIN"] == "1"
    assert mod.campaign_env("campaign", 2.0, 10**9, 900)["MC2_BOOT_QUICKLOAD_AT"] == "900"
