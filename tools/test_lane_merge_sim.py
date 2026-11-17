"""Sim/save-sensitive precheck rows, promote/bisect/ratchet-tighten tests (split from test_lane_merge.py: file-size ratchet)."""
import json
import os
import subprocess

import pytest

from test_lane_merge import (  # noqa: F401
    FAKES,
    _branch,
    _load_module,
    _no_receipts_by_default,
    _sim_evidence,
    _stage_fixture,
    env,
    git,
    parents,
    precheck_fixture,
)

import lane_merge as lm  # noqa: E402  (tools/ is on sys.path: test_lane_merge inserts HERE)


# ---- G3: sim/save-sensitive paths make --precheck require and run saveload + determinism rows ----


@pytest.mark.parametrize("target", ["documented", "active", "explicit", "batch"])
def test_precheck_classifies_only_changes_since_play_target(env, monkeypatch, tmp_path, target):
    from tools import lane_merge as lm
    repo, _ = env
    precheck_fixture(lm, repo, monkeypatch, None)
    _branch(repo, "claude/play-test", "code/mission_update.cpp", "play sim change\n", "play")
    git(repo, "checkout", "-q", "claude/play-test")
    play = git(repo, "rev-parse", "HEAD")
    if target == "documented":
        (repo / "CLAUDE.md").write_text("Play branch: `claude/play-test`\n")
        git(repo, "add", "CLAUDE.md")
        git(repo, "commit", "-qm", "play target")
    git(repo, "checkout", "-qb", "tooling-tip")
    (repo / "tools").mkdir(exist_ok=True)
    (repo / "tools/fix.py").write_text("# tooling only\n")
    git(repo, "add", "tools/fix.py")
    git(repo, "commit", "-qm", "tooling")
    tip = git(repo, "rev-parse", "HEAD")
    if target in {"active", "batch"}:
        git(repo, "checkout", "-q", "claude/play-test")
    args = ["--root", str(repo), "--precheck", tip, "--no-link"]
    args += ["--base", play] if target == "explicit" else ["--batch", tip] if target == "batch" else []
    monkeypatch.setattr(lm, "PRECHECK_RECEIPTS", str(tmp_path / "receipts"))
    assert lm.main(args) == 0
    rec = json.loads((tmp_path / "receipts" / (tip + ".json")).read_text())
    assert rec["sim_rows"] == {"required": False}
    assert lm.merge_target(str(repo), "claude/nifty-mendeleev") != play

def test_sim_path_classification():
    from tools import lane_merge as lm
    hit = lm.sim_sensitive_paths(["mclib/mech3d_update.cpp", "mclib/gvactor.h", "mclib/msl_shape.cpp", "mclib/appear.h",
                                  "mclib/terrain_runtime.cpp", "code/mech_value_codec.h", "code/mission_update.cpp",
                                  "code/saveload_load.cpp", "GameOS/gameos/sim_overlap.cpp", "code/tick_commit.cpp"])
    assert len(hit) == 10
    miss = ["mclib/txmmgr.cpp", "GameOS/gameos/gameos_graphics.cpp", "scripts/merge_gate.py", "docs/x.md", "shaders/terrain.frag",
            "code/mech_ui.cpp", "tools/lane_merge.py"]
    assert lm.sim_sensitive_paths(miss) == []
    assert lm.sim_sensitive_paths(["a/b.cpp"], ["a/*"]) == ["a/b.cpp"]
    assert lm.SIM_ROWS == ("saveload-mc2_01", "saveload-mc2_10", "determinism-mc2_10")


def sim_tip(repo, tmp_path, monkeypatch, rel="mclib/mech3d.cpp"):
    """A tip touching `rel`, a fake install deployed from it, receipts in tmp."""
    from tools import lane_merge as lm
    precheck_fixture(lm, repo, monkeypatch, None)
    _branch(repo, "lane_s", rel, "x\n", "sim lane")
    sha = git(repo, "rev-parse", "lane_s")
    install = tmp_path / "inst"
    install.mkdir()
    (install / "mc2.exe").write_bytes(b"x")
    (install / ".deployed_manifest.csv").write_text(
        "manifest_version,v1,,,,\nrelpath,sha256,bytes,src_commit,timestamp,mtime_ns\nmc2.exe,0,1," + sha + ",t,1\n")
    monkeypatch.setattr(lm, "PRECHECK_RECEIPTS", str(tmp_path / "receipts"))
    return lm, sha, str(install)












def test_receipt_path_refuses_a_non_40_hex_sha_and_tests_use_a_private_dir():
    from tools import lane_merge as lm
    for bad in ("abc", "", "g" * 40, "a" * 39):
        with pytest.raises(ValueError):
            lm.receipt_path(bad)
    assert lm.receipt_path("a" * 40).endswith("a" * 40 + ".json")
    assert not os.path.normpath(lm.PRECHECK_RECEIPTS).startswith(os.path.normpath("A:/Games/mc2-ops"))


def test_bisect_halves_a_red_batch_and_rejects_the_guilty_tip():
    bisect_step = _load_module().bisect_step
    s = dict(testing=["a", "b", "c"], queue=["d"])
    s, act = bisect_step(s, passed=False)
    assert act == "retest" and s["testing"] == ["a", "b"] and s["queue"] == ["c", "d"]
    s, act = bisect_step(s, passed=True)
    assert act == "promote" and s["landed"] == ["a", "b"] and s["testing"] == ["c", "d"] and s["queue"] == []
    s, act = bisect_step(s, passed=False)
    assert act == "retest" and s["testing"] == ["c"] and s["queue"] == ["d"]
    s, act = bisect_step(s, passed=False)
    assert act == "guilty" and s["rejected"] == ["c"] and s["testing"] == ["d"]
    assert len(s["history"]) == 4


def test_stage_help_documents_the_queue_commands(capsys):
    with pytest.raises(SystemExit):
        _load_module().main(["--help"])
    out = capsys.readouterr().out
    assert all(k in out for k in ("--stage W --batch", "--promote W", "--bisect W --red", "nifty untouched"))



def test_ratchet_tighten_compares_against_the_accepted_bases_baseline(tmp_path, monkeypatch):
    # p31: the candidate's own (possibly raised) baseline must not be the yardstick; the base's `git show` copy is.
    tools = tmp_path / "tools"
    for rel in FAKES:
        (tools / rel).parent.mkdir(parents=True, exist_ok=True)
        (tools / rel).write_text("")
    shown, ran = [], []
    monkeypatch.setattr(lm, "git", lambda root, *a: (shown.append(a), subprocess.CompletedProcess(a, 0, '{"base": 1}'))[1])

    def fake_run(root, cmd, env=None):
        baseline = cmd[cmd.index("--baseline") + 1]
        ran.append((os.path.basename(cmd[2]), open(baseline).read()))
        return subprocess.CompletedProcess(cmd, 1 if "cxx17" in cmd[2] else 0, "line\nover the base baseline")
    monkeypatch.setattr(lm, "run", fake_run)
    failed = _bbc(tmp_path, tools)
    # W130: paired by label, not position: cxx17 gets ITS baseline; split_map (no baseline) is not re-run
    assert shown == [("show", "b" * 40 + ":tools/cxx17_ratchet/baseline.json")]
    assert [n for n, _b in ran] == ["cxx17_ratchet.py"] and all(body == '{"base": 1}' for _n, body in ran)
    assert len(failed) == 1 and failed[0][0].startswith("cxx17 vs accepted base")


def _bbc(tmp_path, tools):
    return lm.lane_merge_baseline.base_baseline_check(str(tmp_path), str(tools), "b" * 40, lm.RATCHETS, lm.RATCHET_BASELINES, lm.run, lm.git, lm.lines)


def test_base_baseline_check_skips_a_baseline_absent_at_the_base(tmp_path, monkeypatch):
    tools = tmp_path / "tools"
    (tools).mkdir()
    monkeypatch.setattr(lm, "git", lambda root, *a: subprocess.CompletedProcess(a, 128, "missing"))
    monkeypatch.setattr(lm, "run", lambda *a, **k: pytest.fail("ran a ratchet with no base baseline"))
    assert _bbc(tmp_path, tools) == []


def test_promote_requires_the_day_night_evidence_a_capture_verdict_names(env, monkeypatch):
    """p31: promotion read only JSON; a window that required compare-captures landed without its viewable images."""
    lm, repo, old, cand = _stage_fixture(env, monkeypatch)
    pulled = repo.parent / "pulled"
    pulled.mkdir()
    vpath = pulled / "gate-verdict.json"
    _sim_evidence(pulled, cand)
    vpath.write_text(json.dumps(dict(schema=1, head=cand, verdict="PASS", complete=True, skipped={},
                                     required=["capture-before", "compare-captures"],
                                     artifacts={"proof": "G:/laptop-only/proof"})), encoding="utf-8")
    with pytest.raises(lm.Stop) as e:
        lm.promote(str(repo), "t1", str(vpath))
    assert "no capture evidence" in str(e.value) and git(repo, "rev-parse", "main") == old
    ev = pulled / "evidence"
    for b in ("mc2_01_saved_205644", "mc2_10_saved_204523"):
        for rel in (f"before/{b}/{b}.png", f"after/{b}/{b}.png", f"ab/{b}/montage.jpg"):
            (ev / rel).parent.mkdir(parents=True, exist_ok=True)
            (ev / rel).write_bytes(b"x")
    (ev / "ab/mc2_10_saved_204523/montage.jpg").write_bytes(b"")
    with pytest.raises(lm.Stop) as e:
        lm.promote(str(repo), "t1", str(vpath))
    assert "ab" in str(e.value) and "montage.jpg" in str(e.value) and git(repo, "rev-parse", "main") == old
    (ev / "ab/mc2_10_saved_204523/montage.jpg").write_bytes(b"x")
    assert lm.promote(str(repo), "t1", str(vpath)) == 0 and git(repo, "rev-parse", "main") == cand


@pytest.mark.parametrize("no_sim", [False, True])
def test_precheck_records_sim_requirement_without_running_games(env, monkeypatch, tmp_path, no_sim):
    repo, _ = env
    lm, sha, install = sim_tip(repo, tmp_path, monkeypatch)
    assert lm.precheck(str(repo), "lane_s", sim_rows=not no_sim, sim_install=install,
                       sim_runner=lambda *a, **k: pytest.fail("precheck ran games")) == 0
    rec = json.loads((tmp_path / "receipts" / (sha + ".json")).read_text())
    assert rec["sim_rows"] == {"required": True}
    assert lm.missing_receipts(str(repo), ["lane_s"]) == []
