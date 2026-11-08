"""p31: re-run the ratchets against the ACCEPTED base's baselines (split out of lane_merge.py to keep it under the file-size ratchet)."""
from __future__ import annotations

import os
from pathlib import Path
import shutil
import sys
import tempfile


def base_baseline_check(root, tools_dir, base, ratchets, baselines, run, git, lines):
    """The ratchet baselines at the merged HEAD may be the candidate's own (conflicts resolve to the lane's side), so a lane that
    raised one would pass its own check. Re-run each baselined ratchet against the ACCEPTED base's baseline (git show base:path).
    -> the failing (label, rc, tail); a baseline absent at the base is skipped (a new ratchet has nothing to loosen)."""
    failed = []
    scratch = tempfile.mkdtemp(prefix="base-baseline-", dir=os.environ.get("TMP") or None)
    try:
        for (label, rel, args), path in zip(ratchets, baselines):
            script = os.path.join(tools_dir, *rel.split("/"))
            old = git(root, "show", f"{base}:{path}")
            if old.returncode or not os.path.isfile(script):
                continue
            tmp = os.path.join(scratch, label + ".json")
            Path(tmp).write_text(old.stdout, encoding="utf-8", newline="")
            r = run(root, [sys.executable, "-B", script, *[a for a in args if a != "--tighten"], "--baseline", tmp])
            if r.returncode:
                failed.append((f"{label} vs accepted base {base[:9]}", r.returncode, "\n".join(lines(r.stdout)[-60:])))
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    return failed
