"""lane_merge --precheck/--batch grade a candidate tree, so they run the TRUSTED copy of lane_merge.py (mc2-main, the nifty tip),
never the candidate's own, which the candidate could have edited to pass itself."""
from __future__ import annotations

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.append(os.path.join(HERE, "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each)

#: The tree whose tools/lane_merge.py grades candidates. Override: $MC2_TRUSTED_ROOT.
TRUSTED_ROOT = "A:/Games/mc2-main"
OWN_SCRIPT = os.path.join(HERE, "lane_merge.py")


def trusted_script(own_script: str = OWN_SCRIPT) -> str | None:
    """The trusted copy of lane_merge.py, or None when we already are it (or it does not exist, or we were re-executed from it)."""
    if os.environ.get("MC2_LANE_MERGE_TRUSTED"):
        return None
    path = os.path.join(os.environ.get("MC2_TRUSTED_ROOT", TRUSTED_ROOT), "tools", "lane_merge.py")
    if not os.path.isfile(path) or os.path.normcase(os.path.realpath(path)) == os.path.normcase(os.path.realpath(own_script)):
        return None
    return path


def delegate(args, argv: list[str]) -> int | None:
    """For --precheck/--batch: run the trusted lane_merge.py with --root at the candidate tree and return its rc.
    None = not a grading mode, or already trusted: run in-process."""
    script = trusted_script() if (args.precheck or args.batch) else None
    if script is None:
        return None
    cmd = [sys.executable, script, *argv]
    if not any(a == "--root" or a.startswith("--root=") for a in argv):
        cmd += ["--root", args.root]
    print(f"lane_merge: running the trusted {script} against {args.root}", flush=True)
    return subprocess.call(cmd, env={**os.environ, "MC2_LANE_MERGE_TRUSTED": "1"},  # background-launch: allow trusted copy of this tool, console child
                           **console_child.child_kwargs())
