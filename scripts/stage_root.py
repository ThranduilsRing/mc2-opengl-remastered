"""Default staging roots for the disposable-install tools (mod_smoke, battery, reload_leak, sim_trace_diff, sound_smoke, tools_smoke,
run_battle_bench). They default to E:/mc2-installs/<tool>-stage (Defender-excluded, terabytes free); G: filled up during a merge gate
and a full drive failed a staging copy mid-run, which looked like a mod failure."""
import os
import shutil
import time
from pathlib import Path

MIN_FREE_GB = 15
STAGE_MARKER = ".stage-root"  # drop this file in a custom root to opt in to pruning
STAGE_BASE = Path(os.environ.get("MC2_STAGE_BASE", "E:/mc2-installs"))


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


def prune_old(root, days=1):
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
        try:
            if child.is_dir() and child.stat().st_mtime < cutoff:
                shutil.rmtree(child, ignore_errors=True)
                removed += 1
        except OSError:
            pass
    return removed
