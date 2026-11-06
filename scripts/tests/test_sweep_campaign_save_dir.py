"""sweep_campaign's game launch carries a private MC2_SAVE_DIR (the player's savegame folder stays untouched)."""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import sweep_campaign as sc  # noqa: E402


class _FakeProc:
    returncode = 0

    def wait(self, timeout=None):
        return 0


def test_launch_env_has_private_save_dir(tmp_path, monkeypatch):
    seen = {}

    def fake_spawn(cmd, **kw):
        seen["env"] = kw["env"]
        return _FakeProc()

    monkeypatch.setattr(sc.nofocus, "spawn", fake_spawn)
    real = Path.home() / ".mechcommander2" / "savegame"
    monkeypatch.setenv("MC2_SAVE_DIR", str(real))  # a stale parent value must not leak through
    monkeypatch.setattr(sys, "argv", ["sweep_campaign.py", "stock", "5", str(tmp_path)])
    try:
        sc.main()
    except SystemExit:
        pass
    d = Path(seen["env"]["MC2_SAVE_DIR"])
    assert d.is_dir() and d.parent == tmp_path, d
    assert d != real
