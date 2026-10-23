"""WORLD_ORDER negative controls exercise slot, cursor, event and update ordering."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import world_order_diff as diff


def write(path, bodies=None, ticks=2):
    bodies = bodies or ["begin", "snapshot=gom_begin collidables_dirty=0",
                        "view=movers n=2 cursor=-1 slots=1:2:1,3:4:1",
                        "view=weapons n=0 cursor=0 slots=", "event=spawn id=1:2:1 detail=0",
                        "update=mechs id=1:2:1", "update=mechs id=3:4:1", "end"]
    path.write_text("noise\n" + "".join(f"{diff.PREFIX}tick={tick} seq={i} {body}\n"
                                      for tick in range(1, ticks + 1) for i, body in enumerate(bodies)))
    return path


@pytest.mark.parametrize("old,new,expected", [
    ("slots=1:2:1,3:4:1", "slots=3:4:1,1:2:1", "slot 0"),
    ("cursor=0", "cursor=1", "cursor"),
    ("event=spawn", "event=remove", "event"),
    ("update=mechs id=1:2:1", "update=mechs id=3:4:1", "id="),
])
def test_first_divergence(tmp_path, old, new, expected):
    a, b = write(tmp_path / "a"), write(tmp_path / "b")
    b.write_text(b.read_text().replace(old, new))
    ok, message = diff.compare(a, b)
    assert not ok and "tick 1" in message and expected in message


def test_identical_and_length_policy(tmp_path):
    a, b = write(tmp_path / "a"), write(tmp_path / "b", ticks=3)
    assert diff.compare(a, a)[0]
    assert not diff.compare(a, b)[0]
    assert diff.compare(a, b, common_prefix=True)[0]
    assert not diff.compare(a, b, common_prefix=True, min_ticks=3)[0]


@pytest.mark.parametrize("damage", ["empty", "record", "begin", "end", "tick", "malformed"])
def test_incomplete_observations_fail_even_when_identical(tmp_path, damage):
    a = write(tmp_path / "a")
    text = a.read_text()
    if damage == "empty": text = "noise\n"
    if damage == "record": text = "\n".join(x for x in text.splitlines() if "seq=4 " not in x)
    if damage == "begin": text = text.replace("seq=0 begin", "seq=0 absent")
    if damage == "end": text = text.replace("seq=7 end", "seq=7 absent")
    if damage == "tick": text = text.replace("tick=2", "tick=3")
    if damage == "malformed": text += diff.PREFIX + "broken\n"
    a.write_text(text)
    assert not diff.compare(a, a, common_prefix=True)[0]
