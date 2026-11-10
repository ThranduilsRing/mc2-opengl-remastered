"""box_run data overlay: each arm's install carries its own revision of the lane's changed data files."""
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "scripts"))
import box_run


def git(repo, *args):
    return subprocess.run(["git", "-C", str(repo), *args], check=True, capture_output=True, text=True).stdout.strip()


def put(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def make_world(tmp_path):
    repo, gold, inst = tmp_path / "repo", tmp_path / "golden", tmp_path / "install"
    repo.mkdir()
    git(repo, "init", "-q")
    git(repo, "config", "user.email", "t@t")
    git(repo, "config", "user.name", "t")
    put(repo / "data/kit/a.json", "base-a")
    put(repo / "data/kit/b.json", "base-b")
    put(repo / "data/kit/same.json", "same")
    put(repo / "src/x.cpp", "x1")
    git(repo, "add", "-A")
    git(repo, "commit", "-qm", "base")
    base = git(repo, "rev-parse", "HEAD")
    put(repo / "data/kit/a.json", "tip-a")
    put(repo / "data/kit/c.json", "tip-c")
    (repo / "data/kit/b.json").unlink()
    put(repo / "src/x.cpp", "x2")   # not data: never overlaid
    git(repo, "add", "-A")
    git(repo, "commit", "-qm", "tip")
    tip = git(repo, "rev-parse", "HEAD")
    for name, text in (("a.json", "gold-a"), ("b.json", "gold-b"), ("same.json", "same")):
        put(gold / "data/kit" / name, text)
    inst.mkdir()
    for f in (gold / "data/kit").iterdir():   # the install links the golden's data, as box_run's cp -al does
        (inst / "data/kit").mkdir(parents=True, exist_ok=True)
        os.link(f, inst / "data/kit" / f.name)
    return repo, gold, inst, base, tip


def test_overlay_picks_exactly_the_changed_data_files_per_arm(tmp_path, capsys):
    repo, gold, inst, base, tip = make_world(tmp_path)
    # B arm (tip): changed a, deleted b, added c; same.json and src/ untouched
    assert box_run.apply_data_overlay(repo, base, tip, tip, inst, gold) == (3, len("tip-a") + len("tip-c"))
    assert "[box_run] data_overlay files=3 bytes=10" in capsys.readouterr().out   # a, b (deleted at the arm) and c count
    assert (inst / "data/kit/a.json").read_text() == "tip-a"
    assert (inst / "data/kit/c.json").read_text() == "tip-c"
    assert not (inst / "data/kit/b.json").exists()
    assert (inst / "data/kit/same.json").read_text() == "same"
    assert not (inst / "src").exists()
    # the golden is never written through the hardlink
    assert (gold / "data/kit/a.json").read_text() == "gold-a" and (gold / "data/kit/b.json").read_text() == "gold-b"
    # A arm (base) on the same install: the base's version of the same file set, the lane's addition gone
    n, size = box_run.apply_data_overlay(repo, base, tip, base, inst, gold)
    assert (n, size) == (3, len("base-a") + len("base-b"))
    assert (inst / "data/kit/a.json").read_text() == "base-a"
    assert (inst / "data/kit/b.json").read_text() == "base-b"
    assert not (inst / "data/kit/c.json").exists()
    assert (gold / "data/kit/a.json").read_text() == "gold-a"


def test_zero_overlay_restores_golden_and_prints_the_line(tmp_path, capsys):
    repo, gold, inst, base, tip = make_world(tmp_path)
    box_run.apply_data_overlay(repo, base, tip, tip, inst, gold)
    capsys.readouterr()
    assert box_run.apply_data_overlay(repo, "", tip, tip, inst, gold) == (0, 0)   # no base ref: N=0 is legal and printed
    assert "[box_run] data_overlay files=0 bytes=0" in capsys.readouterr().out
    assert (inst / "data/kit/a.json").read_text() == "gold-a"
    assert (inst / "data/kit/b.json").read_text() == "gold-b"
    assert not (inst / "data/kit/c.json").exists()
    assert os.path.samefile(inst / "data/kit/a.json", gold / "data/kit/a.json")


def test_remote_script_applies_overlay_before_manifest():
    r = box_run.REMOTE
    assert r.index("apply_data_overlay") < r.index("write_payload_manifest")
    assert "OVL_BASE=${AB_BEFORE:-" in r
