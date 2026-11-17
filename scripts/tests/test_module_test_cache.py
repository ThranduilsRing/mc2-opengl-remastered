"""module_test_cache.py + test_audit_site/sitecustomize.py: inputs are recorded at run time and a hit needs all of them."""
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import console_child  # noqa: E402
import module_test_cache as mtc  # noqa: E402
import pytest  # noqa: E402


def test_timed_out_module_is_not_retried(monkeypatch):
    import run_tools_tests as runner
    calls = []
    def timed_out(module, env):
        calls.append(module)
        return module, 124, [], "timed out"
    monkeypatch.setattr(runner, "_run_module_once", timed_out)
    assert runner.run_module("test_hang.py")[1] == 124
    assert calls == ["test_hang.py"]


@pytest.mark.parametrize("summary,rc,expected,calls", [
    ("SKIPPED: stock mesh dump not available\n1 skipped in 0.01s", 5, 0, 1),
    ("no tests ran in 0.01s", 5, 0, 2),
    ("ERROR test_fixture.py - missing dependency\n1 skipped, 1 error", 2, 2, 1),
    ("FAILED test_fixture.py::test_bad\n1 failed, 1 skipped", 1, 1, 1),
])
def test_tools_module_skip_is_not_a_script(monkeypatch, summary, rc, expected, calls):
    import run_tools_tests as runner
    from types import SimpleNamespace
    commands = []
    def run(command, **kwargs):
        commands.append(command)
        return SimpleNamespace(stdout=summary if len(commands) == 1 else "", stderr="",
                               returncode=rc if len(commands) == 1 else 0)
    monkeypatch.setattr(runner.subprocess, "run", run)
    result = runner._run_module_once("test_fixture.py")
    assert result[1] == expected
    assert len(commands) == calls


@pytest.mark.parametrize("tail,counts", [
    ("1 skipped in 0.01s", "0 fully green, 1 skipped"),
    ("2 passed, 1 skipped in 0.01s", "1 fully green, 0 skipped"),
])
def test_tools_summary_counts_skips(monkeypatch, capsys, tail, counts):
    import run_tools_tests as runner
    monkeypatch.setattr(sys, "argv", ["run_tools_tests.py", "--suite", "art"])
    monkeypatch.setattr(runner, "discover", lambda suite: ["test_fixture.py"])
    monkeypatch.setattr(runner, "load_ledger", lambda: {})
    monkeypatch.setattr(runner, "run_all", lambda *a: [("test_fixture.py", 0, [], tail)])
    assert runner.main() == 0
    assert counts in capsys.readouterr().out


def test_scripts_timeout_reports_without_diagnostic_rerun(monkeypatch, capsys):
    import run_scripts_tests as runner
    monkeypatch.setattr(sys, "argv", ["run_scripts_tests.py"])
    monkeypatch.setattr(runner, "discover", lambda: ["test_hang.py"])
    monkeypatch.setattr(runner, "load_ownership", lambda: {"aggregate_owner": "scripts_tests", "tests": []})
    monkeypatch.setattr(runner.rtt, "run_all", lambda *a: [("test_hang.py", 124, [], "timed out after 600s")])
    def unexpected(*a, **k):
        pytest.fail("a timed-out module must not be run again for diagnostics")
    monkeypatch.setattr(runner.subprocess, "run", unexpected)
    assert runner.main() == 1
    assert "timed out after 600s" in capsys.readouterr().out


@pytest.fixture(autouse=True)
def _private_cache_dir(tmp_path, monkeypatch):
    """The cache file is shared across worktrees by default; these tests must not read or write the real one."""
    monkeypatch.setenv("MC2_TEST_CACHE_DIR", str(tmp_path / "cache-dir"))
    monkeypatch.delenv("MC2_GATE_BASE", raising=False)  # these runner fixtures exercise full-suite summaries


def test_gate_base_only_controls_outer_selection(monkeypatch):
    import run_tools_tests as runner
    monkeypatch.setenv("MC2_GATE_BASE", "window-base")
    seen = []
    monkeypatch.setattr(runner, "run_module", lambda mod, env: seen.append(env) or (mod, 0, [], "1 passed"))
    runner.run_all(["fixture.py"], 1, "test", True, False)
    assert "MC2_GATE_BASE" not in seen[0]
    assert os.environ["MC2_GATE_BASE"] == "window-base"


def _git(repo, *a):
    subprocess.run(["git", "-C", str(repo), *a], check=True, capture_output=True,
                   **console_child.child_kwargs(redirected=True))


def _repo(tmp_path):
    _git(tmp_path, "init", "-q")
    _git(tmp_path, "config", "user.email", "t@t")
    _git(tmp_path, "config", "user.name", "t")
    (tmp_path / "data").mkdir()
    (tmp_path / "data" / "in.txt").write_text("one\n")
    (tmp_path / "prog.py").write_text(
        "import os, sys\n"
        "print(open('data/in.txt').read())\n"
        "print(sorted(os.listdir('data')))\n"
        "open('out.txt', 'w').write('x')\n")
    _git(tmp_path, "add", "-A")
    _git(tmp_path, "commit", "-qm", "init")
    return tmp_path


def _run(cache, repo):
    env, out = cache.new_audit()
    r = subprocess.run([sys.executable, "-B", "prog.py"], cwd=str(repo), env=env, capture_output=True, text=True,
                       **console_child.child_kwargs(redirected=True))
    assert r.returncode == 0, r.stderr
    return cache.finish("m", "cmd", out, True, 0.1, None)


def test_records_reads_listings_and_ignores_outputs(tmp_path, monkeypatch):
    # The outer --no-cache board must not disable the cache under test.
    monkeypatch.delenv("CONTRACTS_NO_CACHE", raising=False)
    repo = _repo(tmp_path)
    cache = mtc.ModuleTestCache(str(repo), "t")
    assert not cache.lookup("m", "cmd")            # nothing stored yet
    _run(cache, repo)
    e = cache.entries["m"]
    assert "data/in.txt" in e["reads"] and "prog.py" in e["reads"]
    assert "out.txt" not in e["reads"]              # opened for writing first: an output, not an input
    assert "data" in e["dirs"]
    assert cache.lookup("m", "cmd")


def test_hit_needs_every_recorded_input(tmp_path, monkeypatch):
    monkeypatch.delenv("CONTRACTS_NO_CACHE", raising=False)
    repo = _repo(tmp_path)
    cache = mtc.ModuleTestCache(str(repo), "t")
    _run(cache, repo)
    assert cache.lookup("m", "cmd")
    assert not cache.lookup("m", "other cmd")                       # command is part of the key
    # Force the Windows last-write-time alias deterministically, without sleeps.
    stamp = (repo / "data" / "in.txt").stat()
    (repo / "data" / "in.txt").write_text("two\n")                  # a file the module read
    os.utime(repo / "data" / "in.txt", ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
    assert not cache.lookup("m", "cmd")
    (repo / "data" / "in.txt").write_text("one\n")
    os.utime(repo / "data" / "in.txt", ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
    assert cache.lookup("m", "cmd")
    (repo / "data" / "extra.txt").write_text("new\n")               # a new file in a directory it listed
    assert not cache.lookup("m", "cmd")
    (repo / "data" / "extra.txt").unlink()
    assert cache.lookup("m", "cmd")


def test_classify_child_processes(tmp_path):
    repo = _repo(tmp_path)
    cache = mtc.ModuleTestCache(str(repo), "t")
    root = str(repo)
    other = str(tmp_path.parent)
    assert cache.classify({"toks": ["git", "status"], "cwd": other})[0] == "foreign"
    assert cache.classify({"toks": ["git", "-C", other, "status"], "cwd": root})[0] == "foreign"
    assert cache.classify({"toks": ["git", "ls-files", "-z"], "cwd": root})[0] == "list"
    assert cache.classify({"toks": ["git", "rev-parse", "HEAD"], "cwd": root})[0] == "head"
    assert cache.classify({"toks": ["git", "status", "--porcelain"], "cwd": root})[0] == "tree"
    assert cache.classify({"toks": ["sh", "x.sh"], "cwd": root})[0] == "tree"
    assert cache.classify({"toks": ["glslc", "a.frag"], "cwd": root})[0] == "tool"


def test_off_switch_disables_cache(tmp_path, monkeypatch):
    monkeypatch.setenv("CONTRACTS_NO_CACHE", "1")
    repo = _repo(tmp_path)
    cache = mtc.ModuleTestCache(str(repo), "t")
    _run(cache, repo)
    assert not cache.enabled
    assert not cache.entries
    assert not cache.lookup("m", "cmd")
