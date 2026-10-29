"""P0/P2 tooling: integrity, retained swaps, L1 order and exact log comparison."""
import json
from pathlib import Path
import shutil
import sys
from unittest.mock import patch

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/mc2mod"))
from mc2mod import main, pack_mod
from mod_install.management import conflict_lines, swap_version
from validate_mod_package import file_hash, main as validate_main, validate


FIXTURE = Path(__file__).resolve().parents[1] / "fixtures/mods/minimal-mod"


def manifest(root):
    mod = json.loads((root / "mod.json").read_text())
    pkg = dict(schema="mc2-package/1", id=mod["id"], name=mod["name"],
               version=mod["version"], dependencies=[{"id": d, "version": "*"}
                                                   for d in mod.get("dependencies", [])], files=[])
    for path in sorted(root.rglob("*")):
        if path.is_file() and path.name != "package.json":
            pkg["files"].append(dict(path=path.relative_to(root).as_posix(),
                                     size=path.stat().st_size, sha256=file_hash(path)))
    (root / "package.json").write_text(json.dumps(pkg))
    return pkg


def test_fixture():
    assert validate(FIXTURE, True) == []


@pytest.mark.parametrize("mutation", [
    lambda p: p.update(schema="unknown"),
    lambda p: p.update(id="different"),
    lambda p: p.update(files=[None]),
    lambda p: p.update(dependencies=["bad"]),
    lambda p: p["files"][0].update(size=True),
    lambda p: p["files"][0].update(sha256="oops"),
    lambda p: p["files"].append(dict(p["files"][0], path=p["files"][0]["path"].upper())),
    lambda p: p["files"][0].update(path="../escape"),
    lambda p: p["files"][0].update(path="data/C:/escape"),
    lambda p: p["files"][0].update(path="data/.hidden"),
])
def test_shape_errors(tmp_path, mutation):
    root = tmp_path / "mod"
    shutil.copytree(FIXTURE, root)
    pkg = json.loads((root / "package.json").read_text())
    mutation(pkg)
    (root / "package.json").write_text(json.dumps(pkg))
    assert validate(root)


def test_hash_and_unlisted(tmp_path):
    shutil.copytree(FIXTURE, tmp_path / "mod")
    root = tmp_path / "mod"
    (root / "data/example.txt").write_text("edited")
    (root / "data/extra").write_text("extra")
    errors = validate(root, True)
    assert any("integrity" in e for e in errors)
    assert any("undeclared" in e for e in errors)


def test_legacy_inventory(tmp_path, capsys):
    (tmp_path / "data").mkdir()
    assert validate_main([str(tmp_path), "--legacy-install"]) == 0
    assert "LEGACY" in capsys.readouterr().out


def test_pack_dependency_roundtrip(tmp_path):
    source = tmp_path / "project"
    shutil.copytree(FIXTURE, source)
    mod = json.loads((source / "mod.json").read_text())
    mod["dependencies"] = ["first", "second"]
    (source / "mod.json").write_text(json.dumps(mod))
    archive = pack_mod(str(source), tmp_path / "packages")
    assert main(["install", str(archive), "--deploy", str(tmp_path / "deploy")]) == 0
    assert validate(tmp_path / "deploy/mods/minimal-mod", True) == []


def test_legacy_uppercase_and_missing_dependency(tmp_path):
    source = tmp_path / "mods/Legacy/data"
    source.mkdir(parents=True)
    assert conflict_lines(tmp_path, "Legacy", ["Missing"]) == ["WARN missing dependency: Missing"]


def test_malformed_override_manifest(tmp_path):
    root = tmp_path / "mods/active/data/model_overrides"
    root.mkdir(parents=True)
    (root / "models.json").write_text('{"overrides":[null]}')
    assert main(["check", "--deploy", str(tmp_path), "--active", "active"]) == 1


def test_rollback_canonical_and_unsafe_id(tmp_path):
    assert main(["rollback", "../escape", "--to", "1", "--deploy", str(tmp_path)]) == 1
    assert main(["rollback", "minimal-mod", "--to", "1", "--deploy",
                 "A:/Games/mc2-opengl/mc2-win64-v0.4"]) == 1


def test_linked_active_refused(tmp_path):
    deploy = installed_pair(tmp_path)
    current = deploy / "mods/minimal-mod"
    moved = tmp_path / "linked-target"
    current.rename(moved)
    try:
        current.symlink_to(moved, target_is_directory=True)
    except OSError:
        moved.rename(current)
        pytest.skip("symlinks unavailable")
    assert main(["rollback", "minimal-mod", "--to", "1.0.0", "--deploy", str(deploy)]) == 1
    assert (moved / "data/example.txt").read_text() == "2.0.0"


def installed_pair(tmp_path):
    source = tmp_path / "project"
    shutil.copytree(FIXTURE, source)
    deploy = tmp_path / "deploy"
    for version in ("1.0.0", "2.0.0"):
        mod = json.loads((source / "mod.json").read_text())
        mod["version"] = version
        (source / "mod.json").write_text(json.dumps(mod))
        (source / "data/example.txt").write_text(version)
        archive = pack_mod(str(source), tmp_path / "packages")
        assert main(["install", str(archive), "--deploy", str(deploy)]) == 0
    return deploy


def test_upgrade_list_rollback(tmp_path, capsys):
    deploy = installed_pair(tmp_path)
    current = deploy / "mods/minimal-mod"
    assert main(["list", "--deploy", str(deploy)]) == 0
    assert "2.0.0\treceipt=yes" in capsys.readouterr().out
    assert main(["rollback", "minimal-mod", "--to", "1.0.0", "--deploy", str(deploy)]) == 0
    assert (current / "data/example.txt").read_text() == "1.0.0"
    assert main(["rollback", "minimal-mod", "--to", "2.0.0", "--deploy", str(deploy)]) == 0
    assert (current / "data/example.txt").read_text() == "2.0.0"


@pytest.mark.parametrize("damage", ["active", "retained", "missing", "ambiguous", "receipt", "shared"])
def test_failed_rollback_preserves_active(tmp_path, damage):
    deploy = installed_pair(tmp_path)
    current = deploy / "mods/minimal-mod"
    retained = next((deploy / "mods/.trash").iterdir())
    if damage in ("active", "retained"):
        ((current if damage == "active" else retained) / "data/example.txt").write_text("edit")
    elif damage == "ambiguous":
        shutil.copytree(retained, retained.parent / "duplicate")
    elif damage in ("receipt", "shared"):
        receipt = json.loads((retained / ".install-receipt.json").read_text())
        receipt["version" if damage == "receipt" else "shared_file_mutations"] = "bad"
        (retained / ".install-receipt.json").write_text(json.dumps(receipt))
    before = {p.relative_to(current): p.read_bytes() for p in current.rglob("*") if p.is_file()}
    assert main(["rollback", "minimal-mod", "--to", "9" if damage == "missing" else "1.0.0",
                 "--deploy", str(deploy)]) == 1
    assert before == {p.relative_to(current): p.read_bytes() for p in current.rglob("*") if p.is_file()}


def test_swap_rename_failure_restores(tmp_path):
    current, replacement = tmp_path / "mod", tmp_path / "replacement"
    current.mkdir()
    replacement.mkdir()
    original = Path.rename
    def failing(path, target):
        if path == replacement:
            raise OSError("test activation failure")
        return original(path, target)
    with patch.object(Path, "rename", failing), pytest.raises(OSError):
        swap_version(tmp_path, current, replacement, "1")
    assert current.is_dir() and replacement.is_dir()


def test_conflicts_l1_and_log(tmp_path, monkeypatch):
    monkeypatch.setenv("MC2_SCRATCH", "1")
    for name in ("active", "first", "second", "inactive"):
        root = tmp_path / "mods" / name
        (root / "data/art").mkdir(parents=True)
        (root / "mod.json").write_text(json.dumps(dict(id=name, dependencies=["first"] if name == "active" else [])))
        (root / "data/art/Shared.txt").write_text("same" if name in ("first", "second") else name)
        (root / "data/art/.hidden").write_text("hidden")
        (root / "data/model_overrides").mkdir()
        (root / "data/model_overrides/models.json").write_text(json.dumps(dict(overrides=[dict(replaces="tree:oak")])) )
    scratch = tmp_path / "mods/active/.scratch/data/art"
    scratch.mkdir(parents=True)
    (scratch / "shared.txt").write_text("scratch")
    lines = conflict_lines(tmp_path, "active", ["second", "first"])
    assert "[mod-dup] data/art/shared.txt  winner=[active#scratch]  loser=[first]" in lines
    assert any("models key tree:oak" in line for line in lines)
    assert not any("inactive" in line or ".hidden" in line for line in lines)
    log = tmp_path / "engine.log"
    log.write_text("\n".join(line for line in lines if line.startswith("[mod-dup]")))
    args = ["check", "--deploy", str(tmp_path), "--active", "active", "--deps", "second,first", "--log", str(log)]
    assert main(args) == 0
    log.write_text("")
    assert main(args) == 1
    monkeypatch.setenv("MC2_SCRATCH", "0")
    (tmp_path / "mods/active/data/art/Shared.txt").unlink()
    lines = conflict_lines(tmp_path, "active", ["second"])
    assert "INFO identical data/art/shared.txt" in lines
    assert "[mod-dup] data/art/shared.txt  winner=[first]  loser=[second]" in lines
