#!/usr/bin/env python3
"""Write /workspace/STATUS.json (atomically): per GPU the lock holder, whether its pid is alive, its age and its row; plus load and free disk.

Written every 30 s by /workspace/bin/box-supervisor (tools/box/box_supervisor.py). Read it with `py -3 tools/box_run.py --status`.
Holder = /workspace/locks/gpu<N>.owner ("pid branch sha iso-time", written by box_run's remote half at lock time); row =
gpu<N>.row (the row box_run is running). `locked` comes from /proc/locks (no flock is taken, so a lane is never disturbed).
"""
import json, os, shutil, sys, time

LOCKS = os.environ.get("MC2_BOX_LOCKS", "/workspace/locks")
FILLER = os.environ.get("MC2_BOX_FILLER", "/workspace/filler")
OUT = os.environ.get("MC2_BOX_STATUS", "/workspace/STATUS.json")


def lock_holders():
    """{inode: pid} of every lock in /proc/locks ("N: FLOCK ADVISORY WRITE <pid> MAJ:MIN:INODE 0 EOF"); no lock is taken"""
    try:
        with open("/proc/locks") as f:
            return {int(p[5].rsplit(":", 1)[1]): int(p[4]) for p in (ln.split() for ln in f) if len(p) > 5 and ":" in p[5] and p[4].isdigit()}
    except (OSError, ValueError, IndexError):
        return {}


def locked_inodes():
    return set(lock_holders())


def gpu_entry(n, now, inodes):
    base = os.path.join(LOCKS, f"gpu{n}")
    e = {"locked": False, "holder": None, "pid": None, "alive": None, "age_s": None, "row": None, "filler": os.path.exists(os.path.join(FILLER, f"active.{n}"))}
    try:
        e["locked"] = os.stat(base).st_ino in inodes
    except OSError:
        pass
    try:
        parts = open(base + ".owner").read().split()
        mt = os.stat(base + ".owner").st_mtime
        if len(parts) >= 3 and parts[0].isdigit():
            e["pid"], e["alive"], e["age_s"] = int(parts[0]), os.path.exists(f"/proc/{parts[0]}"), int(now - mt)
            e["holder"] = f"{parts[1]}@{parts[2][:8]}"
    except OSError:
        pass
    try:
        e["row"] = open(base + ".row").read().strip() or None
    except OSError:
        pass
    if e["locked"] and not e["alive"] and e["filler"]:
        e["holder"] = "filler"
    return e


def build(now=None):
    now = time.time() if now is None else now
    inodes = locked_inodes()
    du = shutil.disk_usage("/workspace")
    return {"time": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(now)),
            "load": [round(x, 2) for x in os.getloadavg()],
            "free_gb": round(du.free / 2**30, 1),
            "disk_pct": round(100 * du.used / du.total, 1),
            "gpus": {str(n): gpu_entry(n, now, inodes) for n in range(4)}}


def main():
    tmp = OUT + ".tmp"
    with open(tmp, "w") as f:
        json.dump(build(), f)
    os.replace(tmp, OUT)


if __name__ == "__main__":
    sys.exit(main())
