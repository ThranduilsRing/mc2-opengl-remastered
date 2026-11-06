#!/usr/bin/env python3
"""Quiet slot for a perf row: run one command with the GPU to itself, its cores pinned and the idle filler paused (/workspace/bin/quiet-slot).

usage (a box_run row; the row already holds its GPU lock):  gpu:/workspace/bin/quiet-slot -- <perf command ...>
  1. writes /workspace/locks/quiet ("pid gpu iso"): every filler worker (worker.sh) and run_job.py start no new job while its pid lives
  2. waits up to --drain-s for running filler jobs (/workspace/filler/active.*) to end; fails (rc 75) if they do not
  3. pins itself (and so the command) to the GPU's cores from cores.env (GPU<N>_CPUS); the GPU is the one whose gpuN.owner pid is an
     ancestor of this process (--gpu overrides)
  4. runs the command and records load, cgroup CPU throttling, filler activity and foreign GPU processes: one `[quiet-slot]` line and
     $OUT/quiet-slot.json (or ./quiet-slot.json)
  5. removes the quiet file (always); box-supervisor reaps it too if this process dies
Exit code: the command's, or 75 when the slot could not be made quiet.
"""
import argparse, json, os, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.realpath(__file__)))
import box_supervisor as sup   # noqa: E402

RC_NOT_QUIET = 75
CGROUP = os.environ.get("MC2_BOX_CGROUP", "/sys/fs/cgroup")


def quiet_path():
    return f"{sup.LOCKS}/quiet"


def parse_cpus(spec):
    out = set()
    for part in spec.split(","):
        part = part.strip()
        if "-" in part:
            a, b = part.split("-", 1)
            out.update(range(int(a), int(b) + 1))
        elif part:
            out.add(int(part))
    return out


def gpu_cpus(gpu):
    try:
        with open(f"{sup.LOCKS}/cores.env") as f:
            for ln in f:
                if ln.startswith(f"GPU{gpu}_CPUS="):
                    return ln.split("=", 1)[1].strip()
    except OSError:
        pass
    return f"{8 * gpu}-{8 * gpu + 7}"


def my_gpu(pid=None):
    pid = os.getpid() if pid is None else pid
    for g in range(4):
        own = sup.read_pidfile(f"{sup.LOCKS}/gpu{g}.owner")
        if own and sup.alive(own) and sup.descends(pid, own):
            return g
    return None


def filler_active():
    try:
        return sorted(n for n in os.listdir(sup.FILLER) if n.startswith("active."))
    except OSError:
        return []


def cpu_stat():
    d = {}
    try:
        with open(f"{CGROUP}/cpu.stat") as f:
            for ln in f:
                k, _, v = ln.partition(" ")
                if v.strip().isdigit():
                    d[k] = int(v)
    except OSError:
        pass
    return d


def claim(gpu, wait_s, now=time.time, nap=time.sleep):
    """write the quiet file; another live quiet slot is waited for (it pauses the same filler)"""
    q = quiet_path()
    end = now() + wait_s
    while True:
        other = sup.read_pidfile(q)
        if other is None or other == os.getpid() or not sup.alive(other):
            tmp = f"{q}.tmp.{os.getpid()}"
            with open(tmp, "w") as f:
                f.write(f"{os.getpid()} {gpu} {time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}\n")
            os.replace(tmp, q)
            return True
        if now() >= end:
            return False
        nap(2)


def release():
    q = quiet_path()
    if sup.read_pidfile(q) == os.getpid():
        try:
            os.unlink(q)
        except OSError:
            pass


def drain(wait_s, now=time.time, nap=time.sleep):
    t0 = now()
    while filler_active():
        waited = now() - t0
        if waited >= wait_s:
            return False, round(waited, 1)
        nap(2)
    return True, round(now() - t0, 1)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gpu", type=int, help="GPU index (default: the gpuN.owner that is an ancestor of this process)")
    ap.add_argument("--drain-s", type=float, default=180.0)
    ap.add_argument("cmd", nargs=argparse.REMAINDER)
    a = ap.parse_args(argv)
    cmd = a.cmd[1:] if a.cmd[:1] == ["--"] else a.cmd
    if not cmd:
        ap.error("no command after --")
    gpu = a.gpu if a.gpu is not None else my_gpu()
    if gpu is None:
        print("[quiet-slot] FAIL no gpuN.owner is an ancestor of this process (run it as a box_run gpu: row, or pass --gpu)", flush=True)
        return RC_NOT_QUIET
    out_dir = os.environ.get("OUT") or os.getcwd()
    rec = {"gpu": gpu, "cmd": cmd, "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
    try:
        if not claim(gpu, a.drain_s):
            print("[quiet-slot] FAIL another quiet slot did not finish in time", flush=True)
            return RC_NOT_QUIET
        ok, waited = drain(a.drain_s)
        rec["drain_s"] = waited
        if not ok:
            print(f"[quiet-slot] FAIL filler did not drain in {a.drain_s:.0f}s: {filler_active()}", flush=True)
            return RC_NOT_QUIET
        spec = gpu_cpus(gpu)
        cpus = parse_cpus(spec)
        if hasattr(os, "sched_setaffinity"):
            os.sched_setaffinity(0, cpus)
            rec["affinity"] = sorted(os.sched_getaffinity(0))
        rec["cpus"] = spec
        buses, apps = sup.nvidia_query()
        bus = (buses or {}).get(gpu)
        own = sup.read_pidfile(f"{sup.LOCKS}/gpu{gpu}.owner")
        load0, st0, t0 = os.getloadavg(), cpu_stat(), time.time()
        rc = subprocess.run(cmd).returncode  # background-launch: allow runs on the Linux box only (wraps the quiet-slot command); never this desktop
        wall = time.time() - t0
        load1, st1 = os.getloadavg(), cpu_stat()
        buses2, apps2 = sup.nvidia_query()
        foreign = sorted({(pid, name) for b, pid, name, _ in (apps or []) + (apps2 or []) if bus and b == bus and not (own and sup.descends(pid, own))})
        rec.update(rc=rc, wall_s=round(wall, 1), load_start=round(load0[0], 2), load_end=round(load1[0], 2),
                   nr_throttled=st1.get("nr_throttled", 0) - st0.get("nr_throttled", 0),
                   throttled_usec=st1.get("throttled_usec", 0) - st0.get("throttled_usec", 0),
                   filler_active_end=filler_active(), foreign_gpu_procs=[f"{p} {n}" for p, n in foreign],
                   exclusive=not foreign, nvidia_smi_ok=buses is not None and buses2 is not None)
        print(f"[quiet-slot] gpu={gpu} cpus={spec} drained_in={waited}s load={rec['load_start']}->{rec['load_end']} "
              f"throttled_usec=+{rec['throttled_usec']} filler_active=0 exclusive={int(rec['exclusive'])} rc={rc} wall={rec['wall_s']}s", flush=True)
        if foreign:
            print(f"[quiet-slot] WARN other processes on GPU {gpu}: {rec['foreign_gpu_procs']}", flush=True)
        return rc
    finally:
        release()
        try:
            with open(os.path.join(out_dir, "quiet-slot.json"), "w") as f:
                json.dump(rec, f, indent=1)
        except OSError:
            pass


if __name__ == "__main__":
    sys.exit(main())
