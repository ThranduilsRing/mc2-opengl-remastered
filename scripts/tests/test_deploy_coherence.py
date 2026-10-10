#!/usr/bin/env python3
# scripts/tests/test_deploy_coherence.py
"""Tests for write-deploy-manifest.py / check-deploy-coherence.py.

Uses TEMP fake deploy dirs only -- never touches A:/Games/mc2-opengl/.

Run:
  py -3 -m pytest scripts/tests/test_deploy_coherence.py -v
"""
from __future__ import annotations

import importlib.util
import io
import json
import subprocess
import sys
from pathlib import Path

import pytest
import os as _os, sys as _sys  # noqa: E401
_sys.path.append(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..", "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each; scripts/console_child.py)

SCRIPTS = Path(__file__).resolve().parents[1]


def _load(name: str):
    spec = importlib.util.spec_from_file_location(
        name.replace("-", "_"), SCRIPTS / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


writer = _load("write-deploy-manifest")
checker = _load("check-deploy-coherence")


@pytest.fixture
def deploy(tmp_path: Path) -> Path:
    """Fake deploy dir: exe + dll + shaders + sidecar, plus manifest."""
    d = tmp_path / "fake-v0.4"
    (d / "shaders" / "include").mkdir(parents=True)
    (d / "mc2.exe").write_bytes(b"EXE" * 1000)
    (d / "avcodec-61.dll").write_bytes(b"DLL" * 500)
    (d / "shaders" / "gos_terrain.frag").write_text("void main() {}\n")
    (d / "shaders" / "include" / "shadow.hglsl").write_text("// pcf\n")
    (d / "options.cfg").write_text("sidecar=1\n")  # sidecar listed explicitly
    files = writer.collect_files(
        d, ["mc2.exe", "options.cfg"],
        ["*.dll", "shaders/*.frag", "shaders/include/*"])
    manifest = writer.build_manifest(d, files, tmp_path)  # non-git worktree
    (d / writer.MANIFEST_NAME).write_text(json.dumps(manifest))
    return d


def _warns(deploy_dir: Path, worktree: Path) -> tuple[int, str]:
    out = io.StringIO()
    n = checker.check(deploy_dir, worktree, out=out)
    return n, out.getvalue()


def test_fresh_manifest_no_warns(deploy, tmp_path):
    n, text = _warns(deploy, tmp_path)
    assert n == 0
    assert "[DEPLOY_COHERENCE] OK" in text


def test_exe_mutated_warns(deploy, tmp_path):
    (deploy / "mc2.exe").write_bytes(b"NEWEXE" * 999)
    n, text = _warns(deploy, tmp_path)
    assert n >= 1
    assert "mc2.exe" in text and "[DEPLOY_COHERENCE] WARN" in text


def test_exe_same_size_content_drift_warns(deploy, tmp_path):
    # same byte count, different content -> hash drift path
    (deploy / "mc2.exe").write_bytes(b"XXE" * 1000)
    n, text = _warns(deploy, tmp_path)
    assert n >= 1
    assert "hash drift" in text and "mc2.exe" in text


def test_shader_mutated_warns(deploy, tmp_path):
    (deploy / "shaders" / "gos_terrain.frag").write_text("void main() { /*x*/ }\n")
    n, text = _warns(deploy, tmp_path)
    assert n >= 1
    assert "gos_terrain.frag" in text


def test_sidecar_mutated_warns(deploy, tmp_path):
    (deploy / "options.cfg").write_text("sidecar=2 drifted\n")
    n, text = _warns(deploy, tmp_path)
    assert n >= 1
    assert "options.cfg" in text


def test_deployed_file_missing_warns(deploy, tmp_path):
    (deploy / "avcodec-61.dll").unlink()
    n, text = _warns(deploy, tmp_path)
    assert n >= 1
    assert "missing" in text and "avcodec-61.dll" in text


def test_missing_manifest_advisory_exit0(tmp_path):
    d = tmp_path / "no-manifest"
    d.mkdir()
    (d / "mc2.exe").write_bytes(b"EXE")
    # library level: zero warns, advisory note
    n, text = _warns(d, tmp_path)
    assert n == 0
    assert "coherence unknown" in text
    # process level: exit 0 always
    r = subprocess.run(
        [sys.executable, str(SCRIPTS / "check-deploy-coherence.py"),
         str(d), "--worktree", str(tmp_path)],
        capture_output=True, text=True, **console_child.child_kwargs(redirected=True))
    assert r.returncode == 0
    assert "[DEPLOY_COHERENCE]" in r.stdout


def test_check_exit0_even_with_drift(deploy, tmp_path):
    (deploy / "mc2.exe").write_bytes(b"stale!")
    r = subprocess.run(
        [sys.executable, str(SCRIPTS / "check-deploy-coherence.py"),
         str(deploy), "--worktree", str(tmp_path)],
        capture_output=True, text=True, **console_child.child_kwargs(redirected=True))
    assert r.returncode == 0  # advisory only, never hard-fail
    assert "WARN" in r.stdout


def test_writer_cli_writes_manifest_only_when_invoked(tmp_path):
    d = tmp_path / "explicit"
    d.mkdir()
    (d / "mc2.exe").write_bytes(b"EXE")
    assert not (d / writer.MANIFEST_NAME).exists()
    r = subprocess.run(
        [sys.executable, str(SCRIPTS / "write-deploy-manifest.py"),
         str(d), "mc2.exe", "--worktree", str(tmp_path)],
        capture_output=True, text=True, **console_child.child_kwargs(redirected=True))
    assert r.returncode == 0, r.stderr
    data = json.loads((d / writer.MANIFEST_NAME).read_text())
    assert data["files"][0]["path"] == "mc2.exe"
    assert "written_at" in data and "git_head" in data


def test_writer_merge_keeps_other_entries(tmp_path):
    d = tmp_path / "merge"
    d.mkdir()
    (d / "mc2.exe").write_bytes(b"GAME")
    (d / "Mission Editor.exe").write_bytes(b"EDITOR")
    for argset in (["mc2.exe"], ["Mission Editor.exe", "--merge"]):
        r = subprocess.run(
            [sys.executable, str(SCRIPTS / "write-deploy-manifest.py"),
             str(d), *argset, "--worktree", str(tmp_path)],
            capture_output=True, text=True, **console_child.child_kwargs(redirected=True))
        assert r.returncode == 0, r.stderr
    paths = {e["path"] for e in
             json.loads((d / writer.MANIFEST_NAME).read_text())["files"]}
    assert paths == {"mc2.exe", "Mission Editor.exe"}


def test_staleness_warn_against_real_repo(deploy):
    # Point the manifest at an ancient commit of THIS repo; commits touching
    # code/mclib/GameOS/shaders since then must trigger the STALE warn.
    repo = SCRIPTS.parent
    old = subprocess.check_output(
        ["git", "-C", str(repo), "rev-list", "--max-parents=0", "HEAD"],
        text=True, **console_child.child_kwargs(redirected=True)).splitlines()[0].strip()
    mpath = deploy / writer.MANIFEST_NAME
    data = json.loads(mpath.read_text())
    data["git_head"] = old
    mpath.write_text(json.dumps(data))
    n, text = _warns(deploy, repo)
    assert "STALE" in text


def _write_csv_manifest(d: Path, head: str = "deadbeef" * 5) -> Path:
    """Emulate deploy_payload.py manifest v1 for files in the fake deploy."""
    import csv as _csv
    mpath = d / checker.CSV_MANIFEST_NAME
    rows = []
    for rel in ("mc2.exe", "shaders/gos_terrain.frag"):
        p = d / rel
        rows.append([rel, checker._sha256(p), p.stat().st_size,
                     head, "2026-06-11T12:00:00"])
    with open(mpath, "w", newline="") as f:
        w = _csv.writer(f)
        w.writerow(["manifest_version", "v1", "", "", ""])
        w.writerow(["relpath", "sha256", "bytes", "src_commit", "timestamp"])
        w.writerows(rows)
    return mpath


def test_csv_manifest_fresh_ok(deploy, tmp_path):
    (deploy / checker.MANIFEST_NAME).unlink()  # csv only
    _write_csv_manifest(deploy)
    n, text = _warns(deploy, tmp_path)
    # head not in tmp_path repo -> exactly the not-found warn, no drift warns
    assert "hash drift" not in text and "size drift" not in text
    assert "deployed file missing" not in text


def test_csv_manifest_detects_exe_mutation(deploy, tmp_path):
    (deploy / checker.MANIFEST_NAME).unlink()
    _write_csv_manifest(deploy)
    (deploy / "mc2.exe").write_bytes(b"MUTATED" * 999)
    n, text = _warns(deploy, tmp_path)
    assert "drift" in text and "mc2.exe" in text


# --- KIT-COHERENCE-1: post-deploy kit files the payload manifest cannot see --
#
# The bug these guard: .deployed_manifest.csv lists only what deploy_payload.py
# COPIED, so files a lane writes into the install afterwards (the A/B kit) had
# no rows at all -- and the checker reported the install coherent regardless.


def _fake_worktree(tmp_path: Path, rows, globs=("AB-*.bat",),
                   blessed_from: Path | None = None) -> Path:
    """A worktree stub carrying scripts/kit-digests.csv."""
    wt = tmp_path / "wt"
    (wt / "scripts").mkdir(parents=True, exist_ok=True)
    lines = ["# test pin"]
    if blessed_from is not None:
        lines.append(f"# blessed_from: {blessed_from}")
    lines += [f"#glob {g}" for g in globs]
    lines.append("relpath,sha256,bytes,source")
    lines += [",".join(str(c) for c in r) for r in rows]
    (wt / "scripts" / checker.KIT_DIGESTS_NAME).write_text(
        "\n".join(lines) + "\n", encoding="utf-8")
    return wt


def _kit(deploy: Path, name: str = "AB-0-DEFAULT-baseline.bat",
         body: bytes = b"@echo off\r\nREM measured 47.7%\r\n") -> Path:
    p = deploy / name
    p.write_bytes(body)
    return p


def test_kit_absent_digest_file_says_so_not_ok(deploy, tmp_path):
    """No pin file -> the run must SAY the kit is uncovered, never imply green."""
    n, text = _warns(deploy, tmp_path)
    assert n == 0
    assert "post-deploy kit files are NOT covered" in text


def test_kit_match_is_reported(deploy, tmp_path):
    p = _kit(deploy)
    wt = _fake_worktree(tmp_path, [[p.name, checker._sha256(p),
                                    p.stat().st_size, ""]])
    n, text = _warns(deploy, wt)
    assert n == 0
    assert "kit OK: 1/1" in text


def test_kit_one_byte_drift_warns(deploy, tmp_path):
    """Same SIZE, one byte different -> must take the sha256 arm.

    The byte lives in the arm's header prose, which check-ab-kit.py excludes
    from the common block and never parses as a `set` line: exactly the class
    both existing graders pass.
    """
    p = _kit(deploy)
    wt = _fake_worktree(tmp_path, [[p.name, checker._sha256(p),
                                    p.stat().st_size, ""]])
    p.write_bytes(p.read_bytes().replace(b"47.7%", b"47.1%"))
    n, text = _warns(deploy, wt)
    assert n == 1
    assert "kit hash drift" in text and p.name in text


def test_kit_deleted_file_warns(deploy, tmp_path):
    """Deleting the WHOLE kit must be loud, not N/A.

    This test failed the first implementation, which triggered on presence:
    with every pinned file gone there was nothing to trigger on, so wholesale
    deletion produced the quietest verdict the checker has. The blessed-install
    discriminator exists because of this failure.
    """
    p = _kit(deploy)
    wt = _fake_worktree(tmp_path, [[p.name, checker._sha256(p),
                                    p.stat().st_size, ""]],
                        blessed_from=deploy)
    p.unlink()
    n, text = _warns(deploy, wt)
    assert n == 1
    assert "kit file missing" in text


def test_kit_wholesale_deletion_from_blessed_install_warns_every_row(
        deploy, tmp_path):
    files = [_kit(deploy, n) for n in ("AB-0-A.bat", "AB-1-B.bat", "AB-2-C.bat")]
    wt = _fake_worktree(
        tmp_path,
        [[p.name, checker._sha256(p), p.stat().st_size, ""] for p in files],
        blessed_from=deploy)
    for p in files:
        p.unlink()
    n, text = _warns(deploy, wt)
    assert n == 3
    assert text.count("kit file missing") == 3
    assert "N/A" not in text


def test_kit_unpinned_extra_arm_warns(deploy, tmp_path):
    """An added tenth arm must not ride along ungraded."""
    p = _kit(deploy)
    wt = _fake_worktree(tmp_path, [[p.name, checker._sha256(p),
                                    p.stat().st_size, ""]])
    _kit(deploy, "AB-9-INJECTED.bat", b"@echo off\r\n")
    n, text = _warns(deploy, wt)
    assert n == 1
    assert "unpinned kit file" in text and "AB-9-INJECTED.bat" in text


def test_kit_mirror_drift_warns(deploy, tmp_path):
    """The shipped copy of a repo script must track the repo copy."""
    p = _kit(deploy, "verify-ab-kit.py", b"print('grader v1')\n")
    wt = _fake_worktree(tmp_path,
                        [[p.name, checker._sha256(p), p.stat().st_size,
                          "scripts/check-ab-kit.py"]],
                        globs=("verify-ab-kit.py",))
    (wt / "scripts" / "check-ab-kit.py").write_bytes(b"print('grader v2')\n")
    n, text = _warns(deploy, wt)
    assert n == 1
    assert "kit mirror drift" in text


def test_kit_no_kit_in_this_install_is_na_not_pass(deploy, tmp_path):
    """A deploy dir with no kit reports N/A out loud -- never a silent green."""
    wt = _fake_worktree(tmp_path, [["AB-0-DEFAULT-baseline.bat", "0" * 64,
                                    10, ""]])
    n, text = _warns(deploy, wt)
    assert n == 0
    assert "kit coverage N/A (not a pass)" in text


def test_kit_empty_pin_is_a_warn_not_a_green(deploy, tmp_path):
    wt = _fake_worktree(tmp_path, [])
    n, text = _warns(deploy, wt)
    assert n == 1
    assert "pins nothing" in text


def test_strict_flag_turns_drift_into_exit_1(deploy, tmp_path):
    """The whole point: the report can now be used as a gate."""
    (deploy / "mc2.exe").write_bytes(b"stale!")
    argv = [sys.executable, str(SCRIPTS / "check-deploy-coherence.py"),
            str(deploy), "--worktree", str(tmp_path)]
    assert subprocess.run(argv, capture_output=True, **console_child.child_kwargs(redirected=True)).returncode == 0
    r = subprocess.run(argv + ["--strict"], capture_output=True, text=True, **console_child.child_kwargs(redirected=True))
    assert r.returncode == 1
    assert "WARN" in r.stdout


def test_strict_exit_0_when_clean(deploy, tmp_path):
    r = subprocess.run(
        [sys.executable, str(SCRIPTS / "check-deploy-coherence.py"),
         str(deploy), "--worktree", str(tmp_path), "--strict"],
        capture_output=True, text=True, **console_child.child_kwargs(redirected=True))
    assert r.returncode == 0


def test_bless_kit_roundtrip_and_source_preserved(deploy, tmp_path):
    """--bless-kit must be re-runnable and must not drop the source column."""
    p = _kit(deploy, "verify-ab-kit.py", b"print('grader')\n")
    wt = _fake_worktree(tmp_path,
                        [[p.name, checker._sha256(p), p.stat().st_size,
                          "scripts/check-ab-kit.py"]],
                        globs=("verify-ab-kit.py", "AB-*.bat"))
    (wt / "scripts" / "check-ab-kit.py").write_bytes(b"print('grader')\n")
    _kit(deploy, "AB-4-NEW.bat", b"@echo off\r\n")
    assert checker.bless_kit(deploy, wt, out=io.StringIO()) == 2
    rows, globs, blessed_from = checker._load_kit_digests(wt)
    assert checker._samedir(deploy, blessed_from), blessed_from
    by = {r["path"]: r for r in rows}
    assert set(by) == {"verify-ab-kit.py", "AB-4-NEW.bat"}
    assert by["verify-ab-kit.py"]["source"] == "scripts/check-ab-kit.py"
    assert set(globs) == {"verify-ab-kit.py", "AB-*.bat"}
    n, text = _warns(deploy, wt)
    assert n == 0 and "kit OK: 2/2" in text


def test_real_repo_pin_is_parseable_and_nonempty():
    """The shipped scripts/kit-digests.csv must actually load.

    A pin file that silently fails to parse would restore the exact blindness
    this arm removes, with no signal at all.
    """
    rows, globs, blessed_from = checker._load_kit_digests(SCRIPTS.parent)
    assert rows is not None, "scripts/kit-digests.csv is missing"
    assert len(rows) >= 1  # nonempty; do not pin a count that changes on every re-bless
    assert globs and all(g.strip() for g in globs)
    assert blessed_from, "pin records no blessed_from -- deletion would be N/A"
    for r in rows:
        assert len(r["sha256"]) == 64, r
        assert r["size"] and r["size"] > 0, r


def test_newest_manifest_wins(deploy, tmp_path):
    import os
    # json exists (from fixture); csv written later -> csv must be chosen
    mpath = _write_csv_manifest(deploy)
    json_path = deploy / checker.MANIFEST_NAME
    os.utime(json_path, (1, 1))  # force json ancient
    picked = checker._newest_manifest(deploy)
    assert picked.name == checker.CSV_MANIFEST_NAME
    n, text = _warns(deploy, tmp_path)
    assert "deploy_payload.py" in text
