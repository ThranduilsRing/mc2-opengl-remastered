"""Tests for tools/spawn_lane.py (DEV-VELOCITY-LANES-1).

Hermetic: builds throwaway git repos under tmp_path; never touches the real
engine repo, never runs cmake/robocopy against real trees. The dry-run tests
subprocess the tool exactly as an agent would invoke it.

Run:  py -3 -m pytest tools/test_spawn_lane.py -q
"""

import subprocess
import sys
from pathlib import Path

import pytest

TOOL = Path(__file__).resolve().parent / "spawn_lane.py"

sys.path.insert(0, str(Path(__file__).resolve().parent))
import spawn_lane  # noqa: E402


# --- unit: path_variants -----------------------------------------------------

def test_path_variants_cover_all_observed_spellings():
    old, new = "A:/Games/mc2-donor-1", "A:/Games/mc2-lane-x"
    pairs = spawn_lane.path_variants(old, new)
    olds = [ob for ob, _ in pairs]
    # forward slash (CMake), backslash (MSBuild), double-backslash (escaped),
    # UPPERCASE backslash (tlogs), lowercase drive (CMakeCache header) --
    # each in UTF-8 and UTF-16-LE.
    for spelling in [
        b"A:/Games/mc2-donor-1",
        b"A:\\Games\\mc2-donor-1",
        b"A:\\\\Games\\\\mc2-donor-1",
        b"A:\\GAMES\\MC2-DONOR-1",
        b"a:/Games/mc2-donor-1",
        "A:/Games/mc2-donor-1".encode("utf-16-le"),
        "A:\\GAMES\\MC2-DONOR-1".encode("utf-16-le"),
        "a:/Games/mc2-donor-1".encode("utf-16-le"),
    ]:
        assert spelling in olds, f"missing spelling: {spelling!r}"
    # replacements preserve the spelling style of the pattern they replace
    mapping = dict(pairs)
    assert mapping[b"A:\\GAMES\\MC2-DONOR-1"] == b"A:\\GAMES\\MC2-LANE-X"
    assert mapping[b"a:/Games/mc2-donor-1"] == b"a:/Games/mc2-lane-x"


# --- unit: rewrite_tree --------------------------------------------------------

def test_rewrite_tree_handles_utf8_and_utf16_and_skips_binary(tmp_path):
    old, new = "A:/Games/mc2-donor-1", "A:/Games/mc2-lane-x"
    (tmp_path / "a.vcxproj").write_bytes(
        b"<Project>A:\\Games\\mc2-donor-1\\code\\x.cpp</Project>")
    (tmp_path / "cl.command.1.tlog").write_bytes(
        "^A:\\GAMES\\MC2-DONOR-1\\CODE\\X.CPP\r\n".encode("utf-16-le"))
    (tmp_path / "CMakeCache.txt").write_bytes(
        b"# For build in directory: a:/Games/mc2-donor-1/build64\n")
    binary_payload = b"\x00\x01A:\\Games\\mc2-donor-1\x02"
    (tmp_path / "x.obj").write_bytes(binary_payload)

    scanned, changed = spawn_lane.rewrite_tree(tmp_path, old, new)
    assert changed == 3
    assert b"mc2-lane-x" in (tmp_path / "a.vcxproj").read_bytes()
    assert "MC2-LANE-X".encode("utf-16-le") in (
        tmp_path / "cl.command.1.tlog").read_bytes()
    assert b"a:/Games/mc2-lane-x/build64" in (
        tmp_path / "CMakeCache.txt").read_bytes()
    # .obj untouched (binary extension skip)
    assert (tmp_path / "x.obj").read_bytes() == binary_payload


# --- dry-run integration --------------------------------------------------------

def make_repo(root: Path, warm: bool) -> Path:
    repo = root / "donor"
    repo.mkdir()
    subprocess.run(["git", "init", "-q", "-b", "main", str(repo)], check=True)
    subprocess.run(["git", "-C", str(repo), "config", "user.email", "t@t"],
                   check=True)
    subprocess.run(["git", "-C", str(repo), "config", "user.name", "t"],
                   check=True)
    (repo / "hello.txt").write_text("hi\n")
    subprocess.run(["git", "-C", str(repo), "add", "hello.txt"], check=True)
    subprocess.run(["git", "-C", str(repo), "commit", "-q", "-m", "seed"],
                   check=True)
    if warm:
        exe = repo / "build64" / "RelWithDebInfo" / "mc2.exe"
        exe.parent.mkdir(parents=True)
        exe.write_bytes(b"MZ fake")
    return repo


def run_tool(*argv: str) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(TOOL), *argv],
                          capture_output=True, text=True)


def test_dry_run_clone_mode_creates_nothing(tmp_path):
    repo = make_repo(tmp_path, warm=True)
    dest_root = tmp_path / "lanes"
    proc = run_tool("demo-lane", "--donor", str(repo),
                    "--dest-root", str(dest_root), "--dry-run")
    assert proc.returncode == 0, proc.stderr
    out = proc.stdout
    assert "mode:    clone" in out
    assert "claude/demo-lane-1" in out
    assert "--dry-run: creating NOTHING" in out
    # nothing created: no dest dir, no new worktree, no new branch
    assert not (dest_root / "mc2-demo-lane").exists()
    wt = subprocess.run(["git", "-C", str(repo), "worktree", "list"],
                        capture_output=True, text=True).stdout
    assert len(wt.strip().splitlines()) == 1
    br = subprocess.run(["git", "-C", str(repo), "branch", "--list",
                         "claude/demo-lane-1"],
                        capture_output=True, text=True).stdout
    assert br.strip() == ""


def test_dry_run_fresh_mode_when_donor_cold(tmp_path):
    repo = make_repo(tmp_path, warm=False)
    proc = run_tool("demo-lane", "--donor", str(repo),
                    "--dest-root", str(tmp_path / "lanes"), "--dry-run")
    assert proc.returncode == 0, proc.stderr
    assert "mode:    fresh" in proc.stdout
    assert "unzip 3rdparty.zip" in proc.stdout


def test_explicit_clone_mode_refused_when_donor_cold(tmp_path):
    repo = make_repo(tmp_path, warm=False)
    proc = run_tool("demo-lane", "--donor", str(repo),
                    "--dest-root", str(tmp_path / "lanes"),
                    "--mode", "clone", "--dry-run")
    assert proc.returncode != 0
    assert "no built mc2.exe" in proc.stderr


def test_rejects_non_kebab_lane_name(tmp_path):
    repo = make_repo(tmp_path, warm=True)
    proc = run_tool("Bad_Name", "--donor", str(repo), "--dry-run")
    assert proc.returncode == 2
    assert "kebab-case" in proc.stderr


def test_refuses_existing_dest(tmp_path):
    repo = make_repo(tmp_path, warm=True)
    dest_root = tmp_path / "lanes"
    (dest_root / "mc2-demo-lane").mkdir(parents=True)
    proc = run_tool("demo-lane", "--donor", str(repo),
                    "--dest-root", str(dest_root), "--dry-run")
    assert proc.returncode == 4
    assert "refusing to clobber" in proc.stderr


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-q"]))
