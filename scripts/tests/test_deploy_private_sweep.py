"""Stale .private-<pid> temps from a killed deploy are swept from the target only."""
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from scripts import deploy_payload as dp


def test_sweep(tmp_path, capsys):
    d = tmp_path / "mods" / "k"
    d.mkdir(parents=True)
    stale = d / "a.ktx2.private-99999999"
    live = d / f"b.ktx2.private-{os.getpid()}"
    other = d / "foo.private-notes"
    for f in (stale, live, other):
        f.write_bytes(b"x")
    assert dp.sweep_stale_private_temps(tmp_path) == 1
    assert not stale.exists() and live.exists() and other.exists()
    assert "removed 1 stale .private-<pid> temp files" in capsys.readouterr().out
