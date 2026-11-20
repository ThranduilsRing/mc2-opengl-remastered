"""lane_sim_rows must not hand the merge gate's --allow-owner-install to a lane's sim rows (farm_stage strips it too)."""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "scripts"))
import lane_sim_rows as lsr


def test_sim_rows_strip_allow_owner_install(monkeypatch):
    seen = []

    def runner(root, command, env=None):
        seen.append(list(command))
        return subprocess.CompletedProcess(command, 0, stdout="")

    monkeypatch.setattr(lsr, "stale_install", lambda *a, **k: "")
    verdicts = lsr.run_sim_rows(str(ROOT), "0" * 40, "E:/mc2-installs/lane-x", runner=runner, quiet=lambda: "")
    assert seen, verdicts
    for cmd in seen:
        assert "--allow-owner-install" not in cmd
        assert cmd[cmd.index("--install") + 1] == "E:/mc2-installs/lane-x"
