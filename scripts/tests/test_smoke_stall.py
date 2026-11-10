"""[SMOKE_STALL] diagnostic line: the stage from fake engine logs, the holders from fake lease records."""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

import pytest

from scripts.smoke_lib import logparse, stall


def _t(event, ms):
    return f"[TIMING v1] event={event} elapsed_ms={ms}\n"


HEAD = "[INSTR v1] banner\n"
LOGS = {
    "launch": HEAD,
    "load": HEAD + _t("profile_ready", 100) + _t("mission_load_start", 200),
    "pipeline": HEAD + _t("mission_load_start", 200) + _t("mission_ready", 9000),
    "first_frame": HEAD + _t("mission_load_start", 200) + _t("mission_ready", 9000) + _t("first_frame", 9100),
}


def _summary(text):
    return logparse.parse_log(text)


@pytest.mark.parametrize("stage", ["launch", "load", "pipeline", "first_frame"])
def test_last_stage_from_log(stage):
    assert stall.last_stage(_summary(LOGS[stage])) == stage


def test_lease_wait_stage():
    assert stall.last_stage(_summary(""), lease_waited_never_launched=True) == "lease_wait"


def test_is_stall():
    assert stall.is_stall(_summary(LOGS["pipeline"]), False)
    assert stall.is_stall(_summary(LOGS["first_frame"]), True)
    assert not stall.is_stall(_summary(LOGS["first_frame"]), False)


def test_holders_exclude_self():
    recs = [{"pid": 10, "label": "run_smoke x"}, {"pid": 11, "batch": "batch merge-gate-1"}, None]
    assert stall.holders_text(recs, own_pid=10) == "11/merge-gate-1"
    assert stall.holders_text([], own_pid=10) == "none"


def test_line_format():
    assert stall.line("mc2_10", "load", 61.4, "11/lane") == "[SMOKE_STALL] map=mc2_10 last_stage=load secs=61 lease_holders=11/lane"
