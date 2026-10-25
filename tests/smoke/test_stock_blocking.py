import importlib.util
from pathlib import Path

import pytest

spec = importlib.util.spec_from_file_location("stock_blocking", Path(__file__).parents[2] / "tools/stock_blocking.py")
subject = importlib.util.module_from_spec(spec)
spec.loader.exec_module(subject)

ROW = "[BLDG_GAMEPLAY] shed|1,2,3|90|0|4,5|covered=1,2;|state=1,2;/los=1,2,3;|state=absent\n"


def test_art_log_noise_and_observation_order_do_not_change_gameplay():
    assert subject.compare("noise\n" + ROW, ROW + "different render noise\n")["diffs"] == 0


def test_added_removed_and_duplicate_placements_are_detected():
    assert subject.compare(ROW, ROW + ROW)["diffs"] == 1
    assert subject.compare(ROW, ROW.replace("covered=1,2;", "covered=1,3;"))["diffs"] == 2
    assert subject.compare(ROW, ROW.replace("state=absent", "state=2,3;"))["diffs"] == 2
    assert subject.compare(ROW, ROW.replace("los=1,2,3;", "los=1,2,4;"))["diffs"] == 2


def test_empty_or_truncated_observations_fail_closed():
    with pytest.raises(ValueError):
        subject.compare("loading", "loading")
    with pytest.raises(ValueError):
        subject.records(ROW.split("|state=")[0])
