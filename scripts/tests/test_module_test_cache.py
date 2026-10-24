"""module_test_cache.py + test_audit_site/sitecustomize.py: inputs are recorded at run time and a hit needs all of them."""
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import console_child  # noqa: E402
import module_test_cache as mtc  # noqa: E402


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
