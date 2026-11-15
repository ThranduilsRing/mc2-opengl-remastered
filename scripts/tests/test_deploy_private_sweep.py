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
    unowned = tmp_path / "mods/custom/model.private-12345"
    unowned.parent.mkdir()
    unowned.write_bytes(b"user mod")
    for f in (stale, live, other):
        f.write_bytes(b"x")
    assert dp.sweep_stale_private_temps(tmp_path, ["mods/k/a.ktx2", "mods/k/b.ktx2"]) == 1
    assert unowned.read_bytes() == b"user mod"
    assert not stale.exists() and live.exists() and other.exists()
    output = capsys.readouterr().out
    assert "removed 1 stale .private-<pid> temp files" in output
    assert "unowned private-temp match preserved:" in output


def test_sweeps_use_each_deploy_paths_write_set():
    """Trigger: deploy_payload.py; both deploy paths sweep after enumeration, before writes."""
    import inspect
    assert "sweep_stale_private_temps" not in inspect.getsource(dp.main)
    for fn, enumeration, write in ((dp._run_deploy, "write_set =", "prepare_seed_delta("),
                                   (dp.deploy_from_payload, "staged =", "for rel, row in rows.items():")):
        src = inspect.getsource(fn)
        assert src.index(enumeration) < src.index("sweep_stale_private_temps(target,") < src.index(write)
