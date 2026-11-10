"""Scratch lifecycle and packaging guard, using an isolated mod tree."""
import json
from pathlib import Path
from unittest.mock import patch

import pytest

from tools.mod_scratch import main, run
from tools.mod_install.resolver import ResolverConfig, resolve
from tools.mc2mod.mc2mod import main as pack_main, pack_mod


@pytest.fixture
def mod(tmp_path):
    root = tmp_path / "mods" / "project"
    root.mkdir(parents=True)
    (root / "mod.json").write_text(json.dumps({"id": "project", "name": "Project", "version": "1.0.0",
                                              "dependencies": ["dep"]}))
    return root


def put(mod, name, content):
    path = mod / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content)
    return path


def test_diff_and_promote(mod, capsys):
    put(mod, "data/art/changed.txt", "old")
    put(mod, "data/art/same.txt", "same")
    for name, content in (("changed", "new content"), ("same", "same"), ("new", "new")):
        put(mod, f".scratch/data/art/{name}.txt", content)
    run("diff", mod)
    assert capsys.readouterr().out.splitlines() == ["changed data/art/changed.txt", "new data/art/new.txt",
                                                  "same data/art/same.txt"]
    data = mod / "data"
    import os
    os.utime(data, (1, 1))
    run("promote", mod)
    assert (data / "art/changed.txt").read_text() == "new content"
    assert (data / "art/new.txt").read_text() == "new"
    assert data.stat().st_mtime > 1
    assert not (mod / ".scratch").exists()
    result = resolve("data/art/changed.txt", ResolverConfig(str(mod.parent.parent), active_mod="project"))
    assert result["layer"] == "mod:project"
    assert Path(result["path"]).read_text() == "new content"


def test_conflict_refuses_before_any_move_and_force_promotes(mod):
    put(mod, ".scratch/data/a.txt", "a")
    put(mod, ".scratch/data/z.txt", "scratch")
    put(mod.parent / "dep", "data/z.txt", "dependency")
    with pytest.raises(ValueError, match=r"\[mod-dup\]"):
        run("promote", mod)
    assert (mod / ".scratch/data/a.txt").exists()
    assert not (mod / "data").exists()
    run("promote", mod, force=True)
    assert (mod / "data/z.txt").read_text() == "scratch"
    assert (mod.parent / "dep/data/z.txt").read_text() == "dependency"


def test_case_normalized_promotion(mod):
    target = put(mod, "data/Art/Thing.TXT", "old")
    put(mod, ".scratch/data/art/thing.txt", "new")
    run("promote", mod)
    assert target.read_text() == "new"


@pytest.mark.parametrize("switch", ["0", "00", "0disabled"])
def test_scratch_kill_switch(mod, switch):
    put(mod, "data/test.txt", "project")
    put(mod, ".scratch/data/test.txt", "scratch")
    with patch.dict("os.environ", {"MC2_SCRATCH": switch}):
        result = resolve("data/test.txt", ResolverConfig(str(mod.parent.parent), active_mod="project"))
    assert result["layer"] == "mod:project"


def test_hidden_entries_stay_unpromoted(mod):
    hidden = put(mod, ".scratch/data/.hidden/file.txt", "hidden")
    put(mod, ".scratch/data/visible.txt", "visible")
    run("promote", mod)
    assert hidden.exists()
    assert (mod / "data/visible.txt").exists()


def test_nested_junction_in_scratch_is_refused(mod, tmp_path):
    import subprocess
    outside = tmp_path / "outside"
    (outside / "data").mkdir(parents=True)
    (outside / "data" / "stolen.txt").write_text("outside")
    link = mod / ".scratch" / "data" / "linked"
    link.parent.mkdir(parents=True)
    import sys
    sys.path.append(str(Path(__file__).resolve().parents[2] / "scripts"))
    import console_child
    r = subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(outside)],
                       capture_output=True, text=True, **console_child.child_kwargs(redirected=True))
    if r.returncode != 0:
        pytest.skip(f"cannot create junction: {r.stderr.strip() or r.stdout.strip()}")
    with pytest.raises(ValueError, match="scratch must not contain"):
        run("diff", mod)
    assert (outside / "data" / "stolen.txt").is_file()


def test_discard_and_cli_id(mod, monkeypatch):
    put(mod, ".scratch/data/test.txt", "scratch")
    project = put(mod, "data/test.txt", "project")
    monkeypatch.chdir(mod.parent.parent)
    assert main(["discard", "project"]) == 0
    assert main(["discard", "project"]) == 0
    assert not (mod / ".scratch").exists()
    assert project.read_text() == "project"


def test_packer_requires_scratch_resolution_even_if_empty(mod, tmp_path, capsys):
    (mod / ".scratch").mkdir()
    with pytest.raises(ValueError, match="mod_scratch.py promote/discard"):
        pack_mod(str(mod), tmp_path / "packages")
    assert pack_main(["pack", str(mod), "--out", str(tmp_path / "packages")]) == 1
    assert len(capsys.readouterr().err.splitlines()) == 1
    assert not (tmp_path / "packages").exists()
    run("discard", mod)
    assert pack_mod(str(mod), tmp_path / "packages").is_file()
