"""tooling-speed-2a: lane_merge --pre-gate runs the real regenerate-all and lists the changed files (fixture repo, no engine)."""
import os
import subprocess
import sys
import types

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(ROOT, "scripts"))

from tools import lane_merge as lm  # noqa: E402


def _git(repo, *args):
    subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t", *args], cwd=repo, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def _fixture(tmp_path, monkeypatch):
    repo = tmp_path / "repo"
    (repo / "tools").mkdir(parents=True)
    (repo / "out").mkdir()
    gen = repo / "tools" / "gen.py"
    gen.write_text("import pathlib\npathlib.Path('out/artifact.json').write_text('{\"v\": 2}\\n')\n")
    (repo / "out" / "artifact.json").write_text('{"v": 1}\n')
    subprocess.run(["git", "init", "-q"], cwd=repo, check=True)
    _git(repo, "add", "-A")
    _git(repo, "commit", "-q", "-m", "base")
    monkeypatch.setattr(lm, "GENERATORS", [("fixture", "tools/gen.py", [])])
    monkeypatch.setattr(lm, "ARTIFACTS", ["out/artifact.json"])
    return repo


def test_pre_gate_regen_lists_changed_files_and_leaves_them_for_one_commit(tmp_path, monkeypatch, capsys):
    repo = _fixture(tmp_path, monkeypatch)
    changed, fail = lm.pre_gate_regen(str(repo))
    assert fail is None
    assert changed == ["out/artifact.json"]
    out = capsys.readouterr().out
    assert "regenerate-all changed 1 path(s)" in out and "  out/artifact.json" in out and "git add -A -- out/artifact.json" in out
    assert (repo / "out" / "artifact.json").read_text() == '{"v": 2}\n'   # written into the tree, uncommitted


def test_pre_gate_regen_rerun_with_uncommitted_regen_output_fails_with_the_list(tmp_path, monkeypatch):
    repo = _fixture(tmp_path, monkeypatch)
    lm.pre_gate_regen(str(repo))
    changed, fail = lm.pre_gate_regen(str(repo))
    assert changed == ["out/artifact.json"] and "commit them" in fail
    _git(repo, "add", "-A")
    _git(repo, "commit", "-q", "-m", "regen")
    assert lm.pre_gate_regen(str(repo)) == ([], None)   # nothing left to change once committed


def test_pre_gate_prints_regen_and_fails_the_verdict_when_regenerate_all_stops(tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(lm, "pre_gate_regen", lambda root: ([], "generator exploded"))
    monkeypatch.setattr(lm, "run", lambda r, cmd, env=None: types.SimpleNamespace(returncode=0, stdout=""))
    monkeypatch.setattr(lm, "data_fingerprint_check", lambda r: ("OK", ""))
    assert lm.pre_gate(str(tmp_path), str(tmp_path / "none.exe")) == 1
    out = capsys.readouterr().out
    assert "pre-gate: regenerate-all: FAIL" in out and "generator exploded" in out
    assert out.strip().splitlines()[-1] == "PRE-GATE FAIL: regenerate-all"
