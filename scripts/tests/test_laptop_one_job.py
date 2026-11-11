"""laptop_gate: the default run includes the sim rows on the same deployed candidate; the launch is a detached job."""
import json
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "scripts"))

import laptop_gate  # noqa: E402
import detached_job  # noqa: E402

HEAD = "a" * 40


def _fake_pc(monkeypatch, tmp_path, gate_verdict, sim_rc):
    calls = []
    monkeypatch.setattr(laptop_gate, "PROOFS", tmp_path)
    monkeypatch.setattr(laptop_gate.subprocess, "check_output", lambda *a, **k: HEAD + "\n")
    monkeypatch.setattr(laptop_gate.subprocess, "run", lambda *a, **k: SimpleNamespace(returncode=0))
    for name in ("provision", "seed", "seed_ref", "sync_mods", "sync_tree", "scp"):
        monkeypatch.setattr(laptop_gate, name, lambda *a, _n=name, **k: calls.append(_n))
    monkeypatch.setattr(laptop_gate, "published_sha", lambda: "p")
    monkeypatch.setattr(laptop_gate, "remote_py", lambda *a: " ".join(a))
    monkeypatch.setattr(laptop_gate, "remote_run_cmd", lambda *a: "RUN")
    monkeypatch.setattr(laptop_gate, "ssh", lambda cmd, **k: (calls.append("ssh " + cmd) or (0, "")))
    monkeypatch.setattr(laptop_gate, "pull_and_accept", lambda *a: (True, "", {"verdict": gate_verdict, "head": HEAD}))
    monkeypatch.setattr(laptop_gate, "sim_step", lambda w, head, rows, pair, t0: (calls.append(("sim", w, head, rows, pair)) or sim_rc))
    return calls


def _args(**kw):
    base = dict(window="W", gate_args=[], no_sim=False, sim_rows="", sim_pair=False)
    base.update(kw)
    return SimpleNamespace(**base)


def test_default_run_ends_with_sim_on_the_same_head(tmp_path, monkeypatch):
    calls = _fake_pc(monkeypatch, tmp_path, "PASS", 0)
    assert laptop_gate.pc_main(_args()) == 0
    sims = [c for c in calls if isinstance(c, tuple)]
    assert sims == [("sim", "W", HEAD, [], False)]
    assert calls.index("ssh RUN") < calls.index(sims[0])   # after the gate run, no second provision/push
    assert calls.count("provision") == 2   # pc_main's own two (bin-only + full); the sim step adds none


def test_red_sim_fails_the_run_and_red_gate_skips_sim(tmp_path, monkeypatch):
    calls = _fake_pc(monkeypatch, tmp_path, "PASS", 1)
    assert laptop_gate.pc_main(_args()) == 1
    calls = _fake_pc(monkeypatch, tmp_path, "FAIL", 0)
    assert laptop_gate.pc_main(_args()) == 1
    assert not [c for c in calls if isinstance(c, tuple)]


def test_no_sim_and_sim_rows_options(tmp_path, monkeypatch):
    calls = _fake_pc(monkeypatch, tmp_path, "PASS", 0)
    assert laptop_gate.pc_main(_args(no_sim=True)) == 0
    assert not [c for c in calls if isinstance(c, tuple)]
    calls = _fake_pc(monkeypatch, tmp_path, "PASS", 0)
    laptop_gate.pc_main(_args(sim_rows="determinism-mc2_01,saveload-mc2_10", sim_pair=True))
    assert [c for c in calls if isinstance(c, tuple)][0][3:] == (["determinism-mc2_01", "saveload-mc2_10"], True)


def test_default_launch_is_a_detached_job_and_attached_runs_in_process(tmp_path, monkeypatch, capsys):
    monkeypatch.setenv("MC2_JOBS_DIR", str(tmp_path))
    seen = {}

    def fake_run(cmd, name=None, log=None, timeout=None, cwd=None):
        seen.update(cmd=cmd, name=name)
        Path(log).write_text("SUMMARY {}\n")
        return 0, 1, {"name": name, "state": "done", "rc": 0, "started": 0, "ended": 1, "log": log}

    monkeypatch.setattr(detached_job, "run", fake_run)
    monkeypatch.setattr(sys, "argv", ["laptop_gate.py", "W7"])
    assert laptop_gate.main() == 0
    assert seen["name"] == "laptop-gate-W7" and "--attached" in seen["cmd"] and seen["cmd"][-1] == "W7"
    assert "SUMMARY" in capsys.readouterr().out
    ran = []
    monkeypatch.setattr(laptop_gate, "pc_main", lambda a: ran.append(a.window) or 0)
    monkeypatch.setattr(sys, "argv", ["laptop_gate.py", "W7", "--attached"])
    assert laptop_gate.main() == 0 and ran == ["W7"]


def test_relaunch_reattaches_to_a_running_job(tmp_path, monkeypatch):
    monkeypatch.setenv("MC2_JOBS_DIR", str(tmp_path))
    py = sys.executable
    first = detached_job.start([py, "-c", "import time; time.sleep(3)"], name="laptop-gate-W8")
    again = detached_job.start([py, "-c", "raise SystemExit(1)"], name="laptop-gate-W8")
    assert again["supervisor_pid"] == first["supervisor_pid"]
    assert detached_job.wait("laptop-gate-W8", timeout=60)["rc"] == 0
