import json
import pytest
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import nightly_gate  # noqa: E402
import lane_merge_stage  # noqa: E402


def test_refuses_only_when_the_nightly_worktree_is_dirty(monkeypatch, tmp_path):
    """The gate runs in its own worktree: mc2-main's branch and dirty state never refuse the night."""
    monkeypatch.setattr(nightly_gate, "ROOT", str(tmp_path))
    monkeypatch.setattr(nightly_gate, "git", lambda *a, cwd=None: " M x" if cwd == str(tmp_path) else "other")
    assert "nightly worktree" in nightly_gate.refusal()
    monkeypatch.setattr(nightly_gate, "git", lambda *a, cwd=None: "" if cwd == str(tmp_path) else "other or dirty")
    assert nightly_gate.refusal() is None


def test_plan_is_non_none_with_mc2_main_on_another_branch(monkeypatch):
    import datetime

    def fake_git(*a, cwd=None):
        if a[:2] == ("tag", "-l"):
            return "nifty-green-137-1d968ffd3\nnifty-green-138-aaaaaaaaa"
        if a[0] == "rev-parse":
            return "b" * 40
        if a[0] == "branch":
            return "claude/stage-139"   # mc2-main sits on a stage branch
        if a[0] == "status":
            return " M dirty-in-main"
        return "merge fix W138: x"
    monkeypatch.setattr(nightly_gate, "git", fake_git)
    plan, why = nightly_gate.make_plan(False, datetime.date(2026, 11, 10))
    assert plan and why is None and plan["base"] == "b" * 40 and plan["window"] == 138
    assert plan["cmd"][-len(plan["names"]):] == plan["names"]
    assert nightly_gate.ROOT == nightly_gate.NIGHTLY_ROOT != nightly_gate.MAIN_ROOT


def _base_for(monkeypatch, tags):
    shas = {t: t.rsplit("-", 1)[1] * 4 for t in tags}
    monkeypatch.setattr(nightly_gate, "git", lambda *a, cwd=None: "\n".join(tags) if a[:2] == ("tag", "-l")
                        else shas.get(a[-1][:-len("^{commit}")], "") if a[0] == "rev-parse" else "")
    return shas


def test_lettered_window_tags_order_by_number_then_suffix(monkeypatch):
    tags = ["nifty-green-137-1111", "nifty-green-138-2222", "nifty-green-138b-3333"]
    shas = _base_for(monkeypatch, tags)
    assert nightly_gate.nightly_base() == shas["nifty-green-138-2222"]   # newest is 138b, so the base is 138
    tags = ["nifty-green-138b-3333", "nifty-green-138c-4444"]
    shas = _base_for(monkeypatch, tags)
    assert nightly_gate.nightly_base() == shas["nifty-green-138b-3333"]


def test_built_argv_parses_under_merge_gates_own_parser(monkeypatch):
    import datetime
    import merge_gate
    monkeypatch.setattr(nightly_gate, "git", lambda *a, cwd=None: "nifty-green-137-1111\nnifty-green-138-2222" if a[:2] == ("tag", "-l")
                        else "c" * 40 if a[0] == "rev-parse" else "merge W138: x")
    plan, why = nightly_gate.make_plan(False, datetime.date(2026, 11, 10))
    assert why is None
    tail = plan["cmd"][plan["cmd"].index(next(c for c in plan["cmd"] if c.endswith("merge_gate.py"))) + 1:]
    args = merge_gate.build_parser().parse_args(tail)
    assert args.nightly and args.isolated_verdict and args.base == "c" * 40 and args.window == 138 and args.stages == plan["names"]


def test_a_held_host_lease_runs_the_cpu_rows_only():
    from smoke_lib import host_lease

    def busy(label, timeout_s=None):
        raise host_lease.HostLeaseBusy("held")
    assert nightly_gate.wait_for_lease(5, acquire=busy, release=lambda: None) is False
    got = []
    assert nightly_gate.wait_for_lease(5, acquire=lambda label, timeout_s=None: got.append(timeout_s), release=lambda: got.append("rel")) is True
    assert got == [5, "rel"]
    cpu, game = nightly_gate.cpu_only(["deploy", "player-path", "determinism-mc2_01", "contracts"])
    assert cpu == ["deploy", "contracts"] and game == ["player-path", "determinism-mc2_01"]
    assert nightly_gate.LEASE_WAIT_S == 3600


def test_merge_gate_and_nightly_agree_on_the_worktree():
    import merge_gate
    from pathlib import Path
    assert merge_gate.NIGHTLY_ROOT == Path(nightly_gate.NIGHTLY_ROOT)


def test_stage_selection_is_tier1_plus_deploy():
    names = nightly_gate.stage_names(False)
    assert "deploy" in names and "player-path" in names and "mods" not in names
    assert "mods" in nightly_gate.stage_names(True)


def test_promote_refuses_untriaged_nightly_fail(tmp_path):
    d = tmp_path / "nightly-2026-11-06"
    d.mkdir()
    (d / "gate-verdict.json").write_text(json.dumps({"verdict": "FAIL"}))
    assert lane_merge_stage.untriaged_nightly_fail(str(tmp_path))
    (d / "triage.md").write_text("known")
    assert lane_merge_stage.untriaged_nightly_fail(str(tmp_path)) == []
    e = tmp_path / "nightly-2026-11-07"
    e.mkdir()
    (e / "gate-verdict.json").write_text(json.dumps({"verdict": "PASS"}))
    (d / "triage.md").unlink()
    assert lane_merge_stage.untriaged_nightly_fail(str(tmp_path)) == []


def _verdict(tmp_path, name, **kw):
    d = tmp_path / name
    d.mkdir()
    (d / "gate-verdict.json").write_text(json.dumps(kw))
    return d


def test_promote_nightly_fail_closed(tmp_path):
    import time
    now = time.time()
    f = lane_merge_stage.untriaged_nightly_fail
    assert f(str(tmp_path))  # no nightly at all
    _verdict(tmp_path, "nightly-2026-11-07", verdict="PASS", failed=[], written=now - 3600)
    assert f(str(tmp_path)) == []
    assert f(str(tmp_path), now=now + 40 * 3600)  # stale
    partial = _verdict(tmp_path, "nightly-2026-11-08", verdict="PARTIAL", failed=[], written=now, tier=1,
                       executed={n: {"status": "PASS"} for n in nightly_gate.stage_names(False)})
    assert f(str(tmp_path)) == []  # stage-filter partial, every required row of its tier passed
    (partial / "gate-verdict.json").write_text(json.dumps(dict(verdict="INFRA", failed=[], written=now)))
    assert f(str(tmp_path))
    (partial / "gate-verdict.json").write_text(json.dumps(dict(verdict="PARTIAL", failed=[dict(name="x", status="FAIL")], written=now)))
    assert f(str(tmp_path))


def test_nightly_child_uses_g_tmp_and_0430():
    assert nightly_gate.TMP_DIR == "G:/mc2-tmp"
    xml = open(os.path.join(ROOT, "scripts", "nightly_gate.task.xml"), encoding="utf-8").read()
    assert "T04:30:00" in xml


def test_nightly_child_never_writes_the_window_verdict():
    import merge_gate
    src = open(os.path.join(ROOT, "scripts", "nightly_gate.py"), encoding="utf-8").read()
    assert '"--isolated-verdict"' in src
    assert "--isolated-verdict" in open(merge_gate.__file__, encoding="utf-8").read()


def test_publish_refuses_exe_the_gate_did_not_test(tmp_path):
    import deploy_payload
    (tmp_path / "build64-ninja").mkdir()
    exe = tmp_path / "mc2.exe"
    exe.write_bytes(b"gated")
    sha = deploy_payload.sha256_file(str(exe))
    vf = tmp_path / "build64-ninja" / "gate-verdict.json"
    vf.write_text(json.dumps(dict(verdict="PASS", head="abc", artifacts=dict(exe_sha256=sha))))
    f = deploy_payload.verdict_exe_refusal
    assert f(str(tmp_path), str(exe), "abc") is None
    assert f(str(tmp_path), str(exe), "def")          # other HEAD
    exe.write_bytes(b"rebuilt")
    assert "rebuilt" in f(str(tmp_path), str(exe), "abc")
    vf.write_text(json.dumps(dict(verdict="FAIL", head="abc", artifacts=dict(exe_sha256=sha))))
    assert f(str(tmp_path), str(exe), "abc")


def test_promote_refuses_a_partial_that_skipped_a_required_nightly_stage(tmp_path):
    import time
    f = lane_merge_stage.untriaged_nightly_fail
    ran = {n: {"status": "PASS"} for n in nightly_gate.stage_names(False)}
    d = _verdict(tmp_path, "nightly-2026-11-09", verdict="PARTIAL", failed=[], written=time.time(), tier=1, executed=ran)
    assert f(str(tmp_path)) == []
    del ran["player-path"]
    (d / "gate-verdict.json").write_text(json.dumps(dict(verdict="PARTIAL", failed=[], written=time.time(), tier=1, executed=ran)))
    assert any("player-path" in w for w in f(str(tmp_path)))
    (d / "gate-verdict.json").write_text(json.dumps(dict(verdict="PARTIAL", failed=[], written=time.time())))
    assert f(str(tmp_path)) == []   # legacy verdicts without a tier retain pre-p31 acceptance
    (d / "gate-verdict.json").write_text(json.dumps(dict(verdict="PARTIAL", failed=[], written=time.time(), tier=0)))
    assert f(str(tmp_path))   # a modern verdict explicitly recording no nightly tier is refused


def test_nightly_passes_the_window_and_weekly_runs_without_one(monkeypatch, capsys):
    monkeypatch.setattr(nightly_gate, "refusal", lambda: None)
    monkeypatch.setattr(nightly_gate, "nightly_base", lambda: "b" * 40)
    monkeypatch.setattr(nightly_gate, "git", lambda *a: "merge w128: x\nmerge follow-up w127: y\nunrelated")
    assert nightly_gate.current_window() == 128
    assert nightly_gate.main(["--dry-run", "--weekly"]) == 0
    assert "laptop_gate.py nightly -- --weekly" in capsys.readouterr().out
    monkeypatch.setattr(nightly_gate, "git", lambda *a: "no window here")
    assert nightly_gate.main(["--dry-run", "--weekly"]) == 0   # mod_smoke no longer needs a window number
    assert "--window" not in capsys.readouterr().out.split("--stages", 1)[0]


def test_window_regex_reads_uppercase_and_lettered_subjects(monkeypatch):
    """nifty writes 'merge fix W137:' and 'W138a' (meta-1417 N1: the lowercase-only regex dropped mods-slim every night)."""
    monkeypatch.setattr(nightly_gate, "git", lambda *a: "merge fix W137: regen\nmerge W138a: lane\nfixw999 not a window")
    assert nightly_gate.current_window() == 138


def test_nightly_passes_the_previous_green_window_as_base(monkeypatch, capsys, tmp_path):
    """merge_gate exits in 1 s without --base on the published nifty (meta-1417 N1). Every flag of the built argv exists in
    merge_gate's parser."""
    import re
    base = "b26fc9bfe" + "0" * 31
    tags = ["nifty-green-136-b26fc9bfe\nnifty-green-137-1d968ffd3\nnifty-green-x-junk"]

    def fake_git(*a):
        if a[:2] == ("tag", "-l"):
            return tags[0]
        if a[0] == "rev-parse":
            return base if a[-1] == "nifty-green-136-b26fc9bfe^{commit}" else ""
        return "merge fix W137: x"
    monkeypatch.setattr(nightly_gate, "refusal", lambda: None)
    monkeypatch.setattr(nightly_gate, "PROOF_ROOT", str(tmp_path))
    monkeypatch.setattr(nightly_gate, "git", fake_git)
    assert nightly_gate.nightly_base() == base
    assert nightly_gate.main(["--dry-run"]) == 0
    plan, why = nightly_gate.make_plan(False, __import__('datetime').date(2026, 11, 12))
    assert why is None
    argv = plan["cmd"][plan["cmd"].index(next(c for c in plan["cmd"] if c.endswith("merge_gate.py"))) + 1:]
    assert argv[argv.index("--base") + 1] == base and argv[argv.index("--window") + 1] == "137"
    with open(os.path.join(ROOT, "scripts", "merge_gate.py"), encoding="utf-8") as fh:
        declared = set(re.findall(r'add_argument\("(--[a-z0-9-]+)"', fh.read()))
    assert {a for a in argv if a.startswith("--")} <= declared
    tags[0] = "nifty-green-137-1d968ffd3"
    assert nightly_gate.nightly_base() is None
    assert nightly_gate.make_plan(False, __import__('datetime').date(2026, 11, 12))[0] is None


def _stage_verdict(stages, bad=(), verdict="PARTIAL"):
    return dict(verdict=verdict, complete=True, failed=list(bad), tier=1, written=1.0,
                executed={n: {"status": "FAIL" if n in bad else "PASS"} for n in stages})


def test_scheduled_run_partial_verdict_still_issues_per_stage_receipts_and_reuses_them(tmp_path, monkeypatch):
    # p31 item 4: a --stages run ends PARTIAL by design; receipts follow each stage's own PASS.
    import time
    monkeypatch.setattr(nightly_gate, "git", lambda *a: "tree-A")
    stages = nightly_gate.stage_names(False)
    assert len(stages) >= 2
    old = tmp_path / "nightly-2026-11-06"
    old.mkdir()
    (old / "gate-verdict.json").write_text(json.dumps(_stage_verdict(stages, bad=[stages[0]], verdict="FAIL")))
    assert nightly_gate.write_receipts(str(old), False, stages) == []   # a verdict with failed rows issues nothing
    (old / "gate-verdict.json").write_text(json.dumps(_stage_verdict(stages)))
    assert nightly_gate.write_receipts(str(old), False, stages) == stages
    assert nightly_gate.find_stage_receipts(str(tmp_path), False, stages) == {n: str(old) for n in stages}
    assert nightly_gate.find_stage_receipts(str(tmp_path), True, stages) == {}   # weekly is a different key
    monkeypatch.setattr(nightly_gate, "git", lambda *a: "tree-B")
    assert nightly_gate.find_stage_receipts(str(tmp_path), False, stages) == {}   # tree changed
    monkeypatch.setattr(nightly_gate, "git", lambda *a: "tree-A")
    assert nightly_gate.find_stage_receipts(str(tmp_path), False, stages, now=time.time() + 9 * 86400) == {}   # stale
    new = tmp_path / "nightly-2026-11-07"
    new.mkdir()
    found = nightly_gate.find_stage_receipts(str(tmp_path), False, stages)
    nightly_gate.reuse_receipts(found, str(new), False, stages)
    v = json.loads((new / "gate-verdict.json").read_text())
    assert v["written"] > 1.0 and v["failed"] == [] and all(v["executed"][n]["status"].startswith("REUSED:") for n in stages)
    assert lane_merge_stage.untriaged_nightly_fail(str(tmp_path)) == []
    assert lane_merge_stage.nightly_unmet(v) == []


def test_only_the_green_stages_get_a_receipt_when_one_stage_is_skipped(tmp_path, monkeypatch):
    monkeypatch.setattr(nightly_gate, "git", lambda *a: "tree-A")
    stages = nightly_gate.stage_names(False)
    ex = {n: {"status": "PASS"} for n in stages}
    ex[stages[0]] = {"status": "SKIP: x"}
    (tmp_path / "gate-verdict.json").write_text(json.dumps(dict(verdict="PARTIAL", failed=[], tier=1, executed=ex)))
    assert nightly_gate.write_receipts(str(tmp_path), False, stages) == stages[1:]
    (tmp_path / "gate-verdict.json").write_text("{}")
    (tmp_path / "receipt.json").unlink()
    assert nightly_gate.write_receipts(str(tmp_path), False, stages) == [] and not (tmp_path / "receipt.json").exists()


@pytest.mark.skipif(os.name != "nt", reason="Windows Task Scheduler launcher registration")
def test_task_xml_template_resolves_the_launcher_at_registration(tmp_path):
    import nightly_gate_task as t
    raw = open(os.path.join(ROOT, "scripts", "nightly_gate.task.xml"), encoding="utf-8").read()
    assert "<Command>py</Command>" not in raw and "<Command>@PY@</Command>" in raw   # bare `py` is not on the task's PATH
    out = t.render(raw, r"C:\Windows\py.exe")
    assert "<Command>C:\Windows\py.exe</Command>" in out and "<Arguments>-3 A:" in out and "@PY@" not in out
    assert "<Arguments>A:" in t.render(raw, r"C:\Py\python.exe")   # python.exe takes no -3
    which = lambda n: {"py": r"C:\L\py.exe"}.get(n)   # noqa: E731
    assert t.resolve_launcher(which) == os.path.normpath(r"C:\L\py.exe")
    with pytest.raises(SystemExit):
        t.resolve_launcher(lambda n: None)
    dest = tmp_path / "x.xml"
    t.main(["--out", str(dest), "--py", os.path.abspath(sys.executable)])
    assert "@PY@" not in dest.read_text(encoding="utf-16")
