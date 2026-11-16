"""What: scripts/clean_install_byproducts.py lists (dry run) and deletes (--apply) exactly the
offline-cook byproducts under an install's data/missions/*.beauty/ dirs, keeps tracked runtime
sidecars, never follows symlinks/junctions, and refuses every protected root with exit 2.
Why: round-7 item 1 -- deploys WARN that installs carry cook byproducts (deploy never deletes
non-payload files; 2585af79f only stopped new ones from being tracked). The cleaner must match
the check-deploy-hygiene patterns (imported, not copied), keep any name `git ls-files
tests/terrain/beautify` shows as tracked (visual_height_16x.r32 is a tracked runtime file for
mc2_17/24), and never touch the golden mc2-win64-v0.4c, owner-test-* installs,
A:/Games/MC2 OpenGL Remastered or E:/mc2-archive-2026-10. All tests use tmp dirs only --
never a real install."""
import os
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import console_child  # noqa: E402
sys.path.insert(0, str(ROOT / "tools"))
import gate_hosts  # noqa: E402

SCRIPT = "scripts/clean_install_byproducts.py"

BYPRODUCTS = [("visual_height_preview.png", 100),
              ("visual_delta_heatmap.png", 250),
              ("visual_height_report.json", 50)]
DECOYS = [("control_map.png", 10),
          ("visual_height_4x.r32", 4),
          ("gameplay_height.sig", 8)]
OUTSIDE = [("visual_damp_preview.png", 77)]   # matches a pattern, but lives outside any .beauty dir


def run_cli(install, *extra):
    return subprocess.run(
        [sys.executable, SCRIPT, str(install), *extra],
        cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace",
        timeout=120, **console_child.child_kwargs(redirected=True))


def git(repo, *args):
    env = os.environ.copy()
    for name in ("GIT_INDEX_FILE", "GIT_DIR", "GIT_WORK_TREE", "GIT_COMMON_DIR"):
        env.pop(name, None)
    env.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull)
    return subprocess.run(["git", *args], cwd=repo, env=env, check=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30,
                          **console_child.child_kwargs(redirected=True))


def make_install(base):
    install = base / "install"
    beauty = install / "data" / "missions" / "mc2_01.beauty"
    beauty.mkdir(parents=True)
    for name, size in BYPRODUCTS + DECOYS:
        (beauty / name).write_bytes(b"x" * size)
    outside_dir = install / "data" / "missions" / "mc2_01"
    outside_dir.mkdir()
    for name, size in OUTSIDE:
        (outside_dir / name).write_bytes(b"z" * size)
    return install


def plant_byproduct(install_root):
    beauty = install_root / "data" / "missions" / "mc2_01.beauty"
    beauty.mkdir(parents=True)
    (beauty / "visual_height_preview.png").write_bytes(b"x" * 100)
    return beauty / "visual_height_preview.png"


def test_dry_run_lists_byproducts_and_deletes_nothing(tmp_path):
    install = make_install(tmp_path)
    before = {p.relative_to(install).as_posix() for p in install.rglob("*") if p.is_file()}
    r = run_cli(install)
    assert r.returncode == 0, r.stdout + r.stderr
    for name, _ in BYPRODUCTS:
        assert f"data/missions/mc2_01.beauty/{name}" in r.stdout
    assert "400 bytes" in r.stdout
    for name, _ in DECOYS + OUTSIDE:
        assert name not in r.stdout
    after = {p.relative_to(install).as_posix() for p in install.rglob("*") if p.is_file()}
    assert after == before, "dry run must delete nothing"


def test_apply_removes_only_matching_byproducts(tmp_path):
    install = make_install(tmp_path)
    r = run_cli(install, "--apply")
    assert r.returncode == 0, r.stdout + r.stderr
    for name, _ in BYPRODUCTS:
        assert f"deleted data/missions/mc2_01.beauty/{name}" in r.stdout
        assert not (install / "data/missions/mc2_01.beauty" / name).exists()
    for name, _ in DECOYS:
        assert (install / "data/missions/mc2_01.beauty" / name).is_file()
    for name, _ in OUTSIDE:
        assert (install / "data/missions/mc2_01" / name).is_file()


def test_tracked_name_file_is_kept(tmp_path):
    repo = tmp_path / "repo"
    cook = repo / "tests" / "terrain" / "beautify" / "mc2_01.beauty"
    cook.mkdir(parents=True)
    (cook / "visual_height_preview.png").write_bytes(b"tracked cook")
    git(repo, "init", "--quiet")
    git(repo, "add", "tests/terrain/beautify")
    install = make_install(tmp_path)
    r = run_cli(install, "--apply", "--repo", str(repo))
    assert r.returncode == 0, r.stdout + r.stderr
    assert (install / "data/missions/mc2_01.beauty/visual_height_preview.png").is_file()
    assert not (install / "data/missions/mc2_01.beauty/visual_delta_heatmap.png").exists()
    assert not (install / "data/missions/mc2_01.beauty/visual_height_report.json").exists()


@pytest.mark.parametrize("which", ["golden", "golden_nested", "owner_test", "remastered", "archive"])
def test_refused_roots_exit_2(tmp_path, which):
    if which == "golden":
        byproduct = plant_byproduct(tmp_path / "mc2-win64-v0.4c")
        install = tmp_path / "mc2-win64-v0.4c"
    elif which == "golden_nested":
        byproduct = plant_byproduct(tmp_path / "mc2-win64-v0.4c")
        install = tmp_path / "mc2-win64-v0.4c" / "data" / "missions"
    elif which == "owner_test":
        byproduct = plant_byproduct(tmp_path / "owner-test-lane7")
        install = tmp_path / "owner-test-lane7"
    elif which == "remastered":
        if gate_hosts.current_host() != "pc" and not Path("A:/Games/MC2 OpenGL Remastered").exists():
            pytest.skip(f"PC-only install root A:/Games/MC2 OpenGL Remastered (gate host {gate_hosts.current_host()})")
        byproduct = None
        install = Path("A:/Games/MC2 OpenGL Remastered") / "clean-byproducts-test-absent"
    else:
        byproduct = None
        install = Path("E:/mc2-archive-2026-10") / "clean-byproducts-test-absent"
    r = run_cli(install, "--apply")
    assert r.returncode == 2, (r.returncode, r.stdout, r.stderr)
    assert "REFUSED" in r.stderr
    if byproduct is not None:
        assert byproduct.is_file(), "a refused root must not be touched"


def _make_dir_link(link, target):
    if os.name == "nt":
        r = subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(target)],
                           capture_output=True, text=True,
                           **console_child.child_kwargs(redirected=True))
        if r.returncode != 0:
            pytest.skip(f"cannot create junction (needs admin?): {r.stderr.strip() or r.stdout.strip()}")
    else:
        os.symlink(target, link, target_is_directory=True)


def test_junction_inside_beauty_is_not_followed(tmp_path):
    outside = tmp_path / "outside"
    outside.mkdir()
    (outside / "visual_height_preview.png").write_bytes(b"outside target")
    beauty = tmp_path / "install" / "data" / "missions" / "mc2_01.beauty"
    beauty.mkdir(parents=True)
    _make_dir_link(beauty / "linked", outside)
    r = run_cli(tmp_path / "install", "--apply")
    assert r.returncode == 0, r.stdout + r.stderr
    assert (outside / "visual_height_preview.png").is_file(), "junction target must not be followed"
    assert (beauty / "linked").exists(), "the junction itself is not a byproduct file"
