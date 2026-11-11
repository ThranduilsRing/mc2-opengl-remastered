"""The repo copy of the box core map (tools/box/cores.env) matches the live 36-core box: builds on cores 8-35, games on GPU0_CPUS 0-7.
No box, no ssh: pure file assertions on the repo cores.env and the box_run.py fallback."""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CORES_ENV = ROOT / "tools" / "box" / "cores.env"
BOX_RUN = ROOT / "tools" / "box_run.py"


def env():
    return dict(ln.split("=", 1) for ln in CORES_ENV.read_text().splitlines() if re.match(r"^[A-Z0-9_]+=", ln))


def rng(s):
    lo, hi = s.split("-", 1)
    return int(lo), int(hi)


def test_repo_cores_env_build_cpus_is_8_35():
    assert env()["BUILD_CPUS"] == "8-35"
    assert "32-63" not in CORES_ENV.read_text()   # the old 64-core map is gone, comments included


def test_box_run_fallback_matches_the_repo_cores_env():
    m = re.search(r"BCPUS=\$\{BUILD_CPUS:-([^}]+)\}", BOX_RUN.read_text())
    assert m, "box_run.py lost its BCPUS=${BUILD_CPUS:-...} fallback"
    assert m.group(1) == env()["BUILD_CPUS"]


def test_build_and_game_cores_do_not_overlap_and_fit_in_the_36_core_box():
    e = env()
    b, g = rng(e["BUILD_CPUS"]), rng(e["GPU0_CPUS"])
    assert b[0] > g[1]   # build cores start after the GPU 0 game cores end
    assert 0 <= g[0] and g[1] < 36
    assert 0 <= b[0] and b[1] < 36
