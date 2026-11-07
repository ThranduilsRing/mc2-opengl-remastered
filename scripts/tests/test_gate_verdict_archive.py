import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import merge_gate  # noqa: E402


def test_resume_archives_each_prior_verdict(tmp_path):
    assert merge_gate.archive_prior_verdict(tmp_path) is None
    for run in (1, 2, 3):
        (tmp_path / "gate-verdict.json").write_text(json.dumps({"run": run}))
        merge_gate.archive_prior_verdict(tmp_path)
    assert not (tmp_path / "gate-verdict.json").exists()
    assert [json.loads((tmp_path / f"gate-verdict-{n}.json").read_text())["run"] for n in (1, 2, 3)] == [1, 2, 3]


def test_gate_deploy_step_is_full_verify():
    text = open(os.path.join(ROOT, "scripts", "merge_gate.py"), encoding="utf-8").read()
    assert '"--require-clean", "--full-verify"]' in text
