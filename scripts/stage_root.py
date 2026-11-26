"""Default staging roots for the disposable-install tools (mod_smoke, battery, reload_leak, sim_trace_diff, sound_smoke, tools_smoke,
run_battle_bench). They default to G:/mc2-installs/<tool>-stage. Free-space checks run before staging.
"""
import os
import shutil
import time
from pathlib import Path

MIN_FREE_GB = 15
STAGE_MARKER = ".stage-root"  # drop this file in a custom root to opt in to pruning
from smoke_lib.paths import INSTALL_ROOT

STAGE_BASE = Path(os.environ.get("MC2_STAGE_BASE", INSTALL_ROOT))


def default_root(tool):
    return STAGE_BASE / f"{tool}-stage"


def preflight(root, min_gb=MIN_FREE_GB):
    """Raise OSError when the drive holding root has less than min_gb free (the stage need not exist yet)."""
    probe = Path(root).resolve()
    while not probe.exists() and probe.parent != probe:
        probe = probe.parent
    free = shutil.disk_usage(probe).free / 2**30
    if free < min_gb:
        raise OSError(f"refusing to stage under {root}: only {free:.1f} GB free on {probe.anchor} (need {min_gb} GB); "
                      f"free space or pass --stage-root on another drive")


def prune_old(root, days=1, exclude=()):
    """Delete stage directories directly under root that were last modified more than days ago (a PASS deletes its own stage,
    so what is left is failed runs). Returns the number removed."""
    root = Path(root)
    if not root.is_dir():
        return 0
    if not (root.name.endswith("-stage") or (root / STAGE_MARKER).is_file()):
        return 0  # never prune an arbitrary user-supplied root
    cutoff = time.time() - days * 86400
    removed = 0
    for child in root.iterdir():
        if child.name in exclude:
            continue
        try:
            if child.is_dir() and child.stat().st_mtime < cutoff:
                shutil.rmtree(child, ignore_errors=True)
                removed += 1
        except OSError:
            pass
    return removed
