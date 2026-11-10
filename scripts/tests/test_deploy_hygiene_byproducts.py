"""What: the offline-cook byproducts (visual_*_preview.png, visual_delta_heatmap.png, visual_height_report.json)
are not tracked under any .beauty dir, and check-deploy-hygiene.py still flags them with no exemption.
Why: round-3 item 10 -- these byproducts reached an install's data/missions/<m>.beauty/ (the deploy WARN named
mc2_17/mc2_24, the two most recently re-cooked maps) and the hygiene gate had to warn about them on every deploy.
They are authoring aids with no engine loader (grep: no .cpp/.h reads them), so they belong in no shipping tree and
in no git index: a whole-.beauty-dir copy from a clean checkout must be unable to carry them. If a cook re-creates
them locally they are untracked regenerable output, and re-adding them to git fails this pin."""
import importlib.util
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def _hygiene():
    spec = importlib.util.spec_from_file_location(
        "check_deploy_hygiene", ROOT / "scripts" / "check-deploy-hygiene.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _tracked(*pathspecs):
    out = subprocess.run(["git", "-C", str(ROOT), "ls-files", *pathspecs],
                         capture_output=True, text=True)
    assert out.returncode == 0, out.stderr
    return [line for line in out.stdout.splitlines() if line.strip()]


def test_no_tracked_file_under_a_beauty_dir_matches_the_hygiene_patterns():
    h = _hygiene()
    offenders = [p for p in _tracked()
                 if ".beauty/" in p.replace("\\", "/") and h.is_diagnostic(Path(p).name)]
    assert not offenders, f"cook byproducts tracked in git: {offenders}"


def test_the_hygiene_patterns_still_flag_the_byproduct_names():
    h = _hygiene()
    for name in ("visual_height_preview.png", "visual_damp_preview.png",
                 "visual_delta_heatmap.png", "visual_height_report.json"):
        assert h.is_diagnostic(name), f"check-deploy-hygiene no longer flags {name}"


def test_no_tracked_allowlist_exempts_the_byproducts():
    allows = [p for p in _tracked() if p.endswith(".deploy-hygiene-allow")]
    assert not allows, f"tracked hygiene allowlist(s) would exempt byproducts: {allows}"


def test_cook_byproducts_are_gitignored_so_a_cook_cannot_retrack_them():
    for path in ("tests/terrain/beautify/mc2_01.beauty/visual_height_preview.png",
                 "tests/terrain/beautify/mc2_01.beauty/visual_damp_preview.png",
                 "tests/terrain/beautify/mc2_01.beauty/visual_delta_heatmap.png",
                 "tests/terrain/beautify/mc2_01.beauty/visual_height_report.json"):
        out = subprocess.run(["git", "-C", str(ROOT), "check-ignore", path],
                             capture_output=True, text=True)
        assert out.returncode == 0 and out.stdout.strip() == path, \
            f"cook byproduct not gitignored (a cook + git add -A would re-track it): {path} (rc={out.returncode})"
