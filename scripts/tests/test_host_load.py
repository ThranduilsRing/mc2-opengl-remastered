"""The host-load gate waits out a loaded host (bounded, never a failure) and records the cause for the smoke report."""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from scripts.smoke_lib import host_load  # noqa: E402

QUIET = {"builds": {"cl.exe": 2}, "engines": [], "defender_pct": 3.0}
BUILD = {"builds": {"cl.exe": 7, "link.exe": 1}, "engines": [], "defender_pct": None}
SCAN = {"builds": {}, "engines": [], "defender_pct": 90.0}
GAME = {"builds": {}, "engines": [(4242, "mc2.exe")], "defender_pct": None}


class Clock:
    def __init__(self):
        self.t = 0.0

    def __call__(self):
        return self.t

    def sleep(self, s):
        self.t += s


def run(samples, bound=60.0, monkeypatch=None):
    it = iter(samples)
    last = [samples[-1]]

    def sampler():
        try:
            last[0] = next(it)
        except StopIteration:
            pass
        return last[0]
    clock, lines = Clock(), []
    rec = host_load.wait_until_quiet(log=lines.append, sampler=sampler, sleep=clock.sleep, clock=clock, timeout_s=bound)
    return rec, lines


def test_reasons_name_each_cause():
    assert host_load.busy_reason(QUIET) == ""
    assert "build burst: 8" in host_load.busy_reason(BUILD)
    assert "MsMpEng at 90%" in host_load.busy_reason(SCAN)
    assert "mc2.exe pid 4242" in host_load.busy_reason(GAME)
    assert host_load.busy_reason({"builds": {}, "engines": [], "defender_pct": None}) == ""


def test_quiet_host_launches_at_once():
    rec, lines = run([QUIET])
    assert rec["checked"] and rec["waited_s"] == 0 and not rec["timed_out"] and lines == []


def test_waits_then_records_first_cause():
    rec, lines = run([BUILD, SCAN, QUIET])
    assert not rec["timed_out"] and rec["waited_s"] == 2 * host_load.POLL_S
    assert rec["first_reason"].startswith("build burst") and rec["last_reason"].startswith("MsMpEng")
    assert len([l for l in lines if "waiting" in l]) == 1          # one line, not one per poll


def test_bound_launches_anyway_and_says_so():
    rec, lines = run([BUILD], bound=20.0)
    assert rec["timed_out"] and rec["waited_s"] >= 20.0
    assert "launching anyway" in lines[-1]


def test_off_switch_and_bad_knob(monkeypatch):
    monkeypatch.setenv("MC2_HOST_LOAD", "off")
    rec, _ = run([BUILD])
    assert rec["checked"] is False and rec["note"] == "MC2_HOST_LOAD=off"
    monkeypatch.delenv("MC2_HOST_LOAD")
    monkeypatch.setenv("MC2_HOST_LOAD_BUILD_MAX", "lots")
    rec, lines = run([QUIET])
    assert rec["checked"] and any("not a number" in l for l in lines)


def test_sampler_failure_never_blocks():
    def boom():
        raise RuntimeError("psutil gone")
    rec = host_load.wait_until_quiet(log=lambda m: None, sampler=boom, sleep=lambda s: None, clock=lambda: 0.0, timeout_s=5)
    assert rec["checked"] is False and "psutil gone" in rec["last_reason"]


def test_real_sample_shape():
    s = host_load.sample(interval_s=0.05)
    assert set(s) == {"builds", "engines", "defender_pct"}
