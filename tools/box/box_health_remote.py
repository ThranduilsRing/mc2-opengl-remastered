#!/usr/bin/env python3
"""Box health as one JSON document (deployed as /workspace/bin/box-health; PC side: py -3 tools/box_health.py).

usage: box-health [--floor-gb 15] [--rows 12]     exit 0 = healthy, 1 = at least one problem (the JSON says which)
  gpus        per GPU: util, memory, lock holder pid + alive, the row it runs, compute processes
  filler      STOP flag, current/mirror sha, per-worker alive + script version, results/failures at the current sha
  supervisor  heartbeat age, restarts, backoff, gate GPU guard state, the problems of its last tick
  disk        free GB vs the floor, diskwatch state, lane dir sizes, ccache size/max
  cpu         nproc, the cgroup quota (cpu.max), load and load / quota, throttling
  toolchain   clang-20 / cmake / ninja / driver versions, VVL dir
  rows        the last N box_run rows (lane, sha, row, rc) and the latest filler results and failures
Never reads the process environment or /etc/environment (they hold credentials): every field comes from files or tool output listed above.
"""
import argparse, glob, json, os, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.realpath(__file__)))
import box_supervisor as sup   # noqa: E402
import status                  # noqa: E402  (box_supervisor put filler/ on sys.path)

CGROUP = os.environ.get("MC2_BOX_CGROUP", "/sys/fs/cgroup")
VVL_DIR = sup.W + "/toolchains/vvl-1.4.350.0"
HEARTBEAT_MAX_S = 120
WARN_FREE_GB = 30


def sh(cmd, timeout=10):
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)  # background-launch: allow runs on the Linux box only (nvidia-smi/pueue probes); never this desktop
        return r.returncode, r.stdout.strip()
    except (OSError, subprocess.TimeoutExpired):
        return -1, ""


def read(path, default=""):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return default


def read_json(path):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def gpus(now, problems):
    rc, out = sh(["nvidia-smi", "--query-gpu=index,pci.bus_id,utilization.gpu,memory.used,memory.total", "--format=csv,noheader,nounits"], 20)
    if rc != 0:
        problems.append("nvidia-smi failed")
        return []
    _, apps = sup.nvidia_query()
    holders = status.lock_holders()
    res = []
    for ln in out.splitlines():
        p = [x.strip() for x in ln.split(",")]
        if len(p) < 5 or not p[0].isdigit():
            continue
        n, bus = int(p[0]), p[1].upper()
        e = {"index": n, "bus": bus, "util_pct": int(p[2]) if p[2].isdigit() else None,
             "mem_mb": int(p[3]) if p[3].isdigit() else None, "mem_total_mb": int(p[4]) if p[4].isdigit() else None}
        e.update(status.gpu_entry(n, now, set(holders)))
        e["procs"] = [{"pid": pid, "name": name, "mem_mb": mem} for b, pid, name, mem in (apps or []) if b == bus]
        for nm in status.owner_names(n):
            own = f"{sup.LOCKS}/{nm}.owner"
            if os.path.exists(own) and not sup.alive(sup.read_pidfile(own)):
                try:
                    age = now - os.stat(own).st_mtime
                except OSError:
                    age = 0
                if age > HEARTBEAT_MAX_S:
                    problems.append(f"{nm}.owner names a dead pid for {int(age)}s (supervisor not reaping?)")
        if e.get("idle_waiter_s") and e["idle_waiter_s"] > 60:
            problems.append(f"gpu{n}: a lane has waited {e['idle_waiter_s']}s while a slot was free ({e['held']}/{e['k']} held)")
        res.append(e)
    want = sup.gpu_count()   # the one core map (locks/cores.env GPU<n>_CPUS), not a hardcoded count
    if want and len(res) < want:
        problems.append(f"nvidia-smi sees {len(res)} GPUs (configured {want})")
    return res


def filler(problems):
    F = sup.FILLER
    stopped = os.path.exists(f"{F}/STOP")
    sha = read(f"{F}/current_sha")
    rc, tip = sh(["git", "--git-dir", sup.MIRROR, "rev-parse", "-q", "--verify", sup.NIFTY + "^{commit}"])
    workers = {}
    for g in sup.FILLER_GPUS:
        ok = sup.worker_alive(g)
        workers[str(g)] = {"alive": ok, "pid": sup.read_pidfile(f"{F}/worker{g}.pid"), "ver": read(f"{F}/worker{g}.ver"),
                           "active": os.path.exists(f"{F}/active.{g}")}
        if not ok and not stopped:
            problems.append(f"filler worker{g} not running")
    rd = f"{F}/results/{sha}" if sha else ""
    return {"stopped": stopped, "current_sha": sha, "mirror_nifty": tip if rc == 0 else None, "behind_mirror": bool(rc == 0 and tip and tip != sha),
            "workers": workers, "results": len(glob.glob(f"{rd}/*.json")) if rd else 0,
            "failed": len(glob.glob(f"{rd}/failed/*.log")) if rd else 0}


def supervisor(now, problems):
    hb = read(sup.HEARTBEAT).split()
    age = int(now - int(hb[0])) if hb and hb[0].isdigit() else None
    pid = int(hb[1]) if len(hb) > 1 and hb[1].isdigit() else None
    if age is None or age > HEARTBEAT_MAX_S:
        problems.append("box-supervisor heartbeat " + ("missing" if age is None else f"{age}s old") + " (supervisorctl status box-supervisor)")
    st = read_json(sup.STATE) or {}
    for p in st.get("problems", []):   # gate GPU guard (fail closed when nvidia-smi fails), worker restart backoff
        problems.append(f"supervisor: {p}")
    recent = {k: len([t for t in v if now - t < 3600]) for k, v in (st.get("restarts") or {}).items()}
    return {"heartbeat_age_s": age, "pid": pid, "alive": sup.alive(pid), "workers": st.get("workers"), "restarts_1h": recent,
            "backoff": sorted((st.get("backoff") or {}).keys()), "gpu3": st.get("gpu3"), "intruders": st.get("intruders"),
            "autosync": st.get("autosync"), "reaped_last": st.get("reaped_last")}


def ccache():
    rc, out = sh(["ccache", "--print-stats"])
    size_kib = None
    for ln in out.splitlines() if rc == 0 else []:
        k, _, v = ln.partition("\t")
        if k == "cache_size_kibibyte" and v.strip().isdigit():
            size_kib = int(v)
    _, mx = sh(["ccache", "-k", "max_size"])
    return {"size_gb": round(size_kib / 2**20, 2) if size_kib is not None else None, "max": mx or None}


def max_gb(s):
    try:
        num, unit = s.split()
        return float(num) * {"KB": 1e-6, "MB": 1e-3, "GB": 1, "TB": 1e3, "KiB": 2**-20, "MiB": 2**-10, "GiB": 1, "TiB": 2**10}[unit]
    except (AttributeError, ValueError, KeyError):
        return None


def disk(floor, problems, warnings):
    s = os.statvfs("/")
    free = round(s.f_bavail * s.f_frsize / 2**30, 1)
    used = round(100 * (1 - s.f_bavail / s.f_blocks), 1) if s.f_blocks else None
    dw = read_json(sup.W + "/diskwatch.status") or {}
    st = read_json(sup.STATE) or {}
    cc = ccache()
    if free < floor:
        problems.append(f"disk: {free} GB free < floor {floor} GB")
    elif free < WARN_FREE_GB:
        warnings.append(f"disk: {free} GB free (< {WARN_FREE_GB} GB headroom)")
    if dw.get("state") not in (None, "OK"):
        warnings.append(f"diskwatch state {dw.get('state')}")
    if dw.get("paused"):
        problems.append(f"diskwatch paused {dw['paused']} lane processes (disk CRITICAL)")
    mx = max_gb(cc["max"])
    if mx and cc["size_gb"] is not None and cc["size_gb"] > 0.95 * mx:
        warnings.append(f"ccache {cc['size_gb']} GB of {cc['max']} (full: evicting)")
    return {"free_gb": free, "used_pct": used, "floor_gb": floor, "headroom_gb": round(free - floor, 1), "diskwatch": dw,
            "lanes_gb": st.get("lanes_gb"), "ccache": cc}


def cpu(warnings):
    n = os.cpu_count() or 0
    mx = read(f"{CGROUP}/cpu.max").split()
    quota = round(int(mx[0]) / int(mx[1]), 1) if len(mx) == 2 and mx[0].isdigit() and mx[1].isdigit() else None
    load = [round(x, 1) for x in os.getloadavg()]
    stat = dict(ln.split() for ln in read(f"{CGROUP}/cpu.stat").splitlines() if len(ln.split()) == 2)
    lvq = round(load[0] / quota, 2) if quota else None
    if lvq and lvq > 1:
        warnings.append(f"load {load[0]} > cpu quota {quota} (runs are throttled; timings unreliable)")
    return {"nproc": n, "quota_cpus": quota, "load": load, "load_vs_nproc": round(load[0] / n, 2) if n else None, "load_vs_quota": lvq,
            "nr_throttled": int(stat["nr_throttled"]) if stat.get("nr_throttled", "").isdigit() else None}


def toolchain(problems):
    def first(cmd):
        rc, out = sh(cmd)
        return out.splitlines()[0] if rc == 0 and out else None
    t = {"clang": first(["clang-20", "--version"]), "cmake": first(["cmake", "--version"]), "ninja": first(["ninja", "--version"]),
         "driver": first(["nvidia-smi", "--query-gpu=driver_version", "--format=csv,noheader", "-i", "0"]),
         "vvl": os.path.basename(VVL_DIR) if os.path.isdir(VVL_DIR) else None,
         "box_release": os.path.basename(os.path.realpath(sup.W + "/mc2-box/current")) if os.path.islink(sup.W + "/mc2-box/current") else None}
    for k in ("clang", "cmake", "ninja", "vvl"):
        if not t[k]:
            problems.append(f"toolchain: {k} missing")
    return t


def rows(n):
    files = sorted(glob.glob(sup.W + "/lanes/*/out/*/rows.tsv"), key=lambda p: os.path.getmtime(p) if os.path.exists(p) else 0, reverse=True)
    out = []
    for p in files:
        parts = p.split("/")
        lane, sha = parts[-4], parts[-2]
        try:
            with open(p) as f:
                lines = [ln.rstrip("\n").split("\t") for ln in f if ln.strip()]
        except OSError:
            continue
        for r in reversed(lines):
            out.append({"lane": lane, "sha": sha[:8], "row": r[0][:120], "rc": r[1] if len(r) > 1 else None})
            if len(out) >= n:
                return out
    return out


def filler_rows(n):
    sha = read(sup.FILLER + "/current_sha")
    if not sha:
        return {}
    rd = f"{sup.FILLER}/results/{sha}"
    res = sorted(glob.glob(rd + "/*.json"), key=os.path.getmtime, reverse=True)[:n]
    bad = sorted(glob.glob(rd + "/failed/*.log"), key=os.path.getmtime, reverse=True)[:n]
    return {"ok": [os.path.basename(p)[:-5] for p in res], "failed": [os.path.basename(p)[:-4] for p in bad]}


def build(floor=15.0, nrows=12, now=None):
    now = time.time() if now is None else now
    problems, warnings = [], []
    doc = {"time": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(now))}
    doc["gpus"] = gpus(now, problems)
    doc["filler"] = filler(problems)
    doc["supervisor"] = supervisor(now, problems)
    doc["disk"] = disk(floor, problems, warnings)
    doc["cpu"] = cpu(warnings)
    doc["toolchain"] = toolchain(problems)
    doc["rows"] = rows(nrows)
    doc["filler_rows"] = filler_rows(nrows)
    doc["problems"] = list(dict.fromkeys(problems))
    doc["warnings"] = warnings
    doc["ok"] = not doc["problems"]
    return doc


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--floor-gb", type=float, default=15.0, help="free-disk floor; below it the box is unhealthy (box_run's DISK_GUARD is 15)")
    ap.add_argument("--rows", type=int, default=12)
    a = ap.parse_args(argv)
    doc = build(a.floor_gb, a.rows)
    print(json.dumps(doc, indent=1))
    return 0 if doc["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
