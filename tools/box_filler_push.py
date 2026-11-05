"""Push the nifty tip to the box idle filler and rebuild its install (the filler runs the pose jobs on the idle GPUs).

The filler only ever syncs from a bundle this tool sends: nothing on the box polls git, so without a push it stays pinned to its
last sha (the cause of the stale-filler rc=3 failures). Run it after every merge window:  py -3 tools/box_filler_push.py
It sends an incremental bundle of the tip (minus what /workspace/filler/src already has), then runs /workspace/filler/sync.sh in the
pueue `build` group (cores 32-63, no GPU lock) and waits for it; current_sha is written last by sync.sh.
"""
import argparse, subprocess, sys, tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import box_run
from box_run import console_child
from box_run import SSH, SSH_STDIN, run

F = "/workspace/filler"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo", default=str(Path(__file__).resolve().parents[1]))
    ap.add_argument("--ref", default="claude/nifty-mendeleev")
    ap.add_argument("--no-wait", action="store_true", help="queue the sync and return")
    a = ap.parse_args(argv)
    repo = Path(a.repo)
    sha = box_run.git("rev-parse", a.ref, repo=repo)
    cur = run([*SSH, f"cat {F}/current_sha 2>/dev/null"], capture_output=True, text=True).stdout.strip()
    if cur == sha:
        print(f"[filler_push] already at {sha[:12]}")
        return 0
    have = run([*SSH, f"git -C {F}/src for-each-ref --format='%(objectname)'"], capture_output=True, text=True).stdout.split()
    excl = ["^" + h for h in have if subprocess.run(["git", "-C", str(repo), "cat-file", "-e", h + "^{commit}"], capture_output=True, **console_child.child_kwargs(redirected=True)).returncode == 0]
    bundle = Path(tempfile.gettempdir()) / "box_filler_nifty.bundle"
    p = subprocess.run(["git", "-C", str(repo), "bundle", "create", "-q", str(bundle), a.ref, *excl], capture_output=True, text=True, **console_child.child_kwargs(redirected=True))
    if p.returncode:
        print(f"[filler_push] bundle failed: {(p.stderr or p.stdout).strip()[-300:]}")
        return 1
    try:
        r = run([*SSH_STDIN, f"cat > {F}/incoming/nifty.bundle"], input=bundle.read_bytes(), capture_output=True)
    finally:
        bundle.unlink(missing_ok=True)
    if r.returncode:
        print("[filler_push] upload failed")
        return 1
    cmd = f"{box_run.PUEUE} add -g build --print-task-id -l filler_sync:{sha[:8]} -- bash {F}/sync.sh"
    if not a.no_wait:
        cmd = f"{box_run.PUEUE} add -g build --print-task-id -l filler_sync:{sha[:8]} -- bash {F}/sync.sh | tail -1 | xargs -I{{}} {box_run.PUEUE} wait {{}} && cat {F}/current_sha"
    out = run([*SSH, cmd], capture_output=True, text=True)
    print(f"[filler_push] sha={sha[:12]} rc={out.returncode} {out.stdout.strip()[-200:]}")
    return out.returncode


if __name__ == "__main__":
    sys.exit(main())
