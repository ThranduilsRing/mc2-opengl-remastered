import importlib.util
from pathlib import Path

import pytest

spec = importlib.util.spec_from_file_location("stock_blocking", Path(__file__).parents[2] / "tools/stock_blocking.py")
subject = importlib.util.module_from_spec(spec)
spec.loader.exec_module(subject)

ROW = "[BLDG_GAMEPLAY] shed|1,2,3|90|0|4,5|covered=1,2;|kind=0|state=1,2,0,0,0,0,3;/los=1,2,3;/end|state=absent\n"


def test_art_log_noise_and_observation_order_do_not_change_gameplay():
    assert subject.compare("noise\n" + ROW, ROW + "different render noise\n")["diffs"] == 0


def test_added_removed_and_duplicate_placements_are_detected():
    assert subject.compare(ROW, ROW + ROW)["diffs"] == 1
    assert subject.compare(ROW, ROW.replace("covered=1,2;", "covered=1,3;"))["diffs"] == 2
    assert subject.compare(ROW, ROW.replace("state=absent", "state=/los=/end"))["diffs"] == 2
    assert subject.compare(ROW, ROW.replace("los=1,2,3;", "los=1,2,4;"))["diffs"] == 2


def test_empty_or_truncated_observations_fail_closed():
    with pytest.raises(ValueError):
        subject.compare("loading", "loading")
    with pytest.raises(ValueError):
        subject.records(ROW.split("|state=")[0])


def test_unfinished_final_state_fails_closed():
    complete = ROW.replace("state=absent", "state=2,3,0,0,0,0,1;/los=2,3,4;/end")
    subject.records(complete)
    for suffix in ("", "2,3;", "2,3,0,0,0,0,1;", "/los=2,3,", "/los=2,3,4;", "absentjunk"):
        truncated = complete.rsplit("|state=", 1)[0] + "|state=" + suffix
        with pytest.raises(ValueError):
            subject.compare(truncated, truncated)


def test_one_valid_mission_does_not_hide_missing_placement_replacements():
    classic = "[BLDG_IDENTITY] shed|key=tank\n" + ROW
    placement = ROW.removeprefix(subject.PREFIX).split("|kind=", 1)[0]
    valid = f"[BLDG_ART] {placement}|applied=tank\n" + ROW
    assert subject.coverage(classic, valid, {"tank"})["replaced_observations"] == 1
    # A successful import of some other type (or mission) cannot cover this placement.
    ineffective = f"[MODOVERRIDE] staticProp 'other': render override applied\n[BLDG_ART] {placement}|applied=-\n" + ROW
    with pytest.raises(ValueError, match="missing observed"):
        subject.coverage(classic, ineffective, {"tank"})
    with pytest.raises(ValueError, match="missing observed"):
        subject.coverage(classic + ROW.replace("1,2,3|", "2,2,3|"), valid, {"tank"})
