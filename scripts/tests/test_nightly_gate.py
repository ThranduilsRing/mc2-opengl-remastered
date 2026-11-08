import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import nightly_gate  # noqa: E402
import lane_merge_stage  # noqa: E402


def test_refuses_off_nifty_or_dirty(monkeypatch):
    monkeypatch.setattr(nightly_gate, "git", lambda *a: "other" if a[0] == "branch" else "")
    assert "not on" in nightly_gate.refusal()
    monkeypatch.setattr(nightly_gate, "git", lambda *a: nightly_gate.NIFTY if a[0] == "branch" else " M x")
    assert nightly_gate.refusal() == "mc2-main is dirty"
    monkeypatch.setattr(nightly_gate, "git", lambda *a: nightly_gate.NIFTY if a[0] == "branch" else "")
    assert nightly_gate.refusal() is None


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
    assert f(str(tmp_path))   # no recorded tier


def test_nightly_passes_the_window_and_weekly_refuses_without_one(monkeypatch, capsys):
    monkeypatch.setattr(nightly_gate, "refusal", lambda: None)
    monkeypatch.setattr(nightly_gate, "git", lambda *a: "merge w128: x\nmerge follow-up w127: y\nunrelated")
    assert nightly_gate.current_window() == 128
    assert nightly_gate.main(["--dry-run", "--weekly"]) == 0
    assert "--window 128" in capsys.readouterr().out
    monkeypatch.setattr(nightly_gate, "git", lambda *a: "no window here")
    assert nightly_gate.main(["--dry-run", "--weekly"]) == 2


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
