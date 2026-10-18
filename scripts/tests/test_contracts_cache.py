"""contracts_cache.py: a key must move when (and only when) a row's inputs move."""
import os
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import contracts_cache as cc  # noqa: E402


def _git(repo, *a):
    subprocess.run(["git", "-C", str(repo), *a], check=True, capture_output=True)


def _repo(tmp_path):
    _git(tmp_path, "init", "-q")
    _git(tmp_path, "config", "user.email", "t@t")
    _git(tmp_path, "config", "user.name", "t")
    (tmp_path / "scripts").mkdir()
    (tmp_path / "src").mkdir()
    (tmp_path / "other").mkdir()
    (tmp_path / "scripts" / "chk.py").write_text("import helper\nprint('x')\n")
    (tmp_path / "scripts" / "helper.py").write_text("X = 1\n")
    (tmp_path / "scripts" / "chk.baseline.json").write_text("{}")
    (tmp_path / "src" / "a.cpp").write_text("int a;\n")
    (tmp_path / "other" / "b.txt").write_text("b\n")
    (tmp_path / "scripts" / "contracts-cache-inputs.txt").write_text("rowa: src\n")
    _git(tmp_path, "add", "-A")
    _git(tmp_path, "commit", "-qm", "init")
    return tmp_path


def _key(repo, cmd="py -3 scripts/chk.py"):
    uni = cc.Universe(str(repo))
    return cc.compute_key(str(repo), uni, cmd, cc.load_decls(str(repo))["rowa"])


def test_key_follows_inputs_only(tmp_path):
    r = _repo(tmp_path)
    k0 = _key(r)
    assert _key(r) == k0
    (r / "other" / "b.txt").write_text("changed\n")        # not an input
    assert _key(r) == k0
    (r / "src" / "a.cpp").write_text("int a2;\n")           # declared scan root, unstaged edit
    k1 = _key(r)
    assert k1 != k0
    (r / "src" / "new.cpp").write_text("int n;\n")          # untracked file in the root
    assert _key(r) != k1
    (r / "src" / "new.cpp").unlink()
    (r / "src" / "a.cpp").write_text("int a;\n")
    assert _key(r) == k0                                    # back to the original bytes


def test_check_code_is_an_input(tmp_path):
    r = _repo(tmp_path)
    k0 = _key(r)
    (r / "scripts" / "helper.py").write_text("X = 2\n")             # imported module
    assert _key(r) != k0
    (r / "scripts" / "helper.py").write_text("X = 1\n")
    (r / "scripts" / "chk.baseline.json").write_text('{"a":1}')     # same-stem sibling baseline
    assert _key(r) != k0
    (r / "scripts" / "chk.baseline.json").write_text("{}")
    assert _key(r) == k0
    assert _key(r, "py -3 scripts/chk.py --other") != k0            # the command is part of the key


def test_deleted_input_and_undeclared_row(tmp_path):
    r = _repo(tmp_path)
    k0 = _key(r)
    os.remove(r / "src" / "a.cpp")
    assert _key(r) != k0
    assert "nope" not in cc.load_decls(str(r))      # an undeclared row has no declaration, so it is never keyed
