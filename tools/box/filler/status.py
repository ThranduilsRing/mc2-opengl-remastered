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


def gpu_slots():
    """K game slots per GPU: MC2_BOX_GPU_SLOTS, else GPU_SLOTS in locks/cores.env, else 3"""
    v = os.environ.get("MC2_BOX_GPU_SLOTS", "")
    if not v.isdigit():
        v = ""
        try:
            with open(os.path.join(LOCKS, "cores.env")) as f:
                for ln in f:
                    if ln.startswith("GPU_SLOTS="):
                        v = ln.split("=", 1)[1].strip()
        except OSError:
            pass
    return max(1, int(v)) if v.isdigit() else 3


def gpu_count():
    """Number of GPUs in the one core map: the GPU<n>_CPUS lines of locks/cores.env (0 when unreadable)."""
    n = 0
    try:
        with open(os.path.join(LOCKS, "cores.env")) as f:
            for ln in f:
                key = ln.split("=", 1)[0].strip()
                if key.startswith("GPU") and key.endswith("_CPUS") and key[3:-5].isdigit():
                    n += 1
    except OSError:
        pass
    return n


def gate_gpu():
    """The gate GPU from the one core map (GATE_GPU in locks/cores.env); 0 when unset or unreadable (the one-GPU box's only GPU)."""
    try:
        with open(os.path.join(LOCKS, "cores.env")) as f:
            for ln in f:
                if ln.startswith("GATE_GPU="):
                    v = ln.split("=", 1)[1].strip()
                    return int(v) if v.isdigit() else 0
    except OSError:
        pass
    return 0


def slot_names(n):
    """lock-file base names of GPU n: gpu<n>.s<k> for k < K (the gate GPU too: normal rows take one slot, gate/perf rows take all K)"""
    return [f"gpu{n}.s{k}" for k in range(gpu_slots())]


def owner_names(n):
    """names that can carry a gpu<n>.owner/.row file: the slots plus the legacy gpu<n> (held by an exclusive gate/perf row, and by an old lane script)"""
    return slot_names(n) + [f"gpu{n}"]


def all_slot_names(gpus=None):
    gpus = range(gpu_count()) if gpus is None else gpus
    return [b for n in gpus for b in slot_names(n)]


def lock_holders():
    """{inode: pid} of every lock in /proc/locks ("N: FLOCK ADVISORY WRITE <pid> MAJ:MIN:INODE 0 EOF"); no lock is taken"""
    try:
        with open("/proc/locks") as f:
            return {int(p[5].rsplit(":", 1)[1]): int(p[4]) for p in (ln.split() for ln in f) if len(p) > 5 and ":" in p[5] and p[4].isdigit()}
    except (OSError, ValueError, IndexError):
        return {}


def locked_inodes():
    return set(lock_holders())


def slot_entry(name, now, inodes, filler):
    base = os.path.join(LOCKS, name)
    e = {"locked": False, "holder": None, "pid": None, "alive": None, "age_s": None, "row": None, "filler": filler}
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


def gpu_entry(n, now, inodes):
    """Per GPU: the old single-holder keys (from the first held slot) plus slots=[...], held, k, waiters (fresh gpu<n>.want) and idle_waiter_s
    (how long a waiter has had a free slot: should stay ~0; a growing number means the queue is stuck while the GPU idles)"""
    filler = os.path.exists(os.path.join(FILLER, f"active.{n}"))
    slots = [slot_entry(b, now, inodes, filler) for b in slot_names(n)]
    legacy = slot_entry(f"gpu{n}", now, inodes, filler)   # an exclusive gate/perf row (or a lane still on the old exclusive-lock script) holds the legacy gpu<n> file: show it while it is held
    if legacy["locked"]:
        slots.append(legacy)
    held = [s for s in slots if s["locked"]]
    e = dict((held or slots)[0])
    e["locked"] = bool(held)
    e["slots"], e["held"], e["k"] = slots, len(held), len(slots)
    try:
        e["waiters"] = int(now - os.stat(os.path.join(LOCKS, f"gpu{n}.want")).st_mtime < 30)
    except OSError:
        e["waiters"] = 0
    mark = os.path.join(LOCKS, f".gpu{n}.idle_waiter")
    try:
        if e["waiters"] and len(held) < len(slots):
            if not os.path.exists(mark):
                open(mark, "w").close()
            e["idle_waiter_s"] = int(now - os.stat(mark).st_mtime)
        else:
            if os.path.exists(mark):
                os.unlink(mark)
            e["idle_waiter_s"] = 0
    except OSError:
        e["idle_waiter_s"] = None
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
