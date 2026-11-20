"""sim_determinism refuses the owner's installs (A:/Games/mc2-opengl/) unless the merge gate's flag is passed."""
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import sim_determinism as sim

OWNER = "A:/Games/mc2-opengl/mc2-win64-v0.4"


@pytest.mark.parametrize("path", [OWNER, "a:\\games\\MC2-OpenGL\\mc2-win64-v0.4c", "A:/Games/mc2-opengl/mods/x/"])
def test_owner_paths_refused(path):
    assert "refusing" in sim.owner_install_refusal(path, False)


@pytest.mark.parametrize("path", ["E:/mc2-installs/harness-guards-1", "A:/Games/mc2-opengl-other/x"])
def test_other_paths_allowed(path):
    assert sim.owner_install_refusal(path, False) == ""


def test_flag_allows_owner_path():
    assert sim.owner_install_refusal(OWNER, True) == ""


@pytest.mark.parametrize("argv", [["check", "mc2_03"], ["run", "mc2_03", "--install", OWNER]])
def test_default_and_explicit_owner_install_exit_own_rc(argv, capsys):
    assert sim.main(argv) == sim.RC_OWNER_INSTALL
    assert "refusing install" in capsys.readouterr().err


def test_refusal_rc_is_not_the_starved_rc():
    assert sim.RC_OWNER_INSTALL not in (0, 1, 2)


def test_battery_determinism_refuses_owner_install_loudly():
    import battery
    assert battery.RC_OWNER_INSTALL == sim.RC_OWNER_INSTALL and battery.OWNER_INSTALL_ROOT == sim.OWNER_INSTALL_ROOT
    assert battery.install_is_owner_owned(OWNER) and not battery.install_is_owner_owned("E:/mc2-installs/x")

    class Ctx:
        install = OWNER
        dry = False

        def stage_dir(self, name):
            return Path(name)

    res = battery.step_determinism(Ctx())
    assert res.verdict == battery.FAIL and res.rc == sim.RC_OWNER_INSTALL


def test_merge_gate_passes_flag():
    text = (ROOT / "scripts" / "merge_gate.py").read_text(encoding="utf-8")
    line = next(l for l in text.splitlines() if '"tools/sim_determinism.py", "check"' in l)
    assert '"--allow-owner-install"' in line
