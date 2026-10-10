"""Tests for tools/phase/ph_split.py + ph_verify.py (the phase-helper refactor tools) on a throwaway git repo.

A split of a small free function must verify (the inline expansion of the helpers equals the original body), and a
tampered helper body must make the verifier fail.
"""
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
# OWNER RULE: no window for any child process.
from smoke_lib import nofocus  # noqa: E402

SOURCE = """// fixture
#include <cstdio>

static int g_total = 0;

void runAll(int count)
{
    int acc = 0;
    for (int i = 0; i < count; ++i)
        acc += i;
    std::printf("sum=%d\\n", acc);

    g_total += acc;
    std::printf("total=%d\\n", g_total);
}
"""

SPEC = """BASE = '{base}'
SRC = 'fixture.cpp'
FUNC_SIG = r'^void runAll\\('
STRUCTS = {{'f': 'RunFrame'}}
FRAME_METHODS = True
FRAME_GLOBAL = True
PARAMS = [('count', 'int')]
MEMBERS = [('acc', 'int', 'f', [8])]
EDITS = {{}}
ORCH_BEFORE = {{8: '    RunFrame f;'}}
FRAME_DOC = '// Locals the fixture phases share.'
ORCH_DOC = '// fixture orchestrator'
PHASES = [
    ('sumUp', 8, 11, 'Sum the counter.'),
    ('addTotal', 13, 14, 'Add to the running total.'),
]
"""


def _run(args, cwd, **kw):
    return subprocess.run(args, cwd=cwd, capture_output=True, text=True, encoding="utf-8",
                          **nofocus.hidden_kwargs(), **kw)


def _repo(tmp_path):
    def git(*a):
        r = _run(["git", "-c", "user.email=t@t", "-c", "user.name=t", *a], tmp_path)
        assert r.returncode == 0, r.stderr
        return r.stdout.strip()
    git("init", "-q")
    (tmp_path / "fixture.cpp").write_text(SOURCE, encoding="utf-8", newline="\n")
    git("add", "fixture.cpp")
    git("commit", "-q", "-m", "base")
    (tmp_path / "spec.py").write_text(SPEC.format(base=git("rev-parse", "HEAD")), encoding="utf-8")
    return tmp_path


def _tool(name):
    return str(ROOT / "tools" / "phase" / name)


def test_split_then_verify_passes_and_tampering_fails(tmp_path):
    repo = _repo(tmp_path)
    r = _run([sys.executable, _tool("ph_split.py"), "spec.py"], repo)
    assert r.returncode == 0, r.stdout + r.stderr
    text = (repo / "fixture.cpp").read_text(encoding="utf-8")
    assert "struct RunFrame" in text and "void RunFrame::sumUp(int count)" in text
    ok = _run([sys.executable, _tool("ph_verify.py"), "spec.py"], repo)
    assert ok.returncode == 0 and "PASS" in ok.stdout, ok.stdout + ok.stderr

    # a helper body that no longer matches the original must be caught
    (repo / "fixture.cpp").write_text(text.replace("acc += i;", "acc += i + 1;"), encoding="utf-8", newline="\n")
    bad = _run([sys.executable, _tool("ph_verify.py"), "spec.py"], repo)
    assert bad.returncode != 0 and "FAIL" in bad.stdout, bad.stdout + bad.stderr


def test_split_refuses_a_phase_that_cuts_a_block(tmp_path):
    repo = _repo(tmp_path)
    # lines 7-9 open the function body brace and stop inside it: not balanced, so nothing may be written
    spec = (repo / "spec.py").read_text(encoding="utf-8").replace("('sumUp', 8, 11,", "('sumUp', 7, 9,")
    (repo / "spec.py").write_text(spec, encoding="utf-8")
    r = _run([sys.executable, _tool("ph_split.py"), "spec.py"], repo)
    assert r.returncode != 0 and "not brace-balanced" in (r.stdout + r.stderr)
    assert (repo / "fixture.cpp").read_text(encoding="utf-8") == SOURCE
